module;
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

export module bench.client;

import std;
import net.buffer;
import net.epoll;
import net.socket;
import ide.protocol;
import bench.workload;
import bench.stats;

export namespace bench {

using Clock = std::chrono::steady_clock;

// BenchConnection：管理单条 TCP Socket 连接、维持 max_inflight 请求流水线、发送请求并解析响应。
class BenchConnection {
  public:
    BenchConnection(std::string_view host, std::string_view port, std::size_t max_inflight)
        : socket_(net::Socket::connect_tcp_nonblocking(host, port)),
          max_inflight_(max_inflight),
          slots_(max_inflight) {
        if (max_inflight_ == 0 || max_inflight_ > 64) {
            throw std::invalid_argument("max_inflight must be in 1..64");
        }

        // 禁用 Nagle 算法，降低流水线小包延迟。
        int one = 1;
        ::setsockopt(socket_.native_handle(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        free_slots_.reserve(max_inflight_);
        for (std::size_t i = max_inflight_; i > 0; --i) {
            free_slots_.push_back(static_cast<std::uint16_t>(i - 1));
        }
    }

    BenchConnection(const BenchConnection &) = delete;
    BenchConnection &operator=(const BenchConnection &) = delete;
    BenchConnection(BenchConnection &&) noexcept = default;
    BenchConnection &operator=(BenchConnection &&) noexcept = default;

    [[nodiscard]] inline int fd() const noexcept {
        return socket_.native_handle();
    }

    [[nodiscard]] inline bool closed() const noexcept {
        return closed_;
    }

    [[nodiscard]] inline std::size_t inflight() const noexcept {
        return inflight_;
    }

    void register_epoll(net::Epoll &epoll) {
        auto initial = net::EpollInterest::read | net::EpollInterest::write |
                       net::EpollInterest::peer_closed;
        epoll.add(fd(), initial);
        last_interest_ = initial;
    }

    void on_event(net::EpollEvent event, net::Epoll &epoll, Workload &workload, Stats &stats,
                  Clock::time_point warmup_end, Clock::time_point test_end, bool stopping) {
        if (closed_) {
            return;
        }

        try {
            if (!connected_) {
                if (!event.writable() && !event.error() && !event.hangup() &&
                    !event.peer_closed()) {
                    return;
                }
                socket_.check_connect_result();
                connected_ = true;
            }

            if (event.error()) {
                throw std::runtime_error("benchmark socket error");
            }

            if (event.readable() || event.peer_closed() || event.hangup()) {
                receive_responses(workload, stats, warmup_end, test_end);
            }

            if (!stopping && connected_) {
                fill_pipeline(workload);
            }

            if (connected_ && !output_.empty()) {
                flush_output();
            }

            update_interest(epoll);
        } catch (...) {
            stats.record_error(Clock::now() < warmup_end);
            close_from_epoll(epoll);
        }
    }

    void stop_sending(net::Epoll &epoll) {
        if (closed_) {
            return;
        }
        if (connected_ && !output_.empty()) {
            flush_output();
        }
        update_interest(epoll);
    }

  private:
    struct InflightSlot {
        std::uint64_t request_id{0};
        Method method{Method::system_ping};
        std::size_t wire_bytes{0};
        Clock::time_point send_time{};
        bool in_use{false};
    };

    // 将在途请求补充到 max_inflight（保持流水线拉满）。
    void fill_pipeline(Workload &workload) {
        while (inflight_ < max_inflight_ && !free_slots_.empty()) {
            const std::uint16_t slot_idx = free_slots_.back();
            free_slots_.pop_back();

            auto req = workload.next_request();
            const std::uint64_t seq = next_seq_++;
            const std::uint64_t req_id = (seq << 16) | static_cast<std::uint64_t>(slot_idx);

            auto &slot = slots_[slot_idx];
            slot.request_id = req_id;
            slot.method = req.method;
            slot.wire_bytes = req.wire_bytes();
            slot.send_time = Clock::now();
            slot.in_use = true;

            encode_frame(
                Frame{
                    .length = 0,
                    .kind = Kind::request,
                    .request_id = req_id,
                    .method = req.method,
                    .status = Status::ok,
                    .payload = std::move(req.payload),
                },
                output_);

            ++inflight_;
        }
    }

    void flush_output() {
        while (!output_.empty()) {
            auto n = socket_.try_send(output_.data(), output_.readable_bytes());
            if (!n || *n == 0) {
                break;
            }
            output_.retrieve(*n);
        }
    }

    void receive_responses(Workload &workload, Stats &stats, Clock::time_point warmup_end,
                           Clock::time_point test_end) {
        std::array<char, 16384> chunk{};

        while (true) {
            auto n = socket_.try_recv(chunk.data(), chunk.size());
            if (!n) {
                break;
            }
            if (*n == 0) {
                throw std::runtime_error("server closed benchmark connection");
            }

            input_.append(chunk.data(), *n);

            while (auto frame = decode_frame(input_)) {
                if (frame->kind == Kind::event) {
                    continue;
                }
                if (frame->kind != Kind::response) {
                    throw std::runtime_error("unexpected non-response RPC frame");
                }

                const auto now = Clock::now();
                const auto req_id = frame->request_id;
                const auto slot_idx = static_cast<std::uint16_t>(req_id & 0xFFFFu);

                if (slot_idx >= slots_.size() || !slots_[slot_idx].in_use ||
                    slots_[slot_idx].request_id != req_id) {
                    throw std::runtime_error("mismatched RPC response request_id");
                }

                auto &slot = slots_[slot_idx];
                const auto send_time = slot.send_time;
                const auto req_method = slot.method;
                const auto req_bytes = slot.wire_bytes;
                const auto resp_bytes = static_cast<std::size_t>(4 + frame->length);

                slot.in_use = false;
                free_slots_.push_back(slot_idx);
                --inflight_;

                const bool valid = workload.validate_response(req_method, *frame);

                if (now < warmup_end) {
                    stats.record_warmup(valid);
                } else if (now <= test_end) {
                    const auto latency_ns = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now - send_time)
                            .count());
                    const auto sec_idx = static_cast<std::size_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(now - warmup_end)
                            .count());
                    stats.record_measured(valid, latency_ns, req_bytes, resp_bytes, sec_idx);
                }
            }
        }
    }

    void update_interest(net::Epoll &epoll) {
        auto interest = net::EpollInterest::read | net::EpollInterest::peer_closed;
        if (!connected_ || !output_.empty()) {
            interest = interest | net::EpollInterest::write;
        }
        if (static_cast<std::uint32_t>(interest) != static_cast<std::uint32_t>(last_interest_)) {
            epoll.modify(fd(), interest);
            last_interest_ = interest;
        }
    }

    void close_from_epoll(net::Epoll &epoll) noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        try {
            epoll.remove(fd());
        } catch (...) {
        }
    }

    net::Socket socket_;
    net::Buffer input_{16384};
    net::Buffer output_{16384};

    std::size_t max_inflight_{64};
    std::size_t inflight_{0};
    std::uint64_t next_seq_{1};

    std::vector<InflightSlot> slots_;
    std::vector<std::uint16_t> free_slots_;

    bool connected_{false};
    bool closed_{false};
    net::EpollInterest last_interest_{net::EpollInterest::read};
};

struct BenchConfig {
    std::string host{"127.0.0.1"};
    std::string port{"3490"};
    WorkloadConfig workload{};
    std::size_t connections{1};
    std::size_t max_inflight{64};
    std::chrono::seconds warmup_duration{5};
    std::chrono::seconds total_duration{30};
    std::filesystem::path output_file{"benchmark/result.txt"};
};

// BenchRunner：控制测试时长、并发量、负载类型，驱动压测主循环并汇总统计结果。
class BenchRunner {
  public:
    explicit BenchRunner(BenchConfig config = {}) : config_(std::move(config)) {
        if (config_.connections == 0) {
            throw std::invalid_argument("connections must be > 0");
        }
        if (config_.max_inflight == 0 || config_.max_inflight > 64) {
            throw std::invalid_argument("max_inflight must be in 1..64");
        }
        if (config_.total_duration <= config_.warmup_duration) {
            throw std::invalid_argument("total_duration must be greater than warmup_duration");
        }
    }

    BenchSummary run() {
        const auto measured_duration = config_.total_duration - config_.warmup_duration;
        net::Epoll epoll(std::max<std::size_t>(256, config_.connections * 2));
        Workload workload(config_.workload);
        Stats stats(6'000'000, static_cast<std::size_t>(measured_duration.count()));

        std::vector<std::unique_ptr<BenchConnection>> conns;
        std::unordered_map<int, BenchConnection *> conn_map;
        conns.reserve(config_.connections);
        conn_map.reserve(config_.connections);

        for (std::size_t i = 0; i < config_.connections; ++i) {
            auto conn =
                std::make_unique<BenchConnection>(config_.host, config_.port, config_.max_inflight);
            conn->register_epoll(epoll);
            conn_map.emplace(conn->fd(), conn.get());
            conns.push_back(std::move(conn));
        }

        const auto start_time = Clock::now();
        const auto warmup_end = start_time + config_.warmup_duration;
        const auto test_end = start_time + config_.total_duration;

        std::cout << "[BenchRunner] Starting benchmark against " << config_.host << ":"
                  << config_.port << " | workload=" << workload_name(config_.workload.type)
                  << " | connections=" << config_.connections
                  << " | max_inflight=" << config_.max_inflight
                  << " | warmup=" << config_.warmup_duration.count() << "s"
                  << " | total=" << config_.total_duration.count() << "s\n"
                  << std::flush;

        bool warmup_logged = (config_.warmup_duration.count() == 0);

        while (true) {
            const auto now = Clock::now();
            if (now >= test_end) {
                break;
            }

            if (!warmup_logged && now >= warmup_end) {
                warmup_logged = true;
                std::cout << "[BenchRunner] Warmup finished (" << config_.warmup_duration.count()
                          << "s, " << stats.warmup_completed()
                          << " reqs). Measuring performance for " << measured_duration.count()
                          << "s...\n"
                          << std::flush;
            }

            const auto remaining_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(test_end - now).count();
            const int timeout_ms = static_cast<int>(std::clamp< std::int64_t>(remaining_ms, 1, 20));

            auto events = epoll.wait(timeout_ms);
            for (const auto &ev : events) {
                auto it = conn_map.find(ev.fd);
                if (it == conn_map.end()) {
                    continue;
                }
                it->second->on_event(ev, epoll, workload, stats, warmup_end, test_end, false);
            }

            bool any_alive = false;
            for (const auto &conn : conns) {
                if (!conn->closed()) {
                    any_alive = true;
                    break;
                }
            }
            if (!any_alive) {
                throw std::runtime_error("all benchmark connections closed prematurely");
            }
        }

        const auto actual_end = Clock::now();
        const double actual_warmup_s =
            std::chrono::duration<double>(config_.warmup_duration).count();
        const double actual_measured_s =
            std::chrono::duration<double>(actual_end - warmup_end).count();

        auto summary = stats.summarize(workload_name(config_.workload.type), config_.host,
                                       config_.port, config_.connections, config_.max_inflight,
                                       actual_warmup_s, actual_measured_s);

        write_report_file(config_.output_file, summary);
        return summary;
    }

  private:
    BenchConfig config_;
};

} // namespace bench

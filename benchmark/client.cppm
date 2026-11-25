module;
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
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

// ============================================================================
// 压测客户端 CPU 亲和性隔离工具（严格避开 runtime/config.cppm 中的 Server 核心）
// ============================================================================
// Server 默认占用：
//   - I/O EventLoop (4P) : {0, 2, 4, 8}（对应 SMT 兄弟核为 {1, 3, 5, 9}）
//   - Worker / Logger (8E): {16, 17, 18, 19, 20, 21, 22, 23}
// 因此压测客户端优先绑定至完全空闲的另外 4 颗物理 P-core {6, 10, 12, 14}，
// 其次为另外 8 颗物理 E-core {24..31}，最后才是客户端 P-core 的超线程兄弟 {7, 11, 13, 15}。
inline constexpr std::array<unsigned, 16> kPreferredClientCpus = {
    6, 10, 12, 14, 24, 25, 26, 27, 28, 29, 30, 31, 7, 11, 13, 15};

// 【作用】探测当前进程可用的客户端专属逻辑 CPU 列表（与当前进程 affinity 掩码求交集）。
[[nodiscard]] inline std::vector<unsigned> discover_client_cpus() noexcept {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (::sched_getaffinity(0, sizeof(mask), &mask) != 0)
        return {};

    std::vector<unsigned> result;
    result.reserve(kPreferredClientCpus.size());
    for (unsigned cpu : kPreferredClientCpus) {
        if (cpu < static_cast<unsigned>(CPU_SETSIZE) && CPU_ISSET(cpu, &mask))
            result.push_back(cpu);
    }

    // 若运行在核心数较少或已由外部 taskset 限制到其他核的环境中，回退为当前掩码允许的全部核心。
    if (result.empty()) {
        for (unsigned cpu = 0; cpu < static_cast<unsigned>(CPU_SETSIZE); ++cpu) {
            if (CPU_ISSET(cpu, &mask))
                result.push_back(cpu);
        }
    }
    return result;
}

// 【作用】将当前压测线程绑定到给定的 CPU 集合（尽力而为，受限容器环境下失败不抛异常）。
inline void pin_current_thread_to_cpus(std::span<const unsigned> cpus) noexcept {
    if (cpus.empty())
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned cpu : cpus) {
        if (cpu < static_cast<unsigned>(CPU_SETSIZE))
            CPU_SET(cpu, &set);
    }
    (void)::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
}

// 【作用】将当前压测工作线程一对一钉死在指定逻辑 CPU 核心上。
inline void pin_current_thread_to_cpu(unsigned cpu) noexcept {
    pin_current_thread_to_cpus(std::span<const unsigned>{&cpu, 1});
}

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

            Frame frame{
                .length = 0,
                .kind = Kind::request,
                .request_id = req_id,
                .method = req.method,
                .status = Status::ok,
                .payload = std::move(req.payload),
            };
            (void)output_.enqueue(frame);

            ++inflight_;
        }
    }

    void flush_output() {
        output_.write(socket_);
    }

    [[nodiscard]] static bool validate_view(Method request_method,
                                            const FrameView &response) noexcept {
        if (response.kind != Kind::response || response.method != request_method)
            return false;

        switch (request_method) {
        case Method::system_ping:
            return response.status == Status::ok && response.payload == "PONG";
        case Method::kv_set:
            return response.status == Status::ok && response.payload == "OK";
        case Method::kv_get:
        case Method::kv_del:
            return response.status == Status::ok || response.status == Status::not_found;
        default:
            return response.status == Status::ok;
        }
    }

    void receive_responses(Workload &workload, Stats &stats, Clock::time_point warmup_end,
                           Clock::time_point test_end) {
        (void)workload;
        while (true) {
            input_.ensure_writable(65536);
            auto n = socket_.try_recv(input_.writable_data(), input_.writable_bytes());
            if (!n) {
                break;
            }
            if (*n == 0) {
                throw std::runtime_error("server closed benchmark connection");
            }

            input_.commit_write(*n);
            const auto batch_now = Clock::now();

            while (auto frame = decode_frame(input_)) {
                const auto total_frame_bytes = frame->wire_bytes();
                if (frame->kind == Kind::event) {
                    input_.retrieve(total_frame_bytes);
                    continue;
                }
                if (frame->kind != Kind::response) {
                    throw std::runtime_error("unexpected non-response RPC frame");
                }

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
                const auto resp_bytes = total_frame_bytes;

                slot.in_use = false;
                free_slots_.push_back(slot_idx);
                --inflight_;

                const bool valid = validate_view(req_method, *frame);
                input_.retrieve(total_frame_bytes);

                if (batch_now < warmup_end) {
                    stats.record_warmup(valid);
                } else if (batch_now <= test_end) {
                    const auto latency_ns = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(batch_now - send_time)
                            .count());
                    const auto sec_idx = static_cast<std::size_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(batch_now - warmup_end)
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
    net::FrameSendQueue output_;

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
        const auto client_cpus = discover_client_cpus();
        pin_current_thread_to_cpus(client_cpus);

        const auto measured_duration = config_.total_duration - config_.warmup_duration;
        const std::size_t worker_cnt =
            std::max<std::size_t>(1, std::min(config_.connections,
                                              client_cpus.empty() ? 4 : client_cpus.size()));

        Workload base_workload(config_.workload);
        std::vector<Workload> workloads(worker_cnt, base_workload);
        std::vector<Stats> worker_stats;
        worker_stats.reserve(worker_cnt);
        for (std::size_t w = 0; w < worker_cnt; ++w) {
            worker_stats.emplace_back(6'000'000 / worker_cnt + 1024,
                                      static_cast<std::size_t>(measured_duration.count()));
        }

        const auto start_time = Clock::now();
        const auto warmup_end = start_time + config_.warmup_duration;
        const auto test_end = start_time + config_.total_duration;

        std::cout << "[压测引擎] 开始压测 " << config_.host << ":" << config_.port
                  << " | 负载=" << workload_name(config_.workload.type)
                  << " | 连接数=" << config_.connections
                  << " | 压测线程=" << worker_cnt
                  << " | 单连接 Inflight=" << config_.max_inflight
                  << " | 预热=" << config_.warmup_duration.count() << "s"
                  << " | 总时长=" << config_.total_duration.count() << "s\n"
                  << std::flush;

        std::atomic<bool> abort_flag{false};
        std::vector<std::exception_ptr> worker_errors(worker_cnt);

        auto worker_fn = [&](std::size_t w) {
            try {
                if (!client_cpus.empty())
                    pin_current_thread_to_cpu(client_cpus[w % client_cpus.size()]);

                std::size_t my_conns = 0;
                for (std::size_t i = w; i < config_.connections; i += worker_cnt)
                    ++my_conns;

                net::Epoll epoll(std::max<std::size_t>(64, my_conns * 2));
                std::vector<std::unique_ptr<BenchConnection>> conns;
                std::unordered_map<int, BenchConnection *> conn_map;
                conns.reserve(my_conns);
                conn_map.reserve(my_conns);

                for (std::size_t i = w; i < config_.connections; i += worker_cnt) {
                    auto conn = std::make_unique<BenchConnection>(config_.host, config_.port,
                                                                  config_.max_inflight);
                    conn->register_epoll(epoll);
                    conn_map.emplace(conn->fd(), conn.get());
                    conns.push_back(std::move(conn));
                }

                bool warmup_logged = (w != 0) || (config_.warmup_duration.count() == 0);
                while (!abort_flag.load(std::memory_order_relaxed)) {
                    const auto now = Clock::now();
                    if (now >= test_end)
                        break;

                    if (!warmup_logged && now >= warmup_end) {
                        warmup_logged = true;
                        std::uint64_t total_warm = 0;
                        for (const auto &st : worker_stats)
                            total_warm += st.warmup_completed();
                        std::cout << "[压测引擎] 预热完成 (" << config_.warmup_duration.count()
                                  << "s, " << total_warm << " reqs)，开始正式测量 "
                                  << measured_duration.count() << "s...\n"
                                  << std::flush;
                    }

                    const auto remaining_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(test_end - now)
                            .count();
                    const int timeout_ms =
                        static_cast<int>(std::clamp<std::int64_t>(remaining_ms, 1, 20));

                    auto events = epoll.wait(timeout_ms);
                    for (const auto &ev : events) {
                        auto it = conn_map.find(ev.fd);
                        if (it == conn_map.end())
                            continue;
                        it->second->on_event(ev, epoll, workloads[w], worker_stats[w], warmup_end,
                                             test_end, false);
                    }

                    bool any_alive = false;
                    for (const auto &conn : conns) {
                        if (!conn->closed()) {
                            any_alive = true;
                            break;
                        }
                    }
                    if (!any_alive)
                        throw std::runtime_error("all benchmark connections closed prematurely");
                }
            } catch (...) {
                abort_flag.store(true, std::memory_order_relaxed);
                worker_errors[w] = std::current_exception();
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(worker_cnt > 0 ? worker_cnt - 1 : 0);
        for (std::size_t w = 1; w < worker_cnt; ++w)
            threads.emplace_back(worker_fn, w);
        worker_fn(0);
        for (auto &t : threads)
            t.join();

        for (const auto &err : worker_errors) {
            if (err)
                std::rethrow_exception(err);
        }

        const auto actual_end = Clock::now();
        const double actual_warmup_s =
            std::chrono::duration<double>(config_.warmup_duration).count();
        const double actual_measured_s =
            std::chrono::duration<double>(actual_end - warmup_end).count();

        Stats total_stats(6'000'000, static_cast<std::size_t>(measured_duration.count()));
        for (const auto &st : worker_stats)
            total_stats.merge(st);

        auto summary = total_stats.summarize(workload_name(config_.workload.type), config_.host,
                                             config_.port, config_.connections, config_.max_inflight,
                                             actual_warmup_s, actual_measured_s);

        write_report_file(config_.output_file, summary);
        return summary;
    }

  private:
    BenchConfig config_;
};

} // namespace bench

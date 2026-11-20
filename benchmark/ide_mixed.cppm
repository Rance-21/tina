module;
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>

export module bench.ide_mixed;

import std;
import bench.client;
import bench.workload;
import bench.stats;
import bench.mixed_fixture;
import bench.mixed_stats;
import net.buffer;
import net.socket;
import net.epoll;
import ide.client_protocol;

namespace bench::mixed {

volatile sig_atomic_t interrupted = 0;
void interrupt_handler(int) {
    interrupted = 1;
}

struct SignalGuard {
    struct sigaction old_int{}, old_term{};
    SignalGuard() {
        interrupted = 0;
        struct sigaction action{};
        action.sa_handler = interrupt_handler;
        ::sigemptyset(&action.sa_mask);
        ::sigaction(SIGINT, &action, &old_int);
        ::sigaction(SIGTERM, &action, &old_term);
    }
    ~SignalGuard() {
        ::sigaction(SIGINT, &old_int, nullptr);
        ::sigaction(SIGTERM, &old_term, nullptr);
    }
};

enum class Op { hot, cold, directory, write, terminal, control };
constexpr std::array<std::string_view, 5> op_names = {
    "热文件读", "冷文件读", "目录读取", "文件写入", "终端命令"};

struct Pending {
    Method method{};
    Op op{Op::control};
    Clock::time_point sent;
    std::size_t tx{}, rx{};
    std::string_view path;
    std::uint64_t file_size{}, offset{};
    std::uint32_t read_bytes{};
    bool terminal{}, ack{}, done{}, marker_seen{}, command_ok{};
    std::optional<bool> expect_entry;
    bool entry_seen{};
    std::string marker, output;
};

struct Meter {
    Stats stats{4'000'000, 60};
    ClassLatencyRecorder class_latencies{524'288};
    Clock::time_point warmup_end{}, test_end{};
    bool active{};
    std::array<std::uint64_t, 5> completed{};
    std::uint64_t dir_events{}, confirmed_changes{}, drained{};

    void record(const Pending &p, bool ok) {
        if (!active || p.op == Op::control)
            return;
        const auto now = Clock::now();
        if (now > test_end) {
            ++drained;
            return;
        }
        if (p.sent < warmup_end) {
            stats.record_warmup(ok);
            return;
        }
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - p.sent).count());
        const auto sec = std::chrono::duration_cast<std::chrono::seconds>(now - warmup_end).count();
        stats.record_measured(ok, ns, p.tx, p.rx, static_cast<std::size_t>(sec));
        if (ok) {
            ++completed[static_cast<std::size_t>(p.op)];
            if (p.expect_entry)
                ++confirmed_changes;
            if (p.op == Op::hot) {
                const auto avail = (p.file_size > p.offset) ? (p.file_size - p.offset) : 0;
                const auto slice = std::min<std::uint64_t>(avail, p.read_bytes);
                class_latencies.record(slice <= 32768 ? ClassLatencyRecorder::Bucket::hot_inline
                                                      : ClassLatencyRecorder::Bucket::hot_normal,
                                       ns);
            } else if (p.op == Op::cold) {
                class_latencies.record(ClassLatencyRecorder::Bucket::cold_read, ns);
            } else if (p.op == Op::directory) {
                class_latencies.record(ClassLatencyRecorder::Bucket::directory, ns);
            } else if (p.op == Op::write) {
                class_latencies.record(ClassLatencyRecorder::Bucket::file_write, ns);
            } else if (p.op == Op::terminal) {
                class_latencies.record(ClassLatencyRecorder::Bucket::terminal, ns);
            }
        }
    }

    void directory_event(std::size_t bytes) {
        const auto now = Clock::now();
        if (active && now >= warmup_end && now <= test_end) {
            ++dir_events;
            stats.record_event_bytes(bytes);
        }
    }
};

class Connection {
  public:
    Connection(const BenchConfig &config, Fixture &fixture,
               const std::vector<std::string> &hot_payloads,
               const std::vector<std::string> &dir_payloads, std::size_t index, net::Epoll &epoll,
               Meter &meter)
        : socket_(net::Socket::connect_tcp_nonblocking(config.host, config.port)), config_(config),
          fixture_(fixture), hot_payloads_(hot_payloads), dir_payloads_(dir_payloads),
          epoll_(epoll), meter_(meter),
          dir_(fixture.relative_scratch + "/" + std::to_string(index)),
          dir_payload_(client::encode_workspace_list_dir(dir_)),
          write_path_(dir_ + "/write.txt"), hot_pick_(index) {
        int one = 1;
        ::setsockopt(fd(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        pending_.reserve(config.max_inflight * 2);
        epoll_.add(fd(), interest_);
    }

    int fd() const {
        return socket_.native_handle();
    }
    bool idle() const {
        return pending_.empty() && output_.empty();
    }
    std::uint32_t terminal_id{};
    std::uint64_t revision{};
    std::optional<Frame> control_response;
    std::string last_write;

    const std::string &dir() const {
        return dir_;
    }
    const std::string &write_path() const {
        return write_path_;
    }

    void control(Method method, std::string payload) {
        control_response.reset();
        send(method, std::move(payload), Op::control);
        flush();
    }

    void control_command(std::string command) {
        send_terminal(std::move(command), std::nullopt, Op::control);
        flush();
    }

    void fill() {
        // 每 20 个操作排程：9 次文件读 + 5 次目录读 + 3 次文件写入 + 3 次终端命令。
        // 同一连接的 write 与 terminal 各自保持串行，但当某一方在途时不再 break 阻塞整条连接，
        // 而是跳过并继续用读/目录请求填满 max_inflight，避免单路 fsync 饿死全部并发槽位。
        static constexpr std::array<char, 20> plan = {'r', 'd', 'r', 'w', 'r', 't', 'd',
                                                      'r', 'w', 'd', 'r', 't', 'd', 'r',
                                                      'w', 'r', 'd', 'r', 't', 'r'};
        while (pending_.size() < config_.max_inflight && Clock::now() < meter_.test_end) {
            const char op = plan[sequence_ % plan.size()];
            if ((op == 'w' && writing_) || (op == 't' && terminal_request_ != 0)) {
                ++sequence_;
                continue;
            }
            const auto seq = sequence_++;
            if (op == 'r') {
                // 每 100 次文件读按配置准确分配冷热；热文件按交替序列均匀覆盖 <=32K 与 >32K。
                const bool hot = ((read_sequence_++ * 37) % 100) < config_.workload.fs_hot_percent;
                const auto bytes = config_.workload.read_max_bytes;
                if (hot) {
                    const std::size_t idx = hot_pick_++ % fixture_.hot.size();
                    const auto &file = fixture_.hot[idx];
                    auto &p = send(Method::workspace_read_file, hot_payloads_[idx], Op::hot);
                    p.file_size = file.size;
                    p.offset = 0;
                    p.read_bytes = bytes;
                } else {
                    const auto &file = fixture_.cold[(seq * 17 + seq / 20) % fixture_.cold.size()];
                    const auto ranges =
                        std::max<std::uint64_t>(1, (file.size + bytes - 1) / bytes);
                    const auto offset = ((seq * 7919) % ranges) * bytes;
                    auto &p = send(Method::workspace_read_file,
                                   client::encode_workspace_read_file(file.path, offset, bytes),
                                   Op::cold);
                    p.file_size = file.size;
                    p.offset = offset;
                    p.read_bytes = bytes;
                }
            } else if (op == 'd') {
                if (seq % 5 == 1) {
                    auto &p = send(Method::workspace_list_dir, dir_payload_, Op::directory);
                    p.path = dir_;
                } else {
                    const std::size_t idx = (seq * 13) % fixture_.directories.size();
                    auto &p = send(Method::workspace_list_dir, dir_payloads_[idx], Op::directory);
                    p.path = fixture_.directories[idx];
                }
            } else if (op == 'w') {
                last_write = "tina benchmark " + std::to_string(seq) + "\n";
                last_write.resize(4096, static_cast<char>('a' + seq % 26));
                send(Method::workspace_write_file,
                     client::encode_workspace_write_file(write_path_, revision, last_write),
                     Op::write);
                writing_ = true;
            } else {
                std::string command;
                std::optional<bool> expect;
                if ((command_sequence_++ % 2) == 0) {
                    entry_present_ = !entry_present_;
                    expect = entry_present_;
                    command = "cd -- " + shell_quote((fixture_.projects / dir_).string()) +
                              (entry_present_ ? " && command touch -- event-marker"
                                              : " && command rm -- event-marker");
                } else {
                    const auto &file = fixture_.hot[seq % fixture_.hot.size()];
                    command =
                        "command wc -c -- " + shell_quote((fixture_.projects / file.path).string());
                }
                send_terminal(std::move(command), expect, Op::terminal);
            }
        }
        flush();
    }

    void on_event(net::EpollEvent ev) {
        if (!connected_) {
            socket_.check_connect_result();
            connected_ = true;
        }
        if (ev.error())
            throw std::runtime_error("ide_mixed socket error");
        if (ev.readable() || ev.hangup() || ev.peer_closed()) {
            std::array<char, 65536> bytes{};
            // 限制单连接每轮接收量，避免大响应饿死其他连接及计时器。
            for (int i = 0; i < 32; ++i) {
                auto n = socket_.try_recv(bytes.data(), bytes.size());
                if (!n)
                    break;
                if (*n == 0)
                    throw std::runtime_error("server closed ide_mixed connection");
                input_.append(bytes.data(), *n);
                while (auto frame = decode_frame(input_))
                    receive(std::move(*frame));
            }
        }
        flush();
    }

    void check_timeout() const {
        const auto now = Clock::now();
        for (const auto &[id, p] : pending_) {
            if (now - p.sent > std::chrono::seconds(15))
                throw std::runtime_error("ide_mixed request timeout: id=" + std::to_string(id) +
                                         " method=" + std::to_string(std::to_underlying(p.method)) +
                                         " ack=" + std::to_string(p.ack) +
                                         " done=" + std::to_string(p.done) +
                                         " marker=" + std::to_string(p.marker_seen) +
                                         " inotify=" + std::to_string(p.entry_seen));
        }
    }

  private:
    Pending &send(Method method, std::string payload, Op op) {
        const auto id = next_id_++;
        Pending p;
        p.method = method;
        p.op = op;
        p.sent = Clock::now();
        p.tx = frame_header_size + payload.size();
        auto [it, inserted] = pending_.emplace(id, std::move(p));
        (void)inserted;
        Frame frame{.length = 0,
                    .kind = Kind::request,
                    .request_id = id,
                    .method = method,
                    .status = Status::ok,
                    .payload = std::move(payload)};
        if (!output_.enqueue(frame))
            throw std::runtime_error("benchmark send queue is full");
        return it->second;
    }

    void send_terminal(std::string command, std::optional<bool> expect, Op op) {
        terminal_request_ = next_id_;
        const auto tag = "TB:" + std::to_string(terminal_request_) + ":";
        // ACK 只表示进入 PTY 队列；额外校验命令退出码、完成事件，以及目录变更事件。
        command = "{ " + command + "; }; __tina_bench_rc=$?; printf '\\036" + tag +
                  "%s\\037' \"$__tina_bench_rc\"";
        auto &p = send(Method::terminal_input, encode_terminal_input(terminal_id, command), op);
        p.terminal = true;
        p.marker = "\036" + tag;
        p.expect_entry = expect;
    }

    void flush() {
        if (connected_ && !output_.empty())
            output_.write(socket_);
        auto interest = net::EpollInterest::read | net::EpollInterest::peer_closed;
        if (!connected_ || !output_.empty())
            interest = interest | net::EpollInterest::write;
        if (static_cast<std::uint32_t>(interest) != static_cast<std::uint32_t>(interest_)) {
            epoll_.modify(fd(), interest);
            interest_ = interest;
        }
    }

    void finish(std::uint64_t id, bool ok) {
        auto it = pending_.find(id);
        meter_.record(it->second, ok);
        if (it->second.op == Op::write)
            writing_ = false;
        if (it->second.terminal)
            terminal_request_ = 0;
        const auto method = it->second.method;
        pending_.erase(it);
        if (!ok)
            throw std::runtime_error("ide_mixed validation failed, method=" +
                                     std::to_string(std::to_underlying(method)));
    }

    void finish_terminal() {
        if (!terminal_request_)
            return;
        const auto &p = pending_.at(terminal_request_);
        if (p.ack && p.done && p.marker_seen && (!p.command_ok || !p.expect_entry || p.entry_seen))
            finish(terminal_request_, p.command_ok);
    }

    void receive(Frame frame) {
        const auto bytes = frame_header_size + frame.payload.size();
        if (frame.kind == Kind::event) {
            if (frame.method == Method::workspace_list_dir) {
                const auto listing = client::decode_workspace_list_dir(frame.payload);
                if (frame.status != Status::ok || !listing)
                    throw std::runtime_error("invalid inotify directory event");
                meter_.directory_event(bytes);
                if (terminal_request_ && listing->path == dir_) {
                    auto &p = pending_.at(terminal_request_);
                    const bool present =
                        std::ranges::any_of(listing->entries, [](const DirEntry &e) {
                            return e.name == "event-marker";
                        });
                    if (p.expect_entry && present == *p.expect_entry)
                        p.entry_seen = true;
                }
            } else if (terminal_request_ && frame.method == Method::terminal_output) {
                const auto message = decode_terminal_payload_view(frame.payload);
                if (!message || message->terminal_id != terminal_id)
                    throw std::runtime_error("invalid terminal output event");
                auto &p = pending_.at(terminal_request_);
                p.rx += bytes;
                p.output.append(message->data);
                const auto begin = p.output.find(p.marker);
                if (begin != std::string::npos) {
                    const auto code_start = begin + p.marker.size();
                    const auto end = p.output.find('\037', code_start);
                    if (end != std::string::npos) {
                        p.marker_seen = true;
                        p.command_ok =
                            std::string_view(p.output).substr(code_start, end - code_start) == "0";
                    }
                }
                if (p.output.size() > 128 * 1024)
                    throw std::runtime_error("unexpectedly large benchmark terminal output");
            } else if (terminal_request_ && frame.method == Method::terminal_state) {
                const auto id = decode_terminal_state_id(frame.payload);
                if (!id || *id != terminal_id)
                    throw std::runtime_error("invalid terminal state event");
                auto &p = pending_.at(terminal_request_);
                p.rx += bytes;
                p.done = true;
            }
            finish_terminal();
            return;
        }
        auto it = pending_.find(frame.request_id);
        if (frame.kind != Kind::response || it == pending_.end() ||
            frame.method != it->second.method)
            throw std::runtime_error("mismatched ide_mixed RPC response");
        auto &p = it->second;
        p.rx += bytes;
        if (frame.status != Status::ok) {
            std::cerr << "[Workload] RPC status=" << std::to_underlying(frame.status)
                      << " method=" << std::to_underlying(frame.method) << '\n';
            finish(frame.request_id, false);
            return;
        }
        if (p.terminal) {
            const auto ack = decode_terminal_payload_view(frame.payload);
            if (!ack || ack->terminal_id != terminal_id || ack->data != "OK") {
                finish(frame.request_id, false);
                return;
            }
            p.ack = true;
            finish_terminal();
            return;
        }
        bool valid = true;
        if (p.op == Op::hot || p.op == Op::cold) {
            PayloadReader reader(frame.payload);
            auto size = reader.read_int<std::uint64_t>();
            auto rev = reader.read_int<std::uint64_t>();
            valid = size && rev && *size == p.file_size &&
                    reader.remaining_size() ==
                        std::min<std::uint64_t>(p.file_size - p.offset, p.read_bytes);
        } else if (p.op == Op::directory) {
            auto result = client::decode_workspace_list_dir(frame.payload);
            valid = result && result->path == p.path;
        } else if (p.op == Op::write) {
            auto result = client::decode_workspace_write_file(frame.payload);
            valid = result.has_value();
            if (result)
                revision = *result;
        } else if (p.op == Op::control) {
            control_response = std::move(frame);
        }
        finish(it->first, valid);
    }

    net::Socket socket_;
    const BenchConfig &config_;
    Fixture &fixture_;
    const std::vector<std::string> &hot_payloads_;
    const std::vector<std::string> &dir_payloads_;
    net::Epoll &epoll_;
    Meter &meter_;
    net::Buffer input_{65536};
    net::FrameSendQueue output_;
    std::unordered_map<std::uint64_t, Pending> pending_;
    std::string dir_, dir_payload_, write_path_;
    std::uint64_t next_id_{1}, terminal_request_{}, sequence_{}, read_sequence_{},
        command_sequence_{}, hot_pick_{};
    bool connected_{}, writing_{}, entry_present_{};
    net::EpollInterest interest_ =
        net::EpollInterest::read | net::EpollInterest::write | net::EpollInterest::peer_closed;
};

} // namespace bench::mixed

export namespace bench {

class MixedRunner {
  public:
    explicit MixedRunner(BenchConfig config) : config_(std::move(config)) {
        if (!config_.connections || !config_.max_inflight || config_.max_inflight > 64)
            throw std::invalid_argument("connections must be >0; inflight must be in 1..64");
        if (config_.warmup_duration.count() < 0 ||
            config_.total_duration <= config_.warmup_duration)
            throw std::invalid_argument("require 0 <= warmup < total duration");
        if (config_.host != "127.0.0.1" && config_.host != "::1" && config_.host != "localhost")
            throw std::invalid_argument("ide_mixed requires a local server sharing the same HOME");
    }

    BenchSummary run() {
        mixed::SignalGuard signals;
        mixed::Fixture fixture(config_.workload, config_.connections);
        const auto report_path = std::filesystem::weakly_canonical(config_.output_file);
        if (mixed::is_within(report_path, fixture.source) ||
            mixed::is_within(report_path, fixture.scratch))
            throw std::invalid_argument(
                "report output must be outside the source and scratch directories");
        // 任何异常都会先销毁连接、再清理独占目录；命令不会 mkdir，不能在清理后重建目录。
        std::vector<std::string> hot_payloads;
        hot_payloads.reserve(fixture.hot.size());
        for (const auto &file : fixture.hot) {
            hot_payloads.push_back(
                client::encode_workspace_read_file(file.path, 0, config_.workload.read_max_bytes));
        }
        std::vector<std::string> dir_payloads;
        dir_payloads.reserve(fixture.directories.size());
        for (const auto &dir : fixture.directories) {
            dir_payloads.push_back(client::encode_workspace_list_dir(dir));
        }

        net::Epoll epoll;
        mixed::Meter meter;
        std::vector<std::unique_ptr<mixed::Connection>> connections;
        std::unordered_map<int, mixed::Connection *> by_fd;
        for (std::size_t i = 0; i < config_.connections; ++i) {
            auto c = std::make_unique<mixed::Connection>(config_, fixture, hot_payloads,
                                                         dir_payloads, i, epoll, meter);
            by_fd.emplace(c->fd(), c.get());
            connections.push_back(std::move(c));
        }

        auto last_timeout_check = Clock::now();
        auto pump = [&] {
            for (auto event : epoll.wait(10))
                by_fd.at(event.fd)->on_event(event);
            const auto now = Clock::now();
            if (now - last_timeout_check >= std::chrono::milliseconds(100)) {
                last_timeout_check = now;
                for (auto &c : connections)
                    c->check_timeout();
            }
        };
        auto wait_idle = [&](mixed::Connection &c) {
            while (!c.idle())
                pump();
        };
        auto call = [&](mixed::Connection &c, Method method, std::string payload) -> Frame {
            c.control(method, std::move(payload));
            wait_idle(c);
            if (!c.control_response)
                throw std::runtime_error("missing control response");
            return std::move(*c.control_response);
        };

        for (auto &c : connections) {
            // 随机目录名 + 内容探针，确认 RPC 服务端看到的就是本机独占目录。
            auto seed = call(*c, Method::workspace_read_file,
                             client::encode_workspace_read_file(c->write_path(), 0, 4096));
            auto file = client::decode_workspace_read_file(seed.payload);
            if (!file || file->content != fixture.initial_content)
                throw std::runtime_error("server does not share benchmark HOME/projects");
            c->revision = file->revision;
            c->last_write = file->content;
            auto listing =
                call(*c, Method::workspace_list_dir, client::encode_workspace_list_dir(c->dir()));
            auto listed = client::decode_workspace_list_dir(listing.payload);
            if (!listed || listed->path != c->dir())
                throw std::runtime_error("failed to open scratch dir");
            auto opened = call(*c, Method::terminal_state, encode_terminal_state_id(0));
            auto id = decode_terminal_state_id(opened.payload);
            if (!id || !*id)
                throw std::runtime_error("failed to start benchmark terminal");
            c->terminal_id = *id;
            c->control_command("unset HISTFILE; history -c; stty -echo");
            wait_idle(*c);
        }

        // 热集先完整进入 Tina 缓存；准备操作不计入 10s + 50s。
        for (const auto &file : fixture.hot) {
            const auto response = call(
                *connections.front(), Method::workspace_read_file,
                client::encode_workspace_read_file(file.path, 0, config_.workload.read_max_bytes));
            const auto read = client::decode_workspace_read_file(response.payload);
            if (!read || read->file_size != file.size)
                throw std::runtime_error("hot-file prefill failed");
        }
        if (mixed::interrupted)
            throw std::runtime_error("benchmark interrupted during setup");

        const auto start = Clock::now();
        meter.warmup_end = start + config_.warmup_duration;
        meter.test_end = start + config_.total_duration;
        meter.active = true;
        std::cout << "[压测引擎] 开始压测 " << config_.host << ':' << config_.port
                  << " | 负载=ide_mixed | 连接数=" << config_.connections
                  << " | 单连接 Inflight=" << config_.max_inflight
                  << " | 预热=" << config_.warmup_duration.count()
                  << "s | 总时长=" << config_.total_duration.count() << "s\n"
                  << std::flush;
        bool warmed = config_.warmup_duration.count() == 0;
        while (Clock::now() < meter.test_end && !mixed::interrupted) {
            for (auto &c : connections)
                c->fill();
            pump();
            if (!warmed && Clock::now() >= meter.warmup_end) {
                warmed = true;
                std::cout << "[压测引擎] 预热完成 (" << config_.warmup_duration.count() << "s, "
                          << meter.stats.warmup_completed() << " reqs)，开始正式测量 "
                          << (config_.total_duration - config_.warmup_duration).count() << "s...\n"
                          << std::flush;
            }
        }

        // 停止提交后排空，包括命令完成与 inotify 确认；排空耗时不进入测量窗口。
        for (auto &c : connections)
            wait_idle(*c);
        for (auto &c : connections) {
            const auto response =
                call(*c, Method::workspace_read_file,
                     client::encode_workspace_read_file(c->write_path(), 0, 4096));
            const auto read = client::decode_workspace_read_file(response.payload);
            std::ifstream disk(fixture.projects / c->write_path(), std::ios::binary);
            const std::string disk_content{std::istreambuf_iterator<char>(disk), {}};
            if (!read || read->revision != c->revision || read->content != c->last_write ||
                disk_content != c->last_write)
                throw std::runtime_error("write read-back verification failed");
            call(*c, Method::terminal_state, encode_terminal_state_id(c->terminal_id));
            call(*c, Method::workspace_close_dir, client::encode_workspace_close_dir(c->dir()));
        }
        connections.clear();
        fixture.cleanup_checked();
        if (mixed::interrupted)
            throw std::runtime_error("benchmark interrupted; scratch cleaned");

        auto summary = meter.stats.summarize(
            "ide_mixed", config_.host, config_.port, config_.connections, config_.max_inflight,
            static_cast<double>(config_.warmup_duration.count()),
            static_cast<double>((config_.total_duration - config_.warmup_duration).count()));
        std::ostringstream detail;
        auto kv = [&](std::string_view label) -> std::ostream & {
            return detail << "  " << pad_display(label, 22) << " : ";
        };
        kv("并发策略") << "write/terminal 单连接串行，其余槽位持续压满读/目录请求\n";
        kv("文件读冷热占比") << "热读 " << config_.workload.fs_hot_percent << "% (<=32K:"
                             << fixture.hot_small_count << " / >32K:" << fixture.hot_large_count
                             << "), 冷读 " << (100 - config_.workload.fs_hot_percent) << "%\n";
        kv("源码根目录") << fixture.source.string() << " (全程只读)\n";
        kv("单次读上限") << config_.workload.read_max_bytes << " bytes\n";
        kv("冷文件阈值") << "> " << fixture.cache_limit << " bytes (保留 Linux Page Cache)\n";
        kv("终端完成口径") << "ACK + 退出码 + 状态事件 (变更命令另等 inotify)\n";
        for (std::size_t i = 0; i < mixed::op_names.size(); ++i)
            kv(mixed::op_names[i]) << meter.completed[i] << " reqs\n";
        kv("目录变更事件") << meter.dir_events << " (计入 RX 带宽，不计入 QPS)\n";
        kv("已确认目录变更") << meter.confirmed_changes << "\n";
        kv("截止后排空请求") << meter.drained << " (不计入 QPS 与延迟)\n";
        kv("临时目录清理") << "已验证 (源码树未改动)\n";
        detail << meter.class_latencies.format_summary();
        summary.workload_details = detail.str();
        write_report_file(config_.output_file, summary);
        return summary;
    }

  private:
    BenchConfig config_;
};

} // namespace bench

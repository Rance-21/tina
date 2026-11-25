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

// 零拷贝校验 workspace_list_dir 的响应或目录变更 Event 负载：
// 严格遵循协议格式 [u16 dirPathLen][dirPath bytes] + [u8 type][u16 nameLen][name bytes]...，
// 在不分配 std::vector<DirEntry> 与文件名 std::string 的前提下完成完整协议合法性检查与 event-marker 探测。
[[nodiscard]] bool validate_list_dir_payload(std::string_view payload,
                                             std::string_view expected_path = {},
                                             bool *out_has_event_marker = nullptr,
                                             std::string_view *out_dir_path = nullptr) noexcept {
    PayloadReader reader(payload);
    auto path_len = reader.read_int<std::uint16_t>();
    if (!path_len || *path_len == 0)
        return false;

    auto dir_path = reader.read_bytes(*path_len);
    if (!dir_path || dir_path->find('\0') != std::string_view::npos)
        return false;

    if (!expected_path.empty() && *dir_path != expected_path)
        return false;
    if (out_dir_path != nullptr)
        *out_dir_path = *dir_path;

    bool marker_present = false;
    while (!reader.finished()) {
        auto raw_type = reader.read_int<std::uint8_t>();
        auto name_len = reader.read_int<std::uint16_t>();
        if (!raw_type || *raw_type > std::to_underlying(DirEntryType::symlink) || !name_len ||
            *name_len == 0)
            return false;

        auto name = reader.read_bytes(*name_len);
        if (!name || name->find('\0') != std::string_view::npos)
            return false;

        if (out_has_event_marker != nullptr && *name == "event-marker")
            marker_present = true;
    }

    if (out_has_event_marker != nullptr)
        *out_has_event_marker = marker_present;
    return true;
}

struct Pending {
    std::uint64_t request_id{};
    Method method{};
    Op op{Op::control};
    Clock::time_point sent{};
    std::size_t tx{}, rx{};
    std::string_view path{};
    std::uint64_t file_size{}, offset{};
    std::uint32_t read_bytes{};
    bool in_use{};
    bool terminal{}, ack{}, done{}, marker_seen{}, command_ok{};
    std::optional<bool> expect_entry{};
    bool entry_seen{};
    std::string marker{}, output{};
};

struct Meter {
    Stats stats{1'500'000, 60};
    ClassLatencyRecorder class_latencies{262'144};
    Clock::time_point warmup_end{}, test_end{};
    bool active{};
    std::array<std::uint64_t, 5> completed{};
    std::uint64_t dir_events{}, confirmed_changes{}, drained{};

    void record(const Pending &p, bool ok, Clock::time_point now) {
        if (!active || p.op == Op::control)
            return;
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

    void directory_event(std::size_t bytes, Clock::time_point now) {
        if (active && now >= warmup_end && now <= test_end) {
            ++dir_events;
            stats.record_event_bytes(bytes);
        }
    }

    void merge(const Meter &other) {
        stats.merge(other.stats);
        class_latencies.merge(other.class_latencies);
        for (std::size_t i = 0; i < completed.size(); ++i)
            completed[i] += other.completed[i];
        dir_events += other.dir_events;
        confirmed_changes += other.confirmed_changes;
        drained += other.drained;
    }
};

class Connection {
  public:
    static constexpr std::size_t kSlotCount = 128;
    static constexpr std::size_t kSlotMask = kSlotCount - 1;

    Connection(const BenchConfig &config, Fixture &fixture,
               const std::vector<std::string> &hot_payloads,
               const std::vector<std::string> &dir_payloads, std::size_t index)
        : socket_(net::Socket::connect_tcp_nonblocking(config.host, config.port)), config_(config),
          fixture_(fixture), hot_payloads_(hot_payloads), dir_payloads_(dir_payloads),
          dir_(fixture.relative_scratch + "/" + std::to_string(index)),
          dir_payload_(client::encode_workspace_list_dir(dir_)),
          write_path_(dir_ + "/write.txt"), hot_pick_(index) {
        int one = 1;
        ::setsockopt(fd(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        free_slots_.reserve(kSlotCount);
        for (std::size_t i = kSlotCount; i > 0; --i)
            free_slots_.push_back(static_cast<std::uint16_t>(i - 1));
        epoll_.add(fd(), interest_);
    }

    int fd() const {
        return socket_.native_handle();
    }
    bool idle() const {
        return inflight_ == 0 && output_.empty();
    }
    net::Epoll &epoll() noexcept {
        return epoll_;
    }
    Meter &meter() noexcept {
        return meter_;
    }
    const Meter &meter() const noexcept {
        return meter_;
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
        send(method, std::move(payload), Op::control, Clock::now());
        flush();
    }

    void control_command(std::string command) {
        send_terminal(std::move(command), std::nullopt, Op::control, Clock::now());
        flush();
    }

    void fill() {
        // 每 20 个操作排程：9 次文件读 + 5 次目录读 + 3 次文件写入 + 3 次终端命令。
        // 同一连接的 write 与 terminal 各自保持串行，但当某一方在途时不再 break 阻塞整条连接，
        // 而是跳过并继续用读/目录请求填满 max_inflight，避免单路 fsync 饿死全部并发槽位。
        static constexpr std::array<char, 20> plan = {'r', 'd', 'r', 'w', 'r', 't', 'd',
                                                      'r', 'w', 'd', 'r', 't', 'd', 'r',
                                                      'w', 'r', 'd', 'r', 't', 'r'};
        if (inflight_ >= config_.max_inflight)
            return;
        const auto now = Clock::now();
        if (now >= meter_.test_end)
            return;

        while (inflight_ < config_.max_inflight) {
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
                    auto &p = send(Method::workspace_read_file, hot_payloads_[idx], Op::hot, now);
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
                                   Op::cold, now);
                    p.file_size = file.size;
                    p.offset = offset;
                    p.read_bytes = bytes;
                }
            } else if (op == 'd') {
                if (seq % 5 == 1) {
                    auto &p = send(Method::workspace_list_dir, dir_payload_, Op::directory, now);
                    p.path = dir_;
                } else {
                    const std::size_t idx = (seq * 13) % fixture_.directories.size();
                    auto &p =
                        send(Method::workspace_list_dir, dir_payloads_[idx], Op::directory, now);
                    p.path = fixture_.directories[idx];
                }
            } else if (op == 'w') {
                last_write = "tina benchmark " + std::to_string(seq) + "\n";
                last_write.resize(4096, static_cast<char>('a' + seq % 26));
                send(Method::workspace_write_file,
                     client::encode_workspace_write_file(write_path_, revision, last_write),
                     Op::write, now);
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
                send_terminal(std::move(command), expect, Op::terminal, now);
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
            // 直接向 input_ 可写区执行零中转 recv，并用 decode_frame 原地零拷贝校验响应帧。
            for (int i = 0; i < 32; ++i) {
                input_.ensure_writable(65536);
                auto n = socket_.try_recv(input_.writable_data(), input_.writable_bytes());
                if (!n)
                    break;
                if (*n == 0)
                    throw std::runtime_error("server closed ide_mixed connection");
                input_.commit_write(*n);
                const auto batch_now = Clock::now();
                while (auto frame = decode_frame(input_)) {
                    const auto total_bytes = frame->wire_bytes();
                    receive(*frame, batch_now);
                    input_.retrieve(total_bytes);
                }
            }
        }
        flush();
    }

    void check_timeout() const {
        if (inflight_ == 0)
            return;
        const auto now = Clock::now();
        for (const auto &p : slots_) {
            if (!p.in_use)
                continue;
            if (now - p.sent > std::chrono::seconds(15))
                throw std::runtime_error(
                    "ide_mixed request timeout: id=" + std::to_string(p.request_id) +
                    " method=" + std::to_string(std::to_underlying(p.method)) +
                    " ack=" + std::to_string(p.ack) + " done=" + std::to_string(p.done) +
                    " marker=" + std::to_string(p.marker_seen) +
                    " inotify=" + std::to_string(p.entry_seen));
        }
    }

  private:
    [[nodiscard]] Pending *find_pending(std::uint64_t id) noexcept {
        if (id == 0)
            return nullptr;
        auto &slot = slots_[static_cast<std::size_t>(id & kSlotMask)];
        return (slot.in_use && slot.request_id == id) ? &slot : nullptr;
    }

    Pending &send(Method method, std::string payload, Op op, Clock::time_point now) {
        if (free_slots_.empty())
            throw std::runtime_error("no free pending slot");
        const std::uint16_t slot_idx = free_slots_.back();
        free_slots_.pop_back();
        ++inflight_;

        const auto id = (next_id_++ << 8) | static_cast<std::uint64_t>(slot_idx);
        Pending &p = slots_[slot_idx];
        p.request_id = id;
        p.method = method;
        p.op = op;
        p.sent = now;
        p.tx = frame_header_size + payload.size();
        p.rx = 0;
        p.path = {};
        p.file_size = 0;
        p.offset = 0;
        p.read_bytes = 0;
        p.in_use = true;
        p.terminal = false;
        p.ack = false;
        p.done = false;
        p.marker_seen = false;
        p.command_ok = false;
        p.expect_entry.reset();
        p.entry_seen = false;
        p.marker.clear();
        p.output.clear();

        Frame frame{.length = 0,
                    .kind = Kind::request,
                    .request_id = id,
                    .method = method,
                    .status = Status::ok,
                    .payload = std::move(payload)};
        if (!output_.enqueue(frame))
            throw std::runtime_error("benchmark send queue is full");
        return p;
    }

    void send_terminal(std::string command, std::optional<bool> expect, Op op,
                       Clock::time_point now) {
        const auto next_seq = next_id_;
        const auto tag = "TB:" + std::to_string(next_seq) + ":";
        // ACK 只表示进入 PTY 队列；额外校验命令退出码、完成事件，以及目录变更事件。
        command = "{ " + command + "; }; __tina_bench_rc=$?; printf '\\036" + tag +
                  "%s\\037' \"$__tina_bench_rc\"";
        auto &p =
            send(Method::terminal_input, encode_terminal_input(terminal_id, command), op, now);
        terminal_request_ = p.request_id;
        p.terminal = true;
        p.marker.assign("\036").append(tag);
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

    void finish(Pending &p, bool ok, Clock::time_point now) {
        meter_.record(p, ok, now);
        if (p.op == Op::write)
            writing_ = false;
        if (p.terminal)
            terminal_request_ = 0;
        const auto method = p.method;
        const auto slot_idx = static_cast<std::uint16_t>(p.request_id & kSlotMask);
        p.in_use = false;
        free_slots_.push_back(slot_idx);
        --inflight_;
        if (!ok)
            throw std::runtime_error("ide_mixed validation failed, method=" +
                                     std::to_string(std::to_underlying(method)));
    }

    void finish_terminal(Clock::time_point now) {
        if (!terminal_request_)
            return;
        auto *p = find_pending(terminal_request_);
        if (!p)
            return;
        if (p->ack && p->done && p->marker_seen &&
            (!p->command_ok || !p->expect_entry || p->entry_seen))
            finish(*p, p->command_ok, now);
    }

    void receive(const FrameView &frame, Clock::time_point now) {
        const auto bytes = frame_header_size + frame.payload.size();
        if (frame.kind == Kind::event) {
            if (frame.method == Method::workspace_list_dir) {
                bool present = false;
                std::string_view event_dir_path;
                if (frame.status != Status::ok ||
                    !validate_list_dir_payload(frame.payload, {}, &present, &event_dir_path))
                    throw std::runtime_error("invalid inotify directory event");
                meter_.directory_event(bytes, now);
                if (terminal_request_ && event_dir_path == dir_) {
                    if (auto *p = find_pending(terminal_request_)) {
                        if (p->expect_entry && present == *p->expect_entry)
                            p->entry_seen = true;
                    }
                }
            } else if (terminal_request_ && frame.method == Method::terminal_output) {
                const auto message = decode_terminal_payload_view(frame.payload);
                if (!message || message->terminal_id != terminal_id)
                    throw std::runtime_error("invalid terminal output event");
                auto *p = find_pending(terminal_request_);
                if (!p)
                    throw std::runtime_error("missing terminal pending slot");
                p->rx += bytes;
                p->output.append(message->data);
                const auto begin = p->output.find(p->marker);
                if (begin != std::string::npos) {
                    const auto code_start = begin + p->marker.size();
                    const auto end = p->output.find('\037', code_start);
                    if (end != std::string::npos) {
                        p->marker_seen = true;
                        p->command_ok =
                            std::string_view(p->output).substr(code_start, end - code_start) == "0";
                    }
                }
                if (p->output.size() > 128 * 1024)
                    throw std::runtime_error("unexpectedly large benchmark terminal output");
            } else if (terminal_request_ && frame.method == Method::terminal_state) {
                const auto id = decode_terminal_state_id(frame.payload);
                if (!id || *id != terminal_id)
                    throw std::runtime_error("invalid terminal state event");
                auto *p = find_pending(terminal_request_);
                if (!p)
                    throw std::runtime_error("missing terminal pending slot");
                p->rx += bytes;
                p->done = true;
            }
            finish_terminal(now);
            return;
        }

        auto *p_ptr = find_pending(frame.request_id);
        if (frame.kind != Kind::response || !p_ptr || frame.method != p_ptr->method)
            throw std::runtime_error("mismatched ide_mixed RPC response");
        auto &p = *p_ptr;
        p.rx += bytes;
        if (frame.status != Status::ok) {
            std::cerr << "[Workload] RPC status=" << std::to_underlying(frame.status)
                      << " method=" << std::to_underlying(frame.method) << '\n';
            finish(p, false, now);
            return;
        }
        if (p.terminal) {
            const auto ack = decode_terminal_payload_view(frame.payload);
            if (!ack || ack->terminal_id != terminal_id || ack->data != "OK") {
                finish(p, false, now);
                return;
            }
            p.ack = true;
            finish_terminal(now);
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
            valid = validate_list_dir_payload(frame.payload, p.path);
        } else if (p.op == Op::write) {
            auto result = client::decode_workspace_write_file(frame.payload);
            valid = result.has_value();
            if (result)
                revision = *result;
        } else if (p.op == Op::control) {
            control_response = Frame{
                .length = static_cast<std::uint32_t>(min_frame_length + frame.payload.size()),
                .kind = frame.kind,
                .request_id = frame.request_id,
                .method = frame.method,
                .status = frame.status,
                .payload = std::string(frame.payload),
            };
        }
        finish(p, valid, now);
    }

    net::Socket socket_;
    const BenchConfig &config_;
    Fixture &fixture_;
    const std::vector<std::string> &hot_payloads_;
    const std::vector<std::string> &dir_payloads_;
    net::Epoll epoll_{16};
    Meter meter_{};
    net::Buffer input_{131072};
    net::FrameSendQueue output_;
    std::array<Pending, kSlotCount> slots_{};
    std::vector<std::uint16_t> free_slots_;
    std::size_t inflight_{0};
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
        const auto client_cpus = discover_client_cpus();
        pin_current_thread_to_cpus(client_cpus);

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

        std::vector<std::unique_ptr<mixed::Connection>> connections;
        connections.reserve(config_.connections);
        for (std::size_t i = 0; i < config_.connections; ++i) {
            connections.push_back(std::make_unique<mixed::Connection>(config_, fixture, hot_payloads,
                                                                      dir_payloads, i));
        }

        auto pump_conn = [](mixed::Connection &c, Clock::time_point &last_timeout_check) {
            for (auto event : c.epoll().wait(10))
                c.on_event(event);
            const auto now = Clock::now();
            if (now - last_timeout_check >= std::chrono::milliseconds(100)) {
                last_timeout_check = now;
                c.check_timeout();
            }
        };
        auto wait_idle = [&](mixed::Connection &c) {
            auto last_timeout_check = Clock::now();
            while (!c.idle())
                pump_conn(c, last_timeout_check);
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
        const auto warmup_end = start + config_.warmup_duration;
        const auto test_end = start + config_.total_duration;
        for (auto &c : connections) {
            c->meter().warmup_end = warmup_end;
            c->meter().test_end = test_end;
            c->meter().active = true;
        }

        const std::size_t worker_cnt = connections.size();
        std::cout << "[压测引擎] 开始压测 " << config_.host << ':' << config_.port
                  << " | 负载=ide_mixed | 连接数=" << config_.connections
                  << " | 压测线程=" << worker_cnt
                  << " | 单连接 Inflight=" << config_.max_inflight
                  << " | 预热=" << config_.warmup_duration.count()
                  << "s | 总时长=" << config_.total_duration.count() << "s\n"
                  << std::flush;

        std::atomic<bool> abort_flag{false};
        std::vector<std::exception_ptr> worker_errors(worker_cnt);

        // 每条连接由独立线程（一对一绑定至非 Server 核，如 P-core 6,10,12,14）驱动私有 epoll 与 Meter，
        // 压测期间线程间零锁、零共享原子竞争、零跨核伪共享。
        auto worker_entry = [&](std::size_t idx) {
            try {
                if (!client_cpus.empty())
                    pin_current_thread_to_cpu(client_cpus[idx % client_cpus.size()]);

                auto &c = *connections[idx];
                auto last_timeout_check = Clock::now();
                bool warmed = (idx != 0) || (config_.warmup_duration.count() == 0);

                while (Clock::now() < test_end && !mixed::interrupted &&
                       !abort_flag.load(std::memory_order_relaxed)) {
                    c.fill();
                    pump_conn(c, last_timeout_check);
                    if (!warmed && Clock::now() >= warmup_end) {
                        warmed = true;
                        std::uint64_t total_warm = 0;
                        for (const auto &conn_ptr : connections)
                            total_warm += conn_ptr->meter().stats.warmup_completed();
                        std::cout << "[压测引擎] 预热完成 (" << config_.warmup_duration.count()
                                  << "s, " << total_warm << " reqs)，开始正式测量 "
                                  << (config_.total_duration - config_.warmup_duration).count()
                                  << "s...\n"
                                  << std::flush;
                    }
                }

                // 停止提交后在本线程排空在途请求，包括命令完成与 inotify 确认；排空耗时不进入测量窗口。
                while (!c.idle() && !abort_flag.load(std::memory_order_relaxed)) {
                    pump_conn(c, last_timeout_check);
                }
            } catch (...) {
                abort_flag.store(true, std::memory_order_relaxed);
                worker_errors[idx] = std::current_exception();
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(worker_cnt > 0 ? worker_cnt - 1 : 0);
        for (std::size_t idx = 1; idx < worker_cnt; ++idx)
            threads.emplace_back(worker_entry, idx);
        worker_entry(0);
        for (auto &t : threads)
            t.join();

        for (const auto &err : worker_errors) {
            if (err)
                std::rethrow_exception(err);
        }

        if (!mixed::interrupted) {
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
        }

        mixed::Meter meter;
        for (const auto &c : connections)
            meter.merge(c->meter());

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

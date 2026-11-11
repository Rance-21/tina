module;
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

export module ide.server_terminal;

import std;
import net.buffer;
import net.epoll;
import net.event_loop;
import service.terminal;
export import ide.server_protocol;

export namespace server {

// ============================================================================
// 服务端终端管理器（绑定在所属连接 I/O 线程的 EventLoop 上）
// ============================================================================
class TerminalManager : public std::enable_shared_from_this<TerminalManager> {
  public:
    // 由 rpc.handler 注入的事件发送回调（在所属 I/O 线程向对应客户端连接推送 Event 帧）。
    using EventSender = std::function<void(int conn_fd, const Frame &event)>;

    explicit TerminalManager(net::EventLoop &loop, EventSender sender = {})
        : loop_(loop), sender_(std::move(sender)) {
    }

    ~TerminalManager() {
        stop();
    }

    TerminalManager(const TerminalManager &) = delete;
    TerminalManager &operator=(const TerminalManager &) = delete;

    // 停机或析构时清理本 I/O 线程名下的所有终端会话与子进程（在所属 I/O 线程调用）。
    void stop() noexcept {
        std::vector<std::shared_ptr<Session>> all_sessions;
        {
            std::lock_guard lock(mutex_);
            if (stopped_)
                return;
            stopped_ = true;
            for (auto &[_, session] : sessions_)
                all_sessions.push_back(session);
            sessions_.clear();
        }

        for (auto &session : all_sessions)
            cleanup_session(*session, true);
    }

    // 当某条客户端 TCP 连接断开时，自动清理该连接创建的全部 Shell 会话。
    void close_for_connection(int conn_fd) noexcept {
        std::vector<std::shared_ptr<Session>> to_close;
        {
            std::lock_guard lock(mutex_);
            for (auto it = sessions_.begin(); it != sessions_.end();) {
                if (it->second->conn_fd == conn_fd) {
                    to_close.push_back(it->second);
                    it = sessions_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto &session : to_close)
            cleanup_session(*session, true);
    }

    // ========================================================================
    // 1. 处理 0x0303 (terminal_state) 请求：启动或关闭 Shell（在 Worker 线程调用）
    // ========================================================================
    // Payload 约定（[u32 len][data]）：
    //   - 启动 Shell：data 为空、"start" 或 4 字节 0；
    //   - 关闭 Shell：data 为 4 字节大端 terminal_id（> 0）。
    // 成功响应：Frame.status = Status::ok，Payload = [u32 len=4][u32 terminal_id]。
    void handle_terminal_state(Frame &frame, int conn_fd) {
        auto req_id = decode_terminal_state_id(frame.payload);
        if (!req_id) {
            frame.status = Status::invalid_argument;
            frame.payload = encode_terminal_state("invalid terminal_state payload");
            return;
        }

        if (*req_id == 0) {
            start_shell(frame, conn_fd);
        } else {
            close_shell(frame, conn_fd, *req_id);
        }
    }

    // ========================================================================
    // 2. 处理 0x0301 (terminal_input) 请求：发送完整命令（Worker 解码 + IO 线程 EPOLLOUT 写入）
    // ========================================================================
    // 流程：
    //   1. Worker 解码出 terminal_id 和命令文本，在末尾追加 '\r'；
    //   2. 投递到所属 I/O 线程，写入 session->write_buffer，并调用 loop_.modify_event() 注册 EPOLLOUT；
    //   3. 当 master_fd 触发 EPOLLOUT 时写入内核 PTY 缓冲区，写完后立即撤销 EPOLLOUT（只保留 EPOLLIN）；
    //   4. 命令执行期间的输出由 EPOLLIN 触发并通过 terminal_output (0x0381) 推送，
    //      命令执行完毕由 PROMPT_COMMAND 触发并通过 terminal_state (0x0303) 事件推送。
    void handle_terminal_input(Frame &frame, int conn_fd) {
        // 零拷贝校验并解析出 terminal_id 与指向 frame.payload 内部的命令视图
        auto req = decode_terminal_payload_view(frame.payload);
        if (!req || req->terminal_id == 0) {
            frame.status = Status::invalid_argument;
            frame.payload = encode_terminal_input(0, "invalid terminal_input payload");
            return;
        }
        const std::uint32_t terminal_id = req->terminal_id;
        auto session = find_session(terminal_id, conn_fd);
        if (!session) {
            frame.status = Status::not_found;
            frame.payload = encode_terminal_input(terminal_id, "terminal not found");
            return;
        }

        // 直接复用当前 RPC 请求帧的 payload 缓冲区：在末尾追加 '\r' 后整块 move 给 I/O 线程，零额外拷贝。
        frame.payload.push_back('\r');
        session->running_command.store(true);
        std::weak_ptr<Session> weak_session = session;

        // 仅捕获 weak_session 与 loop_ 引用，在 I/O 线程只锁当前 Session，不锁整个 TerminalManager。
        bool posted =
            loop_.post_task([&loop = loop_, weak_session, raw = std::move(frame.payload)]() {
                auto s = weak_session.lock();
                if (!s || s->closed.load() || s->master_fd < 0)
                    return;
                // 跳过前 8 字节头部（[u32 len][u32 terminal_id]），直接将命令 + '\r' 写入发送缓冲
                s->write_buffer.append(raw.data() + 8, raw.size() - 8);
                try {
                    // 注册 EPOLLOUT：通知 epoll 当 master_fd 可写时唤醒 I/O 线程写入命令
                    loop.modify_event(s->master_fd,
                                      net::EpollInterest::read | net::EpollInterest::write);
                } catch (...) {
                }
            });

        if (!posted) {
            session->running_command.store(false);
            frame.status = Status::unavailable;
            frame.payload = encode_terminal_input(terminal_id, "event loop stopped");
            return;
        }
        frame.status = Status::ok;
        frame.payload = encode_terminal_input(terminal_id, "OK");
    }

    // ========================================================================
    // 3. 处理 0x0302 (terminal_on_tab) 请求：Tab 命令补全（在 Worker 线程执行）
    // ========================================================================
    // 流程：
    //   1. 标记 tab_completing = true，暂停 I/O 线程对该终端的常规输出转发；
    //   2. 调用 service::terminal::complete_tab_on_pty() 执行清空 Readline、写入输入+\t、
    //      读取 PTY 字节并通过单行光标模拟器解析出补全后的纯净命令；
    //   3. 恢复 tab_completing = false 并直接在当前 Frame 返回。
    void handle_terminal_on_tab(Frame &frame, int conn_fd) {
        // 零拷贝解析出 terminal_id 与指向 frame.payload 内部的输入切片
        auto req = decode_terminal_payload_view(frame.payload);
        if (!req || req->terminal_id == 0) {
            frame.status = Status::invalid_argument;
            frame.payload = encode_terminal_on_tab(0, "invalid terminal_on_tab payload");
            return;
        }

        const std::uint32_t terminal_id = req->terminal_id;
        auto session = find_session(terminal_id, conn_fd);
        if (!session) {
            frame.status = Status::not_found;
            frame.payload = encode_terminal_on_tab(terminal_id, "terminal not found");
            return;
        }

        std::lock_guard tab_lock(session->tab_mutex);
        session->tab_completing.store(true);
        struct ResetGuard {
            std::atomic<bool> &flag;
            ~ResetGuard() {
                flag.store(false);
            }
        } guard{session->tab_completing};

        auto completed = service::terminal::complete_tab_on_pty(session->master_fd, req->data);
        if (!completed) {
            frame.status = Status::internal;
            frame.payload = encode_terminal_on_tab(terminal_id, req->data);
            return;
        }

        frame.status = Status::ok;
        frame.payload = encode_terminal_on_tab(terminal_id, *completed);
    }

    // 统一分发终端相关的 RPC 请求（0x0301、0x0302、0x0303 连续操作序号）。
    void handle_request(Frame &frame, int conn_fd) {
        switch (frame.method) {
        case Method::terminal_input:
            handle_terminal_input(frame, conn_fd);
            return;
        case Method::terminal_on_tab:
            handle_terminal_on_tab(frame, conn_fd);
            return;
        case Method::terminal_state:
            handle_terminal_state(frame, conn_fd);
            return;
        default:
            frame.status = Status::unsupported;
            frame.payload = "unsupported method";
            return;
        }
    }

  private:
    struct Session {
        std::uint32_t terminal_id{};
        int conn_fd{-1};
        int master_fd{-1};
        pid_t child_pid{-1};

        // 仅在所属 I/O 线程读写的收发缓冲：
        net::Buffer write_buffer{1024}; // 待通过 EPOLLOUT 写入 master_fd 的命令数据
        std::string read_carry;         // 跨 read() 块匹配 PROMPT_COMMAND 结束标记的残留缓冲

        std::atomic<bool> running_command{false};
        std::atomic<bool> tab_completing{false};
        std::atomic<bool> closed{false};
        std::mutex tab_mutex;
    };

    std::shared_ptr<Session> find_session(std::uint32_t terminal_id, int conn_fd) {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(terminal_id);
        if (it == sessions_.end() || it->second->conn_fd != conn_fd || it->second->closed.load())
            return nullptr;
        return it->second;
    }

    // 在 Worker 线程中通过 service::terminal::spawn_bash_pty() 启动 Bash，并把 master_fd 挂载到
    // EventLoop。
    void start_shell(Frame &frame, int conn_fd) {
        int master_fd = -1;
        pid_t pid = -1;
        std::string err = service::terminal::spawn_bash_pty(master_fd, pid);
        if (!err.empty()) {
            frame.status =
                (err == "forkpty failed") ? Status::resource_exhausted : Status::internal;
            frame.payload = encode_terminal_state(err);
            return;
        }

        auto session = std::make_shared<Session>();
        session->conn_fd = conn_fd;
        session->master_fd = master_fd;
        session->child_pid = pid;

        std::uint32_t terminal_id = 0;
        {
            // 给 sessions_ 加锁
            std::lock_guard lock(mutex_);
            if (stopped_) {
                service::terminal::terminate_pty_child(session->master_fd, session->child_pid);
                frame.status = Status::unavailable;
                frame.payload = encode_terminal_state("terminal manager stopped");
                return;
            }
            terminal_id = next_id_++;
            session->terminal_id = terminal_id;
            sessions_.emplace(terminal_id, session);
        }

        // 将 master_fd 跨线程投递到所属连接的 EventLoop 上监听可读事件（EPOLLIN）。
        auto registered = std::make_shared<std::promise<bool>>();
        auto future = registered->get_future();
        std::weak_ptr<Session> weak_session = session;
        std::weak_ptr<TerminalManager> weak = shared_from_this();

        if (!loop_.post_task([&loop = loop_, weak, weak_session, registered]() {
                auto s = weak_session.lock();
                if (!s || s->closed.load() || s->master_fd < 0) {
                    registered->set_value(false);
                    return;
                }
                try {
                    // epoll 回调仅持有 weak_session，不强引用 Session
                    loop.add_event(s->master_fd, net::EpollInterest::read,
                                   [weak, weak_session](net::EpollEvent ev) {
                                       auto sess = weak_session.lock();
                                       if (!sess)
                                           return;
                                       if (auto mgr = weak.lock())
                                           mgr->on_pty_event(sess, ev);
                                   });
                    registered->set_value(true);
                } catch (...) {
                    registered->set_value(false);
                }
            }) ||
            !future.get()) {
            {
                std::lock_guard lock(mutex_);
                sessions_.erase(terminal_id);
            }
            cleanup_session(*session, false);
            frame.status = Status::internal;
            frame.payload = encode_terminal_state("failed to register pty fd to event loop");
            return;
        }

        frame.status = Status::ok;
        frame.payload = encode_terminal_state_id(terminal_id);
    }

    // 在 Worker 线程中关闭指定 terminal_id 的 Shell。
    void close_shell(Frame &frame, int conn_fd, std::uint32_t terminal_id) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(mutex_);
            auto it = sessions_.find(terminal_id);
            if (it != sessions_.end() && it->second->conn_fd == conn_fd) {
                session = it->second;
                sessions_.erase(it);
            }
        }

        if (!session) {
            frame.status = Status::not_found;
            frame.payload = encode_terminal_state_id(terminal_id);
            return;
        }

        session->closed.store(true);
        auto unhooked = std::make_shared<std::promise<void>>();
        auto future = unhooked->get_future();

        if (loop_.post_task([&loop = loop_, fd = session->master_fd, unhooked]() {
                try {
                    loop.del_event(fd);
                } catch (...) {
                }
                unhooked->set_value();
            })) {
            future.wait();
        }

        cleanup_session(*session, false);
        frame.status = Status::ok;
        frame.payload = encode_terminal_state_id(terminal_id);
    }

    // 释放 master_fd 并终止、回收 Shell 子进程。
    void cleanup_session(Session &session, bool on_io_thread) noexcept {
        session.closed.store(true);
        if (on_io_thread && session.master_fd >= 0) {
            try {
                loop_.del_event(session.master_fd);
            } catch (...) {
            }
        }
        service::terminal::terminate_pty_child(session.master_fd, session.child_pid);
    }

    // ========================================================================
    // I/O 线程 Epoll 事件回调（处理 master_fd 的 EPOLLOUT 命令写入与 EPOLLIN 输出读取）
    // ========================================================================
    void on_pty_event(const std::shared_ptr<Session> &session, net::EpollEvent event) {
        if (session->closed.load() || session->master_fd < 0)
            return;

        // 1. 处理可写事件（EPOLLOUT）：将 write_buffer 中的命令写入 master_fd，写完立即撤销
        // EPOLLOUT。
        if (event.writable() && !session->write_buffer.empty()) {
            while (!session->write_buffer.empty()) {
                ssize_t n = ::write(session->master_fd, session->write_buffer.data(),
                                    session->write_buffer.readable_bytes());
                if (n > 0) {
                    session->write_buffer.retrieve(static_cast<std::size_t>(n));
                    continue;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                break; // EAGAIN：等待下次 EPOLLOUT
            }

            if (session->write_buffer.empty()) {
                try {
                    loop_.modify_event(session->master_fd, net::EpollInterest::read);
                } catch (...) {
                }
            }
        }

        // 2. 处理可读事件（EPOLLIN）：若 Worker 正在做 Tab 补全则暂不抢读 master_fd。
        if (session->tab_completing.load())
            return;

        bool child_exited = false;
        if (event.readable() || event.hangup() || event.error()) {
            std::array<char, 8192> buf{};
            for (int i = 0; i < 8; ++i) {
                if (session->tab_completing.load())
                    break;

                ssize_t n = ::read(session->master_fd, buf.data(), buf.size());
                if (n > 0) {
                    session->read_carry.append(buf.data(), static_cast<std::size_t>(n));
                    process_pty_output(*session, false);
                    continue;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;

                // Linux PTY 特性：当子进程退出关闭全部 slave_fd 时，read(master_fd) 返回 0 或 -1
                // (errno == EIO)。
                child_exited = true;
                break;
            }
        }

        if (child_exited) {
            process_pty_output(*session, true);
            {
                std::lock_guard lock(mutex_);
                sessions_.erase(session->terminal_id);
            }
            cleanup_session(*session, true);
            send_state_event(session->conn_fd, session->terminal_id);
        }
    }

    // 从 read_carry 中识别并剥离 PROMPT_COMMAND 结束标记（\033]777;__TINA_DONE__\007）：
    //   - 普通输出通过 terminal_output (0x0381) 事件推送；
    //   - 遇到结束标记说明命令已执行完毕，通过 terminal_state (0x0303) 事件推送通知客户端。
    void process_pty_output(Session &session, bool flush_all) {
        using service::terminal::prompt_done_marker;
        while (!session.read_carry.empty()) {
            const std::size_t pos = session.read_carry.find(prompt_done_marker);
            if (pos != std::string::npos) {
                if (pos > 0)
                    send_output_chunks(session.conn_fd, session.terminal_id,
                                       std::string_view(session.read_carry).substr(0, pos));

                session.read_carry.erase(0, pos + prompt_done_marker.size());
                if (session.running_command.exchange(false))
                    send_state_event(session.conn_fd, session.terminal_id);
                continue;
            }

            if (flush_all) {
                send_output_chunks(session.conn_fd, session.terminal_id, session.read_carry);
                session.read_carry.clear();
                break;
            }

            // 保留尾部可能与 prompt_done_marker 前缀发生半包重叠的少量字节（不超过标记长度 - 1）
            const std::size_t emit_len =
                service::terminal::safe_output_prefix_len(session.read_carry);
            if (emit_len > 0) {
                send_output_chunks(session.conn_fd, session.terminal_id,
                                   std::string_view(session.read_carry).substr(0, emit_len));
                session.read_carry.erase(0, emit_len);
            }
            break;
        }
    }

    // 按 max_terminal_output_bytes 分块推送 0x0381 (terminal_output) 事件。
    void send_output_chunks(int conn_fd, std::uint32_t terminal_id, std::string_view text) {
        if (!sender_ || text.empty())
            return;

        for (std::size_t offset = 0; offset < text.size();) {
            const std::size_t chunk_len =
                std::min<std::size_t>(text.size() - offset, max_terminal_output_bytes);
            sender_(conn_fd,
                    make_terminal_output_event(terminal_id, text.substr(offset, chunk_len)));
            offset += chunk_len;
        }
    }

    // 推送 0x0303 (terminal_state) 事件（通知客户端命令执行完毕或 Shell 退出）。
    void send_state_event(int conn_fd, std::uint32_t terminal_id) {
        if (sender_)
            sender_(conn_fd, make_terminal_state_event(terminal_id));
    }

    net::EventLoop &loop_;
    EventSender sender_;

    std::mutex mutex_;
    bool stopped_{false};
    std::uint32_t next_id_{1};
    std::unordered_map<std::uint32_t, std::shared_ptr<Session>> sessions_;
};

} // namespace server

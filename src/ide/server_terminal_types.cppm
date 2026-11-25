module;
#include <sys/types.h>

export module ide.server_terminal:types;

import std;
import net.buffer;
import net.epoll;
import net.event_loop;
export import ide.server_protocol;

export namespace server {

// ============================================================================
// 服务端终端管理器（绑定在所属连接 I/O 线程的 EventLoop 上）
// ============================================================================
class TerminalManager : public std::enable_shared_from_this<TerminalManager> {
  public:
    // 由 rpc.handler 注入的事件发送回调（在所属 I/O 线程向对应客户端连接推送 Event 帧）。
    using EventSender = std::function<void(int conn_fd, Frame &&event)>;

    explicit TerminalManager(net::EventLoop &loop, EventSender sender = {})
        : loop_(loop), sender_(std::move(sender)) {
    }

    ~TerminalManager() {
        stop();
    }

    TerminalManager(const TerminalManager &) = delete;
    TerminalManager &operator=(const TerminalManager &) = delete;

    void stop() noexcept;

    void close_for_connection(int conn_fd) noexcept;

    void handle_terminal_state(Frame &frame, int conn_fd);

    void handle_terminal_input(Frame &frame, int conn_fd);

    void handle_terminal_on_tab(Frame &frame, int conn_fd);

    void handle_request(Frame &frame, int conn_fd);

  private:
    struct Session {
        std::uint32_t terminal_id{};
        int conn_fd{-1};
        int master_fd{-1};
        pid_t child_pid{-1};

        // 仅在所属 I/O 线程读写的状态与收发缓冲：
        net::Buffer write_buffer{1024}; // 待通过 EPOLLOUT 写入 master_fd 的命令数据
        std::string read_carry;         // 跨 read() 块匹配 PROMPT_COMMAND 结束标记的残留缓冲
        bool running_command{false};
        bool closed{false};

        std::atomic<bool> tab_completing{false};
        std::mutex tab_mutex;
    };

    std::shared_ptr<Session> find_session(std::uint32_t terminal_id, int conn_fd) {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(terminal_id);
        if (it == sessions_.end() || it->second->conn_fd != conn_fd)
            return nullptr;
        return it->second;
    }

    void start_shell(Frame &frame, int conn_fd);

    void close_shell(Frame &frame, int conn_fd, std::uint32_t terminal_id);

    void cleanup_session(Session &session, bool on_io_thread) noexcept;

    void on_pty_event(const std::shared_ptr<Session> &session, net::EpollEvent event);

    void process_pty_output(Session &session, bool flush_all);

    void send_output_chunks(int conn_fd, std::uint32_t terminal_id, std::string_view text);

    void send_state_event(int conn_fd, std::uint32_t terminal_id);

    net::EventLoop &loop_;
    EventSender sender_;

    std::mutex mutex_;
    bool stopped_{false};
    std::uint32_t next_id_{1};
    std::unordered_map<std::uint32_t, std::shared_ptr<Session>> sessions_;
};

} // namespace server

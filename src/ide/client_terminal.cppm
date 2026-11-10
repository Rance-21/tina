export module ide.client_terminal;

import std;
import rpc.client;
export import ide.client_protocol;

export namespace client {

// ============================================================================
// Terminal 客户端会话封装（含 RPC 编解码、异步输出推送与命令完成通知）
// ============================================================================
class TerminalClient {
  public:
    // 收到 0x0281 (terminal_output) 终端输出事件时的回调：(terminal_id, output_text)
    using OutputCallback = std::function<void(std::uint32_t, std::string_view)>;
    // 收到 0x0282 (terminal_state) 命令执行完毕或退出事件时的回调：(terminal_id)
    using StateCallback = std::function<void(std::uint32_t)>;

    explicit TerminalClient(std::string_view host = "127.0.0.1", std::string_view port = "3490")
        : rpc_(host, port) {
        rpc_.set_event_handler([this](Frame event) {
            handle_event(std::move(event));
        });
    }

    // 注册终端输出事件（0x0281）回调。
    void set_output_callback(OutputCallback callback) {
        std::lock_guard lock(mutex_);
        output_callback_ = std::move(callback);
    }

    // 注册终端状态事件（0x0282，如 PROMPT_COMMAND 通知命令执行完毕）回调。
    void set_state_callback(StateCallback callback) {
        std::lock_guard lock(mutex_);
        state_callback_ = std::move(callback);
    }

    // 0x0282 (terminal_state) 请求：启动一个新 Shell，返回服务端分配的 terminal_id。
    std::uint32_t start_terminal() {
        auto frame =
            rpc_.async_call(Method::terminal_state, encode_terminal_state_id(0)).get();
        if (frame.status != Status::ok)
            throw std::runtime_error("terminal_state start failed: " + frame.payload);

        auto terminal_id = decode_terminal_state_id(frame.payload);
        if (!terminal_id || *terminal_id == 0)
            throw std::runtime_error("invalid terminal_state start response payload");

        {
            std::lock_guard lock(mutex_);
            busy_[*terminal_id] = false;
        }
        return *terminal_id;
    }

    // 0x0282 (terminal_state) 请求：关闭指定 terminal_id 的 Shell。
    void close_terminal(std::uint32_t terminal_id) {
        auto frame =
            rpc_.async_call(Method::terminal_state, encode_terminal_state_id(terminal_id)).get();
        if (frame.status != Status::ok)
            throw std::runtime_error("terminal_state close failed: " + frame.payload);

        {
            std::lock_guard lock(mutex_);
            busy_.erase(terminal_id);
        }
        cv_.notify_all();
    }

    // 0x0021 (terminal_input) 请求：向指定终端发送完整命令（服务端自动追加 '\r' 并通过 EPOLLOUT 写入）。
    // 后续命令输出通过 0x0281 (terminal_output) 事件推送，执行完毕通过 0x0282 (terminal_state) 事件通知。
    void send_input(std::uint32_t terminal_id, std::string_view command) {
        {
            std::lock_guard lock(mutex_);
            busy_[terminal_id] = true;
        }

        auto frame =
            rpc_.async_call(Method::terminal_input, encode_terminal_input(terminal_id, command))
                .get();
        if (frame.status != Status::ok) {
            {
                std::lock_guard lock(mutex_);
                busy_[terminal_id] = false;
            }
            cv_.notify_all();
            throw std::runtime_error("terminal_input failed: " + frame.payload);
        }
    }

    // 0x0022 (terminal_on_tab) 请求：发送当前输入行触发 Tab 补全，直接返回服务端解析后的补全字符串。
    std::string on_tab(std::uint32_t terminal_id, std::string_view input) {
        auto frame =
            rpc_.async_call(Method::terminal_on_tab, encode_terminal_on_tab(terminal_id, input))
                .get();
        if (frame.status != Status::ok)
            throw std::runtime_error("terminal_on_tab failed: " + frame.payload);

        auto msg = decode_terminal_payload_view(frame.payload);
        if (!msg || msg->terminal_id != terminal_id)
            throw std::runtime_error("invalid terminal_on_tab response payload");

        return std::string(msg->data);
    }

    // 查询指定终端当前是否正在执行命令。
    [[nodiscard]] bool is_busy(std::uint32_t terminal_id) const {
        std::lock_guard lock(mutex_);
        auto it = busy_.find(terminal_id);
        return it != busy_.end() && it->second;
    }

    // 等待指定终端的当前命令执行完毕（收到服务端 PROMPT_COMMAND 触发的 terminal_state 事件）。
    bool wait_command_done(std::uint32_t terminal_id,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, terminal_id] {
            auto it = busy_.find(terminal_id);
            return it == busy_.end() || !it->second;
        });
    }

  private:
    void handle_event(Frame event) {
        if (event.kind != Kind::event)
            return;

        if (event.method == Method::terminal_output) {
            // 零拷贝解析出 string_view 切片直接传给回调，无需额外构造 std::string
            auto msg = decode_terminal_payload_view(event.payload);
            if (!msg)
                return;

            OutputCallback cb;
            {
                std::lock_guard lock(mutex_);
                cb = output_callback_;
            }
            if (cb)
                cb(msg->terminal_id, msg->data);
            return;
        }

        if (event.method == Method::terminal_state) {
            auto terminal_id = decode_terminal_state_id(event.payload);
            if (!terminal_id)
                return;

            StateCallback cb;
            {
                std::lock_guard lock(mutex_);
                busy_[*terminal_id] = false;
                cb = state_callback_;
            }
            cv_.notify_all();
            if (cb)
                cb(*terminal_id);
        }
    }

    rpc::RpcClient rpc_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::uint32_t, bool> busy_;
    OutputCallback output_callback_;
    StateCallback state_callback_;
};

} // namespace client

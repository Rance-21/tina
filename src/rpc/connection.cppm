export module rpc.connection;

import std;
import net.buffer;
import net.socket;
import net.event_loop;
import ide.protocol;

export namespace rpc {

// Server 用
class Connection {
  public:
    explicit Connection(net::Socket socket) : socket_(std::move(socket)) {
    }

    [[nodiscard]]
    inline int fd() const noexcept {
        return socket_.native_handle();
    }

    [[nodiscard]]
    inline bool wants_write() const noexcept {
        return !output_.empty();
    }

    [[nodiscard]]
    inline bool peer_eof() const noexcept {
        return peer_eof_;
    }

    [[nodiscard]]
    inline bool draining() const noexcept {
        return draining_;
    }

    // 停止读取新的 RPC 请求，已提交的 Worker 任务和响应仍然有效。
    inline void begin_shutdown() noexcept {
        draining_ = true;
    }

    [[nodiscard]]
    bool finished() const noexcept {
        // 完成连接半关闭尾巴或者手动 shutdown
        return (peer_eof_ || draining_) && pending_ == 0 && output_.empty();
    }

    // 读取 Socket，解析请求并提交给线程池。
    template <typename Submit> bool read(Submit &&submit) {
        if (draining_)
            return true;

        std::array<char, 8192> chunk{};

        // 每次事件最多读 64 KiB，避免一个连接长期占用 I/O 线程。
        for (int i = 0; i < 8; ++i) {
            auto n = socket_.try_recv(chunk.data(), chunk.size());

            if (!n)
                return true;

            if (*n == 0) {
                peer_eof_ = true;

                // EOF 时不允许残留不完整的 RPC Frame。
                return input_.empty();
            }

            input_.append(chunk.data(), *n);

            while (auto request = decode_frame(input_)) {
                if (request->kind != Kind::request || pending_ >= max_inflight ||
                    !submit(std::move(*request)))
                    return false;
                ++pending_;
            }
        }

        return true;
    }

    // Worker 返回响应帧后，由所属 I/O 线程调用。
    bool has_complete_response(const Frame &response) {
        const auto frame_size = frame_header_size + response.payload.size();
        if (pending_ == 0 || output_.readable_bytes() + frame_size > max_output)
            return false;

        --pending_;
        encode_frame(response, output_);
        return true;
    }

    // 非阻塞发送输出缓冲区中的数据。
    void write() {
        for (int i = 0; i < 8 && wants_write(); ++i) {
            auto n = socket_.try_send(output_.data(), output_.readable_bytes());

            // EAGAIN：等待下一次 EPOLLOUT。
            if (!n || *n == 0)
                break;

            output_.retrieve(*n);
        }
    }

  private:
    static constexpr std::size_t max_inflight = 64;
    static constexpr std::size_t max_output = 4 * 1024 * 1024;

    net::Socket socket_;
    net::Buffer input_;
    net::Buffer output_;

    // 已提交、但尚未收到业务执行结果的 RPC 数量。
    std::size_t pending_{0};

    bool peer_eof_{false};

    // true 时已经清空
    bool draining_{false};
};

using ConnectionMap = std::unordered_map<int, std::shared_ptr<Connection>>;

// 记录全体 I/O 线程的活跃连接数，供 Acceptor 等待排空。
class ConnectionCounter {
  public:
    void opened() {
        std::lock_guard lock(mutex_);
        ++count_;
    }

    void closed() {
        {
            std::lock_guard lock(mutex_);
            --count_;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool wait_empty_for(std::chrono::seconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] {
            return count_ == 0;
        });
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t count_{0};
};

// 注销监听后再释放 Connection，防止 epoll 中留下旧 fd。
void close_connection(net::EventLoop &loop, ConnectionMap &connections, ConnectionCounter &counter,
                      int fd) {
    auto it = connections.find(fd);
    if (it == connections.end())
        return;

    loop.del_event(fd);
    connections.erase(it);
    counter.closed();
}

} // namespace rpc

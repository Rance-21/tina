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

    // 判断连接是否已满足关闭条件：
    // 对端已半关闭（peer_eof_）或服务端主动停止读新请求（draining_），
    // 且所有已提交给 Worker 的在途请求均已处理完（pending_ == 0），且输出缓冲区已全部发空。
    [[nodiscard]]
    bool finished() const noexcept {
        return (peer_eof_ || draining_) && pending_ == 0 && output_.empty();
    }

    // 非阻塞读取 Socket 数据，切出完整 RPC 请求帧并提交给 WorkerPool。
    // 返回 false 表示协议非法、并发超限或任务队列已满，调用方应立即关闭连接。
    // 分支逻辑：
    //   1. draining_ 为 true：处于停机排空阶段，不再读取新数据，直接返回 true。
    //   2. 循环最多读 8 次（上限 64 KiB）：防止单条活跃连接饥饿同一 I/O 线程上的其他连接。
    //      - !n (EAGAIN)：内核接收缓冲区已读空，正常返回 true 等待下次 EPOLLIN。
    //      - *n == 0 (EOF)：对端发送 FIN 关闭写端；若 input_ 还有未凑齐的半包则视为协议错误返回 false。
    //      - *n > 0：追加到 input_ 并循环 decode_frame() 拆包；若非 request 帧、在途请求达到
    //        max_inflight (64) 或 WorkerPool 拒绝接收任务，则返回 false 关闭连接。
    template <typename Submit> bool read(Submit &&submit) {
        if (draining_)
            return true;

        std::array<char, 8192> chunk{};

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

    // Worker 算完响应后由所属 I/O 线程调用：核销一个在途请求（--pending_），并将响应帧编码进 output_。
    // 分支逻辑：若在途计数异常为 0，或追加后输出缓冲区超过 max_output (4 MiB) 上限，则返回 false 关闭连接。
    bool enqueue_response(const Frame &response) {
        const auto frame_size = frame_header_size + response.payload.size();
        if (pending_ == 0 || output_.readable_bytes() + frame_size > max_output)
            return false;

        --pending_;
        encode_frame(response, output_);
        return true;
    }

    // 非阻塞将 output_ 中的待发数据刷入内核 Socket 发送缓冲区。
    // 分支逻辑：若 try_send 返回 nullopt（EAGAIN，内核发送缓冲区已满）则立即中断循环，
    // 剩余未发完的数据留在 output_ 中，由随后的 refresh_connection() 按需挂载 EPOLLOUT。
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

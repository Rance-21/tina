export module rpc.connection;

import std;
import net.buffer;
import net.socket;
import net.event_loop;
import rpc.protocol;

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
    // 且所有已提交给 Worker 的在途请求均已处理完（pending_ == 0），且待发队列已全部发空。
    [[nodiscard]]
    bool finished() const noexcept {
        return (peer_eof_ || draining_) && pending_ == 0 && output_.empty();
    }

    // 非阻塞读取 Socket 数据，逐帧解析完整 RPC 请求并分流处理：
    //   1. draining_ 为 true：处于停机排空阶段，不再读取新数据，直接返回 true。
    //   2. 循环最多接收 8 次（单次缓冲按 8 KiB 扩容），防止单条高活跃连接饿死同一 I/O 线程上的其他连接。
    //   3. 每拆出一帧完整 request，先执行 fast_handle(*request)：
    //      - 返回 true（快路径命中，如小文件 read_file 命中内存缓存）：
    //        直接在当前 I/O 线程将响应入队 output_，不经过 WorkerPool、不自增 pending_，
    //        随后在本轮 read 结束后的 conn->write() 中统一用 ::writev() 聚集发出；
    //      - 返回 false（慢路径，如缓存未命中需读盘、切片超出阈值或写/终端/Git 请求）：
    //        检查在途请求数未超 max_inflight (64) 后投递给 WorkerPool 异步处理，并递增 pending_。
    // 返回 false 表示协议非法、在途并发超限、发送缓冲满、Worker 队列满或 EOF 时残留半包，调用方将关闭连接。
    // 【作用】read() 兼容重载：无需内联响应后置钩子时，转发到三参数版本。
    template <typename FastHandle, typename Submit>
    bool read(FastHandle &&fast_handle, Submit &&submit) {
        return read(std::forward<FastHandle>(fast_handle), [](const Frame &) {},
                    std::forward<Submit>(submit));
    }

    // 【作用】三参数版 read()：在内联快路径响应成功入队 output_ 后，回调 on_inline_enqueued
    //         （例如目录缓存内联命中后顺手在当前 I/O 线程注册 DirWatcher 的 inotify 监听）。
    // 【原理】单次最多 recv 8 轮防饿死其他连接；每拆出一帧完整请求先试 fast_handle，命中直接入队并触发
    //         on_inline_enqueued；未命中则检查在途数上限 max_inflight 后交给 submit 异步提交并 ++pending_。
    template <typename FastHandle, typename OnInlineEnqueued, typename Submit>
    bool read(FastHandle &&fast_handle, OnInlineEnqueued &&on_inline_enqueued, Submit &&submit) {
        if (draining_)
            return true;

        for (int i = 0; i < 8; ++i) {
            input_.ensure_writable(8192);
            auto n = socket_.try_recv(input_.writable_data(), input_.writable_bytes());

            if (!n)
                return true;

            if (*n == 0) {
                peer_eof_ = true;

                // EOF 时不允许残留不完整的 RPC Frame。
                return input_.empty();
            }

            input_.commit_write(*n);

            while (auto request = decode_frame(input_)) {
                if (request->kind != Kind::request)
                    return false;
                if (fast_handle(*request)) {
                    if (!enqueue_direct_response(*request))
                        return false;
                    on_inline_enqueued(*request);
                    continue;
                }
                if (pending_ >= max_inflight || !submit(std::move(*request)))
                    return false;
                ++pending_;
            }
        }

        return true;
    }

    // I/O 线程快路径直接响应入队（直接复用 FrameSendQueue::enqueue，不占用 pending_ 计数）：
    //   - 当单条连接在一次 read() 流水线中连续命中多条快路径请求时，先发响应会暂存在 output_ 中；
    //   - 为避免突发大流量在尚未退出 read() 循环前就撑爆 4 MiB 的 max_output 上限，
    //     在积压达到 256 KiB 或首次 enqueue 放不下时，立即尝试一次非阻塞 ::writev() 刷出数据再入队；
    //   - 若刷写后依然超出 max_output（说明对端读太慢且内核发送缓冲区已满），返回 false 断开慢连接。
    bool enqueue_direct_response(Frame &response) {
        if (output_.readable_bytes() >= 256 * 1024)
            output_.write(socket_);
        if (output_.enqueue(response, max_output))
            return true;
        output_.write(socket_);
        return output_.enqueue(response, max_output);
    }

    // Worker 算完响应后由所属 I/O 线程调用：
    // 核销一个在途请求（--pending_），编码 17B 帧头并直接 std::move 接管 response.payload（不拷贝负载）。
    bool enqueue_response(Frame &response) {
        if (pending_ == 0 || !output_.enqueue(response, max_output))
            return false;

        --pending_;
        return true;
    }

    // 服务端主动向客户端推送 Event 帧（如 workspace_list_dir 目录变更通知），不核销 pending_。
    bool enqueue_event(const Frame &event) {
        if (peer_eof_ || draining_)
            return false;

        return output_.enqueue(event, max_output);
    }

    bool enqueue_event(Frame &&event) {
        if (peer_eof_ || draining_)
            return false;

        return output_.enqueue(event, max_output);
    }

    // 使用 ::writev() 将 output_ 队列中的“17B 帧头 + Payload”聚集写入内核 Socket 发送缓冲区。
    void write() {
        output_.write(socket_);
    }

    // 【作用】检查当前连接上是否还有尚未完成的同路径写请求（workspace_write_file）。
    // 【原理】Connection 只在所属单个 I/O 线程访问（无需加锁）；绝大多数时候 inflight_writes_ 为空，
    //         直接一次 empty() 判断返回 false，避免内联 read 越过同连接已提交的 write 读到旧缓存。
    [[nodiscard]] bool has_inflight_write(std::string_view path) const noexcept {
        return std::ranges::any_of(inflight_writes_,
                                   [&](const auto &item) { return item.path == path; });
    }

    // 【作用】登记一条已成功提交到 WorkerPool 的写文件请求。
    // 【原理】把 (request_id, path) 追加到本连接私有 vector，等待响应返回时按 request_id 核销。
    void track_inflight_write(std::uint64_t request_id, std::string_view path) {
        inflight_writes_.push_back(InflightWrite{
            .request_id = request_id,
            .path = std::string(path),
        });
    }

    // 【作用】在写文件响应回到 I/O 线程时撤销对应的在途写登记。
    // 【原理】按 request_id 找到记录后与末尾元素交换并 pop_back()（O(1) 删除，无需维持顺序）。
    void finish_inflight_write(std::uint64_t request_id) noexcept {
        for (auto it = inflight_writes_.begin(); it != inflight_writes_.end(); ++it) {
            if (it->request_id == request_id) {
                *it = std::move(inflight_writes_.back());
                inflight_writes_.pop_back();
                return;
            }
        }
    }

  private:
    struct InflightWrite {
        std::uint64_t request_id{0};
        std::string path;
    };

    static constexpr std::size_t max_inflight = 64;
    static constexpr std::size_t max_output = 4 * 1024 * 1024;

    net::Socket socket_;
    net::Buffer input_;
    net::FrameSendQueue output_;
    std::vector<InflightWrite> inflight_writes_;

    // 已提交、但尚未收到业务执行结果的 RPC 数量。
    std::size_t pending_{0};

    bool peer_eof_{false};
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

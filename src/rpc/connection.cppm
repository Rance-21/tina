export module rpc.connection;

import std;
import net.buffer;
import net.socket;
import net.event_loop;
import net.epoll;
import rpc.protocol;

export namespace rpc {

// 连接及其收发缓冲只由所属 EventLoop 访问。Worker 保存 fd 与世代号，不能持有连接引用。
class Connection {
  public:
    explicit Connection(net::Socket socket, std::uint64_t id)
        : socket_(std::move(socket)), id_(id) {
    }

    [[nodiscard]] int fd() const noexcept {
        return socket_.native_handle();
    }

    [[nodiscard]] std::uint64_t id() const noexcept {
        return id_;
    }

    [[nodiscard]] bool wants_write() const noexcept {
        return !output_.empty();
    }

    // 对端半关闭和服务端停机之后，都停止收请求和推送事件，但仍要发完在途响应。
    [[nodiscard]] bool accepting_requests() const noexcept {
        return accepting_requests_;
    }

    void stop_reading() noexcept {
        accepting_requests_ = false;
    }

    [[nodiscard]] bool finished() const noexcept {
        return !accepting_requests_ && pending_ == 0 && output_.empty();
    }

    // ONESHOT 交付一次事件后失效。只有待发队列非空时才订阅 EPOLLOUT，
    // 否则几乎总是可写的 TCP socket 会让 epoll 不断唤醒本线程。
    [[nodiscard]] net::EpollInterest interests() const noexcept {
        auto mask = net::EpollInterest::one_shot;
        if (accepting_requests_)
            mask = mask | net::EpollInterest::read | net::EpollInterest::peer_closed;
        if (wants_write())
            mask = mask | net::EpollInterest::write;
        return mask;
    }

    // 在可读事件中最多 recv 八轮，每轮直接写入输入缓冲，再拆出其中全部完整帧。
    // 快路径响应入队成功后才登记目录订阅。只有提交给 Worker 的帧才占用在途名额。
    // 返回 false 表示收到非法帧，或发送队列、在途上限、Worker 队列已无法接收请求。
    template <typename FastHandle, typename OnInlineEnqueued, typename Submit>
    bool read_requests(FastHandle &&fast_handle, OnInlineEnqueued &&on_inline_enqueued,
                       Submit &&submit) {
        for (int i = 0; i < 8; ++i) {
            input_.ensure_writable(8192);
            auto n = socket_.try_recv(input_.writable_data(), input_.writable_bytes());

            if (!n)
                return true;

            if (*n == 0) {
                stop_reading();

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
    //   - 当单条连接在一次 read_requests() 流水线中连续命中多条快路径请求时，先发响应会暂存在 output_ 中；
    //   - 为避免突发大流量在尚未退出收包循环前就撑爆 4 MiB 的 max_output 上限，
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

    // 单播时发送队列可直接接管正文。广播时第一个订阅者把正文转成共享切片，
    // 后续队列只拷贝引用。事件没有对应请求，因此不核销 pending_。
    bool enqueue_event(Frame &event, bool broadcast) {
        if (broadcast) {
            event.share_payload();
            return output_.enqueue(std::as_const(event), max_output);
        }
        return output_.enqueue(event, max_output);
    }

    // 使用 ::writev() 将 output_ 队列中的“17B 帧头 + Payload”聚集写入内核 Socket 发送缓冲区。
    void flush_output() {
        output_.write(socket_);
    }

  private:
    static constexpr std::size_t max_inflight = 64;
    static constexpr std::size_t max_output = 4 * 1024 * 1024;

    net::Socket socket_;
    const std::uint64_t id_;
    net::Buffer input_;
    net::FrameSendQueue output_;

    // 已提交、但尚未收到业务执行结果的 RPC 数量。
    std::size_t pending_{0};

    bool accepting_requests_{true};
};

// 每个 I/O 线程独占一张连接表。表节点直接拥有 socket，rehash 也不会移动节点里的 Connection。
// epoll 回调可在注册至注销之间借用节点；异步回包和目录订阅跨越此期限，只保存 (fd, id)。
class LoopConnections {
  public:
    explicit LoopConnections(net::EventLoop &loop) : loop_(loop) {
    }

    void add(net::Socket socket, auto on_event) {
        const int fd = socket.native_handle();
        auto [it, inserted] = active_.try_emplace(fd, std::move(socket), next_id_++);
        if (!inserted)
            throw std::logic_error("connection fd already registered");
        Connection &conn = it->second;
        try {
            loop_.add_event(fd, conn.interests(), [&conn, on_event](net::EpollEvent event) {
                on_event(conn, event);
            });
        } catch (...) {
            active_.erase(it);
            throw;
        }
    }

    [[nodiscard]] Connection *find(int fd) {
        auto it = active_.find(fd);
        return it == active_.end() ? nullptr : &it->second;
    }

    // fd 在 close 后可立即复用；迟到的回包和目录事件必须同时匹配原连接的世代号。
    [[nodiscard]] Connection *find(int fd, std::uint64_t id) {
        auto *conn = find(fd);
        return conn && conn->id() == id ? conn : nullptr;
    }

    void close(int fd) {
        loop_.del_event(fd); // 先注销借用节点的回调，再销毁 socket。
        active_.erase(fd);
        if (drain_complete_ && active_.empty()) {
            drain_complete_->set_value();
            drain_complete_.reset();
        }
    }

    // 已经停读且没有待写数据时，只有在途 Worker 回包能推进连接，无需重装 ONESHOT。
    void rearm_or_close(Connection &conn) {
        if (conn.finished())
            close(conn.fd());
        else if (conn.accepting_requests() || conn.wants_write())
            loop_.modify_event(conn.fd(), conn.interests());
    }

    // 目录和 PTY 推送共用此路径。背压超限或 socket 出错会关闭这条连接，
    // 返回 false 让目录监听同时移除相应订阅。调用方已跳过停读的连接。
    bool send_event(Connection &conn, Frame &event, bool broadcast = false) {
        const int fd = conn.fd();
        try {
            if (conn.enqueue_event(event, broadcast)) {
                conn.flush_output();
                rearm_or_close(conn);
                return true;
            }
        } catch (...) {
            // writev 或重装监听失败，走相同的断连清理。
        }
        close(fd);
        return false;
    }

    // 只在停机时建立一次跨线程通知。空表立即就绪，其余由最后一次 close 唤醒主线程。
    std::future<void> begin_drain() {
        std::promise<void> complete;
        auto drained = complete.get_future();
        for (auto it = active_.begin(); it != active_.end();) {
            Connection &conn = (it++)->second; // rearm_or_close 可能删除当前节点。
            conn.stop_reading();
            rearm_or_close(conn);
        }
        if (active_.empty())
            complete.set_value();
        else
            drain_complete_.emplace(std::move(complete));
        return drained;
    }

    void close_all() {
        while (!active_.empty())
            close(active_.begin()->first);
    }

  private:
    net::EventLoop &loop_;
    std::unordered_map<int, Connection> active_;
    std::uint64_t next_id_{1};
    std::optional<std::promise<void>> drain_complete_;
};

} // namespace rpc

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
    // 每拆出一帧就交给调用方；协议错误或调用方拒绝本帧时立即断开。
    bool read_requests(auto &&on_request) {
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
                if (request->kind != Kind::request || !on_request(*request))
                    return false;
            }
        }

        return true;
    }

    // 在借池节点之前占用一个在途名额。若随后的提交失败，收包路径立即关闭
    // 整条连接；提交成功则由该请求唯一的一次 Worker 回包归还名额。
    bool reserve_worker_request() {
        if (pending_ >= max_inflight)
            return false;
        ++pending_;
        return true;
    }

    // 同一批内联命中共用一次 flush。积压到 256 KiB 提前写，首次放不下再
    // 尝试写一次，保留 4 MiB 的慢连接上限。内联响应不占 Worker 名额。
    bool enqueue_inline_response(Frame &response) {
        response.kind = Kind::response;
        if (output_.readable_bytes() >= 256 * 1024)
            output_.write(socket_);
        if (output_.enqueue(response, max_output))
            return true;
        output_.write(socket_);
        return output_.enqueue(response, max_output);
    }

    // 使用 ::writev() 将 output_ 队列中的“17B 帧头 + Payload”聚集写入内核 Socket 发送缓冲区。
    void flush_output() {
        output_.write(socket_);
    }

  private:
    friend class IoConnections; // 连接表在异步回包时核销名额、刷新监听或关闭连接。
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
class IoConnections {
  public:
    IoConnections(net::EventLoop &loop, std::move_only_function<void(int) noexcept> on_close)
        : loop_(loop), on_close_(std::move(on_close)) {
    }

    void register_conn(net::Socket socket, auto on_event) {
        const int fd = socket.native_handle();
        auto [it, inserted] = active_.try_emplace(fd, std::move(socket), next_id_++);

        if (!inserted)
            throw std::logic_error("connection fd already registered");

        Connection &conn = it->second;

        try {
            loop_.add_event(fd, conn.interests(),
                            [&conn, on_event = std::move(on_event)](net::EpollEvent event) {
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

    void close(int fd) noexcept {
        on_close_(fd); // 在 fd 有机会复用前，清理这条连接的 PTY。
        try {
            loop_.del_event(fd);
        } catch (...) {
            // 用户态回调已摘下；即使 DEL 失败，关闭最后一个 socket fd 也会解除内核监听。
        }
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

    // Worker 回包、PTY 与目录事件都在 socket 回调之外送达，要立即尝试写。
    // 只有 Worker 响应对应已占用的名额。广播保留原帧，让各订阅者共享同一份正文。
    bool send_async_frame(Connection &conn, Frame &frame, bool broadcast = false) {
        if (frame.kind == Kind::response) {
            if (conn.pending_ == 0) {
                close(conn.fd());
                return false;
            }
            --conn.pending_;
        }
        const bool had_output = conn.wants_write();
        try {
            if (broadcast)
                frame.share_payload();
            const bool queued =
                broadcast ? conn.output_.enqueue(std::as_const(frame), Connection::max_output)
                          : conn.output_.enqueue(frame, Connection::max_output);
            if (queued) {
                conn.flush_output();
                // 当前 ONESHOT 仍然有效。只有新增/消除写积压或排空最后一次
                // 回包才需要动作；socket 就绪回调则每次都要重新武装。
                if (conn.finished() || conn.wants_write() != had_output)
                    rearm_or_close(conn);
                return true;
            }
        } catch (...) {
            // 编码、writev 或重新武装失败都终止这条连接。
        }
        close(conn.fd());
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
    std::move_only_function<void(int) noexcept> on_close_;
    std::unordered_map<int, Connection> active_;
    std::uint64_t next_id_{1};
    std::optional<std::promise<void>> drain_complete_;
};

} // namespace rpc

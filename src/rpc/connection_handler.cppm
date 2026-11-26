export module rpc.handler;

export import :context;

import std;
import net.socket;
import net.epoll;
import net.event_loop;
import runtime.worker_pool;
import rpc.connection;
import rpc.task_classifier;
import ide.protocol;
import ide.server;
import ide.server_git;
import ide.server_terminal;
import service.dir_watcher;

namespace rpc::detail {

namespace {

// Worker 把节点投回原 I/O 线程。连接可能已关闭甚至 fd 已被重新分配；
// 无论能否送达，都要归还节点，让其中的缓存切片及时释放。
void deliver_worker_response(RpcTaskContext *ctx) noexcept {
    IoSlotContext &slot = ctx->slot;
    struct RecycleGuard {
        RpcTaskContext *ctx;
        ~RecycleGuard() {
            ctx->slot.recycle_task(ctx);
        }
    } recycle{ctx};

    auto *conn = slot.connections.find(ctx->conn_fd, ctx->conn_id);
    if (!conn)
        return;

    Frame &response = ctx->frame;
    const std::string &watched_dir = ctx->watched_dir;
    try {
        if (!watched_dir.empty() && response.status == Status::ok) {
            if (response.method == Method::workspace_list_dir)
                slot.watcher->watch(watched_dir, *conn);
            else if (response.method == Method::workspace_close_dir)
                slot.watcher->unwatch(watched_dir, ctx->conn_fd);
        }

        slot.connections.send_async_frame(*conn, response);
    } catch (...) {
        slot.connections.close(ctx->conn_fd);
    }
}

// 分类器已经就地处理了 ping。Worker 只路由剩余请求，服务端专用事件沿用
// unsupported 响应；KV 的锁在进入其他业务或将结果投回 I/O 线程前释放。
void handle_worker_method(RpcTaskContext &ctx) {
    Frame &request = ctx.frame;
    if (is_event_only_method(request.method)) {
        request.status = Status::unsupported;
        request.payload = "unsupported method";
        return;
    }

    switch (method_group(request.method)) {
    case MethodGroup::system:
        break;

    case MethodGroup::kv: {
        std::lock_guard lock(ctx.slot.kv.mutex);
        ctx.slot.kv.service.handle(request);
        return;
    }

    case MethodGroup::workspace:
        // I/O 分类时抓到的快照在这里直接挂载外置切片返回，避免重新查缓存或重复编码。
        if (ctx.snapshot.file) {
            server::encode_workspace_read_file(request, std::move(ctx.snapshot.file),
                                               ctx.snapshot.slice);
        } else if (auto dir = std::move(ctx.snapshot.directory)) {
            server::encode_workspace_list_dir(request, std::move(dir));
        } else {
            server::handle_workspace_request(request, ctx.watched_dir);
        }
        return;

    case MethodGroup::terminal:
        ctx.slot.terminal_mgr->handle_request(request, ctx.conn_fd);
        return;

    case MethodGroup::git:
        server::handle_git_request(request);
        return;
    }

    request.status = Status::unsupported;
    request.payload = "unsupported method";
}

// 同一个节点依次经过 I/O -> Worker -> 原 I/O。原地改写 kind，保留 request_id 和 method。
// 分类时拿到的大文件或目录快照在此直接消费，避免重新查缓存或磁盘；其余请求进入业务处理器。
void execute_worker_request(RpcTaskContext *ctx) noexcept {
    IoSlotContext &slot = ctx->slot;
    Frame &request = ctx->frame;
    request.kind = Kind::response;
    try {
        handle_worker_method(*ctx);
    } catch (const std::bad_alloc &) {
        request.clear_external_payload();
        request.status = Status::resource_exhausted;
        request.payload = "out of memory";
    } catch (...) {
        request.clear_external_payload();
        request.status = Status::internal;
        request.payload = "internal server error";
    }

    auto complete_lambda = [ctx]() noexcept {
        deliver_worker_response(ctx);
    };
    static_assert(sizeof(complete_lambda) <= 24 && alignof(decltype(complete_lambda)) <= 8 &&
                      std::is_nothrow_move_constructible_v<decltype(complete_lambda)>,
                  "EventLoop completion task must fit in std::move_only_function 24B SBO");

    slot.loop.post_task(std::move(complete_lambda));
}

// 一个可读事件内的所有 Worker 提交共用一次唤醒屏障。先零拷贝分流：
// 内联任务直接消费输入缓冲视图，入队成功后再登记订阅；慢路径仅在需要时拷贝请求正文。
bool dispatch_requests(Connection &conn, IoSlotContext &slot) {
    WorkspaceSnapshot snapshot;
    std::string_view dir_path;
    runtime::WorkerPool::SubmitBatchGuard wake_batch(slot.workers, slot.io_id);

    return conn.read_requests([&](const FrameView &request) {
        const auto route = inline_or_blocking(request, slot.inline_response,
                                              slot.inline_threshold, snapshot, dir_path);

        if (route == runtime::TaskClass::inline_task) {
            if (!conn.enqueue_inline_response(slot.inline_response))
                return false;

            if (request.method == Method::workspace_list_dir)
                slot.watcher->watch(dir_path, conn);

            return true;
        }

        if (!conn.reserve_worker_request()) {
            snapshot = {};
            return false;
        }

        const bool need_payload = !snapshot.file && !snapshot.directory &&
                                  !is_event_only_method(request.method);
        auto *ctx = slot.acquire_task(conn, request, need_payload,
                                      snapshot.directory ? dir_path : std::string_view{});
        if (snapshot.file) {
            ctx->snapshot.file = std::move(snapshot.file);
            ctx->snapshot.slice = snapshot.slice;
        } else if (snapshot.directory) {
            ctx->snapshot.directory = std::move(snapshot.directory);
        }

        if (slot.workers.try_submit(slot.io_id, route, [ctx] {
                execute_worker_request(ctx);
            }))
            return true;

        // 队列拒收时 Worker 没有取得节点，归还快照和池节点后让收包方关闭连接。
        ctx->snapshot = {};
        slot.recycle_task(ctx);
        return false;
    });
}

void handle_connection_event(Connection &conn, IoSlotContext &slot,
                             net::EpollEvent event) noexcept {
    const int fd = conn.fd();
    try {
        const bool read_ready = event.readable() || event.peer_closed() || event.hangup();

        if (event.error() ||
            (read_ready && conn.accepting_requests() && !dispatch_requests(conn, slot))) {
            slot.connections.close(fd);
            return;
        }

        if (conn.wants_write())
            conn.flush_output();

        slot.connections.rearm_or_close(conn);
    } catch (...) {
        slot.connections.close(fd);
    }
}

} // namespace

// Acceptor 只转移 socket 所有权。建连接表节点和注册 epoll 必须在选中的 I/O 线程连续完成。
export void post_accepted_socket(net::Socket socket, IoSlotContext &slot) {
    slot.loop.post_task([socket = std::move(socket), &slot]() mutable {
        slot.connections.register_conn(std::move(socket),
                                       [&slot](Connection &conn, net::EpollEvent event) {
                                           handle_connection_event(conn, slot, event);
                                       });
    });
}

} // namespace rpc::detail

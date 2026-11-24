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
void complete_response(RpcTaskContext *ctx) noexcept {
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

        const bool had_write = conn->wants_write();

        if (!conn->enqueue_response(response)) {
            slot.connections.close(ctx->conn_fd);
            return;
        }
        conn->flush_output(); // 成功入队至少有一个 17 字节帧头可发。
        // 回包不是 socket 就绪通知；读写兴趣没变时无需额外 epoll_ctl。
        if (conn->finished() || conn->wants_write() != had_write)
            slot.connections.rearm_or_close(*conn);
    } catch (...) {
        try {
            slot.connections.close(ctx->conn_fd);
        } catch (...) {
        }
    }
}

// 分类器已经就地处理了 ping。Worker 只路由剩余请求，服务端专用事件沿用
// unsupported 响应；KV 的锁在进入其他业务或将结果投回 I/O 线程前释放。
void dispatch_request(RpcTaskContext &ctx) {
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
        server::handle_workspace_request(request, ctx.watched_dir);
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
    // 移出节点，让快照在本线程结束编码时释放。即使编码抛异常，RAII 也会释放所有权。
    // 成功的非空文件切片由响应帧接管，生命周期延续到 writev 发完。
    request.kind = Kind::response;
    try {
        auto &snapshot = ctx->snapshot;
        if (snapshot.file) {
            const auto slice = std::exchange(snapshot.slice, {});
            server::encode_workspace_read_file(request, std::move(snapshot.file), slice);
        } else if (auto entries = std::move(snapshot.directory)) {
            server::encode_workspace_list_dir_into(ctx->watched_dir, *entries, request.payload);
        } else {
            dispatch_request(*ctx);
        }
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
        complete_response(ctx);
    };
    static_assert(sizeof(complete_lambda) <= 24 && alignof(decltype(complete_lambda)) <= 8 &&
                      std::is_nothrow_move_constructible_v<decltype(complete_lambda)>,
                  "EventLoop completion task must fit in std::move_only_function 24B SBO");

    if (!slot.loop.post_task(std::move(complete_lambda))) {
        delete ctx;
    }
}

// 一个可读事件内的所有 Worker 提交共用一次唤醒屏障。先分流，再分别处理两种结果：
// 内联响应入队成功才登记订阅；慢路径直接移动帧和缓存快照，闭包只携带一个池节点指针。
bool dispatch_requests(Connection &conn, IoSlotContext &slot) {
    // 快路径不会留下快照；提交慢请求时把快照取走。整批共用这些临时量，
    // 连续的内联命中无需逐帧构造、销毁空的 shared_ptr。
    WorkspaceSnapshot snapshot;
    std::string dir_path;
    runtime::WorkerPool::SubmitBatchGuard wake_batch(slot.workers, slot.io_id);
    return conn.read_requests([&](Frame &request) {
        const auto route =
            prepare_request_for_dispatch(request, slot.inline_threshold, snapshot, dir_path);
        if (route == runtime::TaskClass::inline_task) {
            if (!conn.enqueue_direct_response(request))
                return false;
            if (request.method == Method::workspace_list_dir)
                slot.watcher->watch(dir_path, conn);
            return true;
        }

        return conn.try_submit_worker_request([&] {
            auto *ctx = slot.acquire_task(conn, std::move(request));
            if (snapshot.directory)
                ctx->watched_dir.swap(dir_path);
            ctx->snapshot = std::exchange(snapshot, {});
            if (slot.workers.try_submit(slot.io_id, route, [ctx] {
                    execute_worker_request(ctx);
                }))
                return true;

            // 队列拒收时 Worker 没有取得节点，必须在本线程释放快照并归还池。
            ctx->snapshot = {};
            slot.recycle_task(ctx);
            return false;
        });
    });
}

void handle_conn_event(Connection &conn, IoSlotContext &slot, net::EpollEvent event) noexcept {
    const int fd = conn.fd();
    auto close_conn = [&]() noexcept {
        slot.terminal_mgr->close_for_connection(fd);
        try {
            slot.connections.close(fd);
        } catch (...) {
        }
    };

    if (event.error()) {
        close_conn();
        return;
    }

    try {
        if (conn.accepting_requests() && read_ready && !dispatch_requests(conn, slot)) {
            close_conn();
            return;
        }

        if (conn.wants_write())
            conn.flush_output();
        if (conn.finished())
            slot.terminal_mgr->close_for_connection(fd);
        slot.connections.rearm_or_close(conn);
    } catch (...) {
        close_conn();
    }
}

} // namespace

export bool post_connection(net::Socket socket, IoSlotContext &slot) {
    return slot.loop.post_task([socket = std::move(socket), &slot]() mutable {
        slot.connections.add(std::move(socket), [&slot](Connection &conn, net::EpollEvent event) {
            handle_conn_event(conn, slot, event);
        });
    });
}

} // namespace rpc::detail

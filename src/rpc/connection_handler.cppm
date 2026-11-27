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

bool deliver_worker_response(IoSlotContext &slot, Connection &conn, RpcTaskContext &ctx) noexcept {
    Frame &response = ctx.frame;
    try {
        if (response.status == Status::ok) {
            if (response.method == Method::workspace_list_dir &&
                response.external_payload.size() >= 2) {
                const auto len = read_be<std::uint16_t>(response.external_payload.data());
                if (2 + len <= response.external_payload.size())
                    slot.watcher.watch(response.external_payload.substr(2, len), conn);
            } else if (response.method == Method::workspace_close_dir &&
                       response.payload.size() >= 2) {
                slot.watcher.unwatch(std::string_view(response.payload).substr(2), ctx.conn_fd);
            }
        }
        return slot.connections.enqueue_worker_response(conn, response);
    } catch (...) {
        slot.connections.close(ctx.conn_fd);
        return false;
    }
}

// 分类器已经就地处理了 ping 与全部缓存命中。Worker 只路由剩余慢请求与写任务，
// KV 的锁在进入其他业务或将结果投回 I/O 线程前释放。
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
        server::handle_workspace_request(request);
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

// 同一个节点依次经过 I/O -> Worker -> 原 I/O。原地改写 kind，保留 request_id 和 method，
// 通过无锁侵入式完成栈回投，消除多 Worker 对 EventLoop 锁的争用。
void execute_worker_request(RpcTaskContext *ctx) noexcept {
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
    ctx->slot.complete_from_worker(ctx);
}

// 一个可读事件内的所有 Worker 提交共用一次唤醒屏障。先零拷贝分流：
// 内联任务直接消费输入缓冲视图，入队成功后再登记订阅；慢路径仅在需要时拷贝请求正文。
bool dispatch_requests(Connection &conn, IoSlotContext &slot) {
    std::string_view dir_path;
    runtime::WorkerPool::SubmitBatchGuard wake_batch(slot.workers, slot.io_id);

    return conn.read_requests([&](const FrameView &request) {
        const auto route =
            inline_or_blocking(request, slot.inline_response, slot.hot_cache, dir_path);

        if (route == runtime::TaskClass::inline_task) {
            if (!conn.enqueue_inline_response(slot.inline_response)) {
                slot.inline_response.clear_external_payload();
                return false;
            }

            if (request.method == Method::workspace_list_dir)
                slot.watcher.watch(dir_path, conn);

            return true;
        }

        if (!conn.reserve_worker_request())
            return false;

        auto *ctx = slot.acquire_task(conn, request, !is_event_only_method(request.method));
        if (slot.workers.try_submit(slot.io_id, route, [ctx] {
                execute_worker_request(ctx);
            }))
            return true;

        // 队列拒收时 Worker 没有取得节点，归还池节点后让收包方关闭连接。
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
        } else {
            if (conn.wants_write())
                conn.flush_output();
            slot.connections.rearm_or_close(conn);
        }
    } catch (...) {
        slot.connections.close(fd);
    }
    slot.hot_cache.end_batch();
}

} // namespace

// 单条 XCHG 摘下整批已完成节点，反转还原先进先出顺序并按同连接连续批次合并 writev 发送。
export void drain_worker_responses(IoSlotContext &slot) noexcept {
    RpcTaskContext *rev = slot.completed_head_.exchange(nullptr, std::memory_order_acquire);
    RpcTaskContext *head = nullptr;
    while (rev) {
        auto *next = std::exchange(rev->next_complete, head);
        head = std::exchange(rev, next);
    }

    for (auto *ctx = head; ctx;) {
        const int fd = ctx->conn_fd;
        const std::uint64_t id = ctx->conn_id;
        auto *conn = slot.connections.find(fd, id);
        const bool had_output = conn && conn->wants_write();
        do {
            auto *next = ctx->next_complete;
            if (conn && !deliver_worker_response(slot, *conn, *ctx))
                conn = nullptr;
            slot.recycle_task(ctx);
            ctx = next;
        } while (ctx && ctx->conn_fd == fd && ctx->conn_id == id);
        if (conn)
            slot.connections.flush_async(*conn, had_output);
    }
}

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

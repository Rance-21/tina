module;
#include <errno.h>

export module rpc.handler;

export import :context;

import std;
import net.socket;
import net.epoll;
import net.event_loop;
import runtime.worker_pool;
import rpc.connection;
import ide.protocol;
import ide.server;
import ide.server_git;
import ide.server_terminal;
import service.file;
import service.workspace_cache;
import service.dir_watcher;

namespace rpc::detail {

namespace {

bool deliver_worker_response(IoSlotContext &slot, Connection &conn, RpcTaskContext &ctx) noexcept {
    Frame &response = ctx.frame;
    try {
        if (response.status == Status::ok && response.method == Method::workspace_list_dir &&
            response.external_payload.size() >= 2) {
            const auto len = read_be<std::uint16_t>(response.external_payload.data());
            if (2 + len <= response.external_payload.size())
                slot.watcher.watch(response.external_payload.substr(2, len), conn);
        }
        return slot.connections.enqueue_worker_response(conn, response);
    } catch (...) {
        slot.connections.close(ctx.conn_fd);
        return false;
    }
}

// 内联快路径已拦截 ping、缓存命中、close_dir、terminal_input 与各类快速错误；
// 同构 Worker 池仅执行磁盘冷读/写、目录扫描、PTY 启停/Tab、Git 子进程与共享 KV。
void handle_worker_method(RpcTaskContext &ctx) {
    Frame &request = ctx.frame;
    switch (method_group(request.method)) {
    case MethodGroup::kv: {
        std::lock_guard lock(ctx.slot.kv.mutex);
        ctx.slot.kv.service.handle(request);
        return;
    }

    case MethodGroup::workspace:
        server::handle_workspace_request(request);
        return;

    case MethodGroup::terminal:
        ctx.slot.terminal_mgr.handle_request(request, ctx.conn_fd);
        return;

    case MethodGroup::git:
        server::handle_git_request(request);
        return;

    default:
        request.status = Status::unsupported;
        request.payload = "unsupported method";
        return;
    }
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

// 尝试在所属 I/O 线程原地构造响应（返回 true 表示 slot.inline_response 已就绪；false 表示需投递 Worker）。
bool try_prepare_inline(const FrameView &request, IoSlotContext &slot, int conn_fd,
                        std::string_view &dir_path) {
    Frame &response = slot.inline_response;
    auto init_response = [&](Status status = Status::ok) noexcept {
        response.kind = Kind::response;
        response.request_id = request.request_id;
        response.method = request.method;
        response.status = status;
        response.clear_external_payload();
    };
    auto fail = [&](Status status, std::string_view msg) {
        init_response(status);
        response.payload.assign(msg);
        return true;
    };

    if (is_event_only_method(request.method))
        return fail(Status::unsupported, "unsupported method");

    switch (request.method) {
    case Method::system_ping:
        init_response();
        handle_ping(request.payload, response);
        return true;

    case Method::workspace_read_file: {
        auto req = server::decode_workspace_read_file_view(request.payload);
        if (!req)
            return fail(Status::invalid_argument, "invalid workspace_read_file payload");
        if (service::file::workspace_root_fd() >= 0) {
            if (auto file = slot.hot_cache.get_file(service::global_workspace_cache(), req->path)) {
                const auto slice = server::slice_file_content(*file, req->offset, req->max_bytes);
                response.kind = Kind::response;
                response.request_id = request.request_id;
                response.method = request.method;
                response.status = Status::ok;
                server::encode_workspace_read_file(response, std::move(file), slice);
                return true;
            }
        }
        if (!service::file::is_valid_workspace_rel_path(req->path))
            return fail(Status::invalid_argument, "invalid workspace file path");
        if (service::file::workspace_root_fd() < 0) {
            init_response();
            service::file::set_errno_error(response, ENOENT, "failed to open file for reading");
            return true;
        }
        return false;
    }

    case Method::workspace_list_dir: {
        auto path = server::decode_workspace_list_dir_view(request.payload);
        if (!path)
            return fail(Status::invalid_argument, "invalid workspace_list_dir payload");
        if (service::file::workspace_root_fd() >= 0) {
            if (auto cached = slot.hot_cache.get_dir(service::global_workspace_cache(), *path)) {
                dir_path = *path;
                response.kind = Kind::response;
                response.request_id = request.request_id;
                response.method = request.method;
                response.status = Status::ok;
                server::encode_workspace_list_dir(response, std::move(cached));
                return true;
            }
        }
        if (!service::file::is_valid_workspace_rel_path(*path))
            return fail(Status::invalid_argument, "invalid workspace directory path");
        if (service::file::workspace_root_fd() < 0) {
            init_response();
            service::file::set_errno_error(response, ENOENT, "failed to open target directory");
            return true;
        }
        return false;
    }

    case Method::workspace_close_dir: {
        auto path = server::decode_workspace_close_dir_view(request.payload);
        if (!path)
            return fail(Status::invalid_argument, "invalid workspace_close_dir payload");
        if (!service::file::is_valid_workspace_rel_path(*path) ||
            service::file::workspace_root_fd() < 0)
            return fail(Status::invalid_argument, "invalid workspace directory path");
        dir_path = *path;
        init_response();
        server::encode_workspace_close_dir_into(*path, response.payload);
        return true;
    }

    case Method::terminal_input:
        init_response();
        slot.terminal_mgr.handle_terminal_input(request.payload, response, conn_fd);
        return true;

    case Method::kv_set:
    case Method::kv_get:
    case Method::kv_del:
    case Method::workspace_list_projects:
    case Method::workspace_write_file:
    case Method::terminal_on_tab:
    case Method::terminal_state:
    case Method::git_status:
    case Method::git_diff:
    case Method::git_log:
        return false;

    default:
        return fail(Status::unsupported, "unsupported method");
    }
}

// 一个可读事件内的所有 Worker 提交共用一次唤醒屏障。
bool dispatch_requests(Connection &conn, IoSlotContext &slot) {
    runtime::WorkerPool::SubmitBatchGuard wake_batch(slot.workers, slot.io_id);

    return conn.read_requests([&](const FrameView &request) {
        std::string_view dir_path;
        if (try_prepare_inline(request, slot, conn.fd(), dir_path)) {
            if (!conn.enqueue_inline_response(slot.inline_response)) {
                slot.inline_response.clear_external_payload();
                return false;
            }
            if (!dir_path.empty()) {
                if (request.method == Method::workspace_list_dir)
                    slot.watcher.watch(dir_path, conn);
                else
                    slot.watcher.unwatch(dir_path, conn.fd());
            }
            return true;
        }

        if (!conn.reserve_worker_request())
            return false;

        auto *ctx = slot.acquire_task(conn, request);
        if (slot.workers.try_submit(slot.io_id, [ctx] {
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
        slot.inline_response.clear_external_payload();
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
        do {
            auto *next = ctx->next_complete;
            if (conn && !deliver_worker_response(slot, *conn, *ctx))
                conn = nullptr;
            slot.recycle_task(ctx);
            ctx = next;
        } while (ctx && ctx->conn_fd == fd && ctx->conn_id == id);
        if (conn)
            slot.connections.flush_async(*conn);
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

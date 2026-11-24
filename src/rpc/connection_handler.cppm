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

// ============================================================================
// 【步骤 4 / 终点】I/O 线程回写响应并回收任务节点（Worker -> EventLoop::post_task 回调）
// ============================================================================
// 【作用】Worker 线程算完响应后，通过 `loop.post_task([ctx])` 切回连接所属 I/O 线程完成响应入队、
//         非阻塞写出以及池节点 `ctx` 的无锁归还。
// 【原理】
//   1. RAII 回收兜底（RecycleGuard）：无论连接已断开、正常发包还是中途抛异常，退出函数时必然将 ctx
//      归还到当前 I/O 线程私有的 slot.task_pool_，绝不泄漏；
//   2. 存活校验：用 ctx->weak_conn.lock() 检查客户端连接在 Worker 执行期间是否已经断开；
//   3. 状态同步：
//      -
//      若是写文件响应（workspace_write_file），核销该连接上的在途写登记，解除后续同路径内联读拦截；
//      - 若是成功的目录列举/关闭（list_dir / close_dir），同步在本线程 DirWatcher 上注册或移除
//      inotify 监听；
//   4. 零拷贝入队与聚集发送：调用 conn->enqueue_response(response)（内部 std::move 接管 payload
//   缓冲，
//      不拷贝正文）并立刻尝试一次非阻塞 writev()；
//   5. 按需更新
//   epoll：仅当连接已满足关闭条件（conn->finished()）或待写状态（wants_write()）发生翻转时，
//      才调用 refresh_connection()，省去稳态下的冗余 epoll_ctl 系统调用。
void complete_response(RpcTaskContext *ctx) noexcept {
    IoSlotContext &slot = *ctx->slot;
    struct RecycleGuard {
        IoSlotContext &slot;
        RpcTaskContext *ctx;
        ~RecycleGuard() noexcept {
            slot.release_task_ctx(ctx);
        }
    } recycle_guard{slot, ctx};

    auto conn = ctx->weak_conn.lock();
    if (!conn)
        return;

    Frame &response = ctx->frame;
    const std::string &watched_dir = ctx->watched_dir;

    try {
        if (response.method == Method::workspace_write_file)
            conn->finish_inflight_write(response.request_id);

        if (!watched_dir.empty() && response.status == Status::ok && slot.watcher != nullptr) {
            if (response.method == Method::workspace_list_dir)
                slot.watcher->watch(watched_dir, conn);
            else if (response.method == Method::workspace_close_dir)
                slot.watcher->unwatch(watched_dir, conn->fd());
        }

        const bool had_write = conn->wants_write();

        if (!conn->enqueue_response(response)) {
            close_connection(*slot.loop, slot.connections, *slot.counter, conn->fd());
            return;
        }
        if (conn->wants_write())
            conn->write();
        if (conn->finished() || conn->wants_write() != had_write)
            refresh_connection(slot, conn);
    } catch (...) {
        close_connection(*slot.loop, slot.connections, *slot.counter, conn->fd());
    }
}

// ============================================================================
// 【步骤 3b】Worker 线程业务路由表（按 Method 高 8 位模块分组跳转）
// ============================================================================
// 【作用】在 Worker 线程内按 Method 的高 8 位分组码（MethodGroup）把请求分发到对应的业务处理器。
// 【原理】先拦截仅允许服务端主动推送的 event_only 方法，再通过位运算提取分组号做紧凑 switch 跳转。
void dispatch_request(Frame &request, KvState &kv, std::string &watched_dir,
                      server::TerminalManager *terminal_mgr, int conn_fd) {
    if (is_event_only_method(request.method)) {
        request.status = Status::unsupported;
        request.payload = "unsupported method";
        return;
    }

    switch (method_group(request.method)) {
    case MethodGroup::system:
        if (request.method == Method::system_ping) {
            handle_ping(request);
            return;
        }
        break;

    case MethodGroup::kv: {
        std::lock_guard lock(kv.mutex);
        kv.service.handle(request);
        return;
    }

    case MethodGroup::workspace:
        server::handle_workspace_request(request, &watched_dir);
        return;

    case MethodGroup::terminal:
        if (terminal_mgr != nullptr) {
            terminal_mgr->handle_request(request, conn_fd);
            return;
        }
        break;

    case MethodGroup::git:
        server::handle_git_request(request);
        return;
    }

    request.status = Status::unsupported;
    request.payload = "unsupported method";
}

// ============================================================================
// 【步骤 3a】Worker 线程执行单条慢路径 RPC 任务（WorkerPool::PoolTask 入口）
// ============================================================================
// 【作用】在 Normal 或 Blocking Worker 线程中处理 ctx 承载的请求，并将结果投递回原 I/O 线程。
// 【原理】
//   1. 原地复用帧：将 ctx->frame.kind 从 request 原地改为 response（保留 request_id 与 method）；
//   2. 优先消费预取快照：若 I/O 线程在分类阶段已经抓到了 >32KiB 的文件或大目录只读缓存快照
//      （prepared.cached_file / cached_dir），直接通过 std::move
//      消费快照组装响应并当场释放快照引用， 既不二次查缓存锁也不碰磁盘；否则进入 dispatch_request()
//      执行常规/阻塞业务；
//   3. 单指针回投（命中 24B SBO）：以 `[ctx]() noexcept`（8 字节 <= 24 字节）投递给
//   slot.loop->post_task()，
//      全程零闭包堆分配。仅当停机阶段 EventLoop 已关闭拒绝新任务时，才由 Worker 直接 delete ctx
//      兜底。
void handle_request(RpcTaskContext *ctx) noexcept {
    IoSlotContext &slot = *ctx->slot;
    Frame &request = ctx->frame;
    PreparedDispatch &prepared = ctx->prepared;

    request.kind = Kind::response;
    ctx->watched_dir.clear();

    try {
        if (prepared.cached_file) {
            server::finish_cached_read_file(request, std::move(prepared.cached_file),
                                            prepared.file_slice_offset, prepared.file_slice_size);
        } else if (prepared.cached_dir) {
            ctx->watched_dir = std::move(prepared.cached_dir_path);
            auto cached_dir = std::move(prepared.cached_dir);
            server::finish_cached_list_dir(request, ctx->watched_dir, *cached_dir,
                                           &ctx->watched_dir);
        } else {
            dispatch_request(request, *slot.kv, ctx->watched_dir, slot.terminal_mgr, ctx->conn_fd);
        }
    } catch (const std::bad_alloc &) {
        prepared.clear_snapshots();
        request.clear_external_payload();
        request.status = Status::resource_exhausted;
        request.payload = "out of memory";
    } catch (...) {
        prepared.clear_snapshots();
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

    if (!slot.loop->post_task(std::move(complete_lambda))) {
        delete ctx;
    }
}

// ============================================================================
// 【步骤 2】I/O 线程将非内联请求装入池节点并投递至 WorkerPool
// ============================================================================
// 【作用】在所属 I/O 线程从本线程对象池借出 RpcTaskContext，并以 8B 单指针闭包提交到 WorkerPool
// V2。 【原理】
//   1. 无锁借节点：从本线程私有的 slot.acquire_task_ctx() O(1) 弹出热节点 ctx，将 request 与
//   prepared 原地移入；
//   2. 同连接读写一致性登记：若是写文件请求（workspace_write_file），在提交给 Worker 之前先从
//      ctx->frame.payload 零拷贝读取目标路径并登记到 conn->track_inflight_write()，防止：
//      (a) 同连接后续的 read_file 在内联快路径上“超车”读到尚未写入完成的旧缓存；
//      (b) 提交后 Worker 线程并发改写 payload 与 I/O 线程读取路径之间的数据竞态；
//   3. 单指针提交（命中 24B SBO）：构造 `[ctx]() noexcept`（8 字节 <= 24 字节）提交进本 I/O
//   线程专属的
//      SPSC lane；若队列满或正在停机导致提交失败，立刻回滚在途写登记并将 ctx 归还本线程池。
bool submit_to_worker(Frame &&request, PreparedDispatch &prepared,
                      const std::shared_ptr<Connection> &conn, int conn_fd, IoSlotContext &slot) {
    const std::uint64_t req_id = request.request_id;
    const runtime::TaskClass task_class = prepared.task_class;

    RpcTaskContext *ctx =
        slot.acquire_task_ctx(std::move(request), std::move(prepared), conn, conn_fd);

    std::optional<std::string_view> write_path;
    if (ctx->frame.method == Method::workspace_write_file) {
        write_path = peek_write_file_path(ctx->frame.payload);
        if (write_path)
            conn->track_inflight_write(req_id, *write_path);
    }

    auto worker_lambda = [ctx]() noexcept {
        handle_request(ctx);
    };

    if (!slot.workers->try_submit(slot.io_view.io_id, task_class, std::move(worker_lambda))) {
        if (write_path)
            conn->finish_inflight_write(req_id);
        ctx->prepared.clear_snapshots();
        slot.release_task_ctx(ctx);
        return false;
    }

    return true;
}

// ============================================================================
// 【步骤 1】I/O 线程单条客户端连接的 epoll 事件主回调
// ============================================================================
// 【作用】处理客户端 Socket 的可读（收包拆帧 + 快慢路径分流）、可写（writev 发送）与异常关闭事件。
// 【原理】
//   1. 错误快速清理：若触发 EPOLLERR，立即清理该连接关联的 PTY 终端会话并关闭连接；
//   2. 批量拆包与三级分流（在 SubmitBatchGuard 合并唤醒作用域内执行 conn->read()）：
//      - 回调 1（try_inline_or_classify）：判断能否在当前 I/O 线程就地内联响应（如 ping、<=32KiB 的
//        热文件/目录缓存命中）。内联命中时直接编码进发送队列，不碰对象池、不进 WorkerPool；
//      - 回调 2（on_inline_enqueued）：若目录缓存内联命中并成功入队，顺手在本线程 DirWatcher
//      注册监听；
//      - 回调 3（submit_request）：未走内联的请求按 Normal / Blocking 分类装入池节点投递给
//      WorkerPool。 退出 SubmitBatchGuard 作用域时，本轮拆包触达的所有沉睡 Worker 只做 1
//      次内存栅栏并统一合并唤醒；
//   3. 聚集写回与重装监听：若发送队列有待发数据（含刚产生的内联响应），立即调 conn->write() 用
//   ::writev()
//      写出，最后调 refresh_connection() 决定销毁连接还是重装 EPOLLONESHOT 监听掩码。
void handle_connection_event(const std::shared_ptr<Connection> &conn, IoSlotContext &slot,
                             net::EpollEvent event) noexcept {
    const int conn_fd = conn->fd();
    auto close_conn = [&]() noexcept {
        if (slot.terminal_mgr != nullptr)
            slot.terminal_mgr->close_for_connection(conn_fd);
        try {
            close_connection(*slot.loop, slot.connections, *slot.counter, conn_fd);
        } catch (...) {
        }
    };

    if (event.error()) {
        close_conn();
        return;
    }

    try {
        if (!conn->peer_eof() && !conn->draining() &&
            (event.readable() || event.peer_closed() || event.hangup())) {
            PreparedDispatch prepared;
            std::string inline_watched_dir;

            bool ok = false;
            {
                runtime::WorkerPool::SubmitBatchGuard wake_batch(*slot.workers, slot.io_view.io_id);
                ok = conn->read(
                    [&](Frame &request) {
                        return try_inline_or_classify(request, *conn, slot.io_view.inline_threshold,
                                                      prepared, inline_watched_dir);
                    },
                    [&](const Frame &response) {
                        if (!inline_watched_dir.empty() && response.status == Status::ok &&
                            slot.watcher != nullptr) {
                            slot.watcher->watch(inline_watched_dir, conn);
                        }
                        inline_watched_dir.clear();
                    },
                    [&](Frame request) {
                        return submit_to_worker(std::move(request), prepared, conn, conn_fd, slot);
                    });
            }

            if (!ok) {
                close_conn();
                return;
            }
        }

        if (conn->wants_write())
            conn->write();
        if (conn->finished() && slot.terminal_mgr != nullptr)
            slot.terminal_mgr->close_for_connection(conn_fd);
        refresh_connection(slot, conn);
    } catch (...) {
        close_conn();
    }
}

// ============================================================================
// 【步骤 0b】目标 I/O 线程接管新连接并挂载到 epoll
// ============================================================================
// 【作用】在所属 I/O 线程内构造 Connection 放入 slot.connections，并向本线程 epoll 注册事件回调。
// 【原理】epoll 回调仅捕获 `[conn, &slot]`（16B + 8B = 24B），刚好卡在 EventLoop::Handler 的 24B
// SBO 上限内，避免 std::move_only_function 内部二次堆分配。
void register_connection(net::Socket socket, IoSlotContext &slot) {
    auto conn = std::make_shared<Connection>(std::move(socket));
    const int fd = conn->fd();

    slot.connections.emplace(fd, conn);
    try {
        auto handler = [conn, &slot](net::EpollEvent event) noexcept {
            handle_connection_event(conn, slot, event);
        };
        slot.loop->add_event(fd, connection_interests(*conn), std::move(handler));
    } catch (...) {
        slot.connections.erase(fd);
        throw;
    }

    slot.counter->opened();
}

} // namespace

// ============================================================================
// 【步骤 0a / 起点】Acceptor 主线程将新接收的客户端 Socket 移交给目标 I/O 线程槽位
// ============================================================================
// 【作用】由 Acceptor 线程按轮询选定一个 IoSlotContext 后调用，把新连接安全投递到目标 I/O
// 线程注册。 【原理】不跨线程加锁修改目标 EventLoop 的连接表，而是通过 slot.loop->post_task
// 投递异步注册任务；
//         闭包仅捕获 `[socket, &slot]`（4B + 4B pad + 8B = 16 Bytes <= 24 Bytes），直接命中
//         EventLoop::Task 的 24B SBO 本地存储，新连接接入同样零闭包堆分配、零 shared_ptr 原子计数。
export bool post_connection(net::Socket socket, IoSlotContext &slot) {
    auto register_lambda = [socket = std::move(socket), &slot]() mutable noexcept {
        register_connection(std::move(socket), slot);
    };
    return slot.loop->post_task(std::move(register_lambda));
}

} // namespace rpc::detail

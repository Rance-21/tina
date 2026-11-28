export module rpc.handler:context;

import std;
import net.event_loop;
import runtime.worker_pool;
import rpc.connection;
import ide.protocol;
import ide.server_terminal;
import service.dir_watcher;
import service.kv;
import service.workspace_cache;

namespace rpc::detail {

// 各 Worker 共享一个 KV 存储；锁只覆盖一次 KV 操作。
export struct KvState {
    std::mutex mutex;
    service::KvService service;
};

export struct IoSlotContext;

// 一次慢请求在同一 I/O 线程借出和归还。提交闭包捕获单指针放进 24B SBO，Worker 原地生成
// 响应后经侵入式无锁完成栈直接串回原 I/O 线程，全程零堆分配、两缓存行封顶。
export struct alignas(64) RpcTaskContext {
    IoSlotContext &slot;
    RpcTaskContext *next_complete{nullptr};
    int conn_fd{-1};
    std::uint64_t conn_id{0};
    Frame frame{};
};

static_assert(sizeof(RpcTaskContext) == 2 * 64, "RPC task must fit in two cache lines");

export struct alignas(64) IoSlotContext {
    // 启动时装配，之后只读。
    net::EventLoop &loop;
    runtime::WorkerPool &workers;
    KvState &kv;
    const std::size_t io_id;

    // 独占缓存行承载跨簇无锁回包栈顶，与只读区及 I/O 线程私有状态物理隔离。
    alignas(64) std::atomic<RpcTaskContext *> completed_head_{nullptr};

    // 从新缓存行开始，避免 I/O 线程修改连接表、热表和任务池时使 Worker 缓存行失效。
    // terminal_mgr 先于 connections 构造、后于其析构，确保断连清理回调始终有效。
    alignas(64) server::TerminalManager terminal_mgr;
    IoConnections connections;
    Frame inline_response{};
    service::WorkspaceHotCache hot_cache{};
    service::DirWatcher watcher;

    IoSlotContext(net::EventLoop &loop, runtime::WorkerPool &workers, KvState &kv,
                  std::size_t io_id)
        : loop(loop), workers(workers), kv(kv), io_id(io_id),
          terminal_mgr(loop,
                       [this](int fd, Frame &&event) {
                           // PTY 回调由同一 EventLoop 执行；停读之后只发已接受请求的响应。
                           auto *conn = connections.find(fd);
                           if (conn && conn->accepting_requests())
                               connections.send_async_frame(*conn, event);
                       }),
          connections(loop, [this](int fd) noexcept { terminal_mgr.close_for_connection(fd); }),
          watcher(loop, connections, workers, io_id) {
        task_pool_.reserve(max_pooled_tasks);
        for (std::size_t i = 0; i < 64; ++i)
            task_pool_.push_back(std::make_unique<RpcTaskContext>(*this));
    }

    // 回调和已借出的节点都引用本槽位，构造后地址必须固定。
    IoSlotContext(const IoSlotContext &) = delete;
    IoSlotContext &operator=(const IoSlotContext &) = delete;

    // Worker 线程无锁压入已完成节点；仅在栈由空变非空时通知一次 eventfd。
    void complete_from_worker(RpcTaskContext *ctx) noexcept {
        auto *old = completed_head_.load(std::memory_order_relaxed);
        do {
            ctx->next_complete = old;
        } while (!completed_head_.compare_exchange_weak(
            old, ctx, std::memory_order_release, std::memory_order_relaxed));
        if (!old)
            loop.wakeup();
    }

    // 从本线程空闲栈借节点。只有所有节点都在途时才分配，借出后由 Worker 独占，直至回包投回本线程。
    RpcTaskContext *acquire_task(const Connection &conn, const FrameView &request) {
        std::unique_ptr<RpcTaskContext> guard;
        if (task_pool_.empty()) {
            guard = std::make_unique<RpcTaskContext>(*this);
        } else {
            guard = std::move(task_pool_.back());
            task_pool_.pop_back();
        }
        guard->conn_fd = conn.fd();
        guard->conn_id = conn.id();
        guard->frame.kind = Kind::request;
        guard->frame.request_id = request.request_id;
        guard->frame.method = request.method;
        guard->frame.status = Status::ok;
        guard->frame.payload.assign(request.payload);
        return guard.release();
    }

    void recycle_task(RpcTaskContext *ctx) noexcept {
        if (task_pool_.size() == max_pooled_tasks) {
            delete ctx;
            return;
        }
        // 已发送的 payload 通常已经移入发送队列。连接提前关闭时，节点可能还持有
        // 大响应或只读缓存切片。归还前释放外置所有权，仅保留小字符串的容量。
        ctx->frame.clear_external_payload();
        if (ctx->frame.payload.capacity() > 128 * 1024)
            std::string{}.swap(ctx->frame.payload);
        else
            ctx->frame.payload.clear();
        task_pool_.emplace_back(ctx);
    }

  private:
    static constexpr std::size_t max_pooled_tasks = 1024;
    std::vector<std::unique_ptr<RpcTaskContext>> task_pool_;
};

} // namespace rpc::detail

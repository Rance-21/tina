export module rpc.handler:context;

import std;
import net.event_loop;
import runtime.worker_pool;
import rpc.connection;
import rpc.task_classifier;
import ide.protocol;
import ide.server_terminal;
import service.dir_watcher;
import service.kv;

namespace rpc::detail {

// 各 Worker 共享一个 KV 存储；锁只覆盖一次 KV 操作。
export struct KvState {
    std::mutex mutex;
    service::KvService service;
};

export struct IoSlotContext;

// 一次慢请求在同一 I/O 线程借出和归还。两个跨线程闭包各只捕获一个任务指针，
// 帧和缓存快照都留在节点里，Worker 原地生成响应。这样闭包只有一个指针大小，能放进
// 当前标准库 move_only_function 的 24 字节小对象缓冲；若按值捕获 Frame，则需另行堆分配。
// 提交队列和 EventLoop 的回投队列负责跨线程交接，两个线程不会同时改写同一个节点。
export struct alignas(64) RpcTaskContext {
    // 首条缓存行含归属线程、连接标识和帧头。fd 可能被内核复用，回写必须核对 conn_id。
    IoSlotContext &slot;
    int conn_fd{-1};
    std::uint64_t conn_id{0};
    Frame frame{};
    std::string watched_dir{};

    // 大缓存命中时分类器已取得快照，Worker 直接消费。正常回包和回收无需访问节点尾部。
    PreparedDispatch prepared{};
};

static_assert(sizeof(RpcTaskContext) == 4 * 64, "RPC task must fit in four cache lines");

export struct alignas(64) IoSlotContext {
    // 启动时装配，之后只读。Worker 借用这些组件的生命周期到 shutdown / join 为止。
    net::EventLoop &loop;
    runtime::WorkerPool &workers;
    KvState &kv;
    const std::size_t io_id;
    const std::size_t inline_threshold;
    // 组件的异步回调通过 weak_ptr 判活；正常读写这里的 shared_ptr 不产生计数拷贝。
    const std::shared_ptr<server::TerminalManager> terminal_mgr;

    // 从新缓存行开始，避免 I/O 线程修改连接表和任务池时使 Worker 的只读缓存行失效。
    alignas(64) LoopConnections connections;
    const std::shared_ptr<service::DirWatcher> watcher;

    IoSlotContext(net::EventLoop &loop, runtime::WorkerPool &workers, KvState &kv,
                  std::size_t io_id, std::size_t inline_threshold)
        : loop(loop), workers(workers), kv(kv), io_id(io_id), inline_threshold(inline_threshold),
          terminal_mgr(std::make_shared<server::TerminalManager>(
              loop, [this](int fd, Frame &&event) {
                  // PTY 回调由同一 EventLoop 执行；停读之后只发已接受请求的响应。
                  auto *conn = connections.find(fd);
                  if (conn && conn->accepting_requests())
                      connections.send_event(*conn, event);
              })),
          connections(loop),
          watcher(std::make_shared<service::DirWatcher>(loop, connections, workers, io_id)) {
        // 空闲栈预留到上限，保证归还不分配；启动时先准备 64 个节点供首批请求使用。
        task_pool_.reserve(max_pooled_tasks);
        for (std::size_t i = 0; i < 64; ++i)
            task_pool_.push_back(std::make_unique<RpcTaskContext>(*this));
    }

    // 回调和已借出的节点都引用本槽位，构造后地址必须固定。
    IoSlotContext(const IoSlotContext &) = delete;
    IoSlotContext &operator=(const IoSlotContext &) = delete;

    // 从本线程空闲栈借节点。只有所有节点都在途时才分配，借出后由 Worker 独占，
    // 直至回包任务投回本线程。slot 在构造时绑定，跨多次请求都指向同一线程。
    RpcTaskContext *acquire_task(const Connection &conn, Frame &&request,
                                 PreparedDispatch &&prepared) {
        RpcTaskContext *ctx;
        if (task_pool_.empty()) {
            ctx = new RpcTaskContext{*this};
        } else {
            ctx = task_pool_.back().release();
            task_pool_.pop_back();
        }
        ctx->conn_fd = conn.fd();
        ctx->conn_id = conn.id();
        ctx->frame = std::move(request);
        ctx->prepared = std::move(prepared);
        return ctx;
    }

    void recycle_task(RpcTaskContext *ctx) noexcept {
        if (task_pool_.size() == max_pooled_tasks) {
            delete ctx;
            return;
        }
        // 已发送的 payload 通常已经移入发送队列。连接提前关闭时，节点可能还持有
        // 大响应或只读缓存切片。归还前释放外置所有权，仅保留小字符串的容量。
        ctx->frame.clear_external_payload();
        if (ctx->frame.payload.capacity() > 16 * 1024)
            std::string{}.swap(ctx->frame.payload);
        else
            ctx->frame.payload.clear();
        if (ctx->watched_dir.capacity() > 1024)
            std::string{}.swap(ctx->watched_dir);
        else
            ctx->watched_dir.clear();
        task_pool_.emplace_back(ctx);
    }

  private:
    static constexpr std::size_t max_pooled_tasks = 1024;
    std::vector<std::unique_ptr<RpcTaskContext>> task_pool_;
};

} // namespace rpc::detail

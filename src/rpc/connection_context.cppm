module;
#include <sys/epoll.h>
#include <unistd.h>

export module rpc.handler:context;

import std;

import net.socket;
import net.epoll;
import net.event_loop;
import runtime.worker_pool;
import rpc.connection;
import rpc.task_classifier;
import ide.protocol;
import ide.server_terminal;
import service.dir_watcher;
import service.file_watcher;
import service.kv;

namespace rpc::detail {

// 所有 Worker 线程共享同一个内存 KV 存储实例（内部通过 mutex 串行化访问）。
export struct KvState {
    std::mutex mutex;
    service::KvService service;
};

export inline constexpr std::size_t kCacheLineBytes = 64;

// 绑定到单个 EventLoop（单一 I/O 线程）的分流配置视图（共 16 Bytes）：
//   - io_id                 : 当前 I/O 线程编号（0..3），用于定位 Worker 的专属 SPSC
//   队列通道（lane）；
//   - inline_cache_threshold: 允许在 I/O 线程直接内联编码响应的最大字节数（默认 32 KiB）。
export struct IoDispatchView {
    std::size_t io_id{0};
    std::size_t inline_threshold{32 * 1024};
};

export struct IoSlotContext;

// ============================================================================
// 单条慢路径 RPC 请求的在途任务上下文（256B = 4 条 64B Cache Line，单 I/O 线程内池化复用）
// ============================================================================
// 【为什么要设计这个结构体？】
//   1. 让跨线程 Lambda 只捕获 1 个裸指针（8B），100% 命中 std::move_only_function 的 24B SBO：
//      在 libstdc++ 中，PoolTask 和 EventLoop::Task 内部的小对象优化缓冲区（SBO）只有 24 字节。
//      如果把 Frame（88B）、PreparedDispatch（88B）和各种 shared_ptr 按值塞进 Lambda，闭包会膨胀到
//      272B / 176B，导致每次 I/O -> Worker 和 Worker -> I/O 投递都在堆上 new/delete。
//      把这些字段收敛到 RpcTaskContext 后，两处跨线程 Lambda 都简化为 `[ctx]() noexcept`（仅 8
//      字节）， 直接内联存进队列槽位，实现跨线程任务包装 0 堆分配。
//
//   2. 按“谁先读、谁后读”排布 4 条缓存行，减少跨核 L1d Cache Miss，利好 Prefetch：
//      +-----------------------------------------------------------------------+
//      | Cache Line 0 (0..63B)   : slot(8B) + conn_fd(8B) + weak_conn(16B)     |
//      |                           + frame 的前 32B（完整 24B 帧头 + payload 指针）|
//      +-----------------------------------------------------------------------+
//      | Cache Line 1 (64..127B) : frame 剩余 56B + watched_dir 的前 8B        |
//      +-----------------------------------------------------------------------+
//      | Cache Line 2 (128..191B): watched_dir 剩余 24B + prepared 的前 40B    |
//      +-----------------------------------------------------------------------+
//      | Cache Line 3 (192..255B): prepared 剩余 48B                           |
//      +-----------------------------------------------------------------------+
//      - 无论是 Worker 刚拿到 ctx，还是 I/O 线程收到回传的 ctx，解引用第 1 条缓存行（Cache Line 0）
//        就能一次性拿到槽位指针、连接弱引用和完整的 RPC
//        帧头（length/kind/request_id/method/status）；
//      - 尾部的 prepared 仅在 Worker 刚启动时消费一次并当场清空；回写阶段的 I/O
//      线程只读前两条半缓存行，
//        完全不触碰 Cache Line 3，省去跨核缓存行回传与写失效开销。
//
//   3. 同 I/O 线程借还（0 锁、0 原子指令）：
//      ctx 在第 i 号 I/O 线程的 submit_request() 中借出，经 Worker 原地把 ctx->frame 从 Request
//      改写为 Response 后，最终必回到第 i 号 I/O 线程的 complete_response() 中归还。
export struct alignas(kCacheLineBytes) RpcTaskContext {
    // --- Cache Line 0 (offset 0..63B): 核心路由控制头 (32B) + Frame 头部字段 (前 32B) ---
    IoSlotContext *slot{nullptr}; // 8B (offset 0): 指向所属 I/O 线程的运行时槽位上下文
    int conn_fd{-1};              // 4B (offset 8): 客户端连接 fd（供 Terminal 会话校验）
    // 4B 编译器对齐填充 (offset 12..15)
    std::weak_ptr<Connection>
        weak_conn{}; // 16B (offset 16): 连接弱引用（回写前检查连接是否仍存活）
    Frame frame{};   // 88B (offset 32..119): 请求与响应原地复用的协议帧

    // --- Cache Line 1..2: 回写阶段按需读取的目录监听路径 ---
    std::string watched_dir{}; // 32B (offset 120..151): 需向 DirWatcher 注册/注销的目录

    // --- Cache Line 2..3: 仅在 Worker 入口消费一次的预分类结果与只读缓存快照 ---
    PreparedDispatch prepared{}; // 88B (offset 152..239): 预分类与 >32KiB 缓存快照
};

static_assert(sizeof(RpcTaskContext) == 4 * kCacheLineBytes,
              "RpcTaskContext must occupy exactly 4 cache lines (256B)");

// ============================================================================
// 单个 I/O 线程（EventLoop）专属的运行时槽位上下文（按 64B Cache Line 读写冷热隔离）
// ============================================================================
// 【内存布局设计原理】
//   1. 第 1 条缓存行（offset 0..63B）：跨核高频只读指针区
//      依次存放 loop (8B)、workers (8B)、counter (8B)、kv (8B)、watcher (8B)、terminal_mgr (8B)
//      和 io_view (16B)，刚好凑满 64 字节（1 条 L1d Cache Line）。
//      - 这些指针在 Server 启动装配完毕后全程只读不变，生命周期覆盖整个运行期；
//      - 用裸指针替代原先每次请求按值拷贝 3 个 shared_ptr（kv / watcher / terminal_mgr），
//        每条慢路径请求直接省去 6 次 shared_ptr 原子引用计数加减；
//      - 因为第 1 条缓存行在运行期零写入，它在所有 P-core（I/O）和 E-core（Worker）的 L1d 缓存中
//        可长期保持 MESI Shared ('S') 状态，多核并发读取零伪共享失效（Zero False Sharing）。
//
//   2. 第 2 条缓存行起（offset 64B..，通过 alignas(64) 物理隔离）：本 I/O 线程私有可变区
//      存放 connections（本线程活跃连接表）、task_pool_（在途任务节点空闲栈）及组件所有权对象。
//      I/O 线程高频借还 task_pool_ 或增删连接时只修改第 2 条及之后的缓存行，绝不弄脏第 1
//      条只读缓存行。
export struct alignas(kCacheLineBytes) IoSlotContext {
    static constexpr std::size_t kPrewarmTaskCount = 64;
    static constexpr std::size_t kMaxPooledTaskCount = 1024;
    static constexpr std::size_t kMaxRetainedPayloadCap = 16 * 1024;
    static constexpr std::size_t kMaxRetainedPathCap = 1024;

    // --- Cache Line 0 (offset 0..63 Bytes): 跨线程高频只读指针与分流阈值 ---
    net::EventLoop *loop{nullptr};                  // 8B (offset 0)
    runtime::WorkerPool *workers{nullptr};          // 8B (offset 8)
    ConnectionCounter *counter{nullptr};            // 8B (offset 16)
    KvState *kv{nullptr};                           // 8B (offset 24)
    service::DirWatcher *watcher{nullptr};          // 8B (offset 32)
    server::TerminalManager *terminal_mgr{nullptr}; // 8B (offset 40)
    IoDispatchView io_view{};                       // 16B (offset 48..63)

    // --- Cache Line 1+ (offset 64B..): 仅所属 I/O 线程单线程读写的可变数据与所有权管理 ---
    alignas(kCacheLineBytes) ConnectionMap connections{};
    std::vector<std::unique_ptr<RpcTaskContext>> task_pool_{};
    std::shared_ptr<service::DirWatcher> watcher_owner{};
    std::shared_ptr<server::TerminalManager> terminal_mgr_owner{};

    // 【作用】启动阶段预留空闲栈容量，并预分配 64 个连续的 RpcTaskContext 节点。
    // 【原理】提前 reserve(1024) 确保运行期归还节点 push_back 绝不触发 vector 重分配；
    //         预热 64 个节点让冷启动后的第一批并发慢请求直接命中对象池，零 operator new。
    void prewarm_task_pool(std::size_t count = kPrewarmTaskCount) {
        task_pool_.reserve(kMaxPooledTaskCount);
        const std::size_t target = std::min(count, kMaxPooledTaskCount);
        while (task_pool_.size() < target) {
            auto ctx = std::make_unique<RpcTaskContext>();
            ctx->slot = this;
            task_pool_.push_back(std::move(ctx));
        }
    }

    // 【作用】在所属 I/O 线程内无锁借出一个 RpcTaskContext，并将本次请求的帧与预取快照移入其中。
    // 【原理】直接从本线程私有的 task_pool_ 栈顶 O(1) 弹出热节点（单线程操作，0 锁、0 原子指令）；
    //         仅当瞬时并发超过池存量时才按需 new 新节点，用完后会自动回收到池中。
    [[nodiscard]] RpcTaskContext *acquire_task_ctx(Frame &&request, PreparedDispatch &&prepared,
                                                   const std::shared_ptr<Connection> &conn,
                                                   int conn_fd) {
        RpcTaskContext *ctx = nullptr;
        if (!task_pool_.empty()) {
            ctx = task_pool_.back().release();
            task_pool_.pop_back();
        } else {
            ctx = new RpcTaskContext{};
        }

        ctx->slot = this;
        ctx->conn_fd = conn_fd;
        ctx->weak_conn = conn;
        ctx->frame = std::move(request);
        ctx->watched_dir.clear();
        ctx->prepared = std::move(prepared);
        return ctx;
    }

    // 【作用】在所属 I/O 线程完成响应回写（或提交失败回滚）后，清理并归还 RpcTaskContext。
    // 【原理】
    //   1. 立即释放 weak_conn
    //   与外置缓存切片引用（payload_owner），防止空闲池节点延长外部对象生命周期；
    //   2. 对常规大小的字符串仅调 clear() 保留底层堆容量供下次请求复用；若偶发超大缓冲（payload >
    //   16KiB）
    //      未被移动走，则主动替换为空串释放内存，防止长连接池内存膨胀；
    //   3. 尾部的 prepared 已在 Worker 线程消费清空，此处不再触碰 Cache Line
    //   3，避免跨核脏缓存行写失效。
    void release_task_ctx(RpcTaskContext *ctx) noexcept {
        if (ctx == nullptr)
            return;

        ctx->conn_fd = -1;
        ctx->weak_conn.reset();
        ctx->frame.clear_external_payload();
        if (ctx->frame.payload.capacity() > kMaxRetainedPayloadCap) {
            ctx->frame.payload = {};
        } else {
            ctx->frame.payload.clear();
        }

        if (ctx->watched_dir.capacity() > kMaxRetainedPathCap) {
            ctx->watched_dir = {};
        } else {
            ctx->watched_dir.clear();
        }

        if (task_pool_.size() < kMaxPooledTaskCount) {
            try {
                task_pool_.emplace_back(ctx);
                return;
            } catch (...) {
            }
        }
        delete ctx;
    }
};

// 【作用】根据连接当前读写状态，拼出下一次 epoll_ctl 需要监听的事件掩码。
// 【原理】始终带上 EPOLLONESHOT 防止多线程误触发；未读到 EOF 且未处于停机排空时监听可读/对端关闭；
//         仅当 output_ 发送队列里还有未发完的积压字节时才按需挂上 EPOLLOUT，避免可写事件空转唤醒。
[[nodiscard]] inline net::EpollInterest connection_interests(const Connection &conn) noexcept {
    auto mask = static_cast<net::EpollInterest>(EPOLLONESHOT);

    if (!conn.peer_eof() && !conn.draining())
        mask = mask | net::EpollInterest::read | net::EpollInterest::peer_closed;

    if (conn.wants_write())
        mask = mask | net::EpollInterest::write;

    return mask;
}

// 【作用】在每次读写、响应回传或停机冻结后，统一更新连接的关闭状态与 epoll 监听掩码（仅所属 I/O
// 线程调用）。 【原理】
//   1. 已满足关闭条件（conn->finished()：已半关闭/停读 + 在途请求归零 + 发送队列发空）：摘除 epoll
//   并销毁连接；
//   2. 已停读且当前无待发数据（但还有在途 Worker 任务未返回）：静默等待 Worker 回调，无需挂任何
//   socket 事件；
//   3. 其余常规状态：调用 modify_event 重新武装 EPOLLONESHOT 与最新读写掩码。
export inline void refresh_connection(net::EventLoop &loop, ConnectionMap &connections,
                                      ConnectionCounter &counter,
                                      const std::shared_ptr<Connection> &conn) {
    if (conn->finished()) {
        close_connection(loop, connections, counter, conn->fd());
        return;
    }

    if ((conn->peer_eof() || conn->draining()) && !conn->wants_write())
        return;

    loop.modify_event(conn->fd(), connection_interests(*conn));
}

// 【作用】基于聚合槽位 IoSlotContext 的连接状态刷新快捷重载。
export inline void refresh_connection(IoSlotContext &slot,
                                      const std::shared_ptr<Connection> &conn) {
    refresh_connection(*slot.loop, slot.connections, *slot.counter, conn);
}

// 【作用】基于 IoSlotContext 为当前 I/O 线程创建专属目录变更监听器（DirWatcher）。
// 【原理】注入的刷新回调仅捕获单个 `&slot` 指针（8B <= std::function 16B SBO），避免 std::function
// 堆分配。
export inline std::shared_ptr<service::DirWatcher> make_dir_watcher(IoSlotContext &slot) {
    return std::make_shared<service::DirWatcher>(*slot.loop, slot.connections, *slot.workers,
                                                 *slot.counter, slot.io_view.io_id,
                                                 [&slot](const std::shared_ptr<Connection> &conn) {
                                                     refresh_connection(slot, conn);
                                                 });
}

// 【作用】基于 IoSlotContext 为当前 I/O 线程创建专属 PTY 终端管理器（TerminalManager）。
// 【原理】当本线程管辖的 PTY 产生输出或命令结束时，通过仅捕获单个 `&slot`
// 指针（8B）的回调查找对应连接，
//         将 Event 帧入队 output_ 并立刻尝试一次非阻塞 writev() 发出。
export inline std::shared_ptr<server::TerminalManager> make_terminal_manager(IoSlotContext &slot) {
    return std::make_shared<server::TerminalManager>(
        *slot.loop, [&slot](int conn_fd, Frame &&event) {
            auto it = slot.connections.find(conn_fd);
            if (it == slot.connections.end())
                return;

            auto conn = it->second;
            if (!conn || conn->peer_eof() || conn->draining())
                return;

            try {
                if (!conn->enqueue_event(std::move(event))) {
                    close_connection(*slot.loop, slot.connections, *slot.counter, conn->fd());
                    return;
                }
                if (conn->wants_write())
                    conn->write();
                refresh_connection(slot, conn);
            } catch (...) {
                close_connection(*slot.loop, slot.connections, *slot.counter, conn->fd());
            }
        });
}

// 【作用】在所属 I/O 线程启动进程级全局文件监听器（FileWatcher，通常挂载在 0 号 I/O 线程）。
export inline void start_file_watcher(net::EventLoop &loop) {
    service::global_file_watcher().start(loop);
}

// 【作用】在停机阶段停止全局文件监听器并释放内核 inotify 描述符。
export inline void stop_file_watcher() noexcept {
    service::global_file_watcher().stop();
}

} // namespace rpc::detail

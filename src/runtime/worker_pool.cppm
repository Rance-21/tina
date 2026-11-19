module;
#include <pthread.h>

export module runtime.worker_pool;

import std;
export import runtime.config;
export import runtime.dispatcher;
import runtime.cpu_affinity;
import runtime.spsc_queue;
import runtime.log;

namespace {

thread_local std::optional<std::size_t> t_current_worker_id;

} // namespace

export namespace runtime {

// 【作用】查询当前正在执行的线程是第几号 Worker（0..N-1）；在非 Worker 线程调用时返回 nullopt。
// 【原理】每个 Worker 线程启动进入 entry() 时把自己的 worker_id 写入 thread_local
// 变量，零开销读取。
[[nodiscard]] std::optional<std::size_t> current_worker_id() noexcept {
    return t_current_worker_id;
}

// ============================================================================
// WorkerPool V2：固定 4P+8E 拓扑 + 主动借用调度 + Per-producer SPSC 队列矩阵 + 批量唤醒
// ============================================================================
//   - 启动时创建 N 个固定工作线程（默认 8 个，分别绑定到 E-core 16..23），[0, k) 处理 Normal，
//     [k, N) 处理 Blocking（仅在精确 state == kIdleBit 时允许被借用接 1 个 Normal 任务）；
//   - I/O 线程调用 try_submit() 时由 Dispatcher 直接把任务投进目标 Worker 专属的 io_id SPSC lane；
//   - I/O 线程可通过 RAII 的 SubmitBatchGuard 把一轮 epoll/recv
//   内提交的多个任务合并成一次批次唤醒，
//     且仅对挂有 kSleepingBit 的真正沉睡 Worker 触发 notify_one()；
//   - 每个 Worker 线程循环轮询自己的 4 条 SPSC lane 取任务执行，开始执行前按需清除 kIdleBit，
//     空载时挂出 IDLE 牌子并进入按需休眠，全过程无全局锁。
class WorkerPool {
  public:
    using Task = PoolTask;

    // 【作用】RAII 批量提交/合并唤醒守卫，用于在单次 epoll/recv 或 inotify 批次内延迟并合并唤醒。
    // 【原理】构造时递增本 I/O 线程的 batch_depth；析构时自动 flush_submit_batch()，无论正常返回、
    //         提前退出还是抛异常，都能确保本批次内所有已入队任务统一执行一次栅栏并唤醒沉睡 Worker。
    class SubmitBatchGuard {
      public:
        SubmitBatchGuard(WorkerPool &pool, std::size_t io_id) noexcept
            : pool_(pool), io_id_(io_id) {
            pool_.begin_submit_batch(io_id_);
        }

        ~SubmitBatchGuard() {
            pool_.flush_submit_batch(io_id_);
        }

        SubmitBatchGuard(const SubmitBatchGuard &) = delete;
        SubmitBatchGuard &operator=(const SubmitBatchGuard &) = delete;

      private:
        WorkerPool &pool_;
        std::size_t io_id_;
    };

    // 【作用】按配置初始化 SPSC 队列矩阵、创建并启动所有 Worker 线程，并可选逐一钉到指定 CPU 核。
    // 【原理】
    //   1. 先校验 config 并构造内部的 Dispatcher（分配好 8 x 4 条 SPSC 环形队列）；
    //   2. 逐个创建 pthread（若开启绑核，先用 ScopedThreadAttr 在创建时就指定目标核）；
    //   3. 用 std::latch 同步屏障等待所有 Worker
    //   完成线程内绑核校验；只要任意一个线程创建或绑核失败，
    //      立刻调用 shutdown() 回收已经拉起的线程，再向上抛出异常，保证不泄漏孤儿线程。
    explicit WorkerPool(const ServerConfig &config)
        : config_(config), dispatcher_(config_.io_threads_cnt(), config_.worker_threads(),
                                       config_.non_blk_worker_cnt, config_.queue_cap) {
        config_.validate();

        const std::size_t count = config_.worker_threads();
        auto ready_latch = std::make_shared<std::latch>(static_cast<std::ptrdiff_t>(count));
        auto startup_errors = std::make_shared<std::vector<std::exception_ptr>>(count);

        try {
            threads_.reserve(count);
            contexts_.reserve(count);

            for (std::size_t w = 0; w < count; ++w) {
                auto ctx = std::make_unique<WorkerContext>(WorkerContext{
                    .pool = this,
                    .worker_id = w,
                    .pin_cpu = config_.enable_cpu_affinity
                                   ? std::optional<unsigned>{config_.worker_cpus[w]}
                                   : std::nullopt,
                    .ready = ready_latch,
                    .error = &(*startup_errors)[w],
                });

                affinity::ScopedThreadAttr attr;
                if (ctx->pin_cpu)
                    attr.set_affinity(*ctx->pin_cpu);

                pthread_t thread{};
                const int rc = ::pthread_create(&thread, ctx->pin_cpu ? attr.get() : nullptr,
                                                &WorkerPool::entry, ctx.get());
                if (rc != 0)
                    throw std::system_error(rc, std::generic_category(), "pthread_create");

                contexts_.push_back(std::move(ctx));
                threads_.push_back(thread);
            }
        } catch (...) {
            shutdown();
            throw;
        }

        ready_latch->wait();
        for (const auto &err : *startup_errors) {
            if (err) {
                shutdown();
                std::rethrow_exception(err);
            }
        }
    }

    // 【作用】对象销毁时兜底触发优雅停机，确保所有后台 Worker 线程都被安全 join。
    ~WorkerPool() {
        shutdown();
    }

    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    // 【作用】开启第 io_id 号 I/O 线程的批量提交作用域，期间入队任务暂存至本地唤醒位图。
    // 【原理】直接委托 Dispatcher::begin_submit_batch(io_id)。
    void begin_submit_batch(std::size_t io_id) noexcept {
        dispatcher_.begin_submit_batch(io_id);
    }

    // 【作用】结束第 io_id 号 I/O 线程的批量提交作用域并统一唤醒本批次触达的沉睡 Worker。
    // 【原理】直接委托 Dispatcher::flush_submit_batch(io_id)。
    void flush_submit_batch(std::size_t io_id) noexcept {
        dispatcher_.flush_submit_batch(io_id);
    }

    // 【作用】供第 io_id 号 I/O 线程非阻塞提交一个 Normal 或 Blocking 任务。
    // 【原理】直接委托给 Dispatcher::try_submit()；若正在停机或合法通道全满，返回 false 且不消耗
    // task。
    [[nodiscard]] bool try_submit(std::size_t io_id, TaskClass task_class, Task &&task) noexcept {
        return dispatcher_.try_submit(io_id, task_class, std::move(task), stopping_);
    }

    // 【作用】优雅关闭线程池：停止接收新任务，等所有已接受和已预留任务跑完后再回收全部线程。
    // 【原理】用 stopping_.exchange(true, seq_cst) 幂等切入停机状态，再给每个 Worker 的 wake_epoch
    //         清除 kSleepingBit 并递增 kEpochStep(2) 后 notify_all()，唤醒所有沉睡 Worker
    //         完成排空退出， 最后逐个 pthread_join 回收线程资源。
    void shutdown() noexcept {
        if (!stopping_.exchange(true, std::memory_order_seq_cst)) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            const std::size_t count = dispatcher_.worker_cnt();
            for (std::size_t w = 0; w < count; ++w) {
                auto &slot = dispatcher_.worker(w);
                std::uint64_t cur = slot.wake_epoch.load(std::memory_order_relaxed);
                while (!slot.wake_epoch.compare_exchange_weak(
                    cur, (cur + kEpochStep) & ~kSleepingBit, std::memory_order_release,
                    std::memory_order_relaxed)) {
                }
                slot.wake_epoch.notify_all();
            }
        }

        for (pthread_t thread : threads_) {
            if (::pthread_join(thread, nullptr) != 0)
                std::terminate();
        }
        threads_.clear();
        contexts_.clear();
    }

    // 【作用】统计当前全池尚未执行完的任务总数（包含已预留、队列中等待、以及正在运行的任务）。
    // 【原理】把每个 Worker 控制块中 state 的低 63 位（outstanding）以 relaxed 累加得到近似快照。
    [[nodiscard]] std::size_t pending_cnt() const noexcept {
        std::size_t total = 0;
        for (std::size_t w = 0; w < dispatcher_.worker_cnt(); ++w)
            total += static_cast<std::size_t>(
                dispatcher_.r_only_worker(w).state.load(std::memory_order_relaxed) & kCountMask);
        return total;
    }

    // 暴露底层 Dispatcher 引用（主要供单元测试校验 Worker 状态字、队列回滚及借用行为）。
    [[nodiscard]] Dispatcher &dispatcher() noexcept {
        return dispatcher_;
    }

    // 只读版本：暴露底层 Dispatcher 引用。
    [[nodiscard]] const Dispatcher &r_only_dispatcher() const noexcept {
        return dispatcher_;
    }

  private:
    struct WorkerContext {
        WorkerPool *pool{nullptr};
        std::size_t worker_id{0};
        std::optional<unsigned> pin_cpu;
        std::shared_ptr<std::latch> ready;
        std::exception_ptr *error{nullptr};
    };

    // 【作用】pthread 要求的 C 风格静态线程入口函数。
    // 【原理】写入当前线程的 thread_local 编号 -> 若开启绑核则执行 pin_thread_to_cpu 并回读校验 ->
    //         把启动结果（成功或异常）通过 latch 通知构造函数 -> 正式进入 worker_loop() 主循环。
    static void *entry(void *arg) noexcept {
        auto *ctx = static_cast<WorkerContext *>(arg);
        t_current_worker_id = ctx->worker_id;
        try {
            if (ctx->pin_cpu)
                affinity::pin_thread_to_cpu(*ctx->pin_cpu);
        } catch (...) {
            *ctx->error = std::current_exception();
            ctx->ready->count_down();
            return nullptr;
        }
        ctx->ready->count_down();
        ctx->pool->worker_loop(ctx->worker_id);
        return nullptr;
    }

    // 【作用】从当前 Worker 专属的 4 条 SPSC lane 中公平地取出一个任务。
    // 【原理】从本线程栈上的私有游标 lane_cursor 开始依次尝试 try_pop()；一旦在第 idx 条 lane
    // 取到任务，
    //         立即把下次扫描起点移到 (idx + 1) % io_count，防止某条高负载 I/O 通道饿死其他通道。
    [[nodiscard]] bool pop_from_lanes(WorkerControl &slot, PoolTask &task,
                                      std::size_t &lane_cursor) noexcept {
        const std::size_t io_count = slot.lanes.size();
        for (std::size_t step = 0, idx = lane_cursor; step < io_count;
             ++step, idx = (idx + 1 == io_count) ? 0 : idx + 1) {
            if (slot.lanes[idx]->try_pop(task)) {
                lane_cursor = (idx + 1 == io_count) ? 0 : idx + 1;
                return true;
            }
        }
        return false;
    }

    // 【作用】脱离队列后真正执行业务闭包：必要时清除 kIdleBit、屏蔽未捕获异常、并保证计数精准回收。
    // 【原理】
    //   1. 因为普通提交允许 kIdleBit 与 count > 0 共存，执行前先用 relaxed 检查 bit63：
    //      - 稳态连续执行时 bit63 已为 0，直接跳过（零额外原子写）；
    //      - 仅在刚从空闲切入工作态时调用 fetch_and(kCountMask, seq_cst) 清除 kIdleBit，确保任务
    //        执行完毕 CountGuard 减 1 后 state 回到 0（而非意外直接变回 kIdleBit）；
    //   2. 用局部 RAII 守卫 CountGuard 兜底：先执行 task() 并立即清空 task={} 释放闭包捕获对象，
    //      最后 CountGuard 析构调用 state.fetch_sub(1, acq_rel)，即便抛异常也绝不漏减计数。
    void execute_task(WorkerControl &slot, PoolTask task) noexcept {
        if (slot.state.load(std::memory_order_relaxed) & kIdleBit) {
            slot.state.fetch_and(kCountMask, std::memory_order_seq_cst);
        }

        struct CountGuard {
            WorkerControl &control;
            ~CountGuard() {
                control.state.fetch_sub(1, std::memory_order_acq_rel);
            }
        } guard{slot};

        try {
            task();
            task = {};
        } catch (...) {
            task = {};
            try {
                log::error("Worker task threw an exception");
            } catch (...) {
            }
        }
    }

    // 【作用】若当前 wake_epoch 仍挂着 kSleepingBit（如二次扫描命中或虚假唤醒），将其清除。
    // 【原理】先 relaxed 检查，仅当 bit0 为 1 时才执行一次 fetch_and(~kSleepingBit, relaxed)。
    static void clear_sleeping_bit(WorkerControl &slot) noexcept {
        if (slot.wake_epoch.load(std::memory_order_relaxed) & kSleepingBit)
            slot.wake_epoch.fetch_and(~kSleepingBit, std::memory_order_relaxed);
    }

    // 【作用】单个 Worker 线程的主调度循环：高速轮询取活 -> 空载挂 IDLE 牌子 -> 休眠等唤醒 ->
    // 优雅退出。 【原理】
    //   - 热路径：只要 4 条 SPSC lane 有活，完全不触碰 wake_epoch、不做任何 CAS
    //   或睡眠检查，直接执行；
    //   - 冷路径（4 条 lane 全空）：
    //     1. 若正在停机且 state 低 63 位为 0（连已预留未入队的任务也没了），直接安全退出；
    //     2. 若未停机且 state == 0，用 CAS(0, kIdleBit) 挂出空闲牌子允许借用；
    //     3. 挂上 kSleepingBit（fetch_or）并执行 seq_cst 栅栏（与生产者 flush 时的 seq_cst 栅栏构成
    //        严格 Dekker 全序，杜绝丢唤醒），然后二次扫描 4 条 lane 并复查停机退出条件；
    //     4. 仍无任务则调 wake_epoch.wait(parked_epoch) 休眠；醒来后清掉残留 kSleepingBit
    //     回到热路径。
    void worker_loop(std::size_t worker_id) noexcept {
        WorkerControl &slot = dispatcher_.worker(worker_id);
        std::size_t lane_cursor = 0;

        while (true) {
            PoolTask task;
            if (pop_from_lanes(slot, task, lane_cursor)) {
                execute_task(slot, std::move(task));
                continue;
            }

            if (stopping_.load(std::memory_order_seq_cst)) {
                if ((slot.state.load(std::memory_order_seq_cst) & kCountMask) == 0)
                    return;
            } else if (slot.state.load(std::memory_order_relaxed) == 0) {
                std::uint64_t expected = 0;
                (void)slot.state.compare_exchange_strong(
                    expected, kIdleBit, std::memory_order_acq_rel, std::memory_order_relaxed);
            }

            // 宣告即将休眠并立下 seq_cst 栅栏，再做二次扫描防止与生产者入队/批量唤醒错位。
            const std::uint64_t parked_epoch =
                slot.wake_epoch.fetch_or(kSleepingBit, std::memory_order_acq_rel) | kSleepingBit;
            std::atomic_thread_fence(std::memory_order_seq_cst);

            if (pop_from_lanes(slot, task, lane_cursor)) {
                clear_sleeping_bit(slot);
                execute_task(slot, std::move(task));
                continue;
            }

            if (stopping_.load(std::memory_order_seq_cst) &&
                (slot.state.load(std::memory_order_seq_cst) & kCountMask) == 0) {
                clear_sleeping_bit(slot);
                return;
            }

            slot.wake_epoch.wait(parked_epoch, std::memory_order_acquire);
            clear_sleeping_bit(slot);
        }
    }

    ServerConfig config_;
    std::atomic<bool> stopping_{false};
    Dispatcher dispatcher_;

    std::vector<std::unique_ptr<WorkerContext>> contexts_;
    std::vector<pthread_t> threads_;
};

} // namespace runtime

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
// 每个 Worker 启动时写入自己的 thread_local 编号，查询只访问当前线程。
[[nodiscard]] std::optional<std::size_t> current_worker_id() noexcept {
    return t_current_worker_id;
}

// ============================================================================
// WorkerPool：固定数量的工作线程，按任务类别分区，支持空闲借用和批量唤醒
// ============================================================================
//   - 启动时创建 N 个固定工作线程（默认 8 个，分别绑定到 E-core 16..23），[0, k) 处理 Normal，
//     [k, N) 处理 Blocking（仅在精确 state == kIdleBit 时允许被借用接 1 个 Normal 任务）；
//   - I/O 线程调用 try_submit() 时由 Dispatcher 直接把任务投进目标 Worker 专属的 io_id SPSC lane；
//   - I/O 线程可通过 RAII 的 SubmitBatchGuard 把一轮 epoll/recv
//   内提交的多个任务合并成一次批次唤醒，
//     且仅对挂有 kSleepingBit 的真正沉睡 Worker 触发 notify_one()；
//   - 每个 Worker 轮询本核的 SPSC lane（每个 I/O 线程一条） 取任务执行，开始执行前按需清除
//   kIdleBit，
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
            : dispatcher_(pool.dispatcher_), io_id_(io_id) {
            dispatcher_.begin_submit_batch(io_id_);
        }

        ~SubmitBatchGuard() {
            dispatcher_.flush_submit_batch(io_id_);
        }

        SubmitBatchGuard(const SubmitBatchGuard &) = delete;
        SubmitBatchGuard &operator=(const SubmitBatchGuard &) = delete;

      private:
        Dispatcher &dispatcher_;
        std::size_t io_id_;
    };

    // 队列、负载和唤醒状态归 Dispatcher；线程池在启动后只需保留 pthread 句柄。
    explicit WorkerPool(const ServerConfig &config)
        : dispatcher_(config.io_threads_cnt(), config.worker_threads(), config.non_blk_worker_cnt,
                      config.queue_cap) {
        config.validate();
        std::span<const unsigned> cpus;
        if (config.enable_cpu_affinity)
            cpus = config.worker_cpus;
        try {
            affinity::start_threads(threads_, config.worker_threads(), cpus,
                                    [this](std::size_t id) {
                                        t_current_worker_id = id;
                                        worker_loop(id);
                                    });
        } catch (...) {
            shutdown();
            throw;
        }
    }

    // 【作用】对象销毁时兜底触发优雅停机，确保所有后台 Worker 线程都被安全 join。
    ~WorkerPool() {
        shutdown();
    }

    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

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

        affinity::join_threads(threads_);
    }

    // 【作用】统计当前全池尚未执行完的任务总数（包含已预留、队列中等待、以及正在运行的任务）。
    // 【原理】把每个 Worker 控制块中 state 的低 63 位（outstanding）以 relaxed 累加得到近似快照。
    [[nodiscard]] std::size_t pending_cnt() const noexcept {
        std::size_t total = 0;
        for (std::size_t w = 0; w < dispatcher_.worker_cnt(); ++w)
            total += static_cast<std::size_t>(
                dispatcher_.worker(w).state.load(std::memory_order_relaxed) & kCountMask);
        return total;
    }

    // 暴露底层 Dispatcher 引用（主要供单元测试校验 Worker 状态字、队列回滚及借用行为）。
    [[nodiscard]] Dispatcher &dispatcher() noexcept {
        return dispatcher_;
    }

  private:
    // 【作用】从当前 Worker 专属的各条 SPSC lane 中公平地取出一个任务。
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
    //   - 热路径：只要任何一条 SPSC lane 有活，完全不触碰 wake_epoch、不做任何 CAS
    //   或睡眠检查，直接执行；
    //   - 冷路径（所有 lane 都为空）：
    //     1. 若正在停机且 state 低 63 位为 0（连已预留未入队的任务也没了），直接安全退出；
    //     2. 若未停机且 state == 0，用 CAS(0, kIdleBit) 挂出空闲牌子允许借用；
    //     3. 挂上 kSleepingBit（fetch_or）并执行 seq_cst 栅栏（与生产者 flush 时的 seq_cst 栅栏构成
    //        严格 Dekker 全序，杜绝丢唤醒），然后二次扫描全部 lane 并复查停机退出条件；
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

    std::atomic<bool> stopping_{false};
    Dispatcher dispatcher_;

    std::vector<pthread_t> threads_;
};

} // namespace runtime

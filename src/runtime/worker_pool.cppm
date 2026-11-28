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

// 查询当前正在执行的线程是第几号 Worker（0..N-1）；在非 Worker 线程调用时返回 nullopt。
[[nodiscard]] std::optional<std::size_t> current_worker_id() noexcept {
    return t_current_worker_id;
}

// ============================================================================
// WorkerPool：固定数量的同构工作线程池，支持按在途最轻路由与批量合并唤醒
// ============================================================================
//   - 启动时创建 N 个固定工作线程（默认 8 个，分别绑定到 E-core 16..23）；
//   - I/O 线程调用 try_submit() 时由 Dispatcher 直接把任务投进目标 Worker 专属的 io_id SPSC lane；
//   - I/O 线程可通过 RAII 的 SubmitBatchGuard 把单轮收包/事件内提交的多个任务合并为一次唤醒，
//     且仅对挂有 kSleepingBit 的真正沉睡 Worker 触发 notify_one()；
//   - 每个 Worker 公平轮询本核的各条 SPSC lane 取任务执行，空载时经二次扫描确认后休眠，全程无锁。
class WorkerPool {
  public:
    using Task = PoolTask;

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
        : dispatcher_(config.io_threads_cnt(), config.worker_threads(), config.queue_cap) {
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

    ~WorkerPool() {
        shutdown();
    }

    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    // 供第 io_id 号 I/O 线程非阻塞提交一个任务；若正在停机或全池通道已满，返回 false 且保留原 task。
    [[nodiscard]] bool try_submit(std::size_t io_id, Task &&task) noexcept {
        return dispatcher_.try_submit(io_id, std::move(task), stopping_);
    }

    // 优雅关闭线程池：停止接收新任务，等所有已接受和已预留任务跑完后再回收全部线程。
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

    // 统计当前全池尚未执行完的任务总数（包含已预留、队列中等待、以及正在运行的任务）。
    [[nodiscard]] std::size_t pending_cnt() const noexcept {
        std::size_t total = 0;
        for (std::size_t w = 0; w < dispatcher_.worker_cnt(); ++w)
            total += static_cast<std::size_t>(
                dispatcher_.worker(w).state.load(std::memory_order_relaxed));
        return total;
    }

    [[nodiscard]] Dispatcher &dispatcher() noexcept {
        return dispatcher_;
    }

  private:
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

    void execute_task(WorkerControl &slot, PoolTask task) noexcept {
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

    static void clear_sleeping_bit(WorkerControl &slot) noexcept {
        if (slot.wake_epoch.load(std::memory_order_relaxed) & kSleepingBit)
            slot.wake_epoch.fetch_and(~kSleepingBit, std::memory_order_relaxed);
    }

    void worker_loop(std::size_t worker_id) noexcept {
        WorkerControl &slot = dispatcher_.worker(worker_id);
        std::size_t lane_cursor = 0;

        while (true) {
            PoolTask task;
            if (pop_from_lanes(slot, task, lane_cursor)) {
                execute_task(slot, std::move(task));
                continue;
            }

            if (stopping_.load(std::memory_order_seq_cst) &&
                slot.state.load(std::memory_order_seq_cst) == 0) {
                return;
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
                slot.state.load(std::memory_order_seq_cst) == 0) {
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

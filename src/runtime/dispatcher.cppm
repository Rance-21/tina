export module runtime.dispatcher;

import std;
import runtime.spsc_queue;

export namespace runtime {

using PoolTask = std::move_only_function<void()>;

inline constexpr std::uint64_t kMaxOutstanding = std::uint64_t{1} << 62;

// ============================================================================
// Worker 休眠/唤醒版本号位布局（支持批量合并唤醒 + 仅在睡眠时 notify）
// ============================================================================
//   - Bit 0       : kSleepingBit（Worker 准备进入 atomic::wait 休眠前通过 fetch_or 置 1）
//   - Bits 1..63  : 单调递增的唤醒纪元（每次唤醒加 kEpochStep = 2 并顺手清掉 kSleepingBit）
// 通过“生产者发布任务 -> seq_cst 栅栏 -> 查 kSleepingBit”与“Worker 置 kSleepingBit ->
// seq_cst 栅栏 -> 二次扫队列”形成严格的 Dekker 全序，保证零丢唤醒且运行中零冗余 futex 调用。
inline constexpr std::uint64_t kSleepingBit = std::uint64_t{1};
inline constexpr std::uint64_t kEpochStep = std::uint64_t{2};

// 每个 Worker 独立的控制块：在途计数 state、wake_epoch 与 lanes 各自隔离在独立 64B 缓存行。
struct alignas(kCacheLineBytes) WorkerControl {
    alignas(kCacheLineBytes) std::atomic<std::uint64_t> state{0};
    alignas(kCacheLineBytes) std::atomic<std::uint64_t> wake_epoch{0};
    alignas(kCacheLineBytes) std::vector<std::unique_ptr<SpscQueue<PoolTask>>> lanes;
};

// 每个 I/O 线程私有的调度上下文（无锁、无原子、独立 64B 缓存行），包含轮询游标与批量唤醒位图。
struct alignas(kCacheLineBytes) IoDispatchContext {
    std::size_t rr_cursor{0};
    std::uint64_t waiting_wake_mask{0}; // 当前批次已入队、待统一唤醒的 Worker 位图
    std::uint32_t batch_depth{0};       // 批量作用域嵌套深度（> 0 时延迟至批次结束统一唤醒）
};

// ============================================================================
// 同构矩阵调度器（Dispatcher）
// ============================================================================
//   - 每个 Worker 拥有 io_count 条独占 SPSC 入口通道（默认 8 x 4 = 32 条无锁队列）；
//   - 提交时在全池 [0, worker_count) 中从本 I/O 线程游标出发挑在途最轻的 Worker，直接单条
//     wait-free fetch_add(1) 占坑并写入对应 SPSC lane；
//   - 配合批量提交作用域（begin_submit_batch / flush_submit_batch），单轮收包/事件内提交的多个
//     任务共用末尾 1 次 seq_cst 栅栏，且仅唤醒真正处于睡眠态的 Worker。
class Dispatcher {
  public:
    Dispatcher(std::size_t io_count, std::size_t worker_count, std::size_t queue_capacity)
        : io_count_(io_count), worker_count_(worker_count),
          workers_(std::make_unique<WorkerControl[]>(worker_count)),
          io_contexts_(std::make_unique<IoDispatchContext[]>(io_count)) {

        if (io_count_ == 0 || worker_count_ == 0 || worker_count_ > 64)
            throw std::invalid_argument("invalid Dispatcher worker topology");

        for (std::size_t w = 0; w < worker_count_; ++w) {
            workers_[w].lanes.reserve(io_count_);
            for (std::size_t io = 0; io < io_count_; ++io)
                workers_[w].lanes.push_back(std::make_unique<SpscQueue<PoolTask>>(queue_capacity));
        }

        // 按 I/O 线程均匀错开初始游标，避免冷启动时同时扎堆投向 0 号 Worker。
        for (std::size_t io = 0; io < io_count_; ++io)
            io_contexts_[io].rr_cursor = (io * worker_count_ / io_count_) % worker_count_;
    }

    Dispatcher(const Dispatcher &) = delete;
    Dispatcher &operator=(const Dispatcher &) = delete;

    [[nodiscard]] std::size_t worker_cnt() const noexcept {
        return worker_count_;
    }

    [[nodiscard]] WorkerControl &worker(std::size_t worker_id) noexcept {
        return workers_[worker_id];
    }

    [[nodiscard]] const WorkerControl &worker(std::size_t worker_id) const noexcept {
        return workers_[worker_id];
    }

    void begin_submit_batch(std::size_t io_id) noexcept {
        if (io_id < io_count_)
            ++io_contexts_[io_id].batch_depth;
    }

    void flush_submit_batch(std::size_t io_id) noexcept {
        if (io_id >= io_count_)
            return;
        IoDispatchContext &io = io_contexts_[io_id];
        if (io.batch_depth == 0 || --io.batch_depth != 0)
            return;

        std::uint64_t mask = std::exchange(io.waiting_wake_mask, 0);
        if (mask == 0)
            return;

        // 封死 Store-Load 重排，确认全部 SPSC tail 发布可见后再检查睡眠位。
        std::atomic_thread_fence(std::memory_order_seq_cst);
        while (mask != 0) {
            const unsigned worker_id = static_cast<unsigned>(std::countr_zero(mask));
            mask &= (mask - 1);
            wake_worker_after_fence(workers_[worker_id]);
        }
    }

    static void wake_worker(WorkerControl &slot) noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        wake_worker_after_fence(slot);
    }

    [[nodiscard]] bool try_submit(std::size_t io_id, PoolTask &&task,
                                  const std::atomic<bool> &stopping) noexcept {
        if (io_id >= io_count_ || !task)
            return false;

        IoDispatchContext &io = io_contexts_[io_id];
        std::uint64_t excluded_mask = 0;

        for (std::size_t attempt = 0; attempt < worker_count_; ++attempt) {
            const auto wid = select_lightest(io.rr_cursor, excluded_mask);
            if (!wid)
                return false;

            WorkerControl &slot = workers_[*wid];
            const std::uint64_t bit = std::uint64_t{1} << *wid;

            if (slot.state.fetch_add(1, std::memory_order_seq_cst) >= kMaxOutstanding) {
                rollback_reservation(slot);
                excluded_mask |= bit;
                continue;
            }

            // 占坑后用 seq_cst 复查停机标志，与 Worker 退出检查构成全序，杜绝 shutdown 丢单竞态。
            if (stopping.load(std::memory_order_seq_cst)) {
                rollback_reservation(slot);
                return false;
            }

            if (slot.lanes[io_id]->try_push(std::move(task))) {
                if (io.batch_depth > 0)
                    io.waiting_wake_mask |= bit;
                else
                    wake_worker(slot);
                return true;
            }

            // 对应 SPSC lane 已满：撤销预留计数，按需唤醒可能等待预留归零的 Worker，再试其余候选。
            rollback_reservation(slot);
            excluded_mask |= bit;
        }

        return false;
    }

  private:
    static void wake_worker_after_fence(WorkerControl &slot) noexcept {
        std::uint64_t cur = slot.wake_epoch.load(std::memory_order_relaxed);
        while ((cur & kSleepingBit) != 0) {
            const std::uint64_t desired = (cur + kEpochStep) & ~kSleepingBit;
            if (slot.wake_epoch.compare_exchange_weak(cur, desired, std::memory_order_release,
                                                      std::memory_order_relaxed)) {
                slot.wake_epoch.notify_one();
                return;
            }
        }
    }

    static void rollback_reservation(WorkerControl &slot) noexcept {
        slot.state.fetch_sub(1, std::memory_order_acq_rel);
        wake_worker(slot);
    }

    [[nodiscard]] std::size_t next_worker(std::size_t current) const noexcept {
        return (current + 1 >= worker_count_) ? 0 : (current + 1);
    }

    // 从本 I/O 线程私有游标起扫一圈，跳过已满 Worker，挑出在途计数最轻者（遇到 0 立即短路）。
    [[nodiscard]] std::optional<std::size_t>
    select_lightest(std::size_t &rr_cursor, std::uint64_t excluded_mask) const noexcept {
        std::size_t cursor = rr_cursor;
        std::optional<std::size_t> best_id;
        std::uint64_t best_count = std::numeric_limits<std::uint64_t>::max();

        for (std::size_t step = 0; step < worker_count_; ++step, cursor = next_worker(cursor)) {
            if ((excluded_mask & (std::uint64_t{1} << cursor)) != 0)
                continue;
            const std::uint64_t count = workers_[cursor].state.load(std::memory_order_relaxed);
            if (!best_id || count < best_count) {
                best_id = cursor;
                best_count = count;
                if (count == 0)
                    break;
            }
        }

        if (best_id)
            rr_cursor = next_worker(*best_id);
        return best_id;
    }

    const std::size_t io_count_;
    const std::size_t worker_count_;

    std::unique_ptr<WorkerControl[]> workers_;
    std::unique_ptr<IoDispatchContext[]> io_contexts_;
};

} // namespace runtime

export module runtime.dispatcher;

import std;
import runtime.spsc_queue;

export namespace runtime {

// ============================================================================
// 任务分类枚举：按实际执行路径（而非仅按 RPC Method）决定执行层级
// ============================================================================
enum class TaskClass : std::uint8_t {
    inline_task = 0, // ping、内联阈值内的缓存命中，直接在 EventLoop 执行
    normal = 1,      // 超出内联阈值的缓存响应准备、纯内存 KV / 终端输入 ACK 等非阻塞工作
    blocking = 2,    // 磁盘读盘/写盘/fsync、目录扫描、Git 子进程、需等待的 PTY/Tab 等阻塞工作
};

using PoolTask = std::move_only_function<void()>;

// ============================================================================
// Worker 原子状态字位布局（允许 kIdleBit 与 outstanding 同时存在）
// ============================================================================
//   - Bit 63      : kIdleBit（由 Worker 在空载时通过 CAS(0, kIdleBit) 宣告可借用）
//   - Bits 0..62  : outstanding（reserved + queued + running 任务总数）
//
// 四种状态语义：
//   1. state == kIdleBit               : 已宣告空闲，允许 Normal 任务通过精确 CAS(kIdleBit, 1)
//   借用；
//   2. state == (kIdleBit | count>0)   : 已有普通任务通过无 CAS 的 fetch_add(1) 预留，不可借用；
//   3. state == count (> 0)            : Worker 已清除 Idle Bit 正在工作，不可借用；
//   4. state == 0                      : 空载但尚未宣告空闲，不可借用。
//
// 这样设计的好处：
//   - 普通 Normal / Blocking 提交彻底摆脱 CAS 自旋循环，退化为单条 wait-free fetch_add(1)；
//   - 仅借用空闲 Blocking Worker 时才使用 CAS(kIdleBit, 1) 保证至多 1 个 I/O 抢占成功；
//   - Worker 在执行任务前按需清掉 kIdleBit（稳态连续执行时位已为 0，零额外原子写）。
inline constexpr std::uint64_t kIdleBit = std::uint64_t{1} << 63;
inline constexpr std::uint64_t kCountMask = kIdleBit - 1;
inline constexpr std::uint64_t kMaxOutstanding = kCountMask - 1;

// ============================================================================
// Worker 休眠/唤醒版本号位布局（支持批量合并唤醒 + 仅在睡眠时 notify）
// ============================================================================
//   - Bit 0       : kSleepingBit（Worker 准备进入 atomic::wait 休眠前通过 fetch_or 置 1）
//   - Bits 1..63  : 单调递增的唤醒纪元（每次唤醒加 kEpochStep = 2 并顺手清掉 kSleepingBit）
// 通过“生产者发布任务 -> seq_cst 栅栏 -> 查 kSleepingBit”与“Worker 置 kSleepingBit ->
// seq_cst 栅栏 -> 二次扫队列”形成严格的 Dekker 全序，保证零丢唤醒且运行中零冗余 futex 调用。
inline constexpr std::uint64_t kSleepingBit = std::uint64_t{1};
inline constexpr std::uint64_t kEpochStep = std::uint64_t{2};

// 每个 Worker 独立的控制块：state、wake_epoch 与 lanes 各自隔离在独立 64B 缓存行，消除伪共享。
struct alignas(kCacheLineBytes) WorkerControl {
    alignas(kCacheLineBytes) std::atomic<std::uint64_t> state{0};
    alignas(kCacheLineBytes) std::atomic<std::uint64_t> wake_epoch{0};
    alignas(kCacheLineBytes) std::vector<std::unique_ptr<SpscQueue<PoolTask>>> lanes;
};

// 每个 I/O 线程私有的调度上下文（无锁、无原子、独立 64B 缓存行），包含轮询起点与批量唤醒位图。
struct alignas(kCacheLineBytes) IoDispatchContext {
    std::size_t idx_normal{0};
    std::size_t idx_blocking{0};
    std::uint64_t waiting_wake_mask{0}; // 当前批次已入队、待统一唤醒的 Worker 位图
    std::uint32_t batch_depth{0};       // 批量作用域嵌套深度（> 0 时延迟至批次结束统一唤醒）
};

// ============================================================================
// 提交阶段主动借用调度器（Dispatcher）
// ============================================================================
// 核心直觉：
//   I/O 线程直接调用 Dispatcher。以默认的 4 个 I/O 线程、8 个 Worker 为例：
//   - 8 个 Worker 分成两区：前 k 个 [0, k) 是 Normal 区，后 8-k 个 [k, 8) 是 Blocking 区；
//   - 每个 Worker 有 4 条独占 SPSC 入口通道（对应 4 个 I/O 线程），形成 8 x 4 = 32 条无锁队列；
//   - Blocking 任务只能进 Blocking 区，挑 outstanding 最少的 Worker 并直接 fetch_add(1) 占坑；
//   - Normal 任务优先挑空闲 Normal Worker；若 Normal 全忙，尝试用 CAS(kIdleBit, 1) 借用 1 个
//     精确处于 IDLE 的 Blocking Worker；借不到再退回最轻 Normal Worker 并 fetch_add(1) 占坑；
//   - 配合批量提交作用域（begin_submit_batch / flush_submit_batch），一轮 epoll/recv 内向同一批
//     Worker 提交的多个任务只需在批次末尾做 1 次 seq_cst 栅栏，且只唤醒真正处于睡眠态的 Worker。
class Dispatcher {
  public:
    // 【作用】构建 worker_count x io_count 的 SPSC 队列矩阵并初始化各 I/O 线程的私有轮询起点。
    // 【原理】一次性分配全部 WorkerControl 和每对 I/O、Worker 之间的 SpscQueue；同时按 io_id 错开各
    // I/O 线程的
    //         初始轮询游标，避免冷启动时 4 个 I/O 线程同时扎堆扑向 Worker 0 / k。
    Dispatcher(std::size_t io_count, std::size_t worker_count, std::size_t normal_workers,
               std::size_t queue_capacity)
        : io_count_(io_count), worker_count_(worker_count), normal_count_(normal_workers),
          workers_(std::make_unique<WorkerControl[]>(worker_count)),
          io_contexts_(std::make_unique<IoDispatchContext[]>(io_count)) {

        if (io_count_ == 0 || worker_count_ < 2 || normal_count_ == 0 ||
            normal_count_ >= worker_count_) {
            throw std::invalid_argument("invalid Dispatcher worker topology");
        }

        if (worker_count_ > 64) {
            throw std::invalid_argument("Dispatcher supports up to 64 workers");
        }

        for (std::size_t w = 0; w < worker_count_; ++w) {
            workers_[w].lanes.reserve(io_count_);
            for (std::size_t io = 0; io < io_count_; ++io) {
                workers_[w].lanes.push_back(std::make_unique<SpscQueue<PoolTask>>(queue_capacity));
            }
        }

        for (std::size_t io = 0; io < io_count_; ++io) {
            // 错开轮询起点：给第 io 号 I/O 线程设置 rr_normal = io % normal_count、rr_blocking =
            // normal_count + io % blocking_count，
            //  防止刚启动大家同时把第一波任务全砸向 Worker 0 和 Worker k
            io_contexts_[io].idx_normal = io % normal_count_;
            io_contexts_[io].idx_blocking = normal_count_ + (io % (worker_count_ - normal_count_));
        }
    }

    Dispatcher(const Dispatcher &) = delete;
    Dispatcher &operator=(const Dispatcher &) = delete;

    // 返回总 Worker 数量（Normal + Blocking）。
    [[nodiscard]] std::size_t worker_cnt() const noexcept {
        return worker_count_;
    }

    // 按下标获取指定 Worker 的控制块（供 WorkerPool 访问状态字、唤醒号与该 Worker 的各条 SPSC
    // lane）。
    [[nodiscard]] WorkerControl &worker(std::size_t worker_id) noexcept {
        return workers_[worker_id];
    }

    // 只读版本：按下标获取指定 Worker 的控制块。
    [[nodiscard]] const WorkerControl &worker(std::size_t worker_id) const noexcept {
        return workers_[worker_id];
    }

    // 【作用】开启第 io_id 号 I/O 线程的批量提交/合并唤醒作用域（支持嵌套）。
    // 【原理】将本 I/O 线程私有的非原子计数 batch_depth 加 1；期间 try_submit 成功后只把目标
    //         Worker 编号置位进本地 waiting_wake_mask，不立即触发内存栅栏或 futex 唤醒。
    void begin_submit_batch(std::size_t io_id) noexcept {
        if (io_id < io_count_)
            ++io_contexts_[io_id].batch_depth;
    }

    // 【作用】结束当前批量提交作用域，并对本批次所有收到新任务的睡眠 Worker 执行一次合并唤醒。
    // 【原理】嵌套深度减到 0 后取出并清零 waiting_wake_mask：若为空直接零开销返回；否则整批仅执行
    //         1 次 seq_cst 栅栏保证所有 SPSC tail_.store(release) 对 Worker 可见，再用 tzcnt
    //         遍历置位 Worker，逐个调用 wake_worker_after_fence()（正在忙的 Worker 直接跳过）。
    void flush_submit_batch(std::size_t io_id) noexcept {
        if (io_id >= io_count_)
            return;
        IoDispatchContext &io = io_contexts_[io_id];
        if (io.batch_depth == 0 || --io.batch_depth != 0)
            return;

        std::uint64_t mask = std::exchange(io.waiting_wake_mask, 0);
        if (mask == 0)
            return;

        // 封死 Store-Load 重排，确认全部可见才继续
        std::atomic_thread_fence(std::memory_order_seq_cst);
        while (mask != 0) {
            const unsigned worker_id = static_cast<unsigned>(std::countr_zero(mask));
            mask &= (mask - 1);
            wake_worker_after_fence(workers_[worker_id]);
        }
    }

    // 【作用】在非批量场景（或延迟发布测试钩子）下单独检查并按需唤醒指定 Worker。
    // 【原理】先执行 1 次 seq_cst 栅栏确保前置的入队/回滚对目标 Worker 可见，再调用
    //         wake_worker_after_fence() 仅在目标挂有 kSleepingBit 时才触发 notify_one()。
    static void wake_worker(WorkerControl &slot) noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        wake_worker_after_fence(slot);
    }

    // 【作用】供第 io_id 号 I/O 线程调用：非阻塞地将任务分派到最合适的合法 Worker 队列中。
    // 【原理】
    //   1. 选人：调用 select_candidate() 选出候选 Worker（Normal 优先找空闲 Normal，全忙时尝试
    //      借用精确处于 kIdleBit 的 Blocking Worker，否则退回最轻 Normal；Blocking 只选
    //      Blocking）；
    //   2. 占坑（Reservation）：
    //      - 借用路径：使用精确 CAS(kIdleBit, 1, seq_cst, relaxed) 抢占，多人争抢至多 1 人成功；
    //      - 普通路径：直接单条 wait-free fetch_add(1, seq_cst)，彻底消除 CAS 轮询！即使此时
    //        存在 kIdleBit，加 1 后 state == (kIdleBit | count) != kIdleBit，立即自动禁止借用；
    //   3. 查停机：以 seq_cst 复查 stopping，若已停机则立刻回滚计数并返回 false；
    //   4. 入队：调用专属 SPSC lane.try_push()（纯无锁、0 CAS、带游标本地缓存）：
    //      - 成功：若在批量作用域内则仅记录 waiting_wake_mask 位图，否则立刻按需唤醒，返回 true；
    //      - 满队：撤销 reservation，排除该 Worker 继续尝试其他合法候选；所有候选均满返回 false
    //        且保证不消耗原 task。
    [[nodiscard]] bool try_submit(std::size_t io_id, TaskClass task_class, PoolTask &&task,
                                  const std::atomic<bool> &stopping) noexcept {
        if (io_id >= io_count_ || task_class == TaskClass::inline_task || !task)
            return false;

        IoDispatchContext &io = io_contexts_[io_id];
        std::uint64_t excluded_mask = 0;
        const std::size_t max_attempts = worker_count_ * 2;

        for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
            const auto cand = select_candidate(io, task_class, excluded_mask);
            if (!cand)
                return false;

            WorkerControl &slot = workers_[cand->worker_id];
            const std::uint64_t bit = std::uint64_t{1} << cand->worker_id;

            if (cand->borrow) {
                // 借用路径：仅当目标精确处于 kIdleBit（count == 0 且 bit63 == 1）时才允许抢占。
                std::uint64_t expected = kIdleBit;
                if (!slot.state.compare_exchange_strong(expected, 1, std::memory_order_seq_cst,
                                                        std::memory_order_relaxed)) {
                    continue;
                }
            } else {
                // 普通提交路径：允许 kIdleBit 与 outstanding 共存，直接无锁 fetch_add(1) 占坑。
                if ((slot.state.fetch_add(1, std::memory_order_seq_cst) & kCountMask) >=
                    kMaxOutstanding) {
                    rollback_reservation(slot);
                    excluded_mask |= bit;
                    continue;
                }
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

            // 对应 SPSC lane 已满：撤销预留计数，按需唤醒可能等待该预留落地的
            // Worker，再试其他候选。
            rollback_reservation(slot);
            excluded_mask |= bit;
        }

        return false;
    }

  private:
    struct Candidate {
        std::size_t worker_id{0};
        std::uint64_t outstanding{0};
        bool borrow{false};
    };

    // 【作用】在调用方已执行 seq_cst 栅栏的前提下，仅当目标 Worker 处于睡眠态时才执行唤醒。
    // 【原理】先 relaxed 读取 wake_epoch 检查 bit0 (kSleepingBit)：
    //         - 若为 0：说明 Worker 正在运行（或尚未进入睡眠前二次扫描），直接跳过，0 原子写、0
    //         futex；
    //         - 若为 1：通过 CAS 原子清除 kSleepingBit 并将纪元加 kEpochStep(2)，仅赢下 CAS 的
    //           那个生产者执行 1 次 notify_one()，避免多线程同时唤醒同一个沉睡 Worker。
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

    // 【作用】撤销此前占下的任务名额（在 SPSC 入队满失败或撞上停机时调用）。
    // 【原理】将 state 减 1 归还计数，并调用 wake_worker() 通知可能因看到 count > 0 而陷入休眠的
    //         Worker（尤其在停机等待预留归零阶段），防止其错过计数归零事件。
    static void rollback_reservation(WorkerControl &slot) noexcept {
        slot.state.fetch_sub(1, std::memory_order_acq_rel);
        wake_worker(slot);
    }

    // 【作用】在左闭右开区间 [begin, end) 内把轮询游标前进一步，到末尾则绕回 begin。
    // 【原理】用一次比较代替取模除法指令，兼容任意非 2 的幂 Worker 分区大小。
    [[nodiscard]] static std::size_t next_in_range(std::size_t current, std::size_t begin,
                                                   std::size_t end) noexcept {
        return (current + 1 >= end) ? begin : (current + 1);
    }

    // 【作用】按任务类别（Normal / Blocking）挑出本轮最合适的目标 Worker。
    // 【原理】
    //   - Blocking 任务：严禁进入 Normal 区，直接在 Blocking 区 [k, N) 挑 outstanding 最轻者；
    //   - Normal 任务：
    //     1) 先在 Normal 区 [0, k) 挑最轻者，若其 count == 0（完全空闲）直接选用；
    //     2) 若 Normal 全忙，扫描 Blocking 区 [k, N)，谁满足 state == kIdleBit 就标记 borrow=true；
    //     3) 若 Blocking 区也没人精确空闲，退回第 1 步选出的最轻 Normal Worker。
    [[nodiscard]] std::optional<Candidate> select_candidate(IoDispatchContext &io,
                                                            TaskClass task_class,
                                                            std::uint64_t excluded_mask) noexcept {
        if (task_class == TaskClass::blocking) {
            return select_lightest_in_range(normal_count_, worker_count_, io.idx_blocking,
                                            excluded_mask);
        }

        auto best_normal = select_lightest_in_range(0, normal_count_, io.idx_normal, excluded_mask);
        if (best_normal && best_normal->outstanding == 0)
            return best_normal;

        for (std::size_t step = 0, cursor = io.idx_blocking; step < worker_count_ - normal_count_;
             ++step, cursor = next_in_range(cursor, normal_count_, worker_count_)) {
            if ((excluded_mask & (std::uint64_t{1} << cursor)) == 0 &&
                workers_[cursor].state.load(std::memory_order_relaxed) == kIdleBit) {
                io.idx_blocking = next_in_range(cursor, normal_count_, worker_count_);
                return Candidate{.worker_id = cursor, .outstanding = 0, .borrow = true};
            }
        }

        return best_normal;
    }

    // 【作用】在指定 Worker 编号区间 [begin, end) 内，跳过已满 Worker，选出负载（count）最轻者。
    // 【原理】从本 I/O 线程私有游标 rr_cursor 起转一圈，relaxed 读取各 Worker 的 state 并取低 63 位
    //         count，遇到更轻者就更新，遇到 count == 0
    //         提前终止；最后把游标推进到选中者下一位打散热点。
    [[nodiscard]] std::optional<Candidate>
    select_lightest_in_range(std::size_t begin, std::size_t end, std::size_t &rr_cursor,
                             std::uint64_t excluded_mask) noexcept {
        std::size_t cursor = rr_cursor; // 初始化和每次推进都在本区间内。
        std::optional<Candidate> best;

        for (std::size_t step = 0; step < end - begin;
             ++step, cursor = next_in_range(cursor, begin, end)) {
            if ((excluded_mask & (std::uint64_t{1} << cursor)) != 0)
                continue;
            const std::uint64_t raw = workers_[cursor].state.load(std::memory_order_relaxed);
            const std::uint64_t count = raw & kCountMask;
            if (!best || count < best->outstanding) {
                best = Candidate{.worker_id = cursor, .outstanding = count, .borrow = false};
                if (count == 0)
                    break;
            }
        }

        if (best)
            rr_cursor = next_in_range(best->worker_id, begin, end);
        return best;
    }

    const std::size_t io_count_;
    const std::size_t worker_count_;
    const std::size_t normal_count_;

    std::unique_ptr<WorkerControl[]> workers_;
    std::unique_ptr<IoDispatchContext[]> io_contexts_;
};

} // namespace runtime

export module runtime.spsc_queue;

import std;

export namespace runtime {

// x86-64 L1 缓存行大小（64 字节），用于隔离不同线程写热点与相邻槽位，消除伪共享。
inline constexpr std::size_t kCacheLineBytes = 64;

// ============================================================================
// 有界无锁单生产者单消费者环形队列（SpscQueue）
// ============================================================================
// 设计要点（对应 8 x 4 = 32 条 Per-producer lane 矩阵）：
//   1. 每条 lane 固定只有 1 个 I/O 线程写入（Producer）、1 个 Worker 线程读取（Consumer）
//   2. 双端游标本地缓存（Cached Index）：
//      - 生产者缓存线仅由生产者写 tail_ 和非原子 cached_head_；只有当 (tail - cached_head_) >=
//        capacity_（本地视角看似满）时，才跨核 acquire 刷新一次真正的 head_；
//      - 消费者缓存线仅由消费者写 head_ 和非原子 cached_tail_；只有当 head == cached_tail_
//        （本地视角看似空）时，才跨核 acquire 刷新一次真正的 tail_；
//      稳态吞吐下平均每 capacity 次操作才发生 1 次跨核缓存行读取，全路径 0 次 CAS / RMW 指令；
//   4. Cache Line 友好与伪共享消除：
//      - 只读元数据（capacity_/mask_/slots_）、生产者热点、消费者热点分别独占 64B 缓存行；
//      - 每个 Slot 按 64B 对齐，避免低队列深度下相邻 tail 与 head 槽位共享同一缓存行导致乒乓
//        （128 槽位 = 8 KiB/条，32 条共 256 KiB，可完整驻留于 L2/L3 缓存内）；
//   5. 队列满时 try_push() 绝不移动/消耗调用方传入的 T，方便 Dispatcher 立即改投其他候选 Worker。
template <typename T> class SpscQueue {
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>,
                  "SpscQueue<T> requires nothrow move-constructible and move-assignable T");

  public:
    // 【作用】按指定的 2 的幂容量一次性分配全部环形槽位并初始化双端游标。
    // 【原理】校验 capacity >= 2 且 (capacity & (capacity - 1)) == 0，算出位掩码 mask_ =
    //         capacity - 1，再一次堆分配 64B 对齐的 Slot 数组，运行期零动态内存分配。
    explicit SpscQueue(std::size_t capacity)
        : capacity_(validate_capacity(capacity)), mask_(capacity_ - 1),
          slots_(std::make_unique<Slot[]>(capacity_)) {
    }

    SpscQueue(const SpscQueue &) = delete;
    SpscQueue &operator=(const SpscQueue &) = delete;

    // 【作用】返回本队列的固定槽位容量。
    // 【原理】读取构造期确定的只读常量 capacity_。
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    // 【作用】由唯一生产者（对应 I/O 线程）调用：非阻塞地将一个元素压入环形队列。
    // 【原理】
    //   1. 用 relaxed 读取自己独占修改的 tail_，先用本地缓存 cached_head_ 计算 (t - cached_head_)
    //      判断队列是否看似已满；绝大多数未满场景直接命中本核 L1，完全不碰消费者缓存行；
    //   2. 仅当本地缓存显示已满时，才以 acquire 刷新一次真正的 head_（与消费者 release 同步，
    //      确保目标槽位旧对象已析构）；若刷新后仍满则直接返回 false，绝不移动 value；
    //   3. 在 slots_[t & mask_] 就地构造移动进来的对象，最后以 release 发布 tail_ = t + 1。
    [[nodiscard]] bool try_push(T &&value) noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t - cached_head_ >= capacity_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (t - cached_head_ >= capacity_)
                return false;
        }

        slots_[t & mask_].item.emplace(std::move(value));
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // 【作用】由唯一消费者（所属 Worker 线程）调用：非阻塞地从队头取出一个元素。
    // 【原理】
    //   1. 用 relaxed 读取自己独占修改的 head_，先与本地缓存 cached_tail_ 比对是否为空；
    //      批次连续消费时直接命中本核 L1，完全不碰生产者缓存行；
    //   2. 仅当 h == cached_tail_ 时，才以 acquire 刷新一次真正的 tail_（与生产者 release 同步，
    //      确保能看到槽位里刚刚写入的完整对象）；若刷新后仍空则返回 false；
    //   3. 移出对象并立刻 reset() 释放槽位内资源（防止闭包捕获的 shared_ptr 滞留队列），
    //      最后以 release 发布 head_ = h + 1，将该槽位归还给生产者复用。
    [[nodiscard]] bool try_pop(T &out) noexcept {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        if (h == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (h == cached_tail_)
                return false;
        }

        Slot &slot = slots_[h & mask_];
        out = std::move(*slot.item);
        slot.item.reset();
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // 【作用】仅供单元测试使用的白盒钩子：把四套游标统一重置到靠近 UINT64_MAX 的起点。
    // 【原理】仅允许在无并发访问的空队列上调用，把 tail_、cached_head_、head_、cached_tail_
    //         全部设为 start，用于直接验证 64 位无符号游标回绕到 0 时的满/空判定与槽位映射。
    void reset_cursors_for_wrap_test(std::uint64_t start) noexcept {
        tail_.store(start, std::memory_order_relaxed);
        cached_head_ = start;
        head_.store(start, std::memory_order_relaxed);
        cached_tail_ = start;
    }

  private:
    // 单个环形槽位：按 64B 缓存行对齐，避免低队列深度时相邻的 tail 写槽位与 head 读槽位
    // 落在同一缓存行引起跨核伪共享（False Sharing）。
    struct alignas(kCacheLineBytes) Slot {
        std::optional<T> item;
    };

    // 【作用】校验队列容量是否满足 >= 2 且为 2 的幂。
    // 【原理】利用二进制特性 (cap & (cap - 1)) == 0 判定 2 的幂，非法时抛出 invalid_argument。
    static std::size_t validate_capacity(std::size_t capacity) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0)
            throw std::invalid_argument("SpscQueue capacity must be a power of 2 and >= 2");
        return capacity;
    }

    // 缓存行 0（只读元数据）：构造后永不写入，可安全驻留在双方核的 L1 共享（S）态。
    alignas(kCacheLineBytes) const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<Slot[]> slots_;

    // 缓存行 1（生产者独占写热点）：与消费者缓存行物理隔离，消除 tail/head 伪共享。
    alignas(kCacheLineBytes) std::atomic<std::uint64_t> tail_{0};
    std::uint64_t cached_head_{0};

    // 缓存行 2（消费者独占写热点）：与生产者缓存行物理隔离，消除 head/tail 伪共享。
    alignas(kCacheLineBytes) std::atomic<std::uint64_t> head_{0};
    std::uint64_t cached_tail_{0};
};

} // namespace runtime

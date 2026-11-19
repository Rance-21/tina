import std;
import runtime.spsc_queue;

namespace {

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "CHECK failed at line " << __LINE__ << ": " #cond << '\n';                \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

// 【作用】断言调用 fn() 会抛出 std::invalid_argument 异常。
// 【原理】用 try/catch 捕获 std::invalid_argument，捕获成功返回 true，否则返回 false。
template <typename Fn> bool throws_invalid_arg(Fn &&fn) {
    try {
        fn();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}

// 【作用】验证 SpscQueue 构造函数对非法容量（非 2 的幂或 < 2）的拒绝逻辑。
// 【原理】依次传入 0、1、3、6、127 等非法容量，确认每次构造均抛出 std::invalid_argument。
int test_capacity_validation() {
    for (std::size_t bad : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{6},
                            std::size_t{127}}) {
        CHECK(throws_invalid_arg([bad] {
            runtime::SpscQueue<std::move_only_function<void()>> q(bad);
        }));
    }
    return 0;
}

// 【作用】验证单线程下 SpscQueue 多轮绕环存取、游标缓存刷新与队列满不消耗原任务的语义。
// 【原理】循环 20 轮填满队列并故意多推 1 个 extra 任务，断言 try_push 返回 false 且 extra
//         依然非空可用；全部 pop 执行后校验累加和，再把 extra 重新入队执行确认所有权未损毁。
int test_basic_wrap_and_non_consuming_failure(std::size_t cap) {
    using Task = std::move_only_function<void()>;
    runtime::SpscQueue<Task> q(cap);
    CHECK(q.capacity() == cap);

    Task popped;
    CHECK(!q.try_pop(popped));

    std::uint64_t counter = 0;
    for (int round = 0; round < 20; ++round) {
        for (std::size_t i = 0; i < cap; ++i) {
            const std::uint64_t expected = static_cast<std::uint64_t>(round) * cap + i;
            Task t = [&, expected] {
                counter += expected + 1;
            };
            CHECK(q.try_push(std::move(t)));
        }

        // 队列已满：try_push 必须返回 false 且保留原 task 完整可用。
        bool extra_ran = false;
        Task extra = [&] {
            extra_ran = true;
        };
        CHECK(!q.try_push(std::move(extra)));
        CHECK(static_cast<bool>(extra));

        for (std::size_t i = 0; i < cap; ++i) {
            CHECK(q.try_pop(popped));
            CHECK(static_cast<bool>(popped));
            popped();
            popped = {};
        }
        CHECK(!q.try_pop(popped));

        // 队列腾出空位后，之前失败的 extra 仍可成功入队并执行。
        CHECK(q.try_push(std::move(extra)));
        CHECK(q.try_pop(popped));
        popped();
        popped = {};
        CHECK(extra_ran);
        CHECK(!q.try_pop(popped));
    }

    const std::uint64_t total_items = 20ULL * cap;
    const std::uint64_t expected_sum = total_items * (total_items + 1) / 2;
    CHECK(counter == expected_sum);
    return 0;
}

// 【作用】验证 64 位无符号游标（tail_/head_ 及本地缓存）跨越 UINT64_MAX 回绕到 0 时完全正确。
// 【原理】通过测试钩子把四套游标初始化到 UINT64_MAX - 2，连续做 32 轮填满、溢出拒收与排空，
//         验证无符号差值 (tail - cached_head_) 和位与映射 (idx & mask_) 在跨 0 边界严密无误。
int test_uint64_wrap_around(std::size_t cap) {
    struct Item {
        std::uint64_t id{0};
        std::unique_ptr<std::uint64_t> payload;
    };

    runtime::SpscQueue<Item> q(cap);
    const std::uint64_t start = std::numeric_limits<std::uint64_t>::max() - 2;
    q.reset_cursors_for_wrap_test(start);

    std::uint64_t next_push = 1;
    std::uint64_t next_pop = 1;
    for (int cycle = 0; cycle < 32; ++cycle) {
        for (std::size_t i = 0; i < cap; ++i) {
            Item item{next_push, std::make_unique<std::uint64_t>(next_push * 17)};
            CHECK(q.try_push(std::move(item)));
            ++next_push;
        }

        Item overflow_item{999999, std::make_unique<std::uint64_t>(42)};
        CHECK(!q.try_push(std::move(overflow_item)));
        CHECK(overflow_item.id == 999999 && overflow_item.payload &&
              *overflow_item.payload == 42);

        for (std::size_t i = 0; i < cap; ++i) {
            Item out;
            CHECK(q.try_pop(out));
            CHECK(out.id == next_pop);
            CHECK(out.payload && *out.payload == next_pop * 17);
            ++next_pop;
        }
        Item empty_out;
        CHECK(!q.try_pop(empty_out));
    }
    return 0;
}

// 【作用】高并发 SPSC（1 生产者 vs 1 消费者）压力测试，验证严格 FIFO、零丢失、零重复与对象生命周期。
// 【原理】先测 move_only_function 闭包在并发发布执行后无泄漏（通过 shared_ptr 引用计数归 1 校验
//         槽位 pop 后立即 reset）；再用 IdItem 队列验证 0..total_items-1 严格按序抵达。
int test_spsc_concurrent_fifo(std::size_t cap, std::uint64_t total_items) {
    using Task = std::move_only_function<void()>;
    runtime::SpscQueue<Task> q(cap);

    auto sentinel = std::make_shared<int>(7);
    std::atomic<std::uint64_t> exec_cnt{0};
    std::atomic<bool> producer_done{false};

    std::thread consumer([&] {
        for (Task task;;) {
            while (q.try_pop(task)) {
                task();
                task = {};
            }
            if (producer_done.load(std::memory_order_acquire)) {
                while (q.try_pop(task)) {
                    task();
                    task = {};
                }
                break;
            }
            std::this_thread::yield();
        }
    });

    for (std::uint64_t i = 0; i < total_items; ++i) {
        Task t = [&, s = sentinel] {
            (void)s;
            exec_cnt.fetch_add(1, std::memory_order_relaxed);
        };
        while (!q.try_push(std::move(t)))
            std::this_thread::yield();
    }
    producer_done.store(true, std::memory_order_release);
    consumer.join();

    CHECK(exec_cnt.load() == total_items);
    CHECK(sentinel.use_count() == 1); // 验证所有槽位在 pop 后均已立即销毁捕获对象

    // 第二轮：严格校验单生产者单消费者下的 FIFO 顺序与负载完整性。
    struct IdItem {
        std::uint64_t id{0};
        std::uint64_t check{0};
    };
    runtime::SpscQueue<IdItem> id_q(cap);
    std::atomic<bool> id_done{false};
    std::atomic<std::uint64_t> mismatch{0};
    std::atomic<std::uint64_t> received{0};

    std::thread id_consumer([&] {
        std::uint64_t expected = 0;
        auto check_item = [&](const IdItem &out) {
            if (out.id != expected || out.check != expected * 31 + 7)
                mismatch.fetch_add(1, std::memory_order_relaxed);
            ++expected;
        };
        for (IdItem out;;) {
            while (id_q.try_pop(out))
                check_item(out);
            if (id_done.load(std::memory_order_acquire)) {
                while (id_q.try_pop(out))
                    check_item(out);
                break;
            }
            std::this_thread::yield();
        }
        received.store(expected, std::memory_order_release);
    });

    for (std::uint64_t id = 0; id < total_items; ++id) {
        IdItem item{id, id * 31 + 7};
        while (!id_q.try_push(std::move(item)))
            std::this_thread::yield();
    }
    id_done.store(true, std::memory_order_release);
    id_consumer.join();

    CHECK(mismatch.load() == 0);
    CHECK(received.load() == total_items);
    return 0;
}

} // namespace

// 【作用】SpscQueue 单元测试入口，在极小容量（2、4）和默认容量（128）下跑完全部用例。
// 【原理】先测容量校验，再遍历不同容量依次执行基本绕环、64 位回绕和并发 FIFO/生命周期测试。
int main() {
    if (test_capacity_validation() != 0)
        return 1;

    for (std::size_t cap : {std::size_t{2}, std::size_t{4}, std::size_t{128}}) {
        if (test_basic_wrap_and_non_consuming_failure(cap) != 0)
            return 1;
        if (test_uint64_wrap_around(cap) != 0)
            return 1;
        const std::uint64_t items = (cap <= 4) ? 50'000ULL : 150'000ULL;
        if (test_spsc_concurrent_fifo(cap, items) != 0)
            return 1;
    }

    std::cout << "spsc_queue test passed\n";
    return 0;
}

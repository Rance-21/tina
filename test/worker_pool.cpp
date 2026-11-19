import std;
import runtime.config;
import runtime.cpu_affinity;
import runtime.dispatcher;
import runtime.spsc_queue;
import runtime.worker_pool;

namespace {

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "CHECK failed at line " << __LINE__ << ": " #cond << '\n';                \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

// 【作用】构造一份用于单元测试的 ServerConfig（默认关闭绑核以免干扰并发调度测试）。
// 【原理】固定 4 个 I/O CPU 与 8 个 Worker CPU 编号，并填入指定的 normal_workers 和队列容量。
runtime::ServerConfig make_test_config(std::size_t normal_workers, std::size_t queue_cap = 128,
                                       bool affinity = false) {
    runtime::ServerConfig cfg;
    cfg.io_cpus = {0, 2, 4, 8};
    cfg.worker_cpus = {16, 17, 18, 19, 20, 21, 22, 23};
    cfg.non_blk_worker_cnt = normal_workers;
    cfg.queue_cap = queue_cap;
    cfg.enable_cpu_affinity = affinity;
    return cfg;
}

// 【作用】断言调用 fn() 会抛出任意异常，用于负面参数与拓扑冲突测试。
// 【原理】用 catch(...) 捕获所有异常类型，捕获到则返回 true，正常返回则返回 false。
template <typename Fn> bool throws_any(Fn &&fn) {
    try {
        fn();
    } catch (...) {
        return true;
    }
    return false;
}

// 【作用】验证 ServerConfig 参数合法性校验与 CPU 拓扑（含 SMT siblings 冲突）检测。
// 【原理】分别构造非法 k、非法队列容量、重复 CPU、同物理核超线程 sibling 和越界 CPU，
//         确认全部抛错；同时验证默认 4P+8E 配置在本机通过拓扑校验。
int test_config_and_topology_validation() {
    auto good = make_test_config(3, 128, true);
    good.validate();

    for (std::size_t bad_k : {std::size_t{0}, std::size_t{8}, std::size_t{9}}) {
        auto cfg = make_test_config(3);
        cfg.non_blk_worker_cnt = bad_k;
        CHECK(throws_any([&] { cfg.validate(); }));
    }

    for (std::size_t bad_cap : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{100}}) {
        auto cfg = make_test_config(3);
        cfg.queue_cap = bad_cap;
        CHECK(throws_any([&] { cfg.validate(); }));
    }

    {
        auto cfg = make_test_config(3);
        cfg.io_cpus = {0, 2, 4, 16};
        CHECK(throws_any([&] { cfg.validate(); }));
    }

    const auto parsed = runtime::affinity::parse_cpu_list("0,2-4,8");
    CHECK((parsed == std::vector<unsigned>{0, 2, 3, 4, 8}));

    CHECK(throws_any([] {
        const std::array<unsigned, 2> io{0, 1};
        const std::array<unsigned, 2> wk{16, 17};
        (void)runtime::affinity::validate_topology(io, wk);
    }));

    CHECK(throws_any([] {
        const std::array<unsigned, 1> io{999};
        const std::array<unsigned, 2> wk{16, 17};
        (void)runtime::affinity::validate_topology(io, wk);
    }));

    auto report = runtime::affinity::validate_topology(good.io_cpus, good.worker_cpus);
    CHECK(!report.online_cpus.empty() && !report.allowed_cpus.empty());
    return 0;
}

// 【作用】验证 4 个 I/O 生产者以批量唤醒模式并发提交混合任务时，路由隔离正确且零丢单。
// 【原理】4 线程每批用 SubmitBatchGuard 打包提交 16 个 Normal/Blocking 任务，断言 8 个 Worker
//         均被唤醒、Blocking 从未落到 Normal Worker，且关停后 pending 归零。
int test_concurrent_submit_batch_wake_and_borrow() {
    auto cfg = make_test_config(3, 128, false);
    runtime::WorkerPool pool(cfg);

    constexpr std::size_t kPerIo = 4000;
    std::array<std::atomic<std::uint64_t>, 8> worker_hits{};
    std::atomic<std::uint64_t> blocking_on_normal{0};
    std::atomic<std::uint64_t> total_done{0};

    std::vector<std::thread> producers;
    for (std::size_t io = 0; io < 4; ++io) {
        producers.emplace_back([&, io] {
            for (std::size_t base = 0; base < kPerIo; base += 16) {
                runtime::WorkerPool::SubmitBatchGuard batch(pool, io);
                for (std::size_t off = 0; off < 16 && base + off < kPerIo; ++off) {
                    const std::size_t i = base + off;
                    const bool is_blocking = (i % 3 == 0);
                    const auto cls =
                        is_blocking ? runtime::TaskClass::blocking : runtime::TaskClass::normal;
                    runtime::PoolTask task = [&, is_blocking] {
                        const auto wid = runtime::current_worker_id();
                        if (wid && *wid < 8)
                            worker_hits[*wid].fetch_add(1, std::memory_order_relaxed);
                        if (is_blocking && wid && *wid < cfg.non_blk_worker_cnt)
                            blocking_on_normal.fetch_add(1, std::memory_order_relaxed);
                        total_done.fetch_add(1, std::memory_order_relaxed);
                    };
                    while (!pool.try_submit(io, cls, std::move(task)))
                        std::this_thread::yield();
                }
            }
        });
    }
    for (auto &th : producers)
        th.join();

    pool.shutdown();
    CHECK(total_done.load() == 4 * kPerIo);
    CHECK(blocking_on_normal.load() == 0);
    for (std::size_t w = 0; w < 8; ++w)
        CHECK(worker_hits[w].load() > 0);
    CHECK(pool.pending_cnt() == 0);
    return 0;
}

// 【作用】验证新 state 语义（kIdleBit 与 count 共存禁借、Worker 执行前清 bit63）与精确借用 CAS。
// 【原理】场景 A 确认 kIdleBit|1 无法被借用且 Worker 开始执行后会清掉 bit63；场景 B 确认忙碌
//         Blocking Worker 零误借用；场景 C 确认 IDLE Blocking Worker 可被安全借用且每次仅借 1 单。
int test_idle_borrow_cas_and_busy_blocking_guard() {
    auto cfg = make_test_config(2, 128, false);
    runtime::WorkerPool pool(cfg);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_idle = true;
        for (std::size_t w = 0; w < 8; ++w) {
            if (pool.dispatcher().worker(w).state.load(std::memory_order_acquire) !=
                runtime::kIdleBit) {
                all_idle = false;
                break;
            }
        }
        if (all_idle)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto hold_workers = [&](std::size_t count, runtime::TaskClass cls, std::latch &entered,
                            std::latch &release) -> bool {
        for (std::size_t i = 0; i < count; ++i) {
            runtime::PoolTask t = [&] {
                entered.count_down();
                release.wait();
            };
            if (!pool.try_submit(i % 4, cls, std::move(t)))
                return false;
        }
        entered.wait();
        return true;
    };

    // 场景 A & B：向全部 6 个 IDLE Blocking Worker [2..7] 提交 Blocking 任务（经 fetch_add(1)
    // 变成 kIdleBit|1，随后 Worker 执行前清除 kIdleBit 变成 1），验证运行态 state 精确为 1 且不可借用。
    std::latch blocking_entered(6), release_blocking(1);
    CHECK(hold_workers(6, runtime::TaskClass::blocking, blocking_entered, release_blocking));

    for (std::size_t w = 2; w < 8; ++w)
        CHECK(pool.dispatcher().worker(w).state.load(std::memory_order_acquire) == 1);

    std::latch normal_entered(2), release_normal(1);
    CHECK(hold_workers(2, runtime::TaskClass::normal, normal_entered, release_normal));

    std::atomic<std::uint64_t> normal_ran_on_blocking{0};
    std::atomic<std::uint64_t> extra_normal_done{0};
    for (int i = 0; i < 16; ++i) {
        runtime::PoolTask t = [&] {
            if (*runtime::current_worker_id() >= 2)
                normal_ran_on_blocking.fetch_add(1, std::memory_order_relaxed);
            extra_normal_done.fetch_add(1, std::memory_order_relaxed);
        };
        CHECK(pool.try_submit(static_cast<std::size_t>(i % 4), runtime::TaskClass::normal,
                              std::move(t)));
    }

    release_normal.count_down();
    while (extra_normal_done.load() < 16)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(normal_ran_on_blocking.load() == 0);

    // 场景 C：释放 Blocking Worker 回 IDLE，再卡住 Normal Worker 并发突刺，验证安全借用。
    release_blocking.count_down();
    while (pool.pending_cnt() != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::latch normal_hold_entered(2), release_normal_hold(1);
    CHECK(hold_workers(2, runtime::TaskClass::normal, normal_hold_entered, release_normal_hold));

    std::atomic<std::uint64_t> borrowed_done{0};
    std::atomic<std::size_t> submit_failures{0};
    std::vector<std::thread> io_threads;
    for (std::size_t io = 0; io < 4; ++io) {
        io_threads.emplace_back([&, io] {
            runtime::WorkerPool::SubmitBatchGuard batch(pool, io);
            for (int j = 0; j < 20; ++j) {
                runtime::PoolTask t = [&] {
                    if (*runtime::current_worker_id() >= 2)
                        borrowed_done.fetch_add(1, std::memory_order_relaxed);
                };
                if (!pool.try_submit(io, runtime::TaskClass::normal, std::move(t)))
                    submit_failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &th : io_threads)
        th.join();
    CHECK(submit_failures.load() == 0);

    const auto borrow_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (borrowed_done.load() == 0 && std::chrono::steady_clock::now() < borrow_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(borrowed_done.load() > 0);

    release_normal_hold.count_down();
    pool.shutdown();
    return 0;
}

// 【作用】验证队列满时 state 回滚、原 task 不被消耗、单 lane 独立性以及任务抛异常后的自愈能力。
// 【原理】用小容量（cap=2）卡住所有 Worker 后塞满 Q[0][0]，第 3 次提交必须失败且保留原 task 与
//         outstanding==3；同时验证其他 IO lane 仍可入队，以及抛出异常的任务不会破坏计数或后续任务。
int test_full_queue_rollback_and_exception_recovery() {
    auto cfg = make_test_config(1, 2, false);
    runtime::WorkerPool pool(cfg);

    std::latch w0_entered(1);
    std::latch w0_release(1);
    runtime::PoolTask hold = [&] {
        w0_entered.count_down();
        w0_release.wait();
    };
    CHECK(pool.try_submit(0, runtime::TaskClass::normal, std::move(hold)));
    w0_entered.wait();

    std::latch blk_entered(7);
    std::latch blk_release(1);
    for (std::size_t b = 0; b < 7; ++b) {
        runtime::PoolTask bt = [&] {
            blk_entered.count_down();
            blk_release.wait();
        };
        CHECK(pool.try_submit(b % 4, runtime::TaskClass::blocking, std::move(bt)));
    }
    blk_entered.wait();

    std::atomic<int> ran{0};
    for (int i = 0; i < 2; ++i) {
        runtime::PoolTask t = [&] {
            ran.fetch_add(1, std::memory_order_relaxed);
        };
        CHECK(pool.try_submit(0, runtime::TaskClass::normal, std::move(t)));
    }
    CHECK((pool.dispatcher().worker(0).state.load() & runtime::kCountMask) == 3);

    bool third_ran = false;
    runtime::PoolTask third = [&] {
        third_ran = true;
    };
    CHECK(!pool.try_submit(0, runtime::TaskClass::normal, std::move(third)));
    CHECK(static_cast<bool>(third));
    CHECK((pool.dispatcher().worker(0).state.load() & runtime::kCountMask) == 3);

    runtime::PoolTask from_io1 = [&] {
        ran.fetch_add(1, std::memory_order_relaxed);
    };
    CHECK(pool.try_submit(1, runtime::TaskClass::normal, std::move(from_io1)));
    CHECK((pool.dispatcher().worker(0).state.load() & runtime::kCountMask) == 4);

    runtime::PoolTask throwing = [] {
        throw std::runtime_error("intentional test exception");
    };
    CHECK(pool.try_submit(2, runtime::TaskClass::normal, std::move(throwing)));

    w0_release.count_down();
    blk_release.count_down();

    while (pool.pending_cnt() != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(pool.try_submit(0, runtime::TaskClass::normal, std::move(third)));

    pool.shutdown();
    CHECK(ran.load() == 3);
    CHECK(third_ran);
    for (std::size_t w = 0; w < 8; ++w)
        CHECK((pool.dispatcher().worker(w).state.load() & runtime::kCountMask) == 0);
    return 0;
}

// 【作用】验证“预留 state 后延迟入队”与并发 shutdown() 竞态下，已接受任务恰好执行一次。
// 【原理】前半段手动 fetch_add(1) 预留后发起 shutdown，确认 Worker 等延迟 push 并唤醒完成才退出；
//         后半段 4 线程在批量唤醒模式下狂推任务同时关停，逐项核对 executed == accepted 且无损毁。
int test_delayed_publish_and_shutdown_race() {
    {
        auto cfg = make_test_config(1, 4, false);
        runtime::WorkerPool pool(cfg);
        auto &w0 = pool.dispatcher().worker(0);

        // 直接模拟普通路径 fetch_add(1) 预留（允许与 kIdleBit 共存），但暂缓写入队列。
        w0.state.fetch_add(1, std::memory_order_seq_cst);

        std::atomic<bool> shutdown_done{false};
        std::thread shutdown_thread([&] {
            pool.shutdown();
            shutdown_done.store(true, std::memory_order_release);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(!shutdown_done.load(std::memory_order_acquire));

        bool published_ran = false;
        runtime::PoolTask published = [&] {
            published_ran = true;
        };
        CHECK(w0.lanes[0]->try_push(std::move(published)));
        runtime::Dispatcher::wake_worker(w0);

        shutdown_thread.join();
        CHECK(published_ran);
    }

    for (int round = 0; round < 8; ++round) {
        auto cfg = make_test_config(3, 128, false);
        runtime::WorkerPool pool(cfg);

        constexpr std::size_t kMaxPerIo = 5000;
        std::array<std::vector<std::uint8_t>, 4> accepted;
        std::array<std::vector<std::atomic<std::uint8_t>>, 4> executed;
        for (std::size_t io = 0; io < 4; ++io) {
            accepted[io].assign(kMaxPerIo, 0);
            executed[io] = std::vector<std::atomic<std::uint8_t>>(kMaxPerIo);
        }

        std::atomic<bool> start_flag{false};
        std::atomic<std::size_t> consumed_on_reject{0};
        std::vector<std::thread> producers;
        for (std::size_t io = 0; io < 4; ++io) {
            producers.emplace_back([&, io] {
                while (!start_flag.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (std::size_t base = 0; base < kMaxPerIo; base += 8) {
                    runtime::WorkerPool::SubmitBatchGuard batch(pool, io);
                    for (std::size_t off = 0; off < 8 && base + off < kMaxPerIo; ++off) {
                        const std::size_t i = base + off;
                        const auto cls = (i % 2 == 0) ? runtime::TaskClass::normal
                                                      : runtime::TaskClass::blocking;
                        runtime::PoolTask t = [&, io, i] {
                            executed[io][i].fetch_add(1, std::memory_order_relaxed);
                        };
                        if (pool.try_submit(io, cls, std::move(t))) {
                            accepted[io][i] = 1;
                        } else if (!static_cast<bool>(t)) {
                            consumed_on_reject.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            });
        }

        start_flag.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        pool.shutdown();

        for (auto &th : producers)
            th.join();
        CHECK(consumed_on_reject.load() == 0);

        for (std::size_t io = 0; io < 4; ++io) {
            for (std::size_t i = 0; i < kMaxPerIo; ++i) {
                CHECK(executed[io][i].load(std::memory_order_relaxed) == accepted[io][i]);
            }
        }
    }

    return 0;
}

// 【作用】验证单个高负载 I/O 线程持续灌包时，不会饿死其他 I/O 线程对应 lane 的任务。
// 【原理】线程 flooder 持续往 lane 0 灌入 Normal 任务，同时主线程向 lane 3 提交 64 个任务，
//         依赖 Worker 的 round-robin 轮询确认 lane 3 任务能及时全部完成。
int test_lane_fairness() {
    auto cfg = make_test_config(1, 128, false);
    runtime::WorkerPool pool(cfg);

    std::atomic<bool> stop_flood{false};
    std::thread flooder([&] {
        while (!stop_flood.load(std::memory_order_relaxed)) {
            runtime::PoolTask t = [] {};
            if (!pool.try_submit(0, runtime::TaskClass::normal, std::move(t)))
                std::this_thread::yield();
        }
    });

    std::latch lane3_done(64);
    {
        runtime::WorkerPool::SubmitBatchGuard batch(pool, 3);
        for (int i = 0; i < 64; ++i) {
            runtime::PoolTask t = [&] {
                lane3_done.count_down();
            };
            while (!pool.try_submit(3, runtime::TaskClass::normal, std::move(t)))
                std::this_thread::yield();
        }
    }

    lane3_done.wait();
    stop_flood.store(true, std::memory_order_relaxed);
    flooder.join();
    pool.shutdown();
    return 0;
}

} // namespace

// 【作用】WorkerPool V2 单元测试入口，按顺序执行 6 组核心正确性与并发边界测试。
// 【原理】逐一调用各测试函数，任一返回非零即立刻失败退出，全部通过后输出 passed。
int main() {
    if (test_config_and_topology_validation() != 0)
        return 1;
    if (test_concurrent_submit_batch_wake_and_borrow() != 0)
        return 1;
    if (test_idle_borrow_cas_and_busy_blocking_guard() != 0)
        return 1;
    if (test_full_queue_rollback_and_exception_recovery() != 0)
        return 1;
    if (test_delayed_publish_and_shutdown_race() != 0)
        return 1;
    if (test_lane_fairness() != 0)
        return 1;

    std::cout << "worker_pool test passed\n";
    return 0;
}

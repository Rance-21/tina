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

// 构造一份用于单元测试的 ServerConfig（默认关闭绑核以免干扰并发调度测试）。
runtime::ServerConfig make_test_config(std::size_t queue_cap = 128, bool affinity = false) {
    runtime::ServerConfig cfg;
    cfg.io_cpus = {0, 2, 4, 8};
    cfg.worker_cpus = {16, 17, 18, 19, 20, 21, 22, 23};
    cfg.queue_cap = queue_cap;
    cfg.enable_cpu_affinity = affinity;
    return cfg;
}

template <typename Fn> bool throws_any(Fn &&fn) {
    try {
        fn();
    } catch (...) {
        return true;
    }
    return false;
}

// 验证 ServerConfig 参数合法性校验与 CPU 拓扑（含 SMT siblings 冲突）检测。
int test_config_and_topology_validation() {
    auto good = make_test_config(128, true);
    good.validate();

    {
        auto cfg = make_test_config();
        cfg.io_cpus.clear();
        CHECK(throws_any([&] { cfg.validate(); }));
    }

    {
        auto cfg = make_test_config();
        cfg.worker_cpus.clear();
        CHECK(throws_any([&] { cfg.validate(); }));
    }

    for (std::size_t bad_cap : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{100}}) {
        auto cfg = make_test_config();
        cfg.queue_cap = bad_cap;
        CHECK(throws_any([&] { cfg.validate(); }));
    }

    {
        auto cfg = make_test_config();
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

// 验证 4 个 I/O 生产者以批量唤醒模式并发提交任务时，8 个同构 Worker 均分担负载且零丢单。
int test_concurrent_submit_batch_wake() {
    auto cfg = make_test_config(128, false);
    runtime::WorkerPool pool(cfg);

    constexpr std::size_t kPerIo = 4000;
    std::array<std::atomic<std::uint64_t>, 8> worker_hits{};
    std::atomic<std::uint64_t> total_done{0};

    std::vector<std::thread> producers;
    for (std::size_t io = 0; io < 4; ++io) {
        producers.emplace_back([&, io] {
            for (std::size_t base = 0; base < kPerIo; base += 16) {
                runtime::WorkerPool::SubmitBatchGuard batch(pool, io);
                for (std::size_t off = 0; off < 16 && base + off < kPerIo; ++off) {
                    runtime::PoolTask task = [&] {
                        const auto wid = runtime::current_worker_id();
                        if (wid && *wid < 8)
                            worker_hits[*wid].fetch_add(1, std::memory_order_relaxed);
                        total_done.fetch_add(1, std::memory_order_relaxed);
                    };
                    while (!pool.try_submit(io, std::move(task)))
                        std::this_thread::yield();
                }
            }
        });
    }
    for (auto &th : producers)
        th.join();

    pool.shutdown();
    CHECK(total_done.load() == 4 * kPerIo);
    for (std::size_t w = 0; w < 8; ++w)
        CHECK(worker_hits[w].load() > 0);
    CHECK(pool.pending_cnt() == 0);
    return 0;
}

// 验证全池同通道满队时 state 精准回滚、原 task 不被消耗、单 lane 独立性及异常自愈能力。
int test_full_queue_rollback_and_exception_recovery() {
    auto cfg = make_test_config(2, false);
    runtime::WorkerPool pool(cfg);

    std::latch entered(8);
    std::latch release(1);
    for (std::size_t w = 0; w < 8; ++w) {
        runtime::PoolTask hold = [&] {
            entered.count_down();
            release.wait();
        };
        CHECK(pool.try_submit(w % 4, std::move(hold)));
    }
    entered.wait();

    for (std::size_t w = 0; w < 8; ++w)
        CHECK(pool.dispatcher().worker(w).state.load() == 1);

    std::atomic<int> ran{0};
    for (int i = 0; i < 16; ++i) {
        runtime::PoolTask t = [&] {
            ran.fetch_add(1, std::memory_order_relaxed);
        };
        CHECK(pool.try_submit(0, std::move(t)));
    }
    for (std::size_t w = 0; w < 8; ++w)
        CHECK(pool.dispatcher().worker(w).state.load() == 3);

    bool extra_ran = false;
    runtime::PoolTask extra = [&] {
        extra_ran = true;
    };
    CHECK(!pool.try_submit(0, std::move(extra)));
    CHECK(static_cast<bool>(extra));
    for (std::size_t w = 0; w < 8; ++w)
        CHECK(pool.dispatcher().worker(w).state.load() == 3);

    runtime::PoolTask from_io1 = [&] {
        ran.fetch_add(1, std::memory_order_relaxed);
    };
    CHECK(pool.try_submit(1, std::move(from_io1)));
    CHECK(pool.pending_cnt() == 25);

    runtime::PoolTask throwing = [] {
        throw std::runtime_error("intentional test exception");
    };
    CHECK(pool.try_submit(2, std::move(throwing)));

    release.count_down();

    while (pool.pending_cnt() != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(pool.try_submit(0, std::move(extra)));

    pool.shutdown();
    CHECK(ran.load() == 17);
    CHECK(extra_ran);
    for (std::size_t w = 0; w < 8; ++w)
        CHECK(pool.dispatcher().worker(w).state.load() == 0);
    return 0;
}

// 验证“预留 state 后延迟入队”与并发 shutdown() 竞态下，已接受任务恰好执行一次。
int test_delayed_publish_and_shutdown_race() {
    {
        auto cfg = make_test_config(4, false);
        runtime::WorkerPool pool(cfg);
        auto &w0 = pool.dispatcher().worker(0);

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
        auto cfg = make_test_config(128, false);
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
                        runtime::PoolTask t = [&, io, i] {
                            executed[io][i].fetch_add(1, std::memory_order_relaxed);
                        };
                        if (pool.try_submit(io, std::move(t))) {
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

// 验证单个高负载 I/O 线程持续灌包时，不会饿死其他 I/O 线程对应 lane 的任务。
int test_lane_fairness() {
    auto cfg = make_test_config(128, false);
    runtime::WorkerPool pool(cfg);

    std::atomic<bool> stop_flood{false};
    std::thread flooder([&] {
        while (!stop_flood.load(std::memory_order_relaxed)) {
            runtime::PoolTask t = [] {};
            if (!pool.try_submit(0, std::move(t)))
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
            while (!pool.try_submit(3, std::move(t)))
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

int main() {
    if (test_config_and_topology_validation() != 0)
        return 1;
    if (test_concurrent_submit_batch_wake() != 0)
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

import std;
import runtime.config;
import runtime.spsc_queue;
import runtime.worker_pool;

namespace {

using Clock = std::chrono::steady_clock;

// 【作用】测量可调用对象 fn() 从开始到结束的挂钟耗时（单位：秒）。
// 【原理】在调用前后读取单调时钟 steady_clock::now() 并转为 double 秒数返回。
template <typename Fn> double elapsed_seconds(Fn &&fn) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

// 简单局部 mutex+deque 对照池（仅限本队列微基准对比使用）。
class MutexDequePool {
  public:
    using Task = std::move_only_function<void()>;

    // 【作用】启动指定数量的对照工作线程，共同消费一把全局互斥锁保护的 deque 队列。
    // 【原理】每个线程在条件变量 cv_ 上等待队列非空或 stopping_ 置位，加锁弹出队首任务后释放锁并执行。
    explicit MutexDequePool(std::size_t workers) {
        threads_.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) {
            threads_.emplace_back([this] {
                while (true) {
                    Task task;
                    {
                        std::unique_lock lock(mu_);
                        cv_.wait(lock, [this] { return !q_.empty() || stopping_; });
                        if (q_.empty())
                            return;
                        task = std::move(q_.front());
                        q_.pop_front();
                    }
                    task();
                }
            });
        }
    }

    // 【作用】析构时确保所有对照工作线程安全排空并退出。
    // 【原理】直接委托 shutdown()，保证即使调用方未显式关停也不会泄漏线程。
    ~MutexDequePool() {
        shutdown();
    }

    // 【作用】向全局队列提交一个任务并唤醒一个等待中的工作线程。
    // 【原理】加锁把 task 压入 std::deque 尾部，解锁后调用 cv_.notify_one() 唤醒消费者。
    void submit(Task task) {
        {
            std::lock_guard lock(mu_);
            q_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    // 【作用】优雅关闭对照池，等待已入队任务全部执行完并回收所有线程。
    // 【原理】加锁设置 stopping_ = true，调用 notify_all() 广播唤醒，再逐一 join() 并清空线程列表。
    void shutdown() {
        {
            std::lock_guard lock(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto &th : threads_) {
            if (th.joinable())
                th.join();
        }
        threads_.clear();
    }

  private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Task> q_;
    std::vector<std::thread> threads_;
    bool stopping_{false};
};

// 【作用】在单生产者/单消费者（SPSC）场景下对比传统 mutex+deque 与带游标缓存 SpscQueue 的吞吐率。
// 【原理】分别用两套队列传递 items 个累加任务并统计总耗时，校验双方累加结果一致后输出 M ops/s。
void bench_spsc_raw(std::size_t items) {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::move_only_function<void()>> dq;
    bool done = false;
    std::uint64_t sum_mutex = 0;

    const double sec_mutex = elapsed_seconds([&] {
        std::thread consumer([&] {
            while (true) {
                std::move_only_function<void()> task;
                {
                    std::unique_lock lock(mu);
                    cv.wait(lock, [&] { return !dq.empty() || done; });
                    if (dq.empty())
                        return;
                    task = std::move(dq.front());
                    dq.pop_front();
                }
                task();
            }
        });
        for (std::size_t i = 0; i < items; ++i) {
            {
                std::lock_guard lock(mu);
                dq.push_back([&, i] { sum_mutex += i; });
            }
            cv.notify_one();
        }
        {
            std::lock_guard lock(mu);
            done = true;
        }
        cv.notify_one();
        consumer.join();
    });

    runtime::SpscQueue<std::move_only_function<void()>> q(128);
    std::atomic<bool> q_done{false};
    std::uint64_t sum_spsc = 0;

    const double sec_spsc = elapsed_seconds([&] {
        std::thread consumer([&] {
            std::move_only_function<void()> task;
            while (true) {
                if (q.try_pop(task)) {
                    task();
                    task = {};
                    continue;
                }
                if (q_done.load(std::memory_order_acquire)) {
                    while (q.try_pop(task)) {
                        task();
                        task = {};
                    }
                    return;
                }
            }
        });
        for (std::size_t i = 0; i < items; ++i) {
            std::move_only_function<void()> task = [&, i] { sum_spsc += i; };
            while (!q.try_push(std::move(task))) {
            }
        }
        q_done.store(true, std::memory_order_release);
        consumer.join();
    });

    if (sum_mutex != sum_spsc)
        throw std::runtime_error("SPSC microbench sum mismatch");

    std::cout << "[Queue Microbench - SPSC " << items << " tasks]\n"
              << "  mutex+deque : " << std::fixed << std::setprecision(2)
              << (static_cast<double>(items) / sec_mutex / 1e6) << " M ops/s ("
              << (sec_mutex * 1e3) << " ms)\n"
              << "  SpscQueue128: " << (static_cast<double>(items) / sec_spsc / 1e6)
              << " M ops/s (" << (sec_spsc * 1e3) << " ms)\n";
}

// 【作用】在 4 生产者 -> 8 工作线程的矩阵场景下，对比全局锁池与 WorkerPool V2 的空任务调度吞吐。
// 【原理】各起 4 条提交线程向 8 Worker 投递 4*per_io_items 个轻量计数任务（V2 模拟 epoll 每批 16
//         个请求使用 SubmitBatchGuard 合并唤醒），校验总数一致后打印 M tasks/s 与耗时。
void bench_4io_8worker_matrix(std::size_t per_io_items) {
    std::atomic<std::uint64_t> mutex_done{0};
    const double sec_mutex = elapsed_seconds([&] {
        MutexDequePool pool(8);
        std::vector<std::thread> producers;
        for (std::size_t io = 0; io < 4; ++io) {
            producers.emplace_back([&] {
                for (std::size_t i = 0; i < per_io_items; ++i) {
                    pool.submit([&] { mutex_done.fetch_add(1, std::memory_order_relaxed); });
                }
            });
        }
        for (auto &th : producers)
            th.join();
        pool.shutdown();
    });

    runtime::ServerConfig v2_cfg;
    v2_cfg.enable_cpu_affinity = false;
    std::atomic<std::uint64_t> v2_done{0};
    const double sec_v2 = elapsed_seconds([&] {
        runtime::WorkerPool pool(v2_cfg);
        std::vector<std::thread> producers;
        for (std::size_t io = 0; io < 4; ++io) {
            producers.emplace_back([&, io] {
                for (std::size_t base = 0; base < per_io_items; base += 16) {
                    runtime::WorkerPool::SubmitBatchGuard batch(pool, io);
                    for (std::size_t off = 0; off < 16 && base + off < per_io_items; ++off) {
                        runtime::PoolTask task = [&] {
                            v2_done.fetch_add(1, std::memory_order_relaxed);
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
    });

    if (mutex_done.load() != 4 * per_io_items || v2_done.load() != 4 * per_io_items)
        throw std::runtime_error("4x8 pool microbench count mismatch");

    const double total = static_cast<double>(4 * per_io_items);
    std::cout << "[Pool Microbench - 4 producers -> 8 workers (" << (4 * per_io_items)
              << " empty tasks)]\n"
              << "  mutex+deque baseline       : " << std::fixed << std::setprecision(2)
              << (total / sec_mutex / 1e6) << " M tasks/s (" << (sec_mutex * 1e3) << " ms)\n"
              << "  WorkerPool V2 (SPSC+Batch) : " << (total / sec_v2 / 1e6) << " M tasks/s ("
              << (sec_v2 * 1e3) << " ms)\n"
              << "  Note: microbenchmark reflects empty-task queue overhead only, not end-to-end "
                 "RPC throughput.\n";
}

} // namespace

// 【作用】队列微基准入口，依次运行 SPSC 原生队列对比与 4x8 线程池调度对比。
// 【原理】调用 bench_spsc_raw(300'000) 和 bench_4io_8worker_matrix(100'000) 并输出结果到 stdout。
int main() {
    bench_spsc_raw(300'000);
    bench_4io_8worker_matrix(100'000);
    return 0;
}

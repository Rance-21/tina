export module runtime.config;

import std;

export namespace runtime {

// ============================================================================
// Server 运行时配置（4P I/O + 8E 同构 WorkerPool 默认核绑定参数）
// ============================================================================
// 目标拓扑（Intel Core i9-14900HX）：
//   - I/O EventLoop 线程：4 个，绑定至 P-core 偶数逻辑核心 {0, 2, 4, 8}
//     （对应 SMT siblings {1, 3, 5, 9} 建议压测前由 scripts/smt_ctl.sh 下线或隔离）；
//   - Worker 线程：8 个同构工作线程，绑定至 E-core 物理核心 {16, 17, 18, 19, 20, 21, 22, 23}；
//   - 其余 4P+8E 留给操作系统、Benchmark client 等，Acceptor 与异步 Logger 共享 Worker E-core
//   集合。
struct ServerConfig {
    // 4 个 EventLoop 绑定的 Linux logical CPU ID 列表。
    std::vector<unsigned> io_cpus{0, 2, 4, 8};

    // 8 个同构 Worker 绑定的 Linux logical CPU ID 列表（每个核心独占一个 E-core）。
    std::vector<unsigned> worker_cpus{16, 17, 18, 19, 20, 21, 22, 23};

    // 每条 Per-producer SPSC lane 的槽位容量（2 的幂，>= 2）。
    // 默认 8 * 4 * 128 = 4096 总容量。
    std::size_t queue_cap{128};

    // 是否启用 CPU 拓扑校验与绑核（默认开启；绑核失败明确报错）。
    bool enable_cpu_affinity{true};

    // 返回 I/O 线程总数。
    [[nodiscard]] std::size_t io_threads_cnt() const noexcept {
        return io_cpus.size();
    }

    // 返回 Worker 线程总数（每个 Worker 对应 worker_cpus 中的一个核心）。
    [[nodiscard]] std::size_t worker_threads() const noexcept {
        return worker_cpus.size();
    }

    // 在启动前做纯数值层面的配置合法性检查（硬件核是否在线由 cpu_affinity 负责检查）。
    void validate() const {
        if (io_cpus.empty())
            throw std::invalid_argument("io_cpus must not be empty");

        if (worker_cpus.empty())
            throw std::invalid_argument("worker_cpus must not be empty");

        if (queue_cap < 2 || (queue_cap & (queue_cap - 1)) != 0)
            throw std::invalid_argument("queue_capacity must be a power of 2 and >= 2");

        // 检查配置列表内部及两组之间不存在重复的 logical CPU ID。
        std::vector<unsigned> all;
        all.reserve(io_cpus.size() + worker_cpus.size());
        all.insert(all.end(), io_cpus.begin(), io_cpus.end());
        all.insert(all.end(), worker_cpus.begin(), worker_cpus.end());
        std::ranges::sort(all);
        if (std::ranges::adjacent_find(all) != all.end())
            throw std::invalid_argument("duplicate CPU ID found across io_cpus and worker_cpus");
    }
};

} // namespace runtime

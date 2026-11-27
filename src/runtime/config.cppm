export module runtime.config;

import std;

export namespace runtime {

// ============================================================================
// Server 运行时配置（WorkerPool V2 与 4P+8E 核绑定默认参数）
// ============================================================================
// 目标拓扑（Intel Core i9-14900HX）：
//   - I/O EventLoop 线程：4 个，绑定至 P-core 偶数逻辑核心 {0, 2, 4, 8}
//     （对应 SMT siblings {1, 3, 5, 9} 建议压测前由 scripts/smt_ctl.sh 下线或隔离）；
//   - Worker 线程：8 个，绑定至 E-core 物理核心 {16, 17, 18, 19, 20, 21, 22, 23}
//     其中 [0, normal_workers) 为 Normal Worker，[normal_workers, 8) 为 Blocking Worker；
//   - 其余 4P+8E 留给操作系统、Benchmark client 等，Acceptor 与异步 Logger 共享 Worker E-core
//   集合。
struct ServerConfig {
    // 4 个 EventLoop 绑定的 Linux logical CPU ID 列表。
    std::vector<unsigned> io_cpus{0, 2, 4, 8};

    // 8 个 Worker 绑定的 Linux logical CPU ID 列表（每个核心独占一个 E-core）。
    std::vector<unsigned> worker_cpus{16, 17, 18, 19, 20, 21, 22, 23};

    // Normal Worker 数量 k（1 <= k < worker_cpus.size()）；Blocking Worker 数为 8 - k。
    // 经实测穷举（k=1..7），默认采用 2 Normal + 6 Blocking。
    std::size_t non_blk_worker_cnt{2};

    // 每条 Per-producer SPSC lane 的槽位容量（2 的幂，>= 2）。
    // 默认 8 * 4 * 128 = 4096 总容量。
    std::size_t queue_cap{128};

    // 是否启用 CPU 拓扑校验与绑核（默认开启；绑核失败明确报错）。
    bool enable_cpu_affinity{true};

    // 返回 I/O 线程总数。
    [[nodiscard]] std::size_t inline io_threads_cnt() const noexcept {
        return io_cpus.size();
    }

    // 返回 Worker 线程总数（Normal + Blocking）。
    // 每个 Worker 对应 worker_cpus 中的一个核心，数量直接等于 worker_cpus.size()。
    [[nodiscard]] std::size_t inline worker_threads() const noexcept {
        return worker_cpus.size();
    }

    // 计算 Blocking Worker 数量
    [[nodiscard]] inline std::size_t blocking_workers() const noexcept {
        return worker_cpus.size() - non_blk_worker_cnt;
    }

    // 在启动前做纯数值层面的配置合法性检查（硬件核是否在线由 cpu_affinity 负责检查）。
    // 【原理】依次检查：
    //   1. I/O 核非空、Worker 核至少 2 个（保证 Normal 和 Blocking 至少各有 1 个）；
    //   2. 1 <= k < 总 Worker 数；
    //   3. 队列容量 >= 2 且是 2 的幂（通过 n & (n - 1) == 0 判断，方便环形队列用位与替代取模）；
    //   4. 把 io_cpus 和 worker_cpus 合并排序后查相邻元素，确保没有任何 CPU ID 被重复分配。
    void validate() const {
        if (io_cpus.empty())
            throw std::invalid_argument("io_cpus must not be empty");

        if (worker_cpus.size() < 2)
            throw std::invalid_argument("worker_cpus must have at least 2 CPUs");

        if (non_blk_worker_cnt < 1 || non_blk_worker_cnt >= worker_cpus.size()) {
            throw std::invalid_argument("normal_workers must satisfy 1 <= k < worker_cpus.size()");
        }

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

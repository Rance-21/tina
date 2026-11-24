export module bench.mixed_stats;

import std;
import bench.stats;

export namespace bench::mixed {

// ============================================================================
// 客户端侧按请求类别统计的端到端延迟记录器（不占用服务端任何开销）
// ============================================================================
// 仅在 tina_bench ide_mixed 单线程 epoll 循环内追加样本，压测结束时一次性排序计算各分位。
class ClassLatencyRecorder {
  public:
    enum class Bucket : std::size_t {
        hot_inline = 0, // 热文件读且预期切片 <= 32 KiB（服务端 Inline EventLoop 路径）
        hot_normal = 1, // 热文件读且预期切片 > 32 KiB（服务端 Normal Worker 路径）
        cold_read = 2,  // 冷文件读（> 2 MiB，服务端 Blocking Worker 读盘路径）
        directory = 3,  // 目录枚举（缓存命中走 Inline，未命中/变更后走 Blocking）
        file_write = 4, // 原子文件写（服务端 Blocking Worker + fsync）
        terminal = 5,   // 真实 PTY 命令（含 ACK + 输出标记 + 状态事件 + 可选 inotify）
        count = 6,
    };

    // 【作用】初始化各个分类桶并预先 reserve 容量，减少压测进行中 vector 扩容带来的客户端抖动。
    // 【原理】遍历 6 个固定桶，各自预留 reserve_per_bucket 个 uint64_t 纳秒槽位。
    explicit ClassLatencyRecorder(std::size_t reserve_per_bucket = 65536) {
        for (auto &vec : samples_ns_)
            vec.reserve(reserve_per_bucket);
    }

    // 【作用】向指定分类桶追加一条单次请求的端到端耗时（纳秒）。
    // 【原理】按枚举下标直接定位数组内对应 vector 执行 push_back，单线程无锁。
    void record(Bucket bucket, std::uint64_t latency_ns) {
        samples_ns_[static_cast<std::size_t>(bucket)].push_back(latency_ns);
    }

    // 【作用】将另一个压测线程私有的分类延迟样本合并到当前汇总器（压测结束阶段调用）。
    void merge(const ClassLatencyRecorder &other) {
        for (std::size_t i = 0; i < samples_ns_.size(); ++i) {
            auto &dst = samples_ns_[i];
            const auto &src = other.samples_ns_[i];
            dst.insert(dst.end(), src.begin(), src.end());
        }
    }

    // 【作用】对每个非空分类桶计算样本数与 p50 / p95 / p99（微秒），格式化为报表文本段落。
    // 【原理】拷贝各桶样本数组排序后，按线性插值公式计算精确分位点并除以 1000.0 转成微秒输出。
    [[nodiscard]] std::string format_summary() const {
        static constexpr std::array<std::string_view, static_cast<std::size_t>(Bucket::count)>
            labels = {
                "Inline 热读 (<=32K)",
                "Normal 热读 (>32K)",
                "Blocking 冷读 (>2M)",
                "目录读取 (list_dir)",
                "文件写入 (fsync)",
                "终端命令 (PTY)",
            };

        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const auto &vec = samples_ns_[i];
            if (vec.empty())
                continue;

            std::vector<std::uint64_t> sorted = vec;
            std::ranges::sort(sorted);

            auto pct_us = [&](double p) -> double {
                const double rank = (p / 100.0) * static_cast<double>(sorted.size() - 1);
                const auto lo = static_cast<std::size_t>(std::floor(rank));
                const auto hi = static_cast<std::size_t>(std::ceil(rank));
                const double frac = rank - static_cast<double>(lo);
                const double ns = static_cast<double>(sorted[lo]) * (1.0 - frac) +
                                  static_cast<double>(sorted[hi]) * frac;
                return ns / 1000.0;
            };

            out << "  " << pad_display(labels[i], 22) << " : n=" << std::setw(8) << sorted.size()
                << " P50=" << std::setw(8) << pct_us(50.0) << "us"
                << " P95=" << std::setw(8) << pct_us(95.0) << "us"
                << " P99=" << std::setw(8) << pct_us(99.0) << "us\n";
        }
        return out.str();
    }

  private:
    std::array<std::vector<std::uint64_t>, static_cast<std::size_t>(Bucket::count)> samples_ns_;
};

} // namespace bench::mixed

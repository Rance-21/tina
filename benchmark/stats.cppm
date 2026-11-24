export module bench.stats;

import std;

export namespace bench {

struct LatencyBucket {
    std::string_view label;
    std::uint64_t count{0};
    double percentage{0.0};
};

struct BenchSummary {
    std::string workload_name;
    std::string host;
    std::string port;
    std::size_t connections{1};
    std::size_t max_inflight{64};

    double warmup_seconds{0.0};
    double measured_seconds{0.0};
    double total_seconds{0.0};

    std::uint64_t warmup_completed{0};
    std::uint64_t warmup_errors{0};
    std::uint64_t completed{0};
    std::uint64_t errors{0};
    std::uint64_t bytes_sent{0};
    std::uint64_t bytes_received{0};

    double success_rate{0.0};
    double qps{0.0};
    double peak_qps{0.0};
    double min_qps{0.0};

    double tx_mib_per_sec{0.0};
    double rx_mib_per_sec{0.0};
    double total_mib_per_sec{0.0};

    double min_us{0.0};
    double avg_us{0.0};
    double stddev_us{0.0};
    double p50_us{0.0};
    double p75_us{0.0};
    double p90_us{0.0};
    double p95_us{0.0};
    double p99_us{0.0};
    double p999_us{0.0};
    double p9999_us{0.0};
    double max_us{0.0};

    std::vector<std::uint64_t> per_second_qps;
    std::vector<LatencyBucket> histogram;
    std::string workload_details;
};

// Stats：统计完成量、延迟、错误和吞吐。
class Stats {
  public:
    explicit Stats(std::size_t expected_samples = 4'000'000, std::size_t measured_seconds_hint = 30)
        : per_second_counts_(measured_seconds_hint + 2, 0) {
        latencies_ns_.reserve(expected_samples);
    }

    inline void record_warmup(bool ok) noexcept {
        if (ok) {
            ++warmup_completed_;
        } else {
            ++warmup_errors_;
        }
    }

    void record_measured(bool ok, std::uint64_t latency_ns, std::size_t req_bytes,
                         std::size_t resp_bytes, std::size_t second_index) {
        if (!ok) {
            ++errors_;
            return;
        }

        ++completed_;
        bytes_sent_ += req_bytes;
        bytes_received_ += resp_bytes;
        latencies_ns_.push_back(latency_ns);

        if (second_index >= per_second_counts_.size()) {
            per_second_counts_.resize(second_index + 1, 0);
        }
        ++per_second_counts_[second_index];
    }

    inline void record_error(bool in_warmup) noexcept {
        if (in_warmup) {
            ++warmup_errors_;
        } else {
            ++errors_;
        }
    }

    [[nodiscard]] inline std::uint64_t warmup_completed() const noexcept {
        return warmup_completed_;
    }

    [[nodiscard]] inline std::uint64_t completed() const noexcept {
        return completed_;
    }

    [[nodiscard]] inline std::uint64_t errors() const noexcept {
        return errors_;
    }

    void record_event_bytes(std::size_t bytes) noexcept {
        bytes_received_ += bytes;
    }

    // 【作用】将另一个线程私有的 Stats 统计数据无锁合并到当前汇总实例中（压测结束阶段调用）。
    void merge(const Stats &other) {
        warmup_completed_ += other.warmup_completed_;
        warmup_errors_ += other.warmup_errors_;
        completed_ += other.completed_;
        errors_ += other.errors_;
        bytes_sent_ += other.bytes_sent_;
        bytes_received_ += other.bytes_received_;

        latencies_ns_.insert(latencies_ns_.end(), other.latencies_ns_.begin(),
                             other.latencies_ns_.end());
        if (other.per_second_counts_.size() > per_second_counts_.size()) {
            per_second_counts_.resize(other.per_second_counts_.size(), 0);
        }
        for (std::size_t i = 0; i < other.per_second_counts_.size(); ++i) {
            per_second_counts_[i] += other.per_second_counts_[i];
        }
    }

    [[nodiscard]] BenchSummary summarize(std::string_view workload_name, std::string_view host,
                                         std::string_view port, std::size_t connections,
                                         std::size_t max_inflight, double warmup_seconds,
                                         double measured_seconds) const {
        BenchSummary s;
        s.workload_name = std::string(workload_name);
        s.host = std::string(host);
        s.port = std::string(port);
        s.connections = connections;
        s.max_inflight = max_inflight;
        s.warmup_seconds = warmup_seconds;
        s.measured_seconds = measured_seconds;
        s.total_seconds = warmup_seconds + measured_seconds;

        s.warmup_completed = warmup_completed_;
        s.warmup_errors = warmup_errors_;
        s.completed = completed_;
        s.errors = errors_;
        s.bytes_sent = bytes_sent_;
        s.bytes_received = bytes_received_;

        const auto total_attempts = completed_ + errors_;
        s.success_rate = total_attempts > 0
                             ? (static_cast<double>(completed_) * 100.0 /
                                static_cast<double>(total_attempts))
                             : 0.0;

        if (measured_seconds > 0.0) {
            s.qps = static_cast<double>(completed_) / measured_seconds;
            constexpr double mib = 1024.0 * 1024.0;
            s.tx_mib_per_sec = (static_cast<double>(bytes_sent_) / mib) / measured_seconds;
            s.rx_mib_per_sec = (static_cast<double>(bytes_received_) / mib) / measured_seconds;
            s.total_mib_per_sec = s.tx_mib_per_sec + s.rx_mib_per_sec;
        }

        const auto full_seconds = static_cast<std::size_t>(std::floor(measured_seconds));
        if (full_seconds > 0) {
            s.per_second_qps.assign(
                per_second_counts_.begin(),
                per_second_counts_.begin() +
                    static_cast<std::ptrdiff_t>(std::min(full_seconds, per_second_counts_.size())));
            if (!s.per_second_qps.empty()) {
                auto [min_it, max_it] =
                    std::ranges::minmax_element(s.per_second_qps);
                s.min_qps = static_cast<double>(*min_it);
                s.peak_qps = static_cast<double>(*max_it);
            }
        }

        if (latencies_ns_.empty()) {
            return s;
        }

        std::vector<std::uint64_t> sorted = latencies_ns_;
        std::ranges::sort(sorted);

        auto percentile_us = [&](double p) -> double {
            const double rank = (p / 100.0) * static_cast<double>(sorted.size() - 1);
            const auto lo = static_cast<std::size_t>(std::floor(rank));
            const auto hi = static_cast<std::size_t>(std::ceil(rank));
            const double frac = rank - static_cast<double>(lo);
            const double ns = static_cast<double>(sorted[lo]) * (1.0 - frac) +
                              static_cast<double>(sorted[hi]) * frac;
            return ns / 1000.0;
        };

        s.min_us = static_cast<double>(sorted.front()) / 1000.0;
        s.max_us = static_cast<double>(sorted.back()) / 1000.0;
        s.p50_us = percentile_us(50.0);
        s.p75_us = percentile_us(75.0);
        s.p90_us = percentile_us(90.0);
        s.p95_us = percentile_us(95.0);
        s.p99_us = percentile_us(99.0);
        s.p999_us = percentile_us(99.9);
        s.p9999_us = percentile_us(99.99);

        double sum_ns = 0.0;
        for (const auto ns : sorted) {
            sum_ns += static_cast<double>(ns);
        }
        const double mean_ns = sum_ns / static_cast<double>(sorted.size());
        s.avg_us = mean_ns / 1000.0;

        double sq_diff_sum = 0.0;
        for (const auto ns : sorted) {
            const double diff = static_cast<double>(ns) - mean_ns;
            sq_diff_sum += diff * diff;
        }
        s.stddev_us = std::sqrt(sq_diff_sum / static_cast<double>(sorted.size())) / 1000.0;

        std::array<std::uint64_t, 7> bucket_counts{};
        for (const auto ns : sorted) {
            const auto us = ns / 1000;
            if (us < 20) {
                ++bucket_counts[0];
            } else if (us < 50) {
                ++bucket_counts[1];
            } else if (us < 100) {
                ++bucket_counts[2];
            } else if (us < 200) {
                ++bucket_counts[3];
            } else if (us < 500) {
                ++bucket_counts[4];
            } else if (us < 1000) {
                ++bucket_counts[5];
            } else {
                ++bucket_counts[6];
            }
        }

        static constexpr std::array<std::string_view, 7> labels = {
            "< 20 us      ", "20 - 50 us   ", "50 - 100 us  ", "100 - 200 us ",
            "200 - 500 us ", "500 - 1000 us", ">= 1000 us   "};

        s.histogram.reserve(labels.size());
        const double total_samples = static_cast<double>(sorted.size());
        for (std::size_t i = 0; i < labels.size(); ++i) {
            s.histogram.push_back(LatencyBucket{
                .label = labels[i],
                .count = bucket_counts[i],
                .percentage = (static_cast<double>(bucket_counts[i]) * 100.0) / total_samples,
            });
        }

        return s;
    }

  private:
    std::uint64_t warmup_completed_{0};
    std::uint64_t warmup_errors_{0};
    std::uint64_t completed_{0};
    std::uint64_t errors_{0};
    std::uint64_t bytes_sent_{0};
    std::uint64_t bytes_received_{0};

    std::vector<std::uint64_t> latencies_ns_;
    std::vector<std::uint64_t> per_second_counts_;
};

// 【作用】按终端显示宽度（ASCII 占 1 列、UTF-8 中文字符占 2 列）在右侧补齐空格，保证中英混排报表冒号对齐。
[[nodiscard]] std::string pad_display(std::string_view text, std::size_t target_cols) {
    std::size_t cols = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            cols += 1;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            cols += 1;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            cols += 2;
            i += 3;
        } else {
            cols += 2;
            i += 4;
        }
    }
    std::string result(text);
    if (cols < target_cols)
        result.append(target_cols - cols, ' ');
    return result;
}

std::string format_report(const BenchSummary &s) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    auto kv = [&](std::string_view label) -> std::ostream & {
        return out << "  " << pad_display(label, 22) << " : ";
    };

    out << "====================================================================\n";
    out << "                         Tina RPC 压测报告                          \n";
    out << "====================================================================\n\n";

    out << "[1. 测试配置]\n";
    kv("目标地址") << s.host << ":" << s.port << "\n";
    kv("负载类型") << s.workload_name << "\n";
    kv("TCP 连接数") << s.connections << "\n";
    kv("单连接最大 Inflight") << s.max_inflight << "\n";
    kv("总最大并发") << (s.connections * s.max_inflight) << "\n";
    kv("总时长") << s.total_seconds << " s\n";
    kv("预热时长") << s.warmup_seconds << " s\n";
    kv("正式测量时长") << s.measured_seconds << " s\n\n";

    out << "[2. 吞吐与请求汇总]\n";
    kv("预热完成数") << s.warmup_completed << " reqs (错误: " << s.warmup_errors << ")\n";
    kv("测量完成数") << s.completed << " reqs\n";
    kv("测量错误数") << s.errors << " reqs\n";
    kv("成功率") << s.success_rate << " %\n";
    kv("平均 QPS") << s.qps << " req/s\n";
    kv("峰值 1s QPS") << s.peak_qps << " req/s\n";
    kv("最低 1s QPS") << s.min_qps << " req/s\n";
    kv("TX 吞吐") << s.tx_mib_per_sec << " MiB/s (" << s.bytes_sent << " bytes)\n";
    kv("RX 吞吐") << s.rx_mib_per_sec << " MiB/s (" << s.bytes_received << " bytes)\n";
    kv("总带宽") << s.total_mib_per_sec << " MiB/s\n\n";

    out << "[3. 延迟分布 (us / ms)]\n";
    auto print_lat = [&](std::string_view name, double us) {
        kv(name) << std::right << std::setw(10) << std::fixed << std::setprecision(2) << us
                 << " us  (" << std::setw(8) << std::setprecision(4) << (us / 1000.0) << " ms)\n";
    };
    print_lat("Min", s.min_us);
    print_lat("Avg", s.avg_us);
    print_lat("StdDev", s.stddev_us);
    print_lat("P50", s.p50_us);
    print_lat("P75", s.p75_us);
    print_lat("P90", s.p90_us);
    print_lat("P95", s.p95_us);
    print_lat("P99", s.p99_us);
    print_lat("P99.9", s.p999_us);
    print_lat("P99.99", s.p9999_us);
    print_lat("Max", s.max_us);
    out << "\n";

    if (!s.histogram.empty()) {
        out << "[4. 延迟直方图]\n";
        for (const auto &b : s.histogram) {
            out << "  " << b.label << " : " << std::setw(10) << b.count << "  (" << std::setw(6)
                << std::fixed << std::setprecision(2) << b.percentage << " %)\n";
        }
        out << "\n";
    }

    if (!s.per_second_qps.empty()) {
        out << "[5. 每秒测量 QPS 曲线]\n";
        for (std::size_t i = 0; i < s.per_second_qps.size(); ++i) {
            out << "  第 " << std::setw(2) << (i + 1) << " 秒 (t=" << std::setw(2)
                << (static_cast<std::size_t>(std::round(s.warmup_seconds)) + i + 1)
                << "s) : " << std::setw(10) << s.per_second_qps[i] << " req/s\n";
        }
        out << "\n";
    }

    if (!s.workload_details.empty())
        out << "[6. 混合负载明细]\n" << s.workload_details << "\n";

    out << "====================================================================\n";
    return out.str();
}

void write_report_file(const std::filesystem::path &path, const BenchSummary &summary) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("failed to open output report file: " + path.string());
    }
    file << format_report(summary);
    file.flush();
    if (!file) {
        throw std::runtime_error("failed to write output report file: " + path.string());
    }
}

} // namespace bench

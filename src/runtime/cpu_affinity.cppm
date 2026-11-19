module;
#include <errno.h>
#include <pthread.h>
#include <sched.h>

export module runtime.cpu_affinity;

import std;

export namespace runtime::affinity {

// ============================================================================
// CPU 拓扑校验报告与工具函数（Linux sysfs + sched_getaffinity + pthread）
// ============================================================================
struct CpuTopoReport {
    std::vector<unsigned> online_cpus;
    std::vector<unsigned> allowed_cpus;
    // 非致命提示：例如某 I/O 核心的 SMT sibling 仍处于 online 状态，建议压测前隔离。
    std::vector<std::string> warnings;
};

namespace detail {

// 去掉字符串首尾的空格、制表符和换行符。
// 用 string_view 移动首尾边界，不反复拷贝，最后只生成一次干净的 std::string。
[[nodiscard]] inline std::string trim_ascii(std::string_view text) noexcept {
    constexpr std::string_view kWs = " \t\r\n";
    const std::size_t first = text.find_first_not_of(kWs);
    if (first == std::string_view::npos)
        return {};
    return std::string(text.substr(first, text.find_last_not_of(kWs) - first + 1));
}

// 把纯数字文本（如 "16"）严格解析成无符号整数 CPU ID。
// 用 std::from_chars 做无区域依赖的高速解析，并要求整段字符全部消耗完，防止 "16abc" 混过。
[[nodiscard]] inline unsigned str_to_uint(std::string_view token) {
    if (token.empty())
        throw std::invalid_argument("empty CPU ID token");
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (ec != std::errc{} || ptr != token.data() + token.size())
        throw std::invalid_argument("invalid CPU ID token: " + std::string(token));
    return value;
}

// 读取 Linux /sys 下的单行小型虚拟文件（如 online、thread_siblings_list）。
// 打开文件读取第一行并裁剪末尾换行；若文件不存在或无权限则返回 nullopt。
[[nodiscard]] inline std::optional<std::string> read_sysfs_file(const char *path) {
    std::ifstream file(path);
    if (!file)
        return std::nullopt;
    std::string line;
    std::getline(file, line);
    return detail::trim_ascii(line);
}

// 判断某个 CPU ID 是否在给定的列表里。
// 列表规模很小（最多几十个核），直接线性扫描即可，缓存友好且无需建哈希表。
[[nodiscard]] inline bool contains_core(std::span<const unsigned> list, unsigned cpu) noexcept {
    return std::ranges::find(list, cpu) != list.end();
}

} // namespace detail

// 解析 Linux 标准的 CPU 列表字符串（例如 "0-31" 或 "0,2,4-7"），返回升序去重的 CPU 数组。
// 按逗号拆成若干片段：
//   - 不含 '-' 的单个数字直接解析加入；
//   - 含 '-' 的区间（如 "16-23"）拆出起点和终点并展开加入；
//   - 最后统一排序 + unique 去重，屏蔽重复或乱序输入。
[[nodiscard]] inline std::vector<unsigned> parse_cpu_list(std::string_view text) {
    const std::string trimmed = detail::trim_ascii(text);
    if (trimmed.empty())
        return {};

    std::vector<unsigned> result;
    std::size_t pos = 0;
    while (pos <= trimmed.size()) {
        const std::size_t comma = trimmed.find(',', pos);
        const std::string_view item = (comma == std::string_view::npos)
                                          ? std::string_view(trimmed).substr(pos)
                                          : std::string_view(trimmed).substr(pos, comma - pos);
        if (item.empty())
            throw std::invalid_argument("empty segment in CPU list: " + trimmed);

        const std::size_t dash = item.find('-');
        if (dash == std::string_view::npos) {
            result.push_back(detail::str_to_uint(item));
        } else {
            const unsigned lo = detail::str_to_uint(item.substr(0, dash));
            const unsigned hi = detail::str_to_uint(item.substr(dash + 1));
            if (lo > hi)
                throw std::invalid_argument("inverted CPU range in list: " + std::string(item));
            for (unsigned cpu = lo; cpu <= hi; ++cpu)
                result.push_back(cpu);
        }

        if (comma == std::string_view::npos)
            break;
        pos = comma + 1;
    }

    std::ranges::sort(result);
    auto dup = std::ranges::unique(result);
    result.erase(dup.begin(), dup.end());
    return result;
}

// 获取本机当前处于在线状态的所有逻辑 CPU 编号。
// 读取内核暴露的 /sys/devices/system/cpu/online 文件（例如 "0-31"）并用 parse_cpu_list 展开。
[[nodiscard]] inline std::vector<unsigned> read_online_cpus() {
    auto content = detail::read_sysfs_file("/sys/devices/system/cpu/online");
    if (!content || content->empty())
        throw std::runtime_error("failed to read /sys/devices/system/cpu/online");
    return parse_cpu_list(*content);
}

// 查询操作系统允许当前进程使用哪些逻辑 CPU（受 taskset 或 cgroup cpuset 限制）。
// 调用 Linux 系统调用 sched_getaffinity(0, ...) 获取位图 mask，再逐位检查 CPU_ISSET 转成列表。
[[nodiscard]] std::vector<unsigned> read_cpus_affinity() {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (::sched_getaffinity(0, sizeof(mask), &mask) != 0)
        throw std::system_error(errno, std::generic_category(), "sched_getaffinity");

    std::vector<unsigned> allowed;
    for (unsigned cpu = 0; cpu < static_cast<unsigned>(CPU_SETSIZE); ++cpu) {
        if (CPU_ISSET(cpu, &mask))
            allowed.push_back(cpu);
    }
    return allowed;
}

// 查询某个逻辑 CPU 所属物理核上所有的超线程兄弟（SMT siblings）。
// 读取 /sys/devices/system/cpu/cpu<N>/topology/thread_siblings_list：
//   - 对于 P-core CPU 0，内容通常是 "0-1"（表示 0 和 1 共享同一颗物理大核）；
//   - 对于 E-core CPU 16，内容通常是 "16"（E 核无超线程，只有自己）。
[[nodiscard]] std::optional<std::vector<unsigned>> read_smt_support(unsigned cpu) {
    const std::string path =
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list";
    auto content = detail::read_sysfs_file(path.c_str());
    if (!content || content->empty())
        return std::nullopt;
    return parse_cpu_list(*content);
}

// 把 C++ 的 CPU ID 列表打包成 Linux pthread/sched API 所需的位图结构 cpu_set_t。
// 先用 CPU_ZERO 清零位图，再对每个合法范围内的 CPU ID 调用 CPU_SET 置 1。
[[nodiscard]] cpu_set_t make_cpus_bit_set(std::span<const unsigned> cpus) {
    if (cpus.empty())
        throw std::invalid_argument("cannot build empty cpu_set_t");
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned cpu : cpus) {
        if (cpu >= static_cast<unsigned>(CPU_SETSIZE))
            throw std::out_of_range("CPU ID exceeds CPU_SETSIZE: " + std::to_string(cpu));
        CPU_SET(cpu, &set);
    }
    return set;
}

// 服务端启动前的硬件拓扑总检查，防止绑核绑到不存在、已下线或互相抢核的 CPU 上。
// 分三步校验：
//   1. 读取在线核（online）与进程可用核（affinity mask），确认每个配置的 I/O 和 Worker
//   核都在两个集合内；
//   2. 逐个查 thread_siblings_list，若我们分配的两个线程落到了同一物理核的一对超线程上（如 0 和
//   1），直接报错；
//   3. 若某个 I/O 核（如 0,2,4,8）的超线程兄弟（1,3,5,9）依然在线，记录一条告警提醒压测时隔离。
[[nodiscard]] CpuTopoReport validate_topology(std::span<const unsigned> io_cpus,
                                              std::span<const unsigned> worker_cpus) {
    CpuTopoReport report;
    report.online_cpus = read_online_cpus();
    report.allowed_cpus = read_cpus_affinity();

    std::vector<unsigned> assigned;
    assigned.reserve(io_cpus.size() + worker_cpus.size());
    assigned.insert(assigned.end(), io_cpus.begin(), io_cpus.end());
    assigned.insert(assigned.end(), worker_cpus.begin(), worker_cpus.end());

    auto check_cpu = [&](unsigned cpu, std::string_view role) {
        if (cpu >= static_cast<unsigned>(CPU_SETSIZE)) {
            throw std::runtime_error(std::string(role) + " CPU " + std::to_string(cpu) +
                                     " exceeds CPU_SETSIZE");
        }
        if (!detail::contains_core(report.online_cpus, cpu)) {
            throw std::runtime_error(std::string(role) + " CPU " + std::to_string(cpu) +
                                     " is offline or does not exist");
        }
        if (!detail::contains_core(report.allowed_cpus, cpu)) {
            throw std::runtime_error(std::string(role) + " CPU " + std::to_string(cpu) +
                                     " is outside process affinity mask / cpuset");
        }
    };

    for (unsigned cpu : io_cpus)
        check_cpu(cpu, "IO");
    for (unsigned cpu : worker_cpus)
        check_cpu(cpu, "Worker");

    // 验证任意两个已分配的逻辑核心不是同一物理核上的 SMT siblings。
    for (unsigned cpu : assigned) {
        auto siblings = read_smt_support(cpu);
        if (!siblings) {
            report.warnings.push_back("CPU " + std::to_string(cpu) +
                                      " topology thread_siblings_list unavailable");
            continue;
        }
        for (unsigned sib : *siblings) {
            if (sib != cpu && detail::contains_core(assigned, sib)) {
                throw std::runtime_error("CPU " + std::to_string(cpu) + " and CPU " +
                                         std::to_string(sib) +
                                         " are SMT siblings sharing one physical core");
            }
        }
    }

    // 检查 I/O 核心的超线程兄弟是否仍然在线（压测前可选下线 1,3,5,9）。
    for (unsigned cpu : io_cpus) {
        auto siblings = read_smt_support(cpu);
        if (!siblings)
            continue;
        for (unsigned sib : *siblings) {
            if (sib != cpu && detail::contains_core(report.online_cpus, sib)) {
                report.warnings.push_back(
                    "IO CPU " + std::to_string(cpu) + " has online SMT sibling CPU " +
                    std::to_string(sib) +
                    " (run scripts/smt_ctl.sh offline for strict core isolation)");
            }
        }
    }

    return report;
}

// 把当前线程限制在一组逻辑 CPU 内调度（例如让 Acceptor 和异步日志共享 E-core 集合 16~23）。
// 将多个核打入同一个 cpu_set_t，通过 pthread_setaffinity_np 设定后回读校验。
void pin_thread_to_cpus(std::span<const unsigned> cpus) {
    const cpu_set_t target = make_cpus_bit_set(cpus);
    const pthread_t self = ::pthread_self();

    if (int rc = ::pthread_setaffinity_np(self, sizeof(cpu_set_t), &target); rc != 0)
        throw std::system_error(rc, std::generic_category(), "pthread_setaffinity_np failed");

    cpu_set_t actual;
    CPU_ZERO(&actual);
    if (int rc = ::pthread_getaffinity_np(self, sizeof(cpu_set_t), &actual); rc != 0)
        throw std::system_error(rc, std::generic_category(), "pthread_getaffinity_np failed");

    if (!CPU_EQUAL(&target, &actual))
        throw std::runtime_error("CPU affinity verification mismatch");
}

// 把当前正在运行的线程钉死在单个逻辑 CPU 上（用于 EventLoop 和 Worker 线程）。
// 调用 pthread_setaffinity_np 下发单核位图给内核调度器，随后立即调 pthread_getaffinity_np
//         回读比对（CPU_EQUAL），保证绑核真正生效，失败则直接抛异常。
void pin_thread_to_cpu(unsigned cpu_id) {
    pin_thread_to_cpus(std::span<const unsigned>{&cpu_id, 1});
}

// 查询当前线程此刻正跑在哪个逻辑 CPU 上。
// 封装 Linux vDSO 轻量调用 sched_getcpu()，无需陷入内核态。
[[nodiscard]] inline int cur_running_cpu() noexcept {
    return ::sched_getcpu();
}

// RAII 管理 pthread_attr_t，用于在 pthread_create 创建线程之初就设定好目标 CPU 核。
// 相比线程跑起来之后再绑核，使用 pthread_attr_setaffinity_np
// 可以让新线程从第一条指令起就落在
// 指定核心上，避免启动瞬间的跨核迁移和 L1/L2 缓存污染；析构时自动 destroy
// 防止属性对象泄漏。
class ScopedThreadAttr {
  public:
    // 初始化默认的线程属性结构体。
    ScopedThreadAttr() {
        const int rc = ::pthread_attr_init(&attr_);
        if (rc != 0)
            throw std::system_error(rc, std::generic_category(), "pthread_attr_init");
    }

    // 销毁线程属性结构体，释放内部资源。
    ~ScopedThreadAttr() {
        (void)::pthread_attr_destroy(&attr_);
    }

    ScopedThreadAttr(const ScopedThreadAttr &) = delete;
    ScopedThreadAttr &operator=(const ScopedThreadAttr &) = delete;

    // 往线程属性里写入单个目标 CPU 的亲和性掩码。
    // 把 cpu_id 转成单核 cpu_set_t 后调用 pthread_attr_setaffinity_np 附加到 attr_ 上。
    void set_affinity(unsigned cpu_id) {
        const cpu_set_t set = make_cpus_bit_set(std::span<const unsigned>{&cpu_id, 1});
        if (const int rc = ::pthread_attr_setaffinity_np(&attr_, sizeof(cpu_set_t), &set); rc != 0) {
            throw std::system_error(rc, std::generic_category(),
                                    "pthread_attr_setaffinity_np failed for CPU " +
                                        std::to_string(cpu_id));
        }
    }

    // 返回底层 pthread_attr_t 指针，供 pthread_create 直接传入。
    [[nodiscard]] const pthread_attr_t *get() const noexcept {
        return &attr_;
    }

  private:
    pthread_attr_t attr_{};
};

} // namespace runtime::affinity

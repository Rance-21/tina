module;
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

export module rpc.server;

import std;

import net.socket;
import net.epoll;
import net.event_loop;
import net.event_loop_thread_pool;

import runtime.config;
import runtime.cpu_affinity;
import runtime.worker_pool;
import runtime.log;

import rpc.connection;
import rpc.handler;
import rpc.task_classifier;
import service.dir_watcher;
import ide.server_terminal;

namespace {

// ============================================================================
// 多线程安全退出信号管理器（pthread_sigmask + signalfd）
// ============================================================================
// 在派生任何后台线程前屏蔽 SIGINT / SIGTERM，把异步信号转化为可读文件描述符，
// 交由 Acceptor 的 epoll_wait 像处理普通网络事件一样同步触发优雅停机。
class StopSignals {
  public:
    // 【作用】屏蔽本线程（及后续所有子线程继承）的 SIGINT/SIGTERM，并创建非阻塞 signalfd。
    // 【原理】先调 pthread_sigmask(SIG_BLOCK) 拦截默认杀进程行为，再通过 signalfd() 接管这两个信号。
    StopSignals() {
        ::sigemptyset(&mask_);
        ::sigaddset(&mask_, SIGINT);
        ::sigaddset(&mask_, SIGTERM);

        int rc = ::pthread_sigmask(SIG_BLOCK, &mask_, &previous_mask_);
        if (rc != 0)
            throw std::system_error(rc, std::generic_category(), "pthread_sigmask");

        fd_ = ::signalfd(-1, &mask_, SFD_NONBLOCK | SFD_CLOEXEC);
        if (fd_ < 0) {
            int error = errno;
            ::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
            throw std::system_error(error, std::generic_category(), "signalfd");
        }
    }

    // 【作用】关闭 signalfd 并恢复进程原本的信号掩码。
    ~StopSignals() {
        ::close(fd_);
        ::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
    }

    StopSignals(const StopSignals &) = delete;
    StopSignals &operator=(const StopSignals &) = delete;

    // 返回供 epoll 监听的 signalfd 句柄。
    [[nodiscard]] inline int fd() const noexcept {
        return fd_;
    }

    // 【作用】当 signalfd 触发可读事件时，从内核读走对应的 signalfd_siginfo 结构体。
    // 【原理】循环 ::read 直到完整读出一条信号记录（遇到 EINTR 自动重试）。
    void consume() {
        signalfd_siginfo info{};
        while (true) {
            ssize_t n = ::read(fd_, &info, sizeof(info));
            if (n == sizeof(info))
                return;
            if (n < 0 && errno == EINTR)
                continue;
            throw std::runtime_error("failed to read stop signal");
        }
    }

  private:
    sigset_t mask_{};
    sigset_t previous_mask_{};
    int fd_{-1};
};

// 【作用】将 CPU ID 数组格式化为便于启动日志阅读的 "{0,2,4,8}" 字符串。
std::string format_cpu_list(std::span<const unsigned> cpus) {
    std::ostringstream out;
    out << '{';
    for (std::size_t i = 0; i < cpus.size(); ++i) {
        if (i > 0)
            out << ',';
        out << cpus[i];
    }
    out << '}';
    return out.str();
}

} // namespace

export namespace rpc {

// 【作用】Tina RPC 服务端主生命周期函数：负责拓扑校验、核绑定、启动各线程池、接收连接与五阶段优雅停机。
// 【原理】按严格依赖顺序推进：
//   启动阶段：
//     1. 校验 ServerConfig，第一时间创建 StopSignals 屏蔽退出信号（必须早于所有后台线程创建）；
//     2. 校验物理核与 SMT 拓扑，将当前 Acceptor 主线程绑定到 Worker E-core 集合（后续 spdlog 异步线程会继承）；
//     3. 依次启动 4 个绑核 EventLoop 线程（P-core 0,2,4,8）和 8 个绑核 Worker 线程（E-core 16~23）；
//     4. 为每个 I/O 线程挂好专属 DirWatcher / TerminalManager（及 slots[0] 上的全局 FileWatcher），用 latch 等就绪；
//   运行阶段：
//     Acceptor 在 accept_epoll.wait() 中监听新连接与退出信号，单次最多 accept 64 条连接并按普通 Round Robin
//     轮流投递给 4 个 EventLoop；
//   五阶段优雅停机（收到 SIGINT/SIGTERM 后）：
//     阶段 1 停监听 -> 阶段 2 冻结所有 I/O 线程不再接收新请求/提交新任务 -> 阶段 3 排空并 join WorkerPool ->
//     阶段 4 等待 I/O 线程发完剩余响应（超时 5 秒强关慢连接）-> 阶段 5 停止并 join 所有 I/O 线程。
void run_server(std::string_view port, runtime::ServerConfig config = {}) {
    config.validate();

    // 必须在创建任何后台线程（包括 spdlog 异步日志线程池、I/O 池、Worker 池）之前屏蔽 SIGINT/SIGTERM，
    // 保证所有子线程继承阻塞掩码，从而确保退出信号稳定进入 Acceptor 的 signalfd 触发优雅停机。
    StopSignals signals;

    // 在初始化异步日志与派生子线程之前完成拓扑校验并绑定 Acceptor/Logger 到 E-core 集合，
    // 使 spdlog 后台线程自动继承 E-core 掩码，不抢占 P-core I/O 或 Benchmark client 核心。
    std::vector<std::string> topology_warnings;
    if (config.enable_cpu_affinity) {
        auto report = runtime::affinity::validate_topology(config.io_cpus, config.worker_cpus);
        topology_warnings = std::move(report.warnings);
        runtime::affinity::pin_thread_to_cpus(config.worker_cpus);
    }

    runtime::log::init();
    for (const auto &w : topology_warnings)
        runtime::log::warn("topology: " + w);

    std::optional<net::Socket> listener{net::Socket::listen_tcp(port)};
    listener->set_non_blocking();

    net::Epoll accept_epoll;
    accept_epoll.add(listener->native_handle(), net::EpollInterest::read);
    accept_epoll.add(signals.fd(), net::EpollInterest::read);

    auto kv = std::make_shared<detail::KvState>();
    ConnectionCounter connection_counter;

    struct IoSlot {
        std::size_t io_id{0};
        net::EventLoop *loop{nullptr};
        ConnectionMap connections;
        std::shared_ptr<service::DirWatcher> watcher;
        std::shared_ptr<server::TerminalManager> terminal_mgr;
    };

    std::span<const unsigned> io_cpu_span;
    if (config.enable_cpu_affinity)
        io_cpu_span = config.io_cpus;

    // 析构顺序：先回收 Worker，再回收 I/O 线程，最后销毁连接登记表。
    std::vector<IoSlot> slots;
    net::EventLoopThreadPool io_pool(config.io_threads_cnt(), io_cpu_span);
    runtime::WorkerPool workers(config);

    slots.reserve(config.io_threads_cnt());
    for (std::size_t i = 0; i < config.io_threads_cnt(); ++i) {
        slots.push_back(IoSlot{
            .io_id = i,
            .loop = &io_pool.loop_at(i),
            .connections = {},
            .watcher = nullptr,
            .terminal_mgr = nullptr,
        });
        auto &slot = slots.back();
        slot.watcher =
            detail::make_dir_watcher(*slot.loop, slot.connections, workers, connection_counter, i);
        slot.terminal_mgr =
            detail::make_terminal_manager(*slot.loop, slot.connections, connection_counter);
    }

    // 启动各 I/O 线程的 DirWatcher 以及挂载在 slots[0] 上的全局 FileWatcher。
    {
        auto watchers_started =
            std::make_shared<std::latch>(static_cast<std::ptrdiff_t>(slots.size()));
        for (std::size_t i = 0; i < slots.size(); ++i) {
            auto &slot = slots[i];
            if (!slot.loop->post_task(
                    [i, loop = slot.loop, watcher = slot.watcher, watchers_started] {
                        try {
                            watcher->start();
                            if (i == 0)
                                detail::start_file_watcher(*loop);
                        } catch (...) {
                            runtime::log::error("startup: failed to start inotify watchers");
                        }
                        watchers_started->count_down();
                    })) {
                throw std::runtime_error("cannot start inotify watchers");
            }
        }
        watchers_started->wait();
    }

    runtime::log::info(
        "tina listening on port " + std::string(port) +
        " | io=" + std::to_string(config.io_threads_cnt()) + format_cpu_list(config.io_cpus) +
        " | workers=" + std::to_string(config.worker_threads()) +
        " (normal=" + std::to_string(config.non_blk_worker_cnt) +
        ", blocking=" + std::to_string(config.blocking_workers()) + ")" +
        format_cpu_list(config.worker_cpus) +
        " | lane_cap=" + std::to_string(config.queue_cap) +
        " | inline_bytes=" + std::to_string(config.io_inline_threshold) +
        " | affinity=" + (config.enable_cpu_affinity ? "on" : "off"));

    std::size_t next_io = 0;
    bool stopping = false;
    while (!stopping) {
        auto events = accept_epoll.wait();

        for (const auto event : events) {
            if (event.fd == signals.fd()) {
                signals.consume();
                stopping = true;
                break;
            }
        }
        if (stopping)
            break;

        for (const auto event : events) {
            if (event.fd != listener->native_handle())
                continue;

            if (event.error() || event.hangup())
                throw std::runtime_error("listener error");
            if (!event.readable())
                continue;

            for (int i = 0; i < 64; ++i) {
                auto client = listener->try_accept();
                if (!client)
                    break;

                IoSlot &slot = slots[next_io];
                next_io = (next_io + 1) % slots.size();

                detail::IoDispatchView io_view{
                    .io_id = slot.io_id,
                    .inline_cache_threshold = config.io_inline_threshold,
                };

                bool accepted = detail::post_connection(
                    std::move(*client), *slot.loop, slot.connections, workers, connection_counter,
                    kv, slot.watcher, slot.terminal_mgr, io_view);

                if (!accepted)
                    runtime::log::warn("I/O EventLoop rejected connection");
            }
        }
    }

    // 阶段 1：停止监听，新连接不再进入。
    accept_epoll.remove(listener->native_handle());
    listener.reset();
    runtime::log::info("shutdown: stop accepting connections");

    // 阶段 2：在各 I/O 线程冻结读取与新的 Worker 提交；屏障保证先前 accept 任务已就绪。
    auto frozen = std::make_shared<std::latch>(static_cast<std::ptrdiff_t>(slots.size()));
    for (auto &slot : slots) {
        auto *loop = slot.loop;
        auto *connections = &slot.connections;
        auto watcher = slot.watcher;
        auto terminal_mgr = slot.terminal_mgr;
        const bool stop_fw = (&slot == &slots[0]);

        bool posted = loop->post_task(
            [loop, connections, watcher, terminal_mgr, stop_fw, &connection_counter, frozen] {
                try {
                    if (watcher)
                        watcher->stop();
                    if (stop_fw)
                        detail::stop_file_watcher();
                    if (terminal_mgr)
                        terminal_mgr->stop();
                    for (auto it = connections->begin(); it != connections->end();) {
                        auto conn = it->second;
                        ++it;
                        conn->begin_shutdown();
                        detail::refresh_connection(*loop, *connections, connection_counter, conn);
                    }
                } catch (...) {
                    runtime::log::error("shutdown: failed to freeze an I/O loop");
                }
                frozen->count_down();
            });
        if (!posted)
            throw std::runtime_error("cannot initiate I/O shutdown");
    }
    frozen->wait();

    // 阶段 3：停止 WorkerPool 接收新任务并排空所有已接受与已预留任务。
    workers.shutdown();

    // 阶段 4：等待 I/O 线程将剩余响应发空；慢连接超时强制关闭。
    constexpr auto drain_timeout = std::chrono::seconds(5);
    if (!connection_counter.wait_empty_for(drain_timeout)) {
        runtime::log::warn("shutdown: drain timeout, closing remaining connections");

        auto closed = std::make_shared<std::latch>(static_cast<std::ptrdiff_t>(slots.size()));
        for (auto &slot : slots) {
            auto *loop = slot.loop;
            auto *connections = &slot.connections;

            bool posted = loop->post_task([loop, connections, &connection_counter, closed] {
                try {
                    while (!connections->empty()) {
                        int fd = connections->begin()->first;
                        close_connection(*loop, *connections, connection_counter, fd);
                    }
                } catch (...) {
                    try {
                        runtime::log::error("shutdown: failed to close a connection");
                    } catch (...) {
                    }
                }
                closed->count_down();
            });
            if (!posted)
                throw std::runtime_error("cannot close remaining connections");
        }
        closed->wait();
    }

    // 阶段 5：关闭并 join 所有 I/O 线程。
    io_pool.shutdown();
    runtime::log::info("shutdown: completed");
}

} // namespace rpc

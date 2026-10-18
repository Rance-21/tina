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
import runtime.worker_pool;
import runtime.log;

import rpc.connection;
import rpc.connection_handler;
import broker.service;

namespace {

// 在启动其他线程之前阻塞退出信号，由 Acceptor 的 epoll 统一处理。
class StopSignals {
  public:
    StopSignals() {
        ::sigemptyset(&mask_);
        ::sigaddset(&mask_, SIGINT);
        ::sigaddset(&mask_, SIGTERM);

        // 屏蔽信号合集
        int rc = ::pthread_sigmask(SIG_BLOCK, &mask_, &previous_mask_);
        if (rc != 0)
            throw std::system_error(rc, std::generic_category(), "pthread_sigmask");

        // 信号转描述符
        fd_ = ::signalfd(-1, &mask_, SFD_NONBLOCK | SFD_CLOEXEC);
        if (fd_ < 0) {
            int error = errno;
            ::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
            throw std::system_error(error, std::generic_category(), "signalfd");
        }
    }

    ~StopSignals() {
        ::close(fd_);
        ::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
    }

    StopSignals(const StopSignals &) = delete;
    StopSignals &operator=(const StopSignals &) = delete;

    [[nodiscard]] int fd() const noexcept {
        return fd_;
    }

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

} // namespace

export namespace rpc {

void run_server(std::string_view port) {
    runtime::ServerConfig config;
    config.validate();
    runtime::log::init();

    // 必须先于 I/O / Worker 线程创建，保证所有子线程继承信号屏蔽。
    StopSignals signals;

    std::optional<net::Socket> listener{net::Socket::listen_tcp(port)};
    listener->set_non_blocking();

    net::Epoll accept_epoll;
    accept_epoll.add(listener->native_handle(), net::EpollInterest::read);
    accept_epoll.add(signals.fd(), net::EpollInterest::read);

    // 所有连接、所有 Worker 共享同一个业务状态。
    auto services = std::make_shared<detail::ServiceState>();

    // 暂时通过启动代码显式注册 Topic。
    // Partition 编号均从 0 开始。
    services->broker.create_topic("events", 4);
    services->broker.create_topic("logs", 2);

    ConnectionCounter connection_counter;

    struct IoSlot {
        net::EventLoop *loop;
        // 仅在对应的 I/O 线程中读写，供退出时枚举活跃连接。
        ConnectionMap connections;
    };

    // 析构顺序：先回收 Worker，再回收 I/O 线程，最后销毁连接登记表。
    std::vector<IoSlot> slots;
    net::EventLoopThreadPool io_pool(config.io_threads);
    runtime::WorkerPool workers(config.worker_threads, config.max_pending_tasks);

    slots.reserve(config.io_threads);
    for (std::size_t i = 0; i < config.io_threads; ++i)
        slots.push_back({&io_pool.next_loop(), {}});

    std::size_t next_io = 0;
    runtime::log::info("tina listening on port " + std::string(port));

    bool stopping = false;
    while (!stopping) {
        auto events = accept_epoll.wait();

        // 优先响应退出信号。
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

                bool accepted =
                    detail::post_connection(std::move(*client), *slot.loop, slot.connections,
                                            workers, connection_counter, services);

                if (!accepted)
                    runtime::log::warn("I/O EventLoop rejected connection");
            }
        }
    }

    // 阶段 1：立即停止监听，客户端的新连接不再进入 tina。
    accept_epoll.remove(listener->native_handle());
    listener.reset();
    runtime::log::info("shutdown: stop accepting connections");

    // 阶段 2：在各 I/O 线程冻结读取；屏障保证之前已投递的 accept 任务完成。
    auto frozen = std::make_shared<std::latch>(static_cast<std::ptrdiff_t>(slots.size()));
    for (auto &slot : slots) {
        auto *loop = slot.loop;
        auto *connections = &slot.connections;

        bool posted = loop->post_task([loop, connections, &connection_counter, frozen] {
            try {
                for (auto it = connections->begin(); it != connections->end();) {
                    auto conn = it->second;
                    ++it; // refresh_connection 可能删除当前连接。
                    conn->begin_shutdown();
                    detail::refresh_connection(*loop, *connections, connection_counter, conn);
                }
            } catch (...) {
                // 不允许异常跳过屏障通知，否则管理线程会永久等待。
                try {
                    runtime::log::error("shutdown: failed to freeze an I/O loop");
                } catch (...) {
                }
            }
            frozen->count_down();
        });
        if (!posted)
            throw std::runtime_error("cannot initiate I/O shutdown");
    }
    frozen->wait();

    // 阶段 3：不再接受 Worker 任务，等待此前接受的任务执行完毕并投递响应。
    workers.shutdown();

    // 阶段 4：让 I/O 线程继续发送响应；慢连接不能无限拖住退出。
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

    // 阶段 5：此时不会再有 Worker 向 I/O 线程投递响应，可以安全 join。
    io_pool.shutdown();
    runtime::log::info("shutdown: completed");
}

} // namespace rpc

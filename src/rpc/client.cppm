module;
#include <errno.h>
#include <time.h>
#include <sys/timerfd.h>
#include <unistd.h>

export module rpc.client;

export import :types;
export import :io;

import std;
import net.socket;
import net.epoll;
import net.event_loop;
import net.event_loop_thread_pool;
import rpc.protocol;

namespace rpc {

RpcClient::RpcClient(std::string_view host, std::string_view port)
    : io_pool_(1), loop_(&io_pool_.next_loop()),
      state_(std::make_shared<State>(net::Socket::connect_tcp_nonblocking(host, port), *loop_)) {
    auto ready = std::make_shared<std::promise<void>>();
    auto future = ready->get_future();

    bool posted = loop_->post_task([state = state_, ready] {
        try {
            State::start(state);

            // promise 唤醒 ready
            ready->set_value();
        } catch (...) {
            auto error = std::current_exception();
            state->fail(error);
            ready->set_exception(error);
        }
    });

    if (!posted)
        throw std::runtime_error("RPC client I/O thread stopped");

    future.get();
}

RpcClient::~RpcClient() {
    // 不允许与 async_call 并发析构。
    try {
        (void)loop_->post_task([state = state_] {
            state->fail(std::make_exception_ptr(std::runtime_error("RPC client destroyed")));
        });
    } catch (...) {
    }

    io_pool_.shutdown();
}

std::future<Response> RpcClient::async_call(Method method, std::string payload,
                                            std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<Response>>();
    auto future = promise->get_future();

    if (timeout <= std::chrono::milliseconds::zero()) {
        promise->set_exception(std::make_exception_ptr(TimeoutError{}));
        return future;
    }

    // 从发起调用时开始，而不是等到 I/O 线程拿到任务。
    const auto deadline = Clock::now() + timeout;

    try {
        if (!loop_->post_task([state = state_, method, payload = std::move(payload), deadline,
                               promise]() mutable {
                try {
                    state->send(method, std::move(payload), deadline, promise);
                } catch (...) {
                    auto error = std::current_exception();
                    state->fail(error);

                    try {
                        promise->set_exception(error);
                    } catch (...) {
                    }
                }
            })) {
            promise->set_exception(
                std::make_exception_ptr(std::runtime_error("RPC client I/O thread stopped")));
        }
    } catch (...) {
        promise->set_exception(std::current_exception());
    }

    return future;
}

RpcClient::State::State(net::Socket s, net::EventLoop &l) : socket(std::move(s)), loop(l) {
    timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    if (timer_fd < 0)
        throw std::system_error(errno, std::generic_category(), "timerfd_create");
}

RpcClient::State::~State() {
    if (timer_fd >= 0)
        ::close(timer_fd);
}

// 所有 RPC 共用一个 timerfd。
// 每次只为最早的 Deadline 设置定时器。
void RpcClient::State::update_timer() {
    itimerspec spec{};

    if (!deadlines.empty()) {
        auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadlines.begin()->first - Clock::now());

        // 已经过期时，尽快让 timerfd 变为可读。
        if (remaining <= std::chrono::nanoseconds::zero())
            remaining = std::chrono::nanoseconds{1};

        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);

        spec.it_value.tv_sec = static_cast<time_t>(seconds.count());
        spec.it_value.tv_nsec = static_cast<long>((remaining - seconds).count());
    }

    // deadlines 为空时，spec 全为 0，即停止计时。
    if (::timerfd_settime(timer_fd, 0, &spec, nullptr) < 0)
        throw std::system_error(errno, std::generic_category(), "timerfd_settime");
}

// timerfd 到期后，处理所有已经超过截止时间的请求。
void RpcClient::State::on_timer(net::EpollEvent event) {
    if (closed)
        return;

    try {
        if (event.error() || event.hangup())
            throw std::runtime_error("RPC timerfd error");

        std::uint64_t expirations{};

        // 读取 timerfd，清除可读状态。
        while (true) {
            const ssize_t n = ::read(timer_fd, &expirations, sizeof(expirations));

            // 如果没有超时却读到
            if (n == sizeof(expirations) || (n < 0 && errno == EAGAIN))
                break;

            // syscall 被打断
            if (n < 0 && errno == EINTR)
                continue;

            throw std::system_error(errno, std::generic_category(), "read timerfd");
        }

        const auto now = Clock::now();

        while (!deadlines.empty() && deadlines.begin()->first <= now) {
            const std::uint32_t id = deadlines.begin()->second;

            // 服务器长时间不回应时，关闭连接，
            // 避免保存无限多的迟到响应 ID。
            if (expired_ids.size() >= max_expired_ids)
                throw std::runtime_error("too many unanswered timed-out RPCs");

            expired_ids.insert(id);

            auto it = pendings.find(id);
            auto promise = it->second.promise;

            pendings.erase(it);
            deadlines.erase(deadlines.begin());

            promise->set_exception(std::make_exception_ptr(TimeoutError{}));
        }

        // 为剩余请求中最近的 Deadline 重新计时。
        update_timer();
    } catch (...) {
        fail(std::current_exception());
    }
}

} // namespace rpc

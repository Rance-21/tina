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

void RpcClient::set_event_handler(EventHandler handler) {
    (void)loop_->post_task([state = state_, handler = std::move(handler)]() mutable {
        state->event_handler = std::move(handler);
    });
}

// 发起异步 RPC 请求，立即返回 std::future<Frame>，实际编码与发送由客户端 I/O 线程异步完成。
// 分支逻辑：
//   1. timeout <= 0：不进队列直接令 Promise 以 TimeoutError 失败。
//   2. 在调用方线程提前算好绝对截止时间 deadline，通过 post_task 投递给 I/O 线程执行 State::send()。
//   3. 若 I/O 线程已停止导致投递失败，或执行中抛出异常，均捕获并注入到 Promise 中防止调用方 future.get() 永久挂起。
std::future<Frame> RpcClient::async_call(Method method, std::string payload,
                                            std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<Frame>>();
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

// 所有在途 RPC 共享同一个 timerfd，每次只按最早到期的请求（deadlines.begin()）设置单次内核定时器。
// 分支逻辑：
//   1. deadlines 非空：计算距最早 deadline 的剩余纳秒数（若已过期则设为 1ns 立即触发可读）。
//   2. deadlines 为空：spec 保持全 0，调用 timerfd_settime 停用定时器。
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

// timerfd 到期回调：批量清理所有已到达截止时间的在途请求。
// 分支逻辑：
//   1. 先非阻塞 read(timer_fd) 读出超时计数，清除内核可读状态。
//   2. 循环检查 deadlines 头部所有 <= now 的过期请求：
//      - 将其 ID 记入 expired_ids（以便未来收到迟到响应时能识别并丢弃）；若累计超时未回包数超上限则直接断开连接。
//      - 从 pendings 与 deadlines 中移除，并向对应 Promise 注入 TimeoutError。
//   3. 最后调用 update_timer() 为剩余未超时请求中最近的一个重新武装 timerfd。
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
            const std::uint64_t id = deadlines.begin()->second;

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

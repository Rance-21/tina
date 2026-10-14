module;
#include <errno.h>
#include <time.h>
#include <sys/timerfd.h>
#include <unistd.h>

export module rpc.client;

import std;
import net.socket;
import net.buffer;
import net.epoll;
import net.event_loop;
import net.event_loop_thread_pool;
import rpc.protocol;

export namespace rpc {
// 超时只表示客户端不再等待，不表示服务器取消了请求。
class TimeoutError : public std::runtime_error {
  public:
    TimeoutError() : std::runtime_error("RPC deadline exceeded") {
    }
};

class RpcClient {
  private:
    using Clock = std::chrono::steady_clock;
    using DeadlineEntry = std::pair<Clock::time_point, std::uint32_t>;
    using Promise = std::shared_ptr<std::promise<Response>>;

    // 服务器持续不响应时，不允许无限积累过期请求记录。
    static constexpr std::size_t max_expired_ids = 1024;

    // 以下状态只能由客户端所属的 I/O 线程操作。
    struct State {
        explicit State(net::Socket s, net::EventLoop &l) : socket(std::move(s)), loop(l) {
            timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

            if (timer_fd < 0)
                throw std::system_error(errno, std::generic_category(), "timerfd_create");
        }

        ~State() {
            if (timer_fd >= 0)
                ::close(timer_fd);
        }

        static constexpr std::size_t max_inflight = 64;
        static constexpr std::size_t max_output = 4 * 1024 * 1024;

        // 一个尚未完成的请求，以及它的截止时间。
        struct Pending {
            Promise promise;
            Clock::time_point deadline;
        };

        std::optional<net::Socket> socket;
        net::EventLoop &loop;
        int timer_fd{-1};

        net::Buffer input;
        net::Buffer output;

        // request_id -> Pending
        std::unordered_map<std::uint32_t, Pending> pending;

        // 按截止时间排序，begin() 永远是最近要超时的请求。
        std::set<DeadlineEntry> deadlines;

        // 已超时但可能还会收到响应的 request_id。
        std::unordered_set<std::uint32_t> expired_ids;

        std::uint32_t next_id{1};
        bool connected{false};
        bool closed{false};

        static std::uint32_t get_u32(const char *p) noexcept {
            return (static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(p[3]));
        }

        static void put_u32(net::Buffer &buffer, std::uint32_t value) {
            std::array<char, 4> bytes{static_cast<char>(value >> 24),
                                      static_cast<char>(value >> 16), static_cast<char>(value >> 8),
                                      static_cast<char>(value)};

            buffer.append(bytes.data(), bytes.size());
        }

        // 所有 RPC 共用一个 timerfd。
        // 每次只为最早的 Deadline 设置定时器。
        void arm_timer() {
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

        void refresh() {
            auto interest = net::EpollInterest::read | net::EpollInterest::peer_closed;

            if (!connected || !output.empty())
                interest = interest | net::EpollInterest::write;

            loop.modify(socket->native_handle(), interest);
        }

        // 网络连接失败或客户端关闭，所有尚未完成的请求都失败。
        void fail(std::exception_ptr error) noexcept {
            if (closed)
                return;

            closed = true;

            if (socket) {
                try {
                    loop.unwatch(socket->native_handle());
                } catch (...) {
                }

                socket.reset();
            }

            if (timer_fd >= 0) {
                try {
                    loop.unwatch(timer_fd);
                } catch (...) {
                }

                ::close(timer_fd);
                timer_fd = -1;
            }

            for (auto &[id, request] : pending) {
                try {
                    request.promise->set_exception(error);
                } catch (...) {
                }
            }

            pending.clear();
            deadlines.clear();
            expired_ids.clear();
        }

        void send_pending() {
            for (int i = 0; i < 8 && !output.empty(); ++i) {
                auto n = socket->try_send(output.data(), output.readable_bytes());

                if (!n || *n == 0)
                    break;

                output.retrieve(*n);
            }
        }

        // 解析响应，并判断对应请求是否已经超时。
        void decode() {
            while (input.readable_bytes() >= 4) {
                const char *frame = input.data();
                const std::uint32_t length = get_u32(frame);

                if (length < 5 || length > max_payload_size + 5)
                    throw std::runtime_error("invalid response frame length");

                const std::size_t frame_size = static_cast<std::size_t>(length) + 4;

                if (input.readable_bytes() < frame_size)
                    break;

                const std::uint32_t id = get_u32(frame + 4);
                auto it = pending.find(id);

                if (it == pending.end()) {
                    // 已经超时的请求可能晚些时候才返回。
                    // 识别后直接丢弃，不影响其他 Future。
                    if (expired_ids.erase(id) == 0)
                        throw std::runtime_error("unknown response id");

                    input.retrieve(frame_size);
                    continue;
                }

                auto promise = it->second.promise;
                const auto deadline = it->second.deadline;

                const bool earliest = !deadlines.empty() && deadlines.begin()->second == id;

                deadlines.erase(DeadlineEntry{deadline, id});
                pending.erase(it);

                if (Clock::now() >= deadline) {
                    // 即使 timerfd 事件还没有执行，
                    // 已经过期的响应仍然算超时。
                    input.retrieve(frame_size);

                    promise->set_exception(std::make_exception_ptr(TimeoutError{}));
                } else {
                    Response response{.request_id = id,
                                      .status =
                                          static_cast<Status>(static_cast<unsigned char>(frame[8])),
                                      .payload = std::string(frame + 9, length - 5)};

                    input.retrieve(frame_size);
                    promise->set_value(std::move(response));
                }

                // 仅当最近的 Deadline 改变时才修改 timerfd。
                if (earliest)
                    arm_timer();
            }
        }

        void receive() {
            std::array<char, 8192> chunk{};

            for (int i = 0; i < 8; ++i) {
                auto n = socket->try_recv(chunk.data(), chunk.size());

                if (!n)
                    break;

                if (*n == 0)
                    throw std::runtime_error("server closed connection");

                input.append(chunk.data(), *n);
                decode();
            }
        }

        // timerfd 到期后，处理所有已经超过截止时间的请求。
        void on_timer(net::EpollEvent event) {
            if (closed)
                return;

            try {
                if (event.error() || event.hangup())
                    throw std::runtime_error("RPC timerfd error");

                std::uint64_t expirations{};

                // 必须读取 timerfd，清除可读状态。
                while (true) {
                    const ssize_t n = ::read(timer_fd, &expirations, sizeof(expirations));

                    if (n == sizeof(expirations) || (n < 0 && errno == EAGAIN))
                        break;

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

                    auto it = pending.find(id);
                    auto promise = it->second.promise;

                    pending.erase(it);
                    deadlines.erase(deadlines.begin());

                    promise->set_exception(std::make_exception_ptr(TimeoutError{}));
                }

                // 为剩余请求中最近的 Deadline 重新计时。
                arm_timer();

            } catch (...) {
                fail(std::current_exception());
            }
        }

        void on_event(net::EpollEvent event) {
            if (closed)
                return;

            try {
                if (!connected) {
                    if (!event.writable() && !event.error() && !event.hangup() &&
                        !event.peer_closed())
                        return;

                    socket->check_connect_result();
                    connected = true;
                }

                if (event.error())
                    throw std::runtime_error("client socket error");

                if (event.readable() || event.peer_closed() || event.hangup())
                    receive();

                if (event.writable() || !output.empty())
                    send_pending();

                refresh();

            } catch (...) {
                fail(std::current_exception());
            }
        }

        // 由 EventLoop::post() 提交过来。
        // Deadline 在调用线程计算，不在这里重新开始计时。
        void submit(Method method, std::string payload, Clock::time_point deadline,
                    Promise promise) {
            if (closed || payload.size() > max_payload_size || pending.size() >= max_inflight ||
                output.readable_bytes() + payload.size() + 9 > max_output) {
                promise->set_exception(std::make_exception_ptr(
                    std::runtime_error("RPC client closed or request limit exceeded")));
                return;
            }

            if (Clock::now() >= deadline) {
                promise->set_exception(std::make_exception_ptr(TimeoutError{}));
                return;
            }

            std::uint32_t id;

            do {
                id = next_id++;

                if (next_id == 0)
                    next_id = 1;

            } while (id == 0 || pending.contains(id) || expired_ids.contains(id));

            const bool earliest =
                deadlines.empty() || DeadlineEntry{deadline, id} < *deadlines.begin();

            pending.emplace(id, Pending{promise, deadline});
            deadlines.emplace(deadline, id);

            // 请求帧：[length][request_id][method][payload]
            put_u32(output, static_cast<std::uint32_t>(payload.size() + 5));
            put_u32(output, id);

            char code = static_cast<char>(method);
            output.append(&code, 1);
            output.append(payload.data(), payload.size());

            // 避免每次提交请求都调用 timerfd_settime。EventLoop::post(
            if (earliest)
                arm_timer();

            if (connected)
                send_pending();

            refresh();
        }
    };

  public:
    explicit RpcClient(std::string_view host = "127.0.0.1", std::string_view port = "3490")
        : io_pool_(1), loop_(&io_pool_.next_loop()),
          state_(
              std::make_shared<State>(net::Socket::connect_tcp_nonblocking(host, port), *loop_)) {

        auto ready = std::make_shared<std::promise<void>>();
        auto future = ready->get_future();

        bool posted = loop_->post([state = state_, ready] {
            try {
                // Socket 和 timerfd 注册到同一个 EventLoop。
                state->loop.watch(state->socket->native_handle(),
                                  net::EpollInterest::read | net::EpollInterest::write |
                                      net::EpollInterest::peer_closed,
                                  [state](net::EpollEvent event) {
                                      state->on_event(event);
                                  });

                state->loop.watch(state->timer_fd, net::EpollInterest::read,
                                  [state](net::EpollEvent event) {
                                      state->on_timer(event);
                                  });

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

    ~RpcClient() {
        // 不允许与 async_call 并发析构。
        try {
            (void)loop_->post([state = state_] {
                state->fail(std::make_exception_ptr(std::runtime_error("RPC client destroyed")));
            });
        } catch (...) {
        }

        io_pool_.shutdown();
    }

    RpcClient(const RpcClient &) = delete;
    RpcClient &operator=(const RpcClient &) = delete;

    [[nodiscard]]
    std::future<Response>
    async_call(Method method, std::string payload,
               std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();

        if (timeout <= std::chrono::milliseconds::zero()) {
            promise->set_exception(std::make_exception_ptr(TimeoutError{}));
            return future;
        }

        // 从发起调用时开始，而不是等到 I/O 线程拿到任务。
        const auto deadline = Clock::now() + timeout;

        try {
            if (!loop_->post([state = state_, method, payload = std::move(payload), deadline,
                              promise]() mutable {
                    try {
                        state->submit(method, std::move(payload), deadline, promise);
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

  private:
    net::EventLoopThreadPool io_pool_;
    net::EventLoop *loop_;
    std::shared_ptr<State> state_;
};

} // namespace rpc

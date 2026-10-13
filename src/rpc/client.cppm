export module rpc.client;

import std;
import net.socket;
import net.buffer;
import net.epoll;
import net.event_loop;
import net.event_loop_thread_pool;
import rpc.protocol;

export namespace rpc {

// 保留原有的同步接口，兼容 main.cpp。
Response call(Method method, std::string payload, std::string_view host = "127.0.0.1",
              std::string_view port = "3490") {
    auto socket = net::Socket::connect_tcp(host, port);
    Request request{.id = 1, .method = method, .payload = std::move(payload)};

    send_request(socket, request);

    Response response;
    if (!recv_response(socket, response))
        throw std::runtime_error("server closed before response");

    return response;
}

class RpcClient {
  private:
    // 这些状态只允许所属 I/O 线程访问。
    struct State {
        explicit State(net::Socket s, net::EventLoop &l) : socket(std::move(s)), loop(l) {
        }

        static constexpr std::size_t max_inflight = 64;
        static constexpr std::size_t max_output = 4 * 1024 * 1024;

        std::optional<net::Socket> socket;
        net::EventLoop &loop;
        net::Buffer input;
        net::Buffer output;

        // 请求 ID -> 等待响应的 Promise。
        std::unordered_map<std::uint32_t, std::shared_ptr<std::promise<Response>>> pending;

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

        // 只在有数据待发送或尚未连接成功时监听 EPOLLOUT。
        void refresh() {
            auto interest = net::EpollInterest::read | net::EpollInterest::peer_closed;

            if (!connected || !output.empty())
                interest = interest | net::EpollInterest::write;

            loop.modify(socket->native_handle(), interest);
        }

        // 连接失败或关闭时，让所有未完成 Future 得到异常。
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

            for (auto &[id, promise] : pending) {
                try {
                    promise->set_exception(error);
                } catch (...) {
                }
            }
            pending.clear();
        }

        // 非阻塞发送，处理部分写入。
        void send_pending() {
            for (int i = 0; i < 8 && !output.empty(); ++i) {
                auto n = socket->try_send(output.data(), output.readable_bytes());

                if (!n || *n == 0)
                    break;

                output.retrieve(*n);
            }
        }

        // 从输入缓冲区提取完整的 Response Frame。
        void decode() {
            while (input.readable_bytes() >= 4) {
                const char *frame = input.data();
                std::uint32_t length = get_u32(frame);

                if (length < 5 || length > max_payload_size + 5)
                    throw std::runtime_error("invalid response frame length");

                std::size_t frame_size = static_cast<std::size_t>(length) + 4;

                if (input.readable_bytes() < frame_size)
                    break;

                std::uint32_t id = get_u32(frame + 4);

                auto it = pending.find(id);
                if (it == pending.end())
                    throw std::runtime_error("unknown response id");

                Response response{.request_id = id,
                                  .status =
                                      static_cast<Status>(static_cast<unsigned char>(frame[8])),
                                  .payload = std::string(frame + 9, length - 5)};

                auto promise = std::move(it->second);
                pending.erase(it);
                input.retrieve(frame_size);

                promise->set_value(std::move(response));
            }
        }

        // 非阻塞接收，并及时解析完整响应。
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

        // epoll 就绪事件处理入口。
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

        // EventLoop::post() 投递过来的业务请求。
        void submit(Method method, std::string payload,
                    std::shared_ptr<std::promise<Response>> promise) {
            if (closed || payload.size() > max_payload_size || pending.size() >= max_inflight ||
                output.readable_bytes() + payload.size() + 9 > max_output) {
                promise->set_exception(std::make_exception_ptr(
                    std::runtime_error("RPC client closed or request limit exceeded")));
                return;
            }

            std::uint32_t id;
            do {
                id = next_id++;
                if (next_id == 0)
                    next_id = 1;
            } while (id == 0 || pending.contains(id));

            // 请求帧：[length][request_id][method][payload]
            pending.emplace(id, std::move(promise));

            put_u32(output, static_cast<std::uint32_t>(payload.size() + 5));
            put_u32(output, id);

            char code = static_cast<char>(method);
            output.append(&code, 1);
            output.append(payload.data(), payload.size());

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

        // 让 I/O 线程注册 Socket，避免跨线程操作 epoll。
        auto ready = std::make_shared<std::promise<void>>();
        auto future = ready->get_future();

        bool posted = loop_->post([state = state_, ready] {
            try {
                state->loop.watch(state->socket->native_handle(),
                                  net::EpollInterest::read | net::EpollInterest::write |
                                      net::EpollInterest::peer_closed,
                                  [state](net::EpollEvent event) {
                                      state->on_event(event);
                                  });
                // 唤醒主线程
                ready->set_value();
            } catch (...) {
                ready->set_exception(std::current_exception());
            }
        });

        if (!posted)
            throw std::runtime_error("RPC client I/O thread stopped");

        future.get();
    }

    ~RpcClient() {
        // 析构须由外部线程调用，不可与 async_call 并发。
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

    // 提交请求立即返回 Future，不等待网络响应。
    [[nodiscard]]
    std::future<Response> async_call(Method method, std::string payload) {
        auto promise = std::make_shared<std::promise<Response>>();
        auto future = promise->get_future();

        try {
            if (!loop_->post(
                    [state = state_, method, payload = std::move(payload), promise]() mutable {
                        try {
                            state->submit(method, std::move(payload), promise);
                        } catch (...) {
                            state->fail(std::current_exception());
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

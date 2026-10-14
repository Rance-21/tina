export module rpc.client:types;

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
  public:
    explicit RpcClient(std::string_view host = "127.0.0.1", std::string_view port = "3490");
    ~RpcClient();

    RpcClient(const RpcClient &) = delete;
    RpcClient &operator=(const RpcClient &) = delete;

    [[nodiscard]]
    std::future<Response>
    async_call(Method method, std::string payload,
               std::chrono::milliseconds timeout = std::chrono::milliseconds{3000});

  private:
    using Clock = std::chrono::steady_clock;
    using DeadlineEntry = std::pair<Clock::time_point, std::uint32_t>;
    using Promise = std::shared_ptr<std::promise<Response>>;

    // 服务器持续不响应时，不允许无限积累过期请求记录。
    static constexpr std::size_t max_expired_ids = 1024;

    // 以下状态只能由客户端所属的 I/O 线程操作。
    struct State {
        explicit State(net::Socket s, net::EventLoop &l);
        ~State();

        static void start(const std::shared_ptr<State> &state);
        void send(Method method, std::string payload, Clock::time_point deadline, Promise promise);
        void on_event(net::EpollEvent event);
        void on_timer(net::EpollEvent event);
        void fail(std::exception_ptr error) noexcept;

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

        static std::uint32_t get_u32(const char *p) noexcept;
        static void put_u32(net::Buffer &buffer, std::uint32_t value);

        void refresh();
        void send_pending();
        void receive();
        void decode();
        void arm_timer();
    };

    net::EventLoopThreadPool io_pool_;
    net::EventLoop *loop_;
    std::shared_ptr<State> state_;
};

} // namespace rpc

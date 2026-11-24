module;
#include <pthread.h>

export module net.event_loop_thread_pool;

import std;
import net.event_loop;
import runtime.cpu_affinity;

namespace {

thread_local std::optional<std::size_t> t_current_io_id;

} // namespace

export namespace net {

// 【作用】查询当前线程是第几号 I/O EventLoop 线程（0..io_threads-1）；非 I/O 线程调用返回 nullopt。
// 【原理】每个 I/O 线程在 entry() 启动时把自己的 io_id 记到 thread_local 变量中，查询无锁零开销。
[[nodiscard]] std::optional<std::size_t> current_event_loop_io_id() noexcept {
    return t_current_io_id;
}

// ============================================================================
// I/O 事件循环线程池（EventLoopThreadPool）
// ============================================================================
// 负责管理固定数量（默认 4 个）的独立 EventLoop 线程，并按需把每个线程一对一钉到指定 P-core
// （如 {0, 2, 4, 8}），用于承接客户端 TCP 读写、epoll 调度、内联快路径响应和 writev 发送。
class EventLoopThreadPool {
  public:
    // 【作用】创建 count 个 EventLoop 与对应后台线程，并可选逐一绑定到 cpu_ids 指定的物理核。
    // 【原理】
    //   1. 先在主线程构造全部 EventLoop（各自创建 epoll 与 eventfd）；
    //   2. 逐个使用 ScopedThreadAttr 携带单核亲和性启动 pthread，保证新线程一出生就在目标核上跑；
    //   3. 通过 std::latch 等待所有 I/O 线程完成线程内绑核二次校验；若有任意线程创建或绑核失败，
    //      立刻调用 shutdown() 清理已启动的线程并原样重抛异常，不留僵尸线程。
    explicit EventLoopThreadPool(std::size_t count, std::span<const unsigned> cpu_ids = {}) {
        if (count == 0)
            throw std::invalid_argument("I/O thread count must be > 0");
        if (!cpu_ids.empty() && cpu_ids.size() != count)
            throw std::invalid_argument("EventLoopThreadPool cpu_ids count mismatch");

        loops_.reserve(count);
        contexts_.reserve(count);
        threads_.reserve(count);

        for (std::size_t i = 0; i < count; ++i)
            loops_.push_back(std::make_unique<EventLoop>());

        auto ready_latch = std::make_shared<std::latch>(static_cast<std::ptrdiff_t>(count));
        auto startup_errors = std::make_shared<std::vector<std::exception_ptr>>(count);

        try {
            for (std::size_t i = 0; i < count; ++i) {
                auto ctx = std::make_unique<IoThreadContext>(IoThreadContext{
                    .loop = loops_[i].get(),
                    .io_id = i,
                    .pin_cpu = cpu_ids.empty() ? std::nullopt : std::optional<unsigned>{cpu_ids[i]},
                    .ready = ready_latch,
                    .error = &(*startup_errors)[i],
                });

                runtime::affinity::ScopedThreadAttr attr;
                if (ctx->pin_cpu)
                    attr.set_affinity(*ctx->pin_cpu);

                pthread_t thread{};
                const int rc = ::pthread_create(&thread, ctx->pin_cpu ? attr.get() : nullptr,
                                                &EventLoopThreadPool::entry, ctx.get());
                if (rc != 0)
                    throw std::system_error(rc, std::generic_category(), "pthread_create");

                contexts_.push_back(std::move(ctx));
                threads_.push_back(thread);
            }
        } catch (...) {
            shutdown();
            throw;
        }

        ready_latch->wait();
        for (const auto &err : *startup_errors) {
            if (err) {
                shutdown();
                std::rethrow_exception(err);
            }
        }
    }

    // 【作用】析构时兜底停止并回收所有 I/O 线程。
    ~EventLoopThreadPool() {
        shutdown();
    }

    EventLoopThreadPool(const EventLoopThreadPool &) = delete;
    EventLoopThreadPool &operator=(const EventLoopThreadPool &) = delete;

    // 【作用】由单线程 Acceptor 调用，按普通轮询（Round Robin）为新连接挑一个 EventLoop。
    // 【原理】依次返回 loops_[0], loops_[1], ..., 循环取模，不加权，让 4 条连接正好铺满 4 个核。
    [[nodiscard]] EventLoop &next_loop() {
        EventLoop &loop = *loops_[next_];
        next_ = (next_ + 1) % loops_.size();
        return loop;
    }

    // 【作用】按精确编号（io_id）获取对应的 EventLoop 引用，便于与 IoSlot 一一绑定。
    // 【原理】带越界检查地访问 loops_.at(index)。
    [[nodiscard]] EventLoop &loop_at(std::size_t index) {
        return *loops_.at(index);
    }

    // 【作用】返回当前池中 EventLoop 线程总数。
    [[nodiscard]] std::size_t size() const noexcept {
        return loops_.size();
    }

    // 【作用】优雅关闭全部 I/O 线程（由外部管理线程在停机最后阶段调用）。
    // 【原理】先对每个 EventLoop 调 stop()（设置退出标志并向 eventfd 写字节打断 epoll_wait），
    //         再逐一 pthread_join 等待每个 Loop 排空最后一批任务并退出。
    void shutdown() noexcept {
        for (auto &loop : loops_)
            loop->stop();

        for (pthread_t thread : threads_) {
            if (::pthread_join(thread, nullptr) != 0)
                std::terminate();
        }

        threads_.clear();
        contexts_.clear();
    }

  private:
    struct IoThreadContext {
        EventLoop *loop{nullptr};
        std::size_t io_id{0};
        std::optional<unsigned> pin_cpu;
        std::shared_ptr<std::latch> ready;
        std::exception_ptr *error{nullptr};
    };

    // 【作用】I/O 线程的 pthread 启动入口函数。
    // 【原理】记录 thread_local io_id -> 若指定了绑核则二次执行 pin_thread_to_cpu 并回读校验 ->
    //         通过 ready latch 告知主线程启动结果 -> 正式调用 loop->run() 进入 epoll 事件主循环。
    static void *entry(void *arg) noexcept {
        auto *ctx = static_cast<IoThreadContext *>(arg);
        t_current_io_id = ctx->io_id;

        try {
            if (ctx->pin_cpu)
                runtime::affinity::pin_thread_to_cpu(*ctx->pin_cpu);
        } catch (...) {
            *ctx->error = std::current_exception();
            ctx->ready->count_down();
            return nullptr;
        }

        ctx->ready->count_down();

        try {
            ctx->loop->run();
        } catch (...) {
            // I/O 线程异常退出不能被当作正常停机。
            std::terminate();
        }

        return nullptr;
    }

    std::vector<std::unique_ptr<EventLoop>> loops_;
    std::vector<std::unique_ptr<IoThreadContext>> contexts_;
    std::vector<pthread_t> threads_;

    // 只在 Acceptor 线程中修改。
    std::size_t next_{0};
};

} // namespace net

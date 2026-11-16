module;
#include <pthread.h>

export module net.event_loop_thread_pool;
import std;
import net.event_loop;

export namespace net {

class EventLoopThreadPool {
  public:
    explicit EventLoopThreadPool(std::size_t count) {
        if (count == 0)
            throw std::invalid_argument("I/O thread count must be > 0");

        loops_.reserve(count);
        threads_.reserve(count);

        for (std::size_t i = 0; i < count; ++i)
            loops_.push_back(std::make_unique<EventLoop>());

        try {
            for (auto &loop : loops_) {
                pthread_t thread{};

                int rc = pthread_create(&thread, nullptr, &EventLoopThreadPool::entry, loop.get());

                if (rc != 0)
                    throw std::system_error(rc, std::generic_category(), "pthread_create");

                threads_.push_back(thread);
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~EventLoopThreadPool() {
        shutdown();
    }

    EventLoopThreadPool(const EventLoopThreadPool &) = delete;
    EventLoopThreadPool &operator=(const EventLoopThreadPool &) = delete;

    // 由 Acceptor 线程调用
    [[nodiscard]] EventLoop &next_loop() {
        EventLoop &loop = *loops_[next_];
        next_ = (next_ + 1) % loops_.size();

        return loop;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return loops_.size();
    }

    // 由外部管理线程调用
    void shutdown() noexcept {
        for (auto &loop : loops_)
            loop->stop();

        for (pthread_t thread : threads_) {
            if (pthread_join(thread, nullptr) != 0)
                std::terminate();
        }

        threads_.clear();
    }

  private:
    static void *entry(void *arg) noexcept {
        auto *loop = static_cast<EventLoop *>(arg);

        try {
            loop->run();
        } catch (...) {
            // I/O 线程异常退出不能被当作正常停机。
            std::terminate();
        }

        return nullptr;
    }

    std::vector<std::unique_ptr<EventLoop>> loops_;
    std::vector<pthread_t> threads_;

    // 只在 Acceptor 线程中修改。
    std::size_t next_{0};
};

} // namespace net

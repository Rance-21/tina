module;
#include <pthread.h>

export module net.event_loop_thread_pool;

import std;
import net.event_loop;
import runtime.cpu_affinity;

export namespace net {

// 一个 EventLoop 对应一个固定的 I/O 线程。连接的轮询分配由 Acceptor 完成，
// 它在投递前已经选定 io_id；线程池只需按编号提供同一条 loop。
class EventLoopThreadPool {
  public:
    explicit EventLoopThreadPool(std::size_t count, std::span<const unsigned> cpu_ids = {}) {
        if (count == 0)
            throw std::invalid_argument("I/O thread count must be > 0");
        if (!cpu_ids.empty() && cpu_ids.size() != count)
            throw std::invalid_argument("EventLoopThreadPool cpu_ids count mismatch");

        loops_.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            loops_.push_back(std::make_unique<EventLoop>());

        try {
            runtime::affinity::start_threads(threads_, count, cpu_ids, [this](std::size_t i) {
                loops_[i]->run();
            });
        } catch (...) {
            // 创建或绑核失败时，先用 eventfd 唤醒已经进入 epoll_wait 的线程，再逐个回收。
            shutdown();
            throw;
        }
    }

    ~EventLoopThreadPool() {
        shutdown();
    }

    EventLoopThreadPool(const EventLoopThreadPool &) = delete;
    EventLoopThreadPool &operator=(const EventLoopThreadPool &) = delete;

    [[nodiscard]] EventLoop &loop_at(std::size_t index) {
        return *loops_.at(index);
    }

    // 停止接受新投递。run() 返回前会处理完最后一批已入队任务，join 后才能销毁 loop。
    void shutdown() noexcept {
        for (auto &loop : loops_)
            loop->stop();
        runtime::affinity::join_threads(threads_);
    }

  private:
    std::vector<std::unique_ptr<EventLoop>> loops_;
    std::vector<pthread_t> threads_;
};

} // namespace net

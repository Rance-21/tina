module;
#include <sys/eventfd.h>
#include <unistd.h>
#include <errno.h>

export module net.event_loop;
import std;
import net.epoll;
import runtime.log;

export namespace net {

class EventLoop {
  public:
    using Task = std::move_only_function<void()>;
    using Handler = std::move_only_function<void(EpollEvent)>;

    EventLoop() {
        // 这里是 worker 线程 write 这个 fd，唤醒 eventloop 执行 writable 事件
        wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

        if (wake_fd_ < 0)
            throw std::system_error(errno, std::generic_category(), "eventfd");

        try {
            epoll_.add(wake_fd_, EpollInterest::read);
        } catch (...) {
            ::close(wake_fd_);
            throw;
        }
    }

    ~EventLoop() {
        // 使用者必须先停止并 join 运行此 Loop 的线程。
        ::close(wake_fd_);
    }

    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    // 在所属 I/O 线程调用，注册一个 Socket 的事件处理函数。
    void add_event(int fd, EpollInterest interest, Handler handler) {
        if (fd < 0 || fd == wake_fd_ || !handler)
            throw std::invalid_argument("invalid event handler");

        if (event_handlers_.contains(fd))
            throw std::logic_error("fd already registered");

        // shared_ptr 保证回调执行期间可以安全注销自身。
        auto callback = std::make_shared<Handler>(std::move(handler));
        event_handlers_.emplace(fd, std::move(callback));

        try {
            epoll_.add(fd, interest);
        } catch (...) {
            event_handlers_.erase(fd);
            throw;
        }
    }

    // 更新文件 inst
    void modify_inst(int fd, EpollInterest interest) {
        if (!event_handlers_.contains(fd))
            throw std::logic_error("fd not registered");

        epoll_.modify(fd, interest);
    }

    // 必须先注销事件，再关闭 Socket。
    void del_event(int fd) {
        if (!event_handlers_.contains(fd))
            return;

        epoll_.remove(fd);
        event_handlers_.erase(fd);
    }

    // 提交一个任务
    [[nodiscard]] bool post_task(Task task) {
        if (!task)
            return false;

        {
            std::lock_guard lock(mutex_);

            if (stopping_.load())
                return false;

            tasks_.push_back(std::move(task));
        }

        wakeup();
        return true;
    }

    // 由所属 I/O 线程调用。
    void run() {
        if (started_.exchange(true))
            throw std::logic_error("EventLoop already started");

        while (!stopping_.load()) {
            for (const auto event : epoll_.wait()) {
                if (event.fd == wake_fd_) {
                    drain_wakeup();
                    run_pending_tasks();
                    continue;
                }

                auto it = event_handlers_.find(event.fd);
                if (it == event_handlers_.end())
                    continue;

                // 保持回调存活，即使它调用 unwatch()。
                auto callback = it->second;

                try {
                    (*callback)(event);
                } catch (...) {
                    try {
                        runtime::log::error("EventLoop handler failed");
                    } catch (...) {
                    }

                    // 出错的回调不再接收新事件。
                    try {
                        del_event(event.fd);
                    } catch (...) {
                    }
                }
            }
        }

        // 停止时执行剩下的的跨线程任务。
        run_pending_tasks();
    }

    // 可以由其他线程调用。
    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopping_.store(true);
        }

        wakeup();
    }

  private:
    void wakeup() noexcept {
        std::uint64_t value = 1;

        while (true) {
            ssize_t n = ::write(wake_fd_, &value, sizeof(value));

            if (n == sizeof(value))
                return;

            if (n < 0 && errno == EINTR)
                continue;

            // 计数器已满，说明 eventfd 已经可读。
            if (n < 0 && errno == EAGAIN)
                return;

            std::terminate();
        }
    }

    // 清零 wakefd 计数器
    void drain_wakeup() noexcept {
        std::uint64_t value;

        while (true) {
            ssize_t n = ::read(wake_fd_, &value, sizeof(value));

            if (n == sizeof(value))
                continue;

            if (n < 0 && errno == EINTR)
                continue;

            if (n < 0 && errno == EAGAIN)
                return;

            std::terminate();
        }
    }

    void run_pending_tasks() {
        std::deque<Task> pending;

        {
            std::lock_guard lock(mutex_);
            pending.swap(tasks_);
        }

        // 不持有队列锁执行任务。
        for (auto &task : pending) {
            try {
                task();
            } catch (...) {
                try {
                    runtime::log::error("EventLoop task failed");
                } catch (...) {
                }
            }
        }
    }

    Epoll epoll_;
    int wake_fd_{-1};

    std::unordered_map<int, std::shared_ptr<Handler>> event_handlers_;

    // 多生产者、单消费者任务队列。
    std::mutex mutex_;

    // Worker 线程 post 到此
    std::deque<Task> tasks_;

    std::atomic<bool> stopping_{false};
    std::atomic<bool> started_{false};
};

} // namespace net

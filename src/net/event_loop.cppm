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
        // 其他线程（如 Worker 或主线程）向 wake_fd_ 写入计数后，
        // wake_fd_ 变为可读（EPOLLIN），从而唤醒阻塞在 epoll_.wait() 的 I/O 线程执行跨线程任务。
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

    // 在所属 I/O 线程调用，首次注册 fd 及其关注的事件掩码与回调。
    void add_event(int fd, EpollInterest interest, Handler handler) {
        if (fd < 0 || fd == wake_fd_ || !handler)
            throw std::invalid_argument("invalid event handler");

        if (event_handlers_.contains(fd))
            throw std::logic_error("fd already registered");

        // shared_ptr 保证回调执行期间即使内部调用 del_event() 注销自身也不会悬空。
        auto callback = std::make_shared<Handler>(std::move(handler));
        event_handlers_.emplace(fd, std::move(callback));

        try {
            epoll_.add(fd, interest);
        } catch (...) {
            event_handlers_.erase(fd);
            throw;
        }
    }

    // 修改已注册 fd 的关注事件掩码（如按需开启/关闭 EPOLLOUT 或重装 EPOLLONESHOT）。
    void modify_event(int fd, EpollInterest interest) {
        if (!event_handlers_.contains(fd))
            throw std::logic_error("fd not registered");

        epoll_.modify(fd, interest);
    }

    // 注销 fd 的事件监听并移除回调（必须在 close(fd) 之前调用）。
    void del_event(int fd) {
        if (!event_handlers_.contains(fd))
            return;

        epoll_.remove(fd);
        event_handlers_.erase(fd);
    }

    // 跨线程投递任务到本 I/O 线程执行。
    // 分支逻辑：若 Loop 正在停止则拒绝入队返回 false；否则入队并通过 wake_fd_ 唤醒 I/O 线程。
    [[nodiscard]] bool post_task(Task task) {
        if (!task)
            return false;

        bool empty;

        {
            std::lock_guard lock(mutex_);

            empty = tasks_.empty();

            if (stopping_.load())
                return false;

            tasks_.push_back(std::move(task));
        }

        if (empty) {
            wakeup();
        }

        return true;
    }

    // I/O 线程主事件循环。
    // 分支逻辑：
    //   1. 若就绪 fd 是 wake_fd_：读空 eventfd 计数器，批量执行跨线程队列 tasks_ 中的任务。
    //   2. 若就绪 fd 是普通 Socket/timerfd：查找对应回调并执行；若回调抛异常则注销该 fd
    //   防止反复触发。
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

                // 拷贝一份 shared_ptr，保证回调执行期间调用 del_event() 销毁 map 条目时闭包仍存活。
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

        // 退出循环前排空最后一批已入队的跨线程任务。
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

            // 连接中断，再试一次
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

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
            tasks_.reserve(64);
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

        auto [it, inserted] = event_handlers_.try_emplace(fd, std::move(handler));
        if (!inserted)
            throw std::logic_error("fd already registered");
        try {
            epoll_.add(fd, interest);
        } catch (...) {
            event_handlers_.erase(it);
            throw;
        }
    }

    // 修改已注册 fd 的关注事件掩码（如按需开启/关闭 EPOLLOUT 或重装 EPOLLONESHOT）。
    void modify_event(int fd, EpollInterest interest) {
        epoll_.modify(fd, interest);
    }

    // 注销 fd 的事件监听并移除回调（必须在 close(fd) 之前调用）。
    void del_event(int fd) {
        auto it = event_handlers_.find(fd);
        if (it == event_handlers_.end())
            return;

        // 正在执行的回调由 run() 的栈持有。清掉槽位指针，返回时就不会把旧
        // 回调写进已删除的节点，或盖掉随后注册在相同 fd 上的新回调。
        if (running_slot_ == &it->second)
            running_slot_ = nullptr;
        auto removed = event_handlers_.extract(it); // 其余回调持有的 fd 要活到 DEL 返回。
        epoll_.remove(fd);
    }

    // 跨线程投递任务到本 I/O 线程执行；队列从空变非空时通过 wake_fd_ 唤醒 I/O 线程。
    void post_task(Task task) {
        if (!task)
            return;

        bool empty;
        {
            std::lock_guard lock(mutex_);
            empty = tasks_.empty();
            tasks_.push_back(std::move(task));
        }

        if (empty)
            wakeup();
    }

    // I/O 线程主事件循环。
    // 分支逻辑：
    //   1. 若就绪 fd 是 wake_fd_：读空 eventfd 计数器，批量执行跨线程队列 tasks_ 中的任务。
    //   2. 若就绪 fd 是普通 Socket/timerfd：查找对应回调并执行；若回调抛异常则注销该 fd
    //   防止反复触发。
    void run() {
        std::vector<Task> pending; // 两个队列交换容量，整批任务在锁外执行。
        pending.reserve(64);
        while (!stopping_) {
            for (const auto event : epoll_.wait()) {
                if (event.fd == wake_fd_) {
                    drain_wakeup();
                    run_pending_tasks(pending);
                    continue;
                }

                auto it = event_handlers_.find(event.fd);
                if (it == event_handlers_.end())
                    continue;

                // rehash 不移动节点。把闭包移到栈上执行，它可以在调用中注销自己。
                running_slot_ = &it->second;
                auto callback = std::move(*running_slot_);
                try {
                    callback(event);
                } catch (...) {
                    try {
                        runtime::log::error("EventLoop handler failed");
                    } catch (...) {
                    }
                    if (running_slot_) {
                        try {
                            del_event(event.fd);
                        } catch (...) {
                        }
                    }
                }
                if (running_slot_) {
                    *running_slot_ = std::move(callback);
                    running_slot_ = nullptr;
                }
            }
        }

        // 退出循环前排空最后一批已入队的跨线程任务。
        run_pending_tasks(pending);
    }

    // 通过任务队列把停机切回所属 I/O 线程执行，stopping_ 纯单线程读写，无锁无原子。
    void stop() {
        post_task([this] { stopping_ = true; });
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

    // 普通 eventfd 一次成功读取就会将计数归零。随后交换任务队列，
    // 在交换之后投递的第一项仍会唤醒下一轮，无需再读一次 EAGAIN。
    void drain_wakeup() noexcept {
        std::uint64_t value;

        while (true) {
            ssize_t n = ::read(wake_fd_, &value, sizeof(value));

            if (n == sizeof(value))
                return;

            if (n < 0 && errno == EINTR)
                continue;

            if (n < 0 && errno == EAGAIN)
                return;

            std::terminate();
        }
    }

    void run_pending_tasks(std::vector<Task> &pending) {
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
        pending.clear();
    }

    Epoll epoll_;
    int wake_fd_{-1};
    bool stopping_{false};

    // 只有 run() 所在线程操作回调表。rehash 保持节点地址稳定。
    std::unordered_map<int, Handler> event_handlers_;
    Handler *running_slot_{nullptr};

    // 多生产者、单消费者任务队列（独占缓存行，避免 Worker 投递写锁失效 I/O 线程私有状态）。
    alignas(64) std::mutex mutex_;

    // Worker 线程 post 到此
    std::vector<Task> tasks_;
};

} // namespace net

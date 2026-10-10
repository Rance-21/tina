module;
#include <pthread.h>
export module runtime.worker_pool;
import std;
import runtime.log;

// 防止异常发生时忘记释放 pthread 锁。
namespace {
class Lock {
  public:
    explicit Lock(pthread_mutex_t &mutex) : mutex_(&mutex) {
        int rc = pthread_mutex_lock(mutex_);
        if (rc != 0)
            throw std::system_error(rc, std::generic_category());
    }

    ~Lock() {
        if (pthread_mutex_unlock(mutex_) != 0)
            std::terminate();
    }

    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

  private:
    pthread_mutex_t *mutex_;
};
} // namespace

export namespace runtime {

class WorkerPool {
  public:
    using Task = std::function<void()>;

    WorkerPool(std::size_t count, std::size_t capacity) : capacity_(capacity) {
        if (count == 0 || capacity == 0)
            throw std::invalid_argument("Invalid WorkerPool size");

        int rc = pthread_mutex_init(&mutex_, nullptr);
        if (rc != 0)
            throw std::system_error(rc, std::generic_category());

        rc = pthread_cond_init(&cv_, nullptr);
        if (rc != 0) {
            pthread_mutex_destroy(&mutex_);
            throw std::system_error(rc, std::generic_category());
        }

        try {
            threads_.reserve(count);

            for (std::size_t i = 0; i < count; ++i) {
                pthread_t thread{};

                rc = pthread_create(&thread, nullptr, &WorkerPool::entry, this);

                if (rc != 0)
                    throw std::system_error(rc, std::generic_category());

                threads_.push_back(thread);
            }
        } catch (...) {
            // 创建失败也必须回收已经启动的线程。
            shutdown();
            pthread_cond_destroy(&cv_);
            pthread_mutex_destroy(&mutex_);
            throw;
        }
    }

    ~WorkerPool() {
        shutdown();

        if (pthread_cond_destroy(&cv_) != 0)
            std::terminate();

        if (pthread_mutex_destroy(&mutex_) != 0)
            std::terminate();
    }

    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    // 非阻塞提交：队列满或已经停止时返回 false。
    [[nodiscard]] bool try_submit(Task task) {
        if (!task)
            return false;

        Lock lock(mutex_);

        if (stopping_ || tasks_.size() >= capacity_)
            return false;

        tasks_.push_back(std::move(task));

        if (pthread_cond_signal(&cv_) != 0)
            std::terminate();

        return true;
    }

    // 等待所有已经接受的任务执行完毕。
    // 必须由线程池外部的管理线程调用。
    void shutdown() noexcept {
        {
            Lock lock(mutex_);
            stopping_ = true;

            if (pthread_cond_broadcast(&cv_) != 0)
                std::terminate();
        }

        // join 时不能持有 mutex。
        for (pthread_t thread : threads_) {
            if (pthread_join(thread, nullptr) != 0)
                std::terminate();
        }

        threads_.clear();
    }

    [[nodiscard]] std::size_t pending() const {
        Lock lock(mutex_);
        return tasks_.size();
    }

  private:
    // pthread 需要 C 风格的线程入口。
    static void *entry(void *arg) noexcept {
        static_cast<WorkerPool *>(arg)->worker_loop();
        return nullptr;
    }

    void worker_loop() noexcept {
        while (true) {
            Task task;

            {
                Lock lock(mutex_);

                // 没有任务时释放锁并休眠。
                while (tasks_.empty() && !stopping_) {
                    if (pthread_cond_wait(&cv_, &mutex_) != 0)
                        std::terminate();
                }

                // 停止后仍然会执行队列里剩余的任务。
                if (tasks_.empty())
                    return;

                task = std::move(tasks_.front());
                tasks_.pop_front();
            }

            // 已经释放队列锁，可以与其他 Worker 并行。
            try {
                task();
            } catch (...) {
                try {
                    log::error("Worker task threw an exception");
                } catch (...) {
                    // 日志失败不能导致异常逃出线程入口。
                }
            }
        }
    }

    mutable pthread_mutex_t mutex_{};
    pthread_cond_t cv_{};

    std::deque<Task> tasks_;
    std::vector<pthread_t> threads_;

    std::size_t capacity_;
    bool stopping_{false};
};

} // namespace runtime

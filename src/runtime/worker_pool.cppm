module;
#include <pthread.h>
export module runtime.worker_pool;
import std;
import runtime.log;

export namespace runtime {

class WorkerPool {
  public:
    using Task = std::function<void()>;

    WorkerPool(std::size_t thread_count, std::size_t max_pending_tasks);

    ~WorkerPool();

    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    // 队列已满、线程池已停止或任务为空时返回 false。
    // 不等待队列腾出空间。
    [[nodiscard]] bool try_submit(Task task);

    // 停止接收新任务，执行完已入队任务，等待线程退出。
    // 必须由线程池外部的管理线程调用。
    void shutdown() noexcept;

    // 当前等待执行的任务数量，不包含正在执行的任务。
    [[nodiscard]] std::size_t pending() const;

  private:
    struct State;
    std::unique_ptr<State> state_;

    static void *thread_entry(void *arg) noexcept;
    void worker_loop();
};

} // namespace runtime

namespace {
// 用 RAII 保证发生异常时也能释放 pthread Mutex。
class MutexGuard {
  public:
    explicit MutexGuard(pthread_mutex_t &mutex) : mutex_(&mutex) {
        int rc = pthread_mutex_lock(mutex_);

        if (rc != 0) {
            throw std::system_error(rc, std::generic_category(), "pthread_mutex_lock");
        }
    }

    ~MutexGuard() {
        if (pthread_mutex_unlock(mutex_) != 0) {
            std::terminate();
        }
    }

    MutexGuard(const MutexGuard &) = delete;
    MutexGuard &operator=(const MutexGuard &) = delete;

  private:
    pthread_mutex_t *mutex_;
};

} // namespace

// 隐藏 pthread 的内部数据，外部只接触 WorkerPool 接口。
struct runtime::WorkerPool::State {
    pthread_mutex_t mutex{};
    pthread_cond_t condition{};

    std::deque<Task> tasks;
    std::vector<pthread_t> threads;

    std::size_t capacity;
    bool stopping{false};

    explicit State(std::size_t max_pending) : capacity(max_pending) {
        int rc = pthread_mutex_init(&mutex, nullptr);

        if (rc != 0) {
            throw std::system_error(rc, std::generic_category(), "pthread_mutex_init");
        }

        rc = pthread_cond_init(&condition, nullptr);

        if (rc != 0) {
            pthread_mutex_destroy(&mutex);

            throw std::system_error(rc, std::generic_category(), "pthread_cond_init");
        }
    }

    ~State() {
        if (pthread_cond_destroy(&condition) != 0) {
            std::terminate();
        }

        if (pthread_mutex_destroy(&mutex) != 0) {
            std::terminate();
        }
    }
};

namespace runtime {

WorkerPool::WorkerPool(std::size_t thread_count, std::size_t max_pending_tasks) {
    if (thread_count == 0 || max_pending_tasks == 0) {
        throw std::invalid_argument("WorkerPool requires positive thread and queue counts");
    }

    state_ = std::make_unique<State>(max_pending_tasks);

    auto &state = *state_;
    state.threads.reserve(thread_count);

    try {
        for (std::size_t i = 0; i < thread_count; ++i) {
            pthread_t thread{};

            int rc = pthread_create(&thread, nullptr, &WorkerPool::thread_entry, this);

            if (rc != 0) {
                throw std::system_error(rc, std::generic_category(), "pthread_create");
            }

            state.threads.push_back(thread);
        }
    } catch (...) {
        // 即使只创建成功一部分线程，也要全部回收。
        shutdown();
        throw;
    }
}

WorkerPool::~WorkerPool() {
    shutdown();
}

bool WorkerPool::try_submit(Task task) {
    if (!task) {
        return false;
    }

    auto &state = *state_;
    MutexGuard lock(state.mutex);

    if (state.stopping || state.tasks.size() >= state.capacity) {
        return false;
    }

    state.tasks.push_back(std::move(task));

    // 唤醒一个等待任务的 Worker。
    if (pthread_cond_signal(&state.condition) != 0) {
        std::terminate();
    }

    return true;
}

void *WorkerPool::thread_entry(void *arg) noexcept {
    auto *pool = static_cast<WorkerPool *>(arg);

    try {
        pool->worker_loop();
    } catch (...) {
        // pthread 入口不允许 C++ 异常逃逸。
        std::terminate();
    }

    return nullptr;
}

void WorkerPool::worker_loop() {
    auto &state = *state_;

    while (true) {
        Task task;

        {
            MutexGuard lock(state.mutex);

            // cond_wait 会释放 mutex，唤醒后重新获得。
            // while 同时处理虚假唤醒。
            while (state.tasks.empty() && !state.stopping) {
                int rc = pthread_cond_wait(&state.condition, &state.mutex);

                if (rc != 0) {
                    throw std::system_error(rc, std::generic_category(), "pthread_cond_wait");
                }
            }

            // 停止后继续处理剩余任务，直到队列为空。
            if (state.tasks.empty() && state.stopping) {
                return;
            }

            task = std::move(state.tasks.front());
            state.tasks.pop_front();
        }

        // 这里已经释放锁，多个 Worker 可以并行执行。
        try {
            task();
        } catch (const std::exception &error) {
            try {
                log::error(std::string("Worker task failed: ") + error.what());
            } catch (...) {
                // 日志系统异常不能终止 Worker。
            }
        } catch (...) {
            try {
                log::error("Worker task failed: unknown exception");
            } catch (...) {
            }
        }
    }
}

std::size_t WorkerPool::pending() const {
    auto &state = *state_;
    MutexGuard lock(state.mutex);

    return state.tasks.size();
}

void WorkerPool::shutdown() noexcept {
    if (!state_) {
        return;
    }

    auto &state = *state_;

    // 不允许新任务进入，唤醒全部正在等待的 Worker。
    if (pthread_mutex_lock(&state.mutex) != 0) {
        std::terminate();
    }

    state.stopping = true;

    if (pthread_cond_broadcast(&state.condition) != 0) {
        std::terminate();
    }

    if (pthread_mutex_unlock(&state.mutex) != 0) {
        std::terminate();
    }

    // join 时绝不能持有队列锁。
    for (pthread_t thread : state.threads) {
        if (pthread_join(thread, nullptr) != 0) {
            std::terminate();
        }
    }

    state.threads.clear();
}

} // namespace runtime

export module runtime.config;
import std;

export namespace runtime {
struct ServerConfig {
    // 每个 I/O 线程维护自己的 EventLoop。
    std::size_t io_threads{2};
    std::size_t worker_threads{2};

    // WorkerPool 中最多允许排队的任务数。
    std::size_t max_pending_tasks{4096};

    void validate() const {
        if (io_threads == 0) {
            throw std::invalid_argument("io_threads must be > 0");
        }

        if (worker_threads == 0) {
            throw std::invalid_argument("worker_threads must be > 0");
        }

        if (max_pending_tasks == 0) {
            throw std::invalid_argument("max_pending_tasks must be > 0");
        }
    }
};

} // namespace runtime

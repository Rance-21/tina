module;
#include <errno.h>
#include <sys/inotify.h>
#include <unistd.h>

export module service.file_watcher;

import std;
import net.epoll;
import net.event_loop;
import service.workspace_cache;

export namespace service {

// ============================================================================
// 文件内容缓存专属 inotify 监听管理器（与 DirWatcher 解耦，仅负责文件缓存失效）
// ============================================================================
// - 与 DirWatcher 分离：DirWatcher 仅监听目录项增删改名并推送 list_dir Event，
//   FileWatcher 仅监听被缓存文件的修改/属性/删除/移动事件并使 WorkspaceCache 文件条目失效；
// - 即使文件所属目录从未调用过 workspace_list_dir，只要文件被 workspace_read_file 缓存，
//   也能独立获得 inotify 失效保护；
// - 挂载在某个 I/O 线程的 EventLoop 上接收 epoll 可读通知，供多个 Worker 线程并发注册/注销文件。
class FileWatcher : public std::enable_shared_from_this<FileWatcher> {
  public:
    explicit FileWatcher(WorkspaceCache *cache = &global_workspace_cache()) : cache_(cache) {
    }

    ~FileWatcher() {
        if (inotify_fd_ >= 0)
            ::close(inotify_fd_);
    }

    FileWatcher(const FileWatcher &) = delete;
    FileWatcher &operator=(const FileWatcher &) = delete;

    // 在所属 EventLoop 线程调用：初始化 inotify_fd_ 并挂载到 epoll，同时绑定容量淘汰回调（只启动一次）。
    void start(net::EventLoop &loop) {
        std::lock_guard lock(mutex_);
        if (stopped_ || inotify_fd_ >= 0)
            return;

        inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd_ < 0)
            return;

        std::weak_ptr<FileWatcher> weak = weak_from_this();
        try {
            loop.add_event(inotify_fd_, net::EpollInterest::read, [weak](net::EpollEvent event) {
                if (auto self = weak.lock())
                    self->on_event(event);
            });
            loop_ = &loop;
        } catch (...) {
            ::close(inotify_fd_);
            inotify_fd_ = -1;
            return;
        }

        if (cache_ != nullptr) {
            cache_->set_file_evict_callback([weak](std::string_view evicted_rel_path) {
                if (auto self = weak.lock())
                    self->unwatch_file(evicted_rel_path);
            });
        }
    }

    // 在所属 EventLoop 线程调用：摘除 epoll 监听并关闭 inotify_fd_（停机阶段调用）。
    void stop() noexcept {
        int fd = -1;
        net::EventLoop *loop = nullptr;

        {
            std::lock_guard lock(mutex_);
            if (stopped_)
                return;
            stopped_ = true;
            fd = inotify_fd_;
            inotify_fd_ = -1;
            loop = loop_;
            loop_ = nullptr;
            wd_to_path_.clear();
            path_to_wd_.clear();
        }

        if (loop != nullptr && fd >= 0) {
            try {
                loop->del_event(fd);
            } catch (...) {
            }
        }
        if (fd >= 0)
            ::close(fd);
    }

    // 为缓存文件注册 inotify 监听（线程安全，若已监听则直接返回）。
    void watch_file(std::string_view rel_path, const std::string &full_path) {
        if (rel_path.empty() || full_path.empty())
            return;

        std::lock_guard lock(mutex_);
        if (stopped_ || inotify_fd_ < 0)
            return;

        if (path_to_wd_.contains(rel_path))
            return;

        constexpr std::uint32_t mask =
            IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;
        const int wd = ::inotify_add_watch(inotify_fd_, full_path.c_str(), mask);
        if (wd < 0)
            return;

        // 若多个路径硬链接到同一 inode，内核可能复用同一个 wd，清理旧路径映射避免悬空。
        if (auto old_it = wd_to_path_.find(wd); old_it != wd_to_path_.end())
            path_to_wd_.erase(old_it->second);

        auto [pit, _] = path_to_wd_.emplace(std::string(rel_path), wd);
        wd_to_path_[wd] = pit->first;
    }

    // 取消单个文件的 inotify 监听（线程安全，供 LRU 淘汰或 write_file 替换前调用）。
    void unwatch_file(std::string_view rel_path) noexcept {
        if (rel_path.empty())
            return;

        std::lock_guard lock(mutex_);
        if (stopped_ || inotify_fd_ < 0)
            return;

        auto it = path_to_wd_.find(rel_path);
        if (it == path_to_wd_.end())
            return;

        const int wd = it->second;
        (void)::inotify_rm_watch(inotify_fd_, wd);
        wd_to_path_.erase(wd);
        path_to_wd_.erase(it);
    }

    // 取消指定目录下已监听文件的 inotify watch：
    // subtree == false 时仅取消直接子文件（用于 workspace_close_dir）；
    // subtree == true 时级联取消整棵子目录树（用于目录被删除/移动）。
    void unwatch_dir(std::string_view dir_rel_path, bool subtree = false) noexcept {
        if (dir_rel_path.empty())
            return;

        std::lock_guard lock(mutex_);
        if (stopped_ || inotify_fd_ < 0)
            return;

        for (auto it = path_to_wd_.begin(); it != path_to_wd_.end();) {
            const std::string_view parent = parent_dir_of(it->first);
            const bool matched =
                subtree ? is_same_or_under(parent, dir_rel_path) : (parent == dir_rel_path);
            if (matched) {
                (void)::inotify_rm_watch(inotify_fd_, it->second);
                wd_to_path_.erase(it->second);
                it = path_to_wd_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 返回当前正在监听的文件数量（主要用于单测验证）。
    [[nodiscard]] std::size_t watched_count() const noexcept {
        std::lock_guard lock(mutex_);
        return path_to_wd_.size();
    }

  private:
    struct TransparentStringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }

        [[nodiscard]] std::size_t operator()(const std::string &s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };

    [[nodiscard]] static std::string_view parent_dir_of(std::string_view rel_path) noexcept {
        const std::size_t slash = rel_path.rfind('/');
        return (slash == std::string_view::npos) ? std::string_view{} : rel_path.substr(0, slash);
    }

    [[nodiscard]] static bool is_same_or_under(std::string_view path,
                                               std::string_view dir_prefix) noexcept {
        if (path == dir_prefix)
            return true;
        return path.size() > dir_prefix.size() && path.starts_with(dir_prefix) &&
               path[dir_prefix.size()] == '/';
    }

    void on_event(net::EpollEvent event) {
        if (event.error() || event.hangup()) {
            stop();
            return;
        }

        int fd = -1;
        {
            std::lock_guard lock(mutex_);
            if (stopped_ || inotify_fd_ < 0)
                return;
            fd = inotify_fd_;
        }

        alignas(inotify_event) std::array<char, 4096> buffer{};
        std::vector<std::string> invalidated_paths;
        bool overflow = false;

        while (true) {
            const ssize_t n = ::read(fd, buffer.data(), buffer.size());
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break; // EAGAIN：本轮内核事件已读空
            }
            if (n == 0)
                break;

            std::lock_guard lock(mutex_);
            if (stopped_ || inotify_fd_ != fd)
                return;

            std::size_t offset = 0;
            while (offset + sizeof(inotify_event) <= static_cast<std::size_t>(n)) {
                const auto *ev = reinterpret_cast<const inotify_event *>(buffer.data() + offset);
                offset += sizeof(inotify_event) + ev->len;

                if ((ev->mask & IN_Q_OVERFLOW) != 0) {
                    overflow = true;
                    continue;
                }

                auto it = wd_to_path_.find(ev->wd);
                if (it == wd_to_path_.end())
                    continue;

                invalidated_paths.push_back(it->second);

                if ((ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                    if ((ev->mask & IN_IGNORED) == 0)
                        (void)::inotify_rm_watch(fd, ev->wd);
                    path_to_wd_.erase(it->second);
                    wd_to_path_.erase(it);
                }
            }
        }

        // 在释放 mutex_ 后再操作 WorkspaceCache，彻底杜绝与缓存分片锁交叉加锁引发死锁。
        if (cache_ == nullptr)
            return;

        if (overflow) {
            cache_->clear_files();
            return;
        }

        for (const std::string &path : invalidated_paths)
            cache_->invalidate_file(path);
    }

    mutable std::mutex mutex_;
    net::EventLoop *loop_{nullptr};
    WorkspaceCache *cache_{nullptr};

    int inotify_fd_{-1};
    bool stopped_{false};

    std::unordered_map<int, std::string> wd_to_path_;
    std::unordered_map<std::string, int, TransparentStringHash, std::equal_to<>> path_to_wd_;
};

// 进程级全局 FileWatcher 实例（由主服务端挂载至 I/O 线程 EventLoop 驱动事件）。
[[nodiscard]] inline const std::shared_ptr<FileWatcher> &global_file_watcher_shared() {
    static const auto instance = std::make_shared<FileWatcher>();
    return instance;
}

[[nodiscard]] inline FileWatcher &global_file_watcher() {
    return *global_file_watcher_shared();
}

} // namespace service

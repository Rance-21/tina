module;
#include <errno.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

export module service.file_watcher;

import std;
import net.epoll;
import net.event_loop;
import service.workspace_cache;
import service.file;
import ide.protocol;

export namespace service {

// ============================================================================
// 文件内容缓存专属 inotify 监听管理器（与 DirWatcher 解耦，仅负责文件缓存失效）
// ============================================================================
// - 与 DirWatcher 分离：DirWatcher 仅监听目录项增删改名并推送 list_dir Event，
//   FileWatcher 仅监听被缓存文件的修改/属性/删除/移动事件并使 WorkspaceCache 文件条目失效；
// - 即使文件所属目录从未调用过 workspace_list_dir，只要文件被 workspace_read_file 缓存，
//   也能独立获得 inotify 失效保护；
// - 挂载在某个 I/O 线程的 EventLoop 上接收 epoll 可读通知，供多个 Worker 线程并发注册/注销文件。
class FileWatcher {
    using Paths =
        std::unordered_map<std::string, int, detail::TransparentStringHash, std::equal_to<>>;
    // 每个 fd 有独立的 wd 命名空间。冷文件的 add_watch / rm_watch 可以由不同
    // Worker 并行执行；硬链接落到不同分片时，内核分别向两个 fd 投递事件。
    struct alignas(64) Watches {
        mutable std::mutex mutex;
        // -1 允许独立调用 Workspace 服务（未启动监听）。启动失败或停机后禁止回填。
        static constexpr int unstarted = -1, disabled = -2;
        int fd{unstarted};
        Paths paths;
        std::unordered_multimap<int, std::string_view> aliases;

        ~Watches() {
            if (fd >= 0)
                ::close(fd);
        }

        void remove(Paths::iterator it) noexcept {
            const int wd = it->second;
            auto [first, end] = aliases.equal_range(wd);
            const bool last_alias = std::next(first) == end;
            while (first->second != it->first)
                ++first;
            aliases.erase(first);
            paths.erase(it);
            if (last_alias)
                (void)::inotify_rm_watch(fd, wd);
        }
    };

    Watches &for_path(std::string_view path) {
        return watches_[detail::TransparentStringHash{}(path) % watches_.size()];
    }

  public:
    explicit FileWatcher(WorkspaceCache *cache = &global_workspace_cache()) : cache_(cache) {}

    ~FileWatcher() {
        if (cache_)
            cache_->set_file_evict_callback({});
    }

    FileWatcher(const FileWatcher &) = delete;
    FileWatcher &operator=(const FileWatcher &) = delete;

    // start、stop 和 epoll 回调均属于同一个 EventLoop；Worker 只在对应的 mutex 内借用 fd。
    void start(net::EventLoop &loop) {
        if (watches_.front().fd != Watches::unstarted)
            return;
        loop_ = &loop;
        for (auto &w : watches_) {
            std::lock_guard lock(w.mutex);
            w.fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (w.fd < 0) {
                w.fd = Watches::disabled;
                continue;
            }
            try {
                loop.add_event(w.fd, net::EpollInterest::read,
                               [this, &w](net::EpollEvent event) { on_event(w, event); });
            } catch (...) {
                ::close(std::exchange(w.fd, Watches::disabled));
            }
        }
        if (cache_)
            cache_->set_file_evict_callback(
                [this](std::string_view path) { unwatch_if_uncached(path); });
    }

    void stop() noexcept {
        for (auto &w : watches_) {
            int fd;
            {
                std::lock_guard lock(w.mutex);
                fd = std::exchange(w.fd, Watches::disabled);
                w.aliases.clear();
                w.paths.clear();
            }
            if (fd >= 0) {
                try {
                    loop_->del_event(fd);
                } catch (...) {
                }
                ::close(fd);
            }
        }
        loop_ = nullptr;
        if (cache_)
            cache_->clear_files();
    }

    // 读盘/写盘已经结束，再给当前路径注册监听。最后一次 stat 把 fd 读到的
    // 版本和被监听的路径对应起来，覆盖 open、rename、add_watch 之间的窗口。
    // watch 之后的变更由 inotify 推进 epoch；若容量或 epoch 不允许入缓存，
    // 本次 RPC 仍可发送自己已持有的结果，同时释放没有缓存依赖的 watch。
    WorkspaceCache::FileHandle cache_file_if_current(std::string_view path, int root_fd,
                                                     const char *rel_cstr, int fd,
                                                     std::shared_ptr<const ReadFileResponse> value,
                                                     std::uint64_t epoch) {
        // 通过已打开的 fd 监听实际读写的 inode。即使路径在两次系统调用间
        // 被换走又换回，监听也不会落在无关文件上。缓冲区足以容纳十进制 int。
        char inode_path[32] = "/proc/self/fd/";
        std::to_chars(inode_path + sizeof("/proc/self/fd/") - 1, std::end(inode_path) - 1, fd);
        struct stat current{};
        if (cache_ && watch_inode(path, inode_path) &&
            ::fstatat(root_fd, rel_cstr, &current, 0) == 0 &&
            file::file_revision(current) == value->revision) {
            if (auto pin = cache_->put_file(path, std::move(value), epoch))
                return pin;
        }
        unwatch_if_uncached(path);
        return {};
    }

    // 为缓存文件注册 inotify 监听（同 inode 的重复 add_watch 返回已有 wd）。
    bool watch_file(std::string_view rel_path, const std::string &full_path) {
        return watch_inode(rel_path, full_path.c_str());
    }

    // 写入 rename 换 inode 后强制撤销旧监听。此操作之后的填充必须重新 watch。
    void unwatch_file(std::string_view path) noexcept {
        auto &w = for_path(path);
        std::lock_guard lock(w.mutex);
        if (auto it = w.paths.find(path); it != w.paths.end())
            w.remove(it);
    }

    // 延迟到达的淘汰回调只能移除已无缓存的 watch。锁顺序固定为 watcher -> shard：
    // 检查空缺时同步推进 epoch；稍后取得新 epoch 的填充还须在此 mutex 后注册监听。
    void unwatch_if_uncached(std::string_view path) noexcept {
        auto &w = for_path(path);
        std::lock_guard lock(w.mutex);
        auto it = w.paths.find(path);
        if (it != w.paths.end() && (!cache_ || cache_->cancel_file_fill_if_absent(path)))
            w.remove(it);
    }

    // 取消指定目录下已监听文件的 inotify watch：
    // subtree == false 时仅取消直接子文件（用于 workspace_close_dir）；
    // subtree == true 时级联取消整棵子目录树（用于目录被删除/移动）。
    void unwatch_dir(std::string_view dir_rel_path, bool subtree = false) noexcept {
        if (dir_rel_path.empty())
            return;

        for (auto &w : watches_) {
            std::lock_guard lock(w.mutex);
            for (auto it = w.paths.begin(); it != w.paths.end();) {
                const std::string_view parent = detail::parent_dir_of(it->first);
                const bool matched = subtree ? detail::is_same_or_under(parent, dir_rel_path)
                                             : (parent == dir_rel_path);
                auto current = it++;
                if (matched && (!cache_ || cache_->cancel_file_fill_if_absent(current->first)))
                    w.remove(current);
            }
        }
    }

    // 队列溢出可能连 IN_IGNORED 或目录移动也丢了。先逐片撤销监听，再使所有旧
    // 回填凭据过期。之后读到的文件会按当前路径建立新监听。
    void clear_files_and_watches() noexcept {
        for (auto &w : watches_) {
            std::lock_guard lock(w.mutex);
            while (!w.paths.empty())
                w.remove(w.paths.begin());
        }
        if (cache_)
            cache_->clear_files();
    }

    // 返回当前正在监听的文件数量（主要用于单测验证）。
    [[nodiscard]] std::size_t watched_count() const noexcept {
        std::size_t count = 0;
        for (auto &w : watches_) {
            std::lock_guard lock(w.mutex);
            count += w.paths.size();
        }
        return count;
    }

  private:
    bool watch_inode(std::string_view rel_path, const char *inode_path) {
        auto &w = for_path(rel_path);
        std::lock_guard lock(w.mutex);
        // 直接调用工作区服务时允许使用无监听的独立缓存；服务启动后注册失败则放弃回填。
        if (w.fd < 0)
            return w.fd == Watches::unstarted;
        // 路径可能已被外部 rename 指向新 inode，即使表中同名也要确认 wd。
        // 此操作仅发生于缓存回填；GET 命中不会经过 FileWatcher。
        constexpr std::uint32_t mask =
            IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;
        const int wd = ::inotify_add_watch(w.fd, inode_path, mask);
        if (wd < 0)
            return false;
        if (auto it = w.paths.find(rel_path); it != w.paths.end()) {
            if (it->second == wd)
                return true;
            // 迟到的读盘可能拿着旧 fd。保留已有监听，交由其失效事件或淘汰回调清理。
            if (!w.aliases.contains(wd))
                (void)::inotify_rm_watch(w.fd, wd);
            return false;
        }
        // 一个 inode 可以有多个硬链接。wd 侧只借用路径表中地址稳定的 key，
        // 最后一个别名离开时才释放内核监听。
        try {
            auto it = w.paths.emplace(std::string(rel_path), wd).first;
            try {
                w.aliases.emplace(wd, it->first);
            } catch (...) {
                w.paths.erase(it);
                throw;
            }
        } catch (const std::bad_alloc &) {
            if (!w.aliases.contains(wd))
                (void)::inotify_rm_watch(w.fd, wd);
            return false;
        }
        return true;
    }

    void on_event(Watches &w, net::EpollEvent event) {
        if (event.error() || event.hangup()) {
            stop();
            return;
        }

        // start、stop 和 epoll 回调均在同一 I/O 线程，Worker 只在 mutex 内借用 fd。
        const int fd = w.fd;
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

            std::lock_guard lock(w.mutex);

            std::size_t offset = 0;
            while (offset + sizeof(inotify_event) <= static_cast<std::size_t>(n)) {
                const auto *ev = reinterpret_cast<const inotify_event *>(buffer.data() + offset);
                offset += sizeof(inotify_event) + ev->len;

                if ((ev->mask & IN_Q_OVERFLOW) != 0) {
                    overflow = true;
                    continue;
                }

                auto [first, end] = w.aliases.equal_range(ev->wd);
                // 首次内容/属性变更就使缓存失效。一起撤销旧监听，后续回填会重新
                // 监听当时的 inode；没有再次读取的冷文件不再占用一个内核 watch。
                if (first != end && !(ev->mask & IN_IGNORED))
                    (void)::inotify_rm_watch(fd, ev->wd);
                while (first != end) {
                    auto path = w.paths.find(first->second);
                    first = w.aliases.erase(first);
                    // 直接带走哈希表里的路径，解锁后再使缓存失效。
                    auto node = w.paths.extract(path);
                    if (!overflow) {
                        try {
                            invalidated_paths.push_back(std::move(node.key()));
                        } catch (const std::bad_alloc &) {
                            overflow = true;
                        }
                    }
                }
            }
        }

        if (overflow) {
            clear_files_and_watches();
        } else if (cache_) {
            for (const auto &path : invalidated_paths) {
                cache_->invalidate_file(path);
                // 处理事件期间其他 Worker 可能刚完成同路径回填。清掉这次失效后
                // 无人使用的监听，或保留已经被下一次回填接管的监听。
                unwatch_if_uncached(path);
            }
        }
    }

    std::array<Watches, 16> watches_;
    net::EventLoop *loop_{};
    WorkspaceCache *cache_;
};

// 进程级全局 FileWatcher 实例（由主服务端挂载至 I/O 线程 EventLoop 驱动事件）。
[[nodiscard]] inline FileWatcher &global_file_watcher() {
    static FileWatcher instance{};
    return instance;
}

} // namespace service

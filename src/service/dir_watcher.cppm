module;
#include <errno.h>
#include <sys/inotify.h>
#include <unistd.h>

export module service.dir_watcher;

import std;
import net.epoll;
import net.event_loop;
import runtime.worker_pool;
import rpc.connection;
import ide.protocol;
import ide.server;
import service.file_watcher;
import service.workspace_cache;

export namespace service {

// ============================================================================
// 每个 I/O 线程独立的目录监听管理器（共享单个 inotify fd，无全局监听表）
// ============================================================================
// 仅监听曾通过 workspace_list_dir 返回给客户端的目录结构变更（增删改名/目录自身删除移动）。
// 当 inotify_fd 变为可读时，使 WorkspaceCache 中对应目录缓存失效并提交给 WorkerPool 重新扫描，
// 再通过 post_task 回到所属 I/O 线程向订阅该目录的存活 Connection 推送 Kind::event 帧；
// 文件内容与属性监听则由独立的 FileWatcher 负责。
// 单个 IO 线程对应单个 inotify-fd，因此无须加锁
class DirWatcher : public std::enable_shared_from_this<DirWatcher> {
  public:
    using RefreshConnFn = std::function<void(const std::shared_ptr<rpc::Connection> &)>;

    DirWatcher(net::EventLoop &loop, rpc::ConnectionMap &connections, runtime::WorkerPool &workers,
               rpc::ConnectionCounter &counter, RefreshConnFn refresh_conn = {},
               WorkspaceCache *cache = &global_workspace_cache(),
               FileWatcher *file_watcher = &global_file_watcher())
        : loop_(loop), connections_(connections), workers_(workers), counter_(counter),
          refresh_conn_(std::move(refresh_conn)), cache_(cache), file_watcher_(file_watcher) {
    }

    ~DirWatcher() {
        if (inotify_fd_ >= 0)
            ::close(inotify_fd_);
    }

    DirWatcher(const DirWatcher &) = delete;
    DirWatcher &operator=(const DirWatcher &) = delete;

    // 在所属 I/O 线程初始化并挂载 inotify fd 到 EventLoop epoll（随 I/O 线程启动一次）。
    void start() {
        if (stopped_ || inotify_fd_ >= 0)
            return;

        inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd_ < 0)
            return;

        std::weak_ptr<DirWatcher> weak = shared_from_this();
        try {
            loop_.add_event(inotify_fd_, net::EpollInterest::read, [weak](net::EpollEvent event) {
                if (auto self = weak.lock())
                    self->on_event(event);
            });
        } catch (...) {
            ::close(inotify_fd_);
            inotify_fd_ = -1;
        }
    }

    // 在所属 I/O 线程停止监听并释放 inotify fd（停机阶段调用）。
    void stop() noexcept {
        if (stopped_)
            return;
        stopped_ = true;

        if (inotify_fd_ >= 0) {
            try {
                loop_.del_event(inotify_fd_);
            } catch (...) {
            }
            ::close(inotify_fd_);
            inotify_fd_ = -1;
        }
        // 停机仅发送积压响应并停止主动事件推送，不再继续维护本地 watch 映射。
        watches_.clear();
        path_to_wd_.clear();
    }

    // 记录某条客户端连接已获取过 rel_path 目录列表，将该目录加入本 I/O 线程的 inotify 监听。
    void watch(std::string_view rel_path, const std::shared_ptr<rpc::Connection> &conn) {
        if (stopped_ || inotify_fd_ < 0 || rel_path.empty() || !conn || conn->peer_eof() ||
            conn->draining()) {
            return;
        }

        // 多个 conn，直接更新该 wd 对应的 conn 集合
        if (auto it = path_to_wd_.find(rel_path); it != path_to_wd_.end()) {
            if (auto wit = watches_.find(it->second); wit != watches_.end()) {
                wit->second.subscribers[conn->fd()] = conn;
                return;
            }
            path_to_wd_.erase(it);
        }

        auto full_path = server::resolve_workspace_dir_path(rel_path);
        if (!full_path)
            return;

        // 仅监听目录项增删改名及目录自身删除/移动事件；文件内容修改由 FileWatcher 独立负责。
        constexpr std::uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                       IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
        const int wd = ::inotify_add_watch(inotify_fd_, full_path->c_str(), mask);
        if (wd < 0)
            return;

        auto &entry = watches_[wd];
        if (entry.rel_path.empty())
            entry.rel_path.assign(rel_path);
        entry.subscribers[conn->fd()] = conn;
        path_to_wd_[entry.rel_path] = wd;
    }

    // 取消某条连接对 rel_path 目录的监听（由 workspace_close_dir 触发）。
    // 若移除后该目录已无任何存活订阅者，则立即调用 inotify_rm_watch 释放内核槽位并淘汰缓存。
    void unwatch(std::string_view rel_path, int conn_fd) noexcept {
        if (stopped_ || rel_path.empty() || inotify_fd_ < 0)
            return;

        auto it = path_to_wd_.find(rel_path);
        if (it != path_to_wd_.end()) {
            if (auto wit = watches_.find(it->second); wit != watches_.end()) {
                wit->second.subscribers.erase(conn_fd);
                if (!has_subscribers(wit->second))
                    remove_watch(it->second, true, false);
                return;
            }
            path_to_wd_.erase(it);
        }
        evict_dir_and_files(rel_path);
    }

  private:
    struct WatchEntry {
        std::string rel_path;
        std::unordered_map<int, std::weak_ptr<rpc::Connection>> subscribers;
        bool refreshing{false};
        bool dirty{false};
    };

    static inline std::string join_rel_path(std::string_view dir, std::string_view name) {
        std::string out;
        out.reserve(dir.size() + 1 + name.size());
        out.append(dir);
        out.push_back('/');
        out.append(name);
        return out;
    }

    // 同步清理 WorkspaceCache 缓存与 FileWatcher 监听；subtree == true 时级联清理整棵子树。
    void evict_dir_and_files(std::string_view rel_path, bool subtree = false) noexcept {
        if (rel_path.empty())
            return;
        if (cache_ != nullptr) {
            if (subtree)
                cache_->invalidate_subtree(rel_path);
            else
                cache_->invalidate_dir_and_files(rel_path);
        }
        if (file_watcher_ != nullptr)
            file_watcher_->unwatch_dir(rel_path, subtree);
    }

    // 撤销指定 wd 的监听并同步淘汰其目录及子文件缓存（subtree == true 时级联淘汰整棵子树）。
    void remove_watch(int wd, bool rm_kernel_watch, bool subtree = false) noexcept {
        auto it = watches_.find(wd);
        if (it == watches_.end())
            return;

        if (rm_kernel_watch && inotify_fd_ >= 0)
            (void)::inotify_rm_watch(inotify_fd_, wd);

        evict_dir_and_files(it->second.rel_path, subtree);
        path_to_wd_.erase(it->second.rel_path);
        watches_.erase(it);
    }

    static bool has_subscribers(WatchEntry &entry) {
        for (auto it = entry.subscribers.begin(); it != entry.subscribers.end();) {
            auto conn = it->second.lock();
            if (!conn || conn->peer_eof() || conn->draining())
                it = entry.subscribers.erase(it);
            else
                ++it;
        }
        return !entry.subscribers.empty();
    }

    // epoll 到 inotify-fd 的 handler 函数
    void on_event(net::EpollEvent event) {
        if (stopped_ || inotify_fd_ < 0)
            return;

        if (event.error() || event.hangup()) {
            stop();
            return;
        }

        alignas(inotify_event) std::array<char, 4096> buffer{};
        std::unordered_set<int> changed_wds;
        std::unordered_set<std::uint32_t> tmp_move_cookies;
        bool overflow = false;

        while (true) {
            const ssize_t n = ::read(inotify_fd_, buffer.data(), buffer.size());
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break; // EAGAIN：本轮内核事件已读空
            }
            if (n == 0)
                break;

            std::size_t offset = 0;
            while (offset + sizeof(inotify_event) <= static_cast<std::size_t>(n)) {
                const auto *ev = reinterpret_cast<const inotify_event *>(buffer.data() + offset);
                offset += sizeof(inotify_event) + ev->len;

                // 内核队列满、可能丢事件了
                if ((ev->mask & IN_Q_OVERFLOW) != 0) {
                    overflow = true;
                    continue;
                }

                // 内核通知这个 wd 已经失效（比如目录被删或被卸载），内核通知的，不需要 remove 内核
                if ((ev->mask & IN_IGNORED) != 0) {
                    remove_watch(ev->wd, false, true);
                    changed_wds.erase(ev->wd);
                    continue;
                }

                if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                    remove_watch(ev->wd, true, true);
                    changed_wds.erase(ev->wd);
                    continue;
                }

                std::string_view entry_name;
                if (ev->len > 0) {
                    std::string_view raw(ev->name, ev->len);
                    entry_name = raw.substr(0, raw.find('\0'));
                }

                // 忽略服务端 write_file 原子替换过程中创建的 .tmp.XXXXXX 临时文件及其配对 rename
                // 事件。
                if (!entry_name.empty() && entry_name.find(".tmp.") != std::string_view::npos) {
                    if ((ev->mask & IN_MOVED_FROM) != 0 && ev->cookie != 0)
                        tmp_move_cookies.insert(ev->cookie);
                    continue;
                }
                if ((ev->mask & IN_MOVED_TO) != 0 && ev->cookie != 0 &&
                    tmp_move_cookies.contains(ev->cookie)) {
                    continue;
                }

                auto wit = watches_.find(ev->wd);
                if (wit == watches_.end())
                    continue;

                if (cache_ != nullptr)
                    cache_->invalidate_dir(wit->second.rel_path);
                // 若被删除或移走的是子目录，级联清理该子目录树缓存并解除对应 FileWatcher 监听。
                if (!entry_name.empty() && (ev->mask & IN_ISDIR) != 0 &&
                    (ev->mask & (IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)) != 0) {
                    evict_dir_and_files(join_rel_path(wit->second.rel_path, entry_name), true);
                }
                changed_wds.insert(ev->wd);
            }
        }

        // 丢事件，无法更新 cache ，所有删除重来（单 inotify 对应多 path）
        if (overflow) {
            if (cache_ != nullptr)
                cache_->clear_dirs();
            for (const auto &[wd, _] : watches_)
                changed_wds.insert(wd);
        }

        for (int wd : changed_wds)
            schedule_refresh(wd);
    }

    // 把目录扫描扔到 Worker 线程
    void schedule_refresh(int wd) {
        auto it = watches_.find(wd);
        if (it == watches_.end())
            return;

        WatchEntry &entry = it->second;
        if (!has_subscribers(entry)) {
            remove_watch(wd, true);
            return;
        }

        if (entry.refreshing) {
            entry.dirty = true;
            return;
        }

        entry.refreshing = true;
        entry.dirty = false;
        const std::string rel_path = entry.rel_path;
        std::weak_ptr<DirWatcher> weak = shared_from_this();

        if (!workers_.try_submit([weak, wd, rel_path]() {
                auto event_frame = server::build_workspace_list_dir_event(rel_path);
                if (auto self = weak.lock()) {
                    (void)self->loop_.post_task(
                        [weak, wd, event_frame = std::move(event_frame)]() mutable {
                            if (auto watcher = weak.lock())
                                watcher->complete_refresh(wd, std::move(event_frame));
                        });
                }
            })) {
            entry.refreshing = false;
        }
    }

    void complete_refresh(int wd, std::optional<Frame> event_frame) {
        if (stopped_)
            return;

        auto it = watches_.find(wd);
        if (it == watches_.end())
            return;

        WatchEntry &entry = it->second;
        if (!has_subscribers(entry)) {
            remove_watch(wd, true);
            return;
        }

        if (event_frame) {
            for (auto sub_it = entry.subscribers.begin(); sub_it != entry.subscribers.end();) {
                auto conn = sub_it->second.lock();
                if (!conn || conn->peer_eof() || conn->draining()) {
                    sub_it = entry.subscribers.erase(sub_it);
                    continue;
                }
                ++sub_it;

                try {
                    if (!conn->enqueue_event(*event_frame)) {
                        rpc::close_connection(loop_, connections_, counter_, conn->fd());
                        continue;
                    }
                    if (conn->wants_write())
                        conn->write();
                    if (refresh_conn_)
                        refresh_conn_(conn);
                } catch (...) {
                    rpc::close_connection(loop_, connections_, counter_, conn->fd());
                }
            }
        }

        if (!has_subscribers(entry)) {
            remove_watch(wd, true);
            return;
        }

        if (entry.dirty) {
            entry.refreshing = false;
            entry.dirty = false;
            schedule_refresh(wd);
        } else {
            entry.refreshing = false;
        }
    }

    net::EventLoop &loop_;
    rpc::ConnectionMap &connections_;
    runtime::WorkerPool &workers_;
    rpc::ConnectionCounter &counter_;
    RefreshConnFn refresh_conn_;
    WorkspaceCache *cache_{nullptr};
    FileWatcher *file_watcher_{nullptr};

    int inotify_fd_{-1};
    bool stopped_{false};

    std::unordered_map<int, WatchEntry> watches_;
    std::unordered_map<std::string, int, detail::TransparentStringHash, std::equal_to<>> path_to_wd_;
};

} // namespace service

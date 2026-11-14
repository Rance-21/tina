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

export namespace service {

// ============================================================================
// 每个 I/O 线程独立的目录监听管理器（共享单个 inotify fd，无全局监听表）
// ============================================================================
// 仅监听曾通过 workspace_list_dir 返回给客户端的目录。
// 当 inotify_fd 变为可读时，提交给 WorkerPool 重新执行 list_dir 扫描，
// 再通过 post_task 回到所属 I/O 线程向订阅该目录的存活 Connection 推送 Kind::event 帧。
class DirWatcher : public std::enable_shared_from_this<DirWatcher> {
  public:
    using RefreshConnFn = std::function<void(const std::shared_ptr<rpc::Connection> &)>;

    DirWatcher(net::EventLoop &loop, rpc::ConnectionMap &connections, runtime::WorkerPool &workers,
               rpc::ConnectionCounter &counter, RefreshConnFn refresh_conn = {})
        : loop_(loop), connections_(connections), workers_(workers), counter_(counter),
          refresh_conn_(std::move(refresh_conn)) {
    }

    ~DirWatcher() {
        if (inotify_fd_ >= 0)
            ::close(inotify_fd_);
    }

    DirWatcher(const DirWatcher &) = delete;
    DirWatcher &operator=(const DirWatcher &) = delete;

    // 在所属 I/O 线程延迟初始化并挂载 inotify fd 到 EventLoop epoll。
    void ensure_started() {
        if (started_ || stopped_)
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
            started_ = true;
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

        if (started_ && inotify_fd_ >= 0) {
            try {
                loop_.del_event(inotify_fd_);
            } catch (...) {
            }
        }
        if (inotify_fd_ >= 0) {
            ::close(inotify_fd_);
            inotify_fd_ = -1;
        }
        watches_.clear();
        path_to_wd_.clear();
    }

    // 记录某条客户端连接已获取过 rel_path 目录列表，将该目录加入本 I/O 线程的 inotify 监听。
    void watch(const std::string &rel_path, const std::shared_ptr<rpc::Connection> &conn) {
        if (stopped_ || rel_path.empty() || !conn || conn->peer_eof() || conn->draining())
            return;

        ensure_started();
        if (inotify_fd_ < 0)
            return;

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

        constexpr std::uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                       IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
        const int wd = ::inotify_add_watch(inotify_fd_, full_path->c_str(), mask);
        if (wd < 0)
            return;

        auto &entry = watches_[wd];
        if (entry.rel_path.empty())
            entry.rel_path = rel_path;
        entry.subscribers[conn->fd()] = conn;
        path_to_wd_[entry.rel_path] = wd;
    }

    // 取消某条客户端连接对 rel_path 目录的监听（由 workspace_close_dir 触发）。
    // 若移除该连接后该目录已无任何存活订阅者，则立即调用 inotify_rm_watch 释放内核监听槽位。
    void unwatch(const std::string &rel_path, const std::shared_ptr<rpc::Connection> &conn) noexcept {
        if (!conn)
            return;
        unwatch(rel_path, conn->fd());
    }

    void unwatch(const std::string &rel_path, int conn_fd) noexcept {
        if (stopped_ || rel_path.empty() || inotify_fd_ < 0)
            return;

        auto it = path_to_wd_.find(rel_path);
        if (it == path_to_wd_.end())
            return;

        const int wd = it->second;
        auto wit = watches_.find(wd);
        if (wit == watches_.end()) {
            path_to_wd_.erase(it);
            return;
        }

        wit->second.subscribers.erase(conn_fd);
        if (!prune_expired(wit->second))
            remove_watch(wd, true);
    }

  private:
    struct WatchEntry {
        std::string rel_path;
        std::unordered_map<int, std::weak_ptr<rpc::Connection>> subscribers;
        bool refreshing{false};
        bool dirty{false};
    };

    void remove_watch(int wd, bool rm_kernel_watch) noexcept {
        auto it = watches_.find(wd);
        if (it == watches_.end())
            return;

        if (rm_kernel_watch && inotify_fd_ >= 0)
            (void)::inotify_rm_watch(inotify_fd_, wd);

        path_to_wd_.erase(it->second.rel_path);
        watches_.erase(it);
    }

    static bool prune_expired(WatchEntry &entry) {
        for (auto it = entry.subscribers.begin(); it != entry.subscribers.end();) {
            auto conn = it->second.lock();
            if (!conn || conn->peer_eof() || conn->draining())
                it = entry.subscribers.erase(it);
            else
                ++it;
        }
        return !entry.subscribers.empty();
    }

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

                if ((ev->mask & IN_Q_OVERFLOW) != 0) {
                    overflow = true;
                    continue;
                }

                if ((ev->mask & IN_IGNORED) != 0) {
                    remove_watch(ev->wd, false);
                    changed_wds.erase(ev->wd);
                    continue;
                }

                if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                    remove_watch(ev->wd, true);
                    changed_wds.erase(ev->wd);
                    continue;
                }

                // 忽略服务端 write_file 原子替换过程中创建的 .tmp.XXXXXX 临时文件及其配对 rename 事件。
                if (ev->len > 0) {
                    std::string_view name(ev->name);
                    if (name.find(".tmp.") != std::string_view::npos) {
                        if ((ev->mask & IN_MOVED_FROM) != 0 && ev->cookie != 0)
                            tmp_move_cookies.insert(ev->cookie);
                        continue;
                    }
                }
                if ((ev->mask & IN_MOVED_TO) != 0 && ev->cookie != 0 &&
                    tmp_move_cookies.contains(ev->cookie)) {
                    continue;
                }

                if (watches_.contains(ev->wd))
                    changed_wds.insert(ev->wd);
            }
        }

        if (overflow) {
            for (const auto &[wd, _] : watches_)
                changed_wds.insert(wd);
        }

        for (int wd : changed_wds)
            schedule_refresh(wd);
    }

    void schedule_refresh(int wd) {
        auto it = watches_.find(wd);
        if (it == watches_.end())
            return;

        WatchEntry &entry = it->second;
        if (!prune_expired(entry)) {
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
        if (!prune_expired(entry)) {
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

        if (!prune_expired(entry)) {
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

    int inotify_fd_{-1};
    bool started_{false};
    bool stopped_{false};

    std::unordered_map<int, WatchEntry> watches_;
    std::unordered_map<std::string, int> path_to_wd_;
};

} // namespace service

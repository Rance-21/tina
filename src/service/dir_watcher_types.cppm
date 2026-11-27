module;
#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>

export module service.dir_watcher:types;

import std;
import net.epoll;
import net.event_loop;
import runtime.worker_pool;
import rpc.connection;
import ide.protocol;
import ide.server;
import service.file_watcher;
import service.file;
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
class DirWatcher {
  public:
    DirWatcher(net::EventLoop &loop, rpc::IoConnections &connections, runtime::WorkerPool &workers,
               std::size_t io_id, WorkspaceCache *cache = &global_workspace_cache(),
               FileWatcher *file_watcher = &global_file_watcher())
        : loop_(loop), connections_(connections), workers_(workers), io_id_(io_id), cache_(cache),
          file_watcher_(file_watcher) {}

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

        try {
            loop_.add_event(inotify_fd_, net::EpollInterest::read,
                            [this](net::EpollEvent event) { on_event(event); });
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
        by_path_.clear();
        watches_.clear();
    }

    // 记录某条客户端连接已获取过 rel_path 目录列表，将该目录加入本 I/O 线程的 inotify 监听。
    void watch(std::string_view rel_path, const rpc::Connection &conn) {
        if (inotify_fd_ < 0 || rel_path.empty() || !conn.accepting_requests()) {
            return;
        }

        // 两个索引指向同一条监听。哈希表扩容不会移动节点，已订阅连接先比较世代号避免脏写节点。
        if (auto it = by_path_.find(rel_path); it != by_path_.end()) {
            auto &subs = it->second->second.subscribers;
            if (auto sit = subs.find(conn.fd()); sit == subs.end() || sit->second != conn.id())
                subs[conn.fd()] = conn.id();
            return;
        }

        const int root_fd = file::workspace_root_fd();
        const char *c_path = file::c_rel_path(rel_path);
        if (root_fd < 0 || !c_path)
            return;

        file::ScopedFd dir_fd(::openat(root_fd, c_path, O_PATH | O_DIRECTORY | O_CLOEXEC));
        int wd = -1;
        if (dir_fd.valid()) {
            char inode_path[32] = "/proc/self/fd/";
            std::to_chars(inode_path + sizeof("/proc/self/fd/") - 1, std::end(inode_path) - 1,
                          dir_fd.get());
            // 仅监听目录项增删改名及目录自身删除/移动事件；文件内容修改由 FileWatcher 独立负责。
            constexpr std::uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                           IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
            wd = ::inotify_add_watch(inotify_fd_, inode_path, mask);
        }
        // 首次扫描可能早于注册监听。让下一次 list_dir 能看到这段间隙内的变动；
        // 已经订阅的热路径在上面的路径表命中处直接返回。
        if (cache_)
            cache_->invalidate_dir(rel_path);
        if (wd < 0)
            return;

        auto it = watches_.try_emplace(wd).first;
        auto &entry = it->second;
        if (entry.rel_path.empty())
            entry.rel_path.assign(rel_path);
        entry.subscribers[conn.fd()] = conn.id();
        by_path_.emplace(entry.rel_path, &*it);
    }

    // 取消某条连接对 rel_path 目录的监听（由 workspace_close_dir 触发）。
    // 若移除后该目录已无任何存活订阅者，则立即调用 inotify_rm_watch 释放内核槽位并淘汰缓存。
    void unwatch(std::string_view rel_path, int conn_fd) noexcept {
        if (stopped_ || rel_path.empty())
            return;

        auto it = by_path_.find(rel_path);
        if (it == by_path_.end()) {
            evict_dir_and_files(rel_path);
            return;
        }
        auto &[wd, entry] = *it->second;
        entry.subscribers.erase(conn_fd);
        prune_disconnected_subscribers(entry);
        if (entry.subscribers.empty())
            remove_watch(wd, true);
    }

  private:
    struct WatchEntry {
        std::string rel_path;
        // 只记录订阅时的 fd 和世代号，目录监听不会延长 TCP 连接的寿命。
        std::unordered_map<int, std::uint64_t> subscribers;
        bool refreshing{false};
        bool dirty{false};
    };

    // 目录异步重扫任务上下文（单 I/O 线程内池化复用，两层跨线程闭包收敛为 8B 单指针命中 24B SBO）。
    struct RefreshTaskContext {
        DirWatcher &watcher;
        int wd{-1};
        std::string rel_path{};
        std::optional<Frame> event_frame{};
    };

    static constexpr std::size_t kMaxPooledRefreshTasks = 64;

    [[nodiscard]] RefreshTaskContext *acquire_refresh_ctx(int wd, std::string_view rel_path) {
        std::unique_ptr<RefreshTaskContext> ctx;
        if (!refresh_pool_.empty()) {
            ctx = std::move(refresh_pool_.back());
            refresh_pool_.pop_back();
        } else {
            ctx = std::make_unique<RefreshTaskContext>(*this);
        }
        ctx->wd = wd;
        ctx->rel_path.assign(rel_path);
        ctx->event_frame.reset();
        return ctx.release();
    }

    void release_refresh_ctx(RefreshTaskContext *ctx) noexcept {
        std::unique_ptr<RefreshTaskContext> guard(ctx);
        if (!guard || refresh_pool_.size() >= kMaxPooledRefreshTasks)
            return;
        guard->event_frame.reset();
        if (guard->rel_path.capacity() > 1024)
            std::string{}.swap(guard->rel_path);
        else
            guard->rel_path.clear();
        try {
            refresh_pool_.push_back(std::move(guard));
        } catch (...) {
        }
    }

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
        by_path_.erase(it->second.rel_path);
        watches_.erase(it);
    }

    void prune_disconnected_subscribers(WatchEntry &entry) const {
        std::erase_if(entry.subscribers, [this](const auto &subscriber) {
            auto *conn = connections_.find(subscriber.first, subscriber.second);
            return !conn || !conn->accepting_requests();
        });
    }

    void on_event(net::EpollEvent event);

    void schedule_refresh(int wd);

    void complete_refresh(RefreshTaskContext *ctx);

    net::EventLoop &loop_;
    rpc::IoConnections &connections_;
    runtime::WorkerPool &workers_;
    std::size_t io_id_{0};
    WorkspaceCache *cache_;
    FileWatcher *file_watcher_;

    int inotify_fd_{-1};
    bool stopped_{false};

    std::unordered_map<int, WatchEntry> watches_;
    // key 借用 WatchEntry::rel_path，value 借用同一节点。remove_watch 先移除这个索引。
    std::unordered_map<std::string_view, decltype(watches_)::value_type *> by_path_;
    std::vector<std::unique_ptr<RefreshTaskContext>> refresh_pool_;
};

} // namespace service

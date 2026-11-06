module;
#include <errno.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <unistd.h>

export module rpc.handler;

import std;

import net.socket;
import net.epoll;
import net.event_loop;
import runtime.worker_pool;
import rpc.connection;
import ide.protocol;
import ide.server;
import service.kv;

namespace rpc::detail {

// 所有 Worker 共享一个 KV Service。
export struct KvState {
    std::mutex mutex;
    service::KvService service;
};

namespace {

// 根据连接当前状态计算下一次 epoll 需要监听的事件掩码。
// 分支逻辑：
//   1. 基础掩码带 EPOLLONESHOT：每次触发事件后内核自动屏蔽该 fd，直到处理完后显式重装。
//   2. 读分支（!peer_eof() && !draining()）：只要对端未发 FIN
//   且服务端未停机，就继续监听可读与对端半关闭。
//   3. 写分支（wants_write()）：【按需挂载】仅当 output_ 仍有未发完的积压数据时才监听 EPOLLOUT，
//      避免 Socket 长期可写导致 epoll_wait 忙轮询空转。
net::EpollInterest interests(const Connection &conn) {
    auto mask = static_cast<net::EpollInterest>(EPOLLONESHOT);

    if (!conn.peer_eof() && !conn.draining())
        mask = mask | net::EpollInterest::read | net::EpollInterest::peer_closed;

    if (conn.wants_write())
        mask = mask | net::EpollInterest::write;

    return mask;
}

} // namespace

// 在一次读写事件、响应发送或停机冻结后，同步连接的生命周期与 epoll 监听状态（仅限所属 I/O
// 线程调用）。 分支逻辑：
//   1. conn->finished() 为 true：已停止读新请求、无在途 Worker 任务且 output_ 已排空 ->
//   立即关闭并销毁连接。
//   2. 已停止读（peer_eof 或 draining）且当前无待发数据（!wants_write），但仍有在途 Worker
//   任务（pending_ > 0）：
//      此时无需监听任何 Socket 事件，直接返回等待 Worker 算完后通过 post_task 回调
//      complete_response 即可。
//   3. 常规情况：调用 modify_event 重新激活 EPOLLONESHOT 并更新读写监听掩码。
export void refresh_connection(net::EventLoop &loop, ConnectionMap &connections,
                               ConnectionCounter &counter,
                               const std::shared_ptr<Connection> &conn) {
    if (conn->finished()) {
        close_connection(loop, connections, counter, conn->fd());
        return;
    }

    if ((conn->peer_eof() || conn->draining()) && !conn->wants_write())
        return;

    loop.modify_event(conn->fd(), interests(*conn));
}

// ============================================================================
// 每个 I/O 线程独立的目录监听管理器（共享单个 inotify fd，无全局监听表）
// ============================================================================
// 仅监听曾通过 workspace_list_dir 返回给客户端的目录。
// 当 inotify_fd 变为可读时，提交给 WorkerPool 重新执行 list_dir 扫描，
// 再通过 post_task 回到所属 I/O 线程向订阅该目录的存活 Connection 推送 Kind::event 帧。
export class DirWatcher : public std::enable_shared_from_this<DirWatcher> {
  public:
    DirWatcher(net::EventLoop &loop, ConnectionMap &connections, runtime::WorkerPool &workers,
               ConnectionCounter &counter)
        : loop_(loop), connections_(connections), workers_(workers), counter_(counter) {
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
    void watch(const std::string &rel_path, const std::shared_ptr<Connection> &conn) {
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

  private:
    struct WatchEntry {
        std::string rel_path;
        std::unordered_map<int, std::weak_ptr<Connection>> subscribers;
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
                        close_connection(loop_, connections_, counter_, conn->fd());
                        continue;
                    }
                    if (conn->wants_write())
                        conn->write();
                    refresh_connection(loop_, connections_, counter_, conn);
                } catch (...) {
                    close_connection(loop_, connections_, counter_, conn->fd());
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
    ConnectionMap &connections_;
    runtime::WorkerPool &workers_;
    ConnectionCounter &counter_;

    int inotify_fd_{-1};
    bool started_{false};
    bool stopped_{false};

    std::unordered_map<int, WatchEntry> watches_;
    std::unordered_map<std::string, int> path_to_wd_;
};

namespace {

// Worker 线程执行完业务后，通过 loop.post_task 投递回连接所属 I/O 线程执行的响应收尾函数。
// 分支逻辑：
//   1. !weak.lock()：Worker 执行期间连接若已因错误/断开被关闭，则直接丢弃响应。
//   2. 若本次是成功的 workspace_list_dir 请求，将该目录注册到本 I/O 线程的 DirWatcher。
//   3. !conn->enqueue_response(response)：核销 pending_ 并将响应编码进 output_；若超 4 MiB
//   上限则关闭连接。
//   4. conn->wants_write()：先直接调用一次非阻塞 write() 尝试当场发完；
//      若当场发完则 output_ 变空，随后的 refresh_connection() 不会挂载 EPOLLOUT；
//      若遇 EAGAIN 残留数据，则由 refresh_connection() 挂载 EPOLLOUT 等待内核缓冲区腾空。
void complete_response(const std::weak_ptr<Connection> &weak, net::EventLoop &loop,
                       ConnectionMap &connections, ConnectionCounter &counter,
                       const std::shared_ptr<DirWatcher> &watcher, const Frame &response,
                       const std::string &watched_dir) {
    auto conn = weak.lock();
    if (!conn)
        return;

    try {
        if (!watched_dir.empty() && response.status == Status::ok && watcher)
            watcher->watch(watched_dir, conn);

        if (!conn->enqueue_response(response)) {
            close_connection(loop, connections, counter, conn->fd());
            return;
        }
        if (conn->wants_write())
            conn->write();
        refresh_connection(loop, connections, counter, conn);
    } catch (...) {
        close_connection(loop, connections, counter, conn->fd());
    }
}

// Method 编号采用 16 位分组编码（如 0x0001、0x0011）：
// 用掩码 0xFFF0 提取高 12 位即可判断该请求所属的业务模块分组，低 4 位表示模块内具体方法。
constexpr std::uint16_t method_group_mask = 0xfff0;
constexpr std::uint16_t system_kv_group = 0x0000; // 0x0001 (ping) 及 0x0002~0x0004 (KV)
constexpr std::uint16_t workspace_group = 0x0010; // 0x0011~0x0015 (Workspace 工作区操作)

// 根据 Method 的掩码分组将请求路由到对应的业务模块。
// 分支逻辑：
//   1. 0x0000 分组（系统与 KV）：system_ping 直接无锁返回 PONG；其余交给加锁的 KV 服务处理。
//   2. 0x0010 分组（Workspace）：交由 ide.server 中的 server::handle_workspace_request() 解码并处理。
//   3. 其他未知分组：统一返回 Status::unsupported。
void dispatch_request(Frame &request, KvState &kv, std::string &watched_dir) {
    const auto group =
        static_cast<std::uint16_t>(std::to_underlying(request.method) & method_group_mask);

    switch (group) {
    case system_kv_group:
        if (request.method == Method::system_ping) {
            handle_ping(request);
        } else {
            std::lock_guard lock(kv.mutex);
            kv.service.handle(request);
        }
        return;

    case workspace_group:
        server::handle_workspace_request(request, &watched_dir);
        return;

    default:
        request.status = Status::unsupported;
        request.payload = "unsupported method";
        return;
    }
}

// Worker 线程实际执行的单次请求处理函数。
// 分支逻辑：
//   1. 复用原 Frame 作为响应帧（保留原 method 和 64 位 request_id），将 kind 改为 response。
//   2. 调用 dispatch_request() 按掩码分发业务；捕获所有异常并转化为错误状态码，绝不让异常逃出
//   Worker 线程。
//   3. 业务执行完毕后，通过 loop.post_task 将响应帧投递回所属 I/O 线程执行 complete_response()。
void handle_request(Frame request, const std::weak_ptr<Connection> &weak, net::EventLoop &loop,
                    ConnectionMap &connections, ConnectionCounter &counter,
                    const std::shared_ptr<KvState> &kv,
                    const std::shared_ptr<DirWatcher> &watcher) {
    request.kind = Kind::response;
    std::string watched_dir;
    try {
        dispatch_request(request, *kv, watched_dir);
    } catch (const std::bad_alloc &) {
        request.status = Status::resource_exhausted;
        request.payload = "out of memory";
    } catch (...) {
        request.status = Status::internal;
        request.payload = "internal server error";
    }

    // 目前还没有编码 response
    (void)loop.post_task([weak, &loop, &connections, &counter, watcher,
                          response = std::move(request),
                          watched_dir = std::move(watched_dir)] {
        complete_response(weak, loop, connections, counter, watcher, response, watched_dir);
    });
}

// 将拆包得到的完整请求帧封装为任务提交给 WorkerPool 异步执行。
bool submit_request(Frame request, const std::weak_ptr<Connection> &weak, net::EventLoop &loop,
                    ConnectionMap &connections, runtime::WorkerPool &workers,
                    ConnectionCounter &counter, const std::shared_ptr<KvState> &kv,
                    const std::shared_ptr<DirWatcher> &watcher) {
    return workers.try_submit([request = std::move(request), weak, &loop, &connections, &counter,
                               kv, watcher]() mutable {
        handle_request(std::move(request), weak, loop, connections, counter, kv, watcher);
    });
}

// 客户端连接在所属 I/O 线程上的 epoll 事件主回调。
// 分支逻辑：
//   1. event.error()：Socket 发生底层 EPOLLERR，立即关闭连接。
//   2. 可读/对端关闭分支：连接未处于半关闭或停机排空时，调用 conn->read() 读数据、拆帧并提交给
//   Worker；
//      若帧格式非法、在途请求超限、Worker 队列满或 EOF 时残留半包（!ok），立即关闭连接。
//   3. 可写分支（conn->wants_write()）：将 output_ 中积压的响应数据非阻塞写入内核缓冲区。
//   4. 末尾调用 refresh_connection()：根据最新状态决定关闭连接、静默等待 Worker 还是重装 epoll
//   事件。
void handle_connection_event(const std::shared_ptr<Connection> &conn,
                             const std::weak_ptr<Connection> &weak, net::EventLoop &loop,
                             ConnectionMap &connections, runtime::WorkerPool &workers,
                             ConnectionCounter &counter, const std::shared_ptr<KvState> &kv,
                             const std::shared_ptr<DirWatcher> &watcher, net::EpollEvent event) {
    if (event.error()) {
        close_connection(loop, connections, counter, conn->fd());
        return;
    }

    try {
        if (!conn->peer_eof() && !conn->draining() &&
            (event.readable() || event.peer_closed() || event.hangup())) {
            bool ok = conn->read([&](Frame request) {
                return submit_request(std::move(request), weak, loop, connections, workers, counter,
                                      kv, watcher);
            });

            if (!ok) {
                close_connection(loop, connections, counter, conn->fd());
                return;
            }
        }

        if (conn->wants_write())
            conn->write();
        refresh_connection(loop, connections, counter, conn);
    } catch (...) {
        close_connection(loop, connections, counter, conn->fd());
    }
}

// 在所属 I/O 线程中初始化 Connection 对象、放入本线程连接表并首次注册到 epoll。
void register_connection(net::Socket socket, net::EventLoop &loop, ConnectionMap &connections,
                         runtime::WorkerPool &workers, ConnectionCounter &counter,
                         const std::shared_ptr<KvState> &kv,
                         const std::shared_ptr<DirWatcher> &watcher) {
    if (watcher)
        watcher->ensure_started();

    auto conn = std::make_shared<Connection>(std::move(socket));
    int fd = conn->fd();
    std::weak_ptr<Connection> weak = conn;

    connections.emplace(fd, conn);
    try {
        loop.add_event(fd, interests(*conn),
                       [conn, weak, &loop, &connections, &workers, &counter, kv,
                        watcher](net::EpollEvent event) {
                           handle_connection_event(conn, weak, loop, connections, workers, counter,
                                                   kv, watcher, event);
                       });
    } catch (...) {
        connections.erase(fd);
        throw;
    }
    counter.opened();
}

} // namespace

// 由主线程（Acceptor）调用：将新 accept 的客户端 Socket 跨线程投递给选定的 I/O 线程完成注册。
export bool post_connection(net::Socket socket, net::EventLoop &loop, ConnectionMap &connections,
                            runtime::WorkerPool &workers, ConnectionCounter &counter,
                            std::shared_ptr<KvState> kv,
                            std::shared_ptr<DirWatcher> watcher = nullptr) {
    return loop.post_task([socket = std::move(socket), &loop, &connections, &workers, &counter,
                           kv = std::move(kv), watcher = std::move(watcher)]() mutable {
        register_connection(std::move(socket), loop, connections, workers, counter, kv, watcher);
    });
}

} // namespace rpc::detail

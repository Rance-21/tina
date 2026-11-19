module;
#include <sys/epoll.h>
#include <unistd.h>

export module rpc.handler;

import std;

import net.socket;
import net.epoll;
import net.event_loop;
import runtime.worker_pool;
import rpc.connection;
import rpc.task_classifier;
import ide.protocol;
import ide.server;
import ide.server_git;
import ide.server_terminal;
import service.dir_watcher;
import service.file_watcher;
import service.kv;

namespace rpc::detail {

// 所有 Worker 共享一个 KV Service。
export struct KvState {
    std::mutex mutex;
    service::KvService service;
};

// 绑定到单个 EventLoop（单一 I/O 线程）的分流运行时上下文。
export struct IoDispatchView {
    std::size_t io_id{0};
    std::size_t inline_cache_threshold{32 * 1024};
};

namespace {

// 【作用】根据连接当前读写状态，拼出下一次 epoll_ctl 需要监听的事件掩码。
// 【原理】始终带上 EPOLLONESHOT 防止多线程误触发；未读到 EOF 且未停机时监听可读/对端关闭；
//         仅当 output_ 发送队列里还有积压字节时才按需挂上 EPOLLOUT，避免空转唤醒。
net::EpollInterest interests(const Connection &conn) {
    auto mask = static_cast<net::EpollInterest>(EPOLLONESHOT);

    if (!conn.peer_eof() && !conn.draining())
        mask = mask | net::EpollInterest::read | net::EpollInterest::peer_closed;

    if (conn.wants_write())
        mask = mask | net::EpollInterest::write;

    return mask;
}

} // namespace

// 【作用】在每次读写、响应回传或停机冻结后，统一更新连接的关闭状态与 epoll 监听掩码（仅所属 I/O 线程调用）。
// 【原理】
//   1. 已满足关闭条件（conn->finished()）：直接摘除 epoll 并销毁连接；
//   2. 已停读且当前无待发数据（还有在途 Worker 任务）：静默等待 Worker 回调，无需挂任何 socket 事件；
//   3. 其余常规状态：调用 modify_event 重新武装 EPOLLONESHOT 与最新读写掩码。
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

// 【作用】为第 io_id 号 I/O 线程创建专属目录变更监听器（DirWatcher）。
// 【原理】把所属 EventLoop、连接表、WorkerPool、io_id 与 refresh_connection 回调注入 DirWatcher；
//         后续该线程收到 inotify 目录变更时，将用自己的 io_id 向 Blocking Worker 投递目录重扫任务。
export inline std::shared_ptr<service::DirWatcher>
make_dir_watcher(net::EventLoop &loop, ConnectionMap &connections, runtime::WorkerPool &workers,
                 ConnectionCounter &counter, std::size_t io_id = 0) {
    return std::make_shared<service::DirWatcher>(
        loop, connections, workers, counter, io_id,
        [&loop, &connections, &counter](const std::shared_ptr<Connection> &conn) {
            refresh_connection(loop, connections, counter, conn);
        });
}

// 【作用】在所属 I/O 线程启动进程级全局文件监听器（FileWatcher）。
// 【原理】将全局 FileWatcher 的 inotify_fd 挂进该 EventLoop 的 epoll，用于监听缓存文件的外部改动并触发失效。
export void start_file_watcher(net::EventLoop &loop) {
    service::global_file_watcher().start(loop);
}

// 【作用】在停机阶段停止全局文件监听器并释放内核 inotify 描述符。
// 【原理】由挂载 FileWatcher 的那个 I/O 线程调用，先摘除 epoll 再 close(inotify_fd)。
export void stop_file_watcher() noexcept {
    service::global_file_watcher().stop();
}

namespace {

// 【作用】Worker 算完响应后，通过 loop.post_task 切回连接所属 I/O 线程执行的响应发送收尾函数。
// 【原理】所有对 Socket、Connection::output_ 和 epoll 的操作都必须收敛到所属 I/O 线程串行执行：
//   1. 先用 weak_ptr 检查连接是否仍存活（执行期间客户端可能已经断开）；
//   2. 若是写文件响应，撤销同连接上的在途写登记；若是成功的 list_dir / close_dir，同步更新本线程 DirWatcher；
//   3. 将响应帧入队 output_ 并立刻尝试一次非阻塞 writev()；
//   4. 仅当连接需要关闭或 wants_write() 状态发生变化时才调用 refresh_connection()，省去冗余 epoll_ctl。
void complete_response(const std::weak_ptr<Connection> &weak, net::EventLoop &loop,
                       ConnectionMap &connections, ConnectionCounter &counter,
                       const std::shared_ptr<service::DirWatcher> &watcher, Frame &response,
                       const std::string &watched_dir) {
    auto conn = weak.lock();
    if (!conn)
        return;

    try {
        if (response.method == Method::workspace_write_file)
            conn->finish_inflight_write(response.request_id);

        if (!watched_dir.empty() && response.status == Status::ok && watcher) {
            if (response.method == Method::workspace_list_dir)
                watcher->watch(watched_dir, conn);
            else if (response.method == Method::workspace_close_dir)
                watcher->unwatch(watched_dir, conn->fd());
        }

        const bool had_write = conn->wants_write();

        if (!conn->enqueue_response(response)) {
            close_connection(loop, connections, counter, conn->fd());
            return;
        }
        if (conn->wants_write())
            conn->write();
        if (conn->finished() || conn->wants_write() != had_write)
            refresh_connection(loop, connections, counter, conn);
    } catch (...) {
        close_connection(loop, connections, counter, conn->fd());
    }
}

// 【作用】在 Worker 线程内按 Method 的高 8 位分组码把请求路由到对应业务模块。
// 【原理】先拦截只允许服务端推送的 event_only 方法，再用 method_group() 位与提取模块号做紧凑 switch 跳转。
void dispatch_request(Frame &request, KvState &kv, std::string &watched_dir,
                      server::TerminalManager *terminal_mgr, int conn_fd) {
    if (is_event_only_method(request.method)) {
        request.status = Status::unsupported;
        request.payload = "unsupported method";
        return;
    }

    switch (method_group(request.method)) {
    case MethodGroup::system:
        if (request.method == Method::system_ping) {
            handle_ping(request);
            return;
        }
        break;

    case MethodGroup::kv: {
        std::lock_guard lock(kv.mutex);
        kv.service.handle(request);
        return;
    }

    case MethodGroup::workspace:
        server::handle_workspace_request(request, &watched_dir);
        return;

    case MethodGroup::terminal:
        if (terminal_mgr != nullptr) {
            terminal_mgr->handle_request(request, conn_fd);
            return;
        }
        break;

    case MethodGroup::git:
        server::handle_git_request(request);
        return;
    }

    request.status = Status::unsupported;
    request.payload = "unsupported method";
}

// 【作用】Worker 线程真正执行的单条 RPC 业务处理外壳函数。
// 【原理】
//   1. 原地把 request.kind 改成 response（保留原 request_id 和 method）；
//   2. 若提交阶段已提前抓好了只读缓存快照（> 32 KiB 文件或大目录），直接调用 finish_cached_* 组装响应，
//      绝不二次查缓存锁也不碰磁盘；否则走 dispatch_request() 执行常规/阻塞业务；
//   3. 捕获一切异常转成规范 RPC 错误码，最后通过 loop.post_task 把响应帧交回原 I/O 线程发送。
void handle_request(Frame request, PreparedDispatch prepared, const std::weak_ptr<Connection> &weak,
                    int conn_fd, net::EventLoop &loop, ConnectionMap &connections,
                    ConnectionCounter &counter, const std::shared_ptr<KvState> &kv,
                    const std::shared_ptr<service::DirWatcher> &watcher,
                    const std::shared_ptr<server::TerminalManager> &terminal_mgr) {
    request.kind = Kind::response;
    std::string watched_dir;
    try {
        if (prepared.cached_file) {
            server::finish_cached_read_file(request, std::move(prepared.cached_file),
                                             prepared.file_slice_offset, prepared.file_slice_size);
        } else if (prepared.cached_dir) {
            watched_dir = std::move(prepared.cached_dir_path);
            server::finish_cached_list_dir(request, watched_dir, *prepared.cached_dir,
                                            &watched_dir);
        } else {
            dispatch_request(request, *kv, watched_dir, terminal_mgr.get(), conn_fd);
        }
    } catch (const std::bad_alloc &) {
        request.clear_external_payload();
        request.status = Status::resource_exhausted;
        request.payload = "out of memory";
    } catch (...) {
        request.clear_external_payload();
        request.status = Status::internal;
        request.payload = "internal server error";
    }

    (void)loop.post_task([weak, &loop, &connections, &counter, watcher,
                          response = std::move(request),
                          watched_dir = std::move(watched_dir)]() mutable {
        complete_response(weak, loop, connections, counter, watcher, response, watched_dir);
    });
}

// 【作用】在 I/O 线程将未走内联路径的请求封装成闭包，按分类提交到 WorkerPool V2。
// 【原理】若请求是 workspace_write_file，先偷看其目标路径；打包闭包后调用 workers.try_submit(io_id, task_class, ...)
//         进入本 I/O 线程专属无锁 lane；提交成功后立刻在 conn 上登记该在途写路径，防止后续内联读超车。
bool submit_request(Frame request, PreparedDispatch &prepared, Connection &conn,
                    const std::weak_ptr<Connection> &weak, int conn_fd, net::EventLoop &loop,
                    ConnectionMap &connections, runtime::WorkerPool &workers,
                    ConnectionCounter &counter, const std::shared_ptr<KvState> &kv,
                    const std::shared_ptr<service::DirWatcher> &watcher,
                    const std::shared_ptr<server::TerminalManager> &terminal_mgr,
                    std::size_t io_id) {
    const std::uint64_t req_id = request.request_id;
    std::optional<std::string_view> write_path;
    if (request.method == Method::workspace_write_file)
        write_path = peek_write_file_path(request.payload);

    const runtime::TaskClass task_class = prepared.task_class;
    runtime::PoolTask task = [request = std::move(request), prepared = std::move(prepared), weak,
                              conn_fd, &loop, &connections, &counter, kv, watcher,
                              terminal_mgr]() mutable {
        handle_request(std::move(request), std::move(prepared), weak, conn_fd, loop, connections,
                       counter, kv, watcher, terminal_mgr);
    };

    if (!workers.try_submit(io_id, task_class, std::move(task)))
        return false;

    if (write_path)
        conn.track_inflight_write(req_id, *write_path);
    return true;
}

// 【作用】单条客户端 TCP 连接在所属 I/O 线程上的 epoll 事件总回调。
// 【原理】
//   1. 发生 EPOLLERR 直接清理终端并关闭连接；
//   2. 可读时调用 conn->read() 逐帧拆包：
//      - 回调 1（try_inline_or_classify）：判断能否就地内联响应，否则给出 Normal/Blocking 分类与缓存快照；
//      - 回调 2（on_inline_enqueued）：若目录缓存内联命中并成功入队，顺手在本线程挂上 DirWatcher 监听；
//      - 回调 3（submit_request）：把非内联请求投递进 WorkerPool 对应 io_id 的队列；
//   3. 本轮若有待发响应（包括内联直出响应），立即调 conn->write() 用 writev() 一次聚集发出；
//   4. 末尾调 refresh_connection() 决定关闭连接还是重装 epoll 事件。
void handle_connection_event(const std::shared_ptr<Connection> &conn,
                             const std::weak_ptr<Connection> &weak, net::EventLoop &loop,
                             ConnectionMap &connections, runtime::WorkerPool &workers,
                             ConnectionCounter &counter, const std::shared_ptr<KvState> &kv,
                             const std::shared_ptr<service::DirWatcher> &watcher,
                             const std::shared_ptr<server::TerminalManager> &terminal_mgr,
                             IoDispatchView io_view, net::EpollEvent event) {
    const int conn_fd = conn->fd();
    auto close_conn = [&] {
        if (terminal_mgr)
            terminal_mgr->close_for_connection(conn_fd);
        close_connection(loop, connections, counter, conn_fd);
    };

    if (event.error()) {
        close_conn();
        return;
    }

    try {
        if (!conn->peer_eof() && !conn->draining() &&
            (event.readable() || event.peer_closed() || event.hangup())) {
            PreparedDispatch prepared;
            std::string inline_watched_dir;

            bool ok = false;
            {
                // 批量提交/合并唤醒守卫：本轮 conn->read() 可能连续拆包提交几十个请求，
                // 期间仅在线程私有位图标记目标 Worker，退出作用域时统一做 1 次栅栏并仅唤醒沉睡 Worker。
                runtime::WorkerPool::SubmitBatchGuard wake_batch(workers, io_view.io_id);
                ok = conn->read(
                    [&](Frame &request) {
                        return try_inline_or_classify(request, *conn,
                                                      io_view.inline_cache_threshold, prepared,
                                                      inline_watched_dir);
                    },
                    [&](const Frame &response) {
                        if (!inline_watched_dir.empty() && response.status == Status::ok &&
                            watcher) {
                            watcher->watch(inline_watched_dir, conn);
                        }
                        inline_watched_dir.clear();
                    },
                    [&](Frame request) {
                        return submit_request(std::move(request), prepared, *conn, weak, conn_fd,
                                              loop, connections, workers, counter, kv, watcher,
                                              terminal_mgr, io_view.io_id);
                    });
            }

            if (!ok) {
                close_conn();
                return;
            }
        }

        if (conn->wants_write())
            conn->write();
        if (conn->finished() && terminal_mgr)
            terminal_mgr->close_for_connection(conn_fd);
        refresh_connection(loop, connections, counter, conn);
    } catch (...) {
        close_conn();
    }
}

// 【作用】在目标 I/O 线程内正式接管一条刚 accept 的客户端 Socket。
// 【原理】构造 Connection 放入本线程独占的连接表 connections，再向本线程 epoll 注册可读监听回调，
//         最后给全局活跃连接计数器加 1；若 epoll 注册失败则回滚 map 条目防泄漏。
void register_connection(net::Socket socket, net::EventLoop &loop, ConnectionMap &connections,
                         runtime::WorkerPool &workers, ConnectionCounter &counter,
                         const std::shared_ptr<KvState> &kv,
                         const std::shared_ptr<service::DirWatcher> &watcher,
                         const std::shared_ptr<server::TerminalManager> &terminal_mgr,
                         IoDispatchView io_view) {
    auto conn = std::make_shared<Connection>(std::move(socket));
    int fd = conn->fd();
    std::weak_ptr<Connection> weak = conn;

    connections.emplace(fd, conn);
    try {
        loop.add_event(fd, interests(*conn),
                       [conn, weak, &loop, &connections, &workers, &counter, kv, watcher,
                        terminal_mgr, io_view](net::EpollEvent event) {
                           handle_connection_event(conn, weak, loop, connections, workers, counter,
                                                   kv, watcher, terminal_mgr, io_view, event);
                       });
    } catch (...) {
        connections.erase(fd);
        throw;
    }
    counter.opened();
}

} // namespace

// 【作用】为单个 I/O 线程创建专属 PTY 终端管理器（TerminalManager）。
// 【原理】注入事件推送闭包：当本线程上的 PTY 有输出或命令完成时，查本线程连接表找到对应 Connection，
//         把 Event 帧入队 output_ 并立刻尝试 writev() 发送。
export std::shared_ptr<server::TerminalManager> make_terminal_manager(net::EventLoop &loop,
                                                                      ConnectionMap &connections,
                                                                      ConnectionCounter &counter) {
    return std::make_shared<server::TerminalManager>(
        loop, [&loop, &connections, &counter](int conn_fd, Frame &&event) {
            auto it = connections.find(conn_fd);
            if (it == connections.end())
                return;

            auto conn = it->second;
            if (!conn || conn->peer_eof() || conn->draining())
                return;

            try {
                if (!conn->enqueue_event(std::move(event))) {
                    close_connection(loop, connections, counter, conn->fd());
                    return;
                }
                if (conn->wants_write())
                    conn->write();
                refresh_connection(loop, connections, counter, conn);
            } catch (...) {
                close_connection(loop, connections, counter, conn->fd());
            }
        });
}

// 【作用】由主线程（Acceptor）调用：把新接收的客户端 Socket 安全移交给选定的 I/O 线程。
// 【原理】不跨线程直接改对方 EventLoop 的 epoll/map，而是通过 loop.post_task 投递一个异步任务，
//         由目标 I/O 线程醒来后在自己线程内执行 register_connection()。
export bool post_connection(net::Socket socket, net::EventLoop &loop, ConnectionMap &connections,
                            runtime::WorkerPool &workers, ConnectionCounter &counter,
                            std::shared_ptr<KvState> kv,
                            std::shared_ptr<service::DirWatcher> watcher = nullptr,
                            std::shared_ptr<server::TerminalManager> terminal_mgr = nullptr,
                            IoDispatchView io_view = {}) {
    return loop.post_task([socket = std::move(socket), &loop, &connections, &workers, &counter,
                           kv = std::move(kv), watcher = std::move(watcher),
                           terminal_mgr = std::move(terminal_mgr), io_view]() mutable {
        register_connection(std::move(socket), loop, connections, workers, counter, kv, watcher,
                            terminal_mgr, io_view);
    });
}

} // namespace rpc::detail

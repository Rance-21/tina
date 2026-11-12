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
import ide.protocol;
import ide.server;
import ide.server_git;
import ide.server_terminal;
import service.dir_watcher;
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
//   2. 读分支（!conn.peer_eof() && !conn.draining()）：只要对端未发 FIN
//   且服务端未停机，就继续监听可读与对端半关闭。
//   3. 写分支（conn.wants_write()）：【按需挂载】仅当 output_ 仍有未发完的积压数据时才监听
//   EPOLLOUT，
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

export using DirWatcher = service::DirWatcher;

// 创建绑定在本 I/O 线程上的 DirWatcher，并注入 refresh_connection 回调。
export std::shared_ptr<DirWatcher> make_dir_watcher(net::EventLoop &loop,
                                                    ConnectionMap &connections,
                                                    runtime::WorkerPool &workers,
                                                    ConnectionCounter &counter) {
    return std::make_shared<DirWatcher>(
        loop, connections, workers, counter,
        [&loop, &connections, &counter](const std::shared_ptr<Connection> &conn) {
            refresh_connection(loop, connections, counter, conn);
        });
}

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

// Method 编号采用 16 位位域编码（[15:8] MethodGroup | [7] event_only | [6:0] op）：
//   1. 先通过 is_event_only_method() 单指令位测试拦截仅限服务端推送的 Event 方法（Bit 7 == 1）。
//   2. 再通过 method_group() 单指令位与（& 0xFF00）提取高 8
//   位业务分组，直接由紧凑跳转表路由到对应模块。
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

    case MethodGroup::agent:
        break;
    }

    request.status = Status::unsupported;
    request.payload = "unsupported method";
}

// Worker 线程实际执行的单次请求处理函数。
// 分支逻辑：
//   1. 复用原 Frame 作为响应帧（保留原 method 和 64 位 request_id），将 kind 改为 response。
//   2. 调用 dispatch_request() 按掩码分发业务；捕获所有异常并转化为错误状态码，绝不让异常逃出
//   Worker 线程。
//   3. 业务执行完毕后，通过 loop.post_task 将响应帧投递回所属 I/O 线程执行 complete_response()。
void handle_request(Frame request, const std::weak_ptr<Connection> &weak, int conn_fd,
                    net::EventLoop &loop, ConnectionMap &connections, ConnectionCounter &counter,
                    const std::shared_ptr<KvState> &kv, const std::shared_ptr<DirWatcher> &watcher,
                    const std::shared_ptr<server::TerminalManager> &terminal_mgr) {
    request.kind = Kind::response;
    std::string watched_dir;
    try {
        dispatch_request(request, *kv, watched_dir, terminal_mgr.get(), conn_fd);
    } catch (const std::bad_alloc &) {
        request.status = Status::resource_exhausted;
        request.payload = "out of memory";
    } catch (...) {
        request.status = Status::internal;
        request.payload = "internal server error";
    }

    // 目前还没有编码 response
    (void)loop.post_task([weak, &loop, &connections, &counter, watcher,
                          response = std::move(request), watched_dir = std::move(watched_dir)] {
        complete_response(weak, loop, connections, counter, watcher, response, watched_dir);
    });
}

// 将拆包得到的完整请求帧封装为任务提交给 WorkerPool 异步执行。
bool submit_request(Frame request, const std::weak_ptr<Connection> &weak, int conn_fd,
                    net::EventLoop &loop, ConnectionMap &connections, runtime::WorkerPool &workers,
                    ConnectionCounter &counter, const std::shared_ptr<KvState> &kv,
                    const std::shared_ptr<DirWatcher> &watcher,
                    const std::shared_ptr<server::TerminalManager> &terminal_mgr) {
    return workers.try_submit([request = std::move(request), weak, conn_fd, &loop, &connections,
                               &counter, kv, watcher, terminal_mgr]() mutable {
        handle_request(std::move(request), weak, conn_fd, loop, connections, counter, kv, watcher,
                       terminal_mgr);
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
                             const std::shared_ptr<DirWatcher> &watcher,
                             const std::shared_ptr<server::TerminalManager> &terminal_mgr,
                             net::EpollEvent event) {
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
            bool ok = conn->read([&](Frame request) {
                return submit_request(std::move(request), weak, conn_fd, loop, connections, workers,
                                      counter, kv, watcher, terminal_mgr);
            });

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

// 在所属 I/O 线程中初始化 Connection 对象、放入本线程连接表并首次注册到 epoll。
void register_connection(net::Socket socket, net::EventLoop &loop, ConnectionMap &connections,
                         runtime::WorkerPool &workers, ConnectionCounter &counter,
                         const std::shared_ptr<KvState> &kv,
                         const std::shared_ptr<DirWatcher> &watcher,
                         const std::shared_ptr<server::TerminalManager> &terminal_mgr) {
    if (watcher)
        watcher->ensure_started();

    auto conn = std::make_shared<Connection>(std::move(socket));
    int fd = conn->fd();
    std::weak_ptr<Connection> weak = conn;

    connections.emplace(fd, conn);
    try {
        loop.add_event(fd, interests(*conn),
                       [conn, weak, &loop, &connections, &workers, &counter, kv, watcher,
                        terminal_mgr](net::EpollEvent event) {
                           handle_connection_event(conn, weak, loop, connections, workers, counter,
                                                   kv, watcher, terminal_mgr, event);
                       });
    } catch (...) {
        connections.erase(fd);
        throw;
    }
    counter.opened();
}

} // namespace

// 创建绑定在本 I/O 线程上的 TerminalManager，并注入向客户端 Connection 推送 Event 帧的回调。
export std::shared_ptr<server::TerminalManager> make_terminal_manager(net::EventLoop &loop,
                                                                      ConnectionMap &connections,
                                                                      ConnectionCounter &counter) {
    return std::make_shared<server::TerminalManager>(
        loop, [&loop, &connections, &counter](int conn_fd, const Frame &event) {
            auto it = connections.find(conn_fd);
            if (it == connections.end())
                return;

            auto conn = it->second;
            if (!conn || conn->peer_eof() || conn->draining())
                return;

            try {
                if (!conn->enqueue_event(event)) {
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

// 由主线程（Acceptor）调用：将新 accept 的客户端 Socket 跨线程投递给选定的 I/O 线程完成注册。
export bool post_connection(net::Socket socket, net::EventLoop &loop, ConnectionMap &connections,
                            runtime::WorkerPool &workers, ConnectionCounter &counter,
                            std::shared_ptr<KvState> kv,
                            std::shared_ptr<DirWatcher> watcher = nullptr,
                            std::shared_ptr<server::TerminalManager> terminal_mgr = nullptr) {
    return loop.post_task([socket = std::move(socket), &loop, &connections, &workers, &counter,
                           kv = std::move(kv), watcher = std::move(watcher),
                           terminal_mgr = std::move(terminal_mgr)]() mutable {
        register_connection(std::move(socket), loop, connections, workers, counter, kv, watcher,
                            terminal_mgr);
    });
}

} // namespace rpc::detail

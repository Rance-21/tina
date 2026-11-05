module;
#include <sys/epoll.h>

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

namespace {

// Worker 线程执行完业务后，通过 loop.post_task 投递回连接所属 I/O 线程执行的响应收尾函数。
// 分支逻辑：
//   1. !weak.lock()：Worker 执行期间连接若已因错误/断开被关闭，则直接丢弃响应。
//   2. !conn->enqueue_response(response)：核销 pending_ 并将响应编码进 output_；若超 4 MiB
//   上限则关闭连接。
//   3. conn->wants_write()：先直接调用一次非阻塞 write() 尝试当场发完；
//      若当场发完则 output_ 变空，随后的 refresh_connection() 不会挂载 EPOLLOUT；
//      若遇 EAGAIN 残留数据，则由 refresh_connection() 挂载 EPOLLOUT 等待内核缓冲区腾空。
void complete_response(const std::weak_ptr<Connection> &weak, net::EventLoop &loop,
                       ConnectionMap &connections, ConnectionCounter &counter,
                       const Frame &response) {
    auto conn = weak.lock();
    if (!conn)
        return;

    try {
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
//   2. 0x0010 分组（Workspace）：交由 ide.server 中的 handle_workspace_request() 解码并处理。
//   3. 其他未知分组：统一返回 Status::unsupported。
void dispatch_request(Frame &request, KvState &kv) {
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
        handle_workspace_request(request);
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
                    const std::shared_ptr<KvState> &kv) {
    request.kind = Kind::response;
    try {
        dispatch_request(request, *kv);
    } catch (const std::bad_alloc &) {
        request.status = Status::resource_exhausted;
        request.payload = "out of memory";
    } catch (...) {
        request.status = Status::internal;
        request.payload = "internal server error";
    }

    // 目前还没有编码 response
    (void)loop.post_task([weak, &loop, &connections, &counter, response = std::move(request)] {
        complete_response(weak, loop, connections, counter, response);
    });
}

// 将拆包得到的完整请求帧封装为任务提交给 WorkerPool 异步执行。
bool submit_request(Frame request, const std::weak_ptr<Connection> &weak, net::EventLoop &loop,
                    ConnectionMap &connections, runtime::WorkerPool &workers,
                    ConnectionCounter &counter, const std::shared_ptr<KvState> &kv) {
    return workers.try_submit(
        [request = std::move(request), weak, &loop, &connections, &counter, kv]() mutable {
            handle_request(std::move(request), weak, loop, connections, counter, kv);
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
                             net::EpollEvent event) {
    if (event.error()) {
        close_connection(loop, connections, counter, conn->fd());
        return;
    }

    try {
        if (!conn->peer_eof() && !conn->draining() &&
            (event.readable() || event.peer_closed() || event.hangup())) {
            bool ok = conn->read([&](Frame request) {
                return submit_request(std::move(request), weak, loop, connections, workers, counter,
                                      kv);
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
                         const std::shared_ptr<KvState> &kv) {
    auto conn = std::make_shared<Connection>(std::move(socket));
    int fd = conn->fd();
    std::weak_ptr<Connection> weak = conn;

    connections.emplace(fd, conn);
    try {
        loop.add_event(
            fd, interests(*conn),
            [conn, weak, &loop, &connections, &workers, &counter, kv](net::EpollEvent event) {
                handle_connection_event(conn, weak, loop, connections, workers, counter, kv, event);
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
                            std::shared_ptr<KvState> kv) {
    return loop.post_task([socket = std::move(socket), &loop, &connections, &workers, &counter,
                           kv = std::move(kv)]() mutable {
        register_connection(std::move(socket), loop, connections, workers, counter, kv);
    });
}

} // namespace rpc::detail

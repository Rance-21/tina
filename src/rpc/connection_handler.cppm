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
//   2. 读分支（!peer_eof() && !draining()）：只要对端未发 FIN 且服务端未停机，就继续监听可读与对端半关闭。
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

// 在一次读写事件、响应发送或停机冻结后，同步连接的生命周期与 epoll 监听状态（仅限所属 I/O 线程调用）。
// 分支逻辑：
//   1. conn->finished() 为 true：已停止读新请求、无在途 Worker 任务且 output_ 已排空 -> 立即关闭并销毁连接。
//   2. 已停止读（peer_eof 或 draining）且当前无待发数据（!wants_write），但仍有在途 Worker 任务（pending_ > 0）：
//      此时无需监听任何 Socket 事件，直接返回等待 Worker 算完后通过 post_task 回调 complete_response 即可。
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
//   2. !conn->enqueue_response(response)：核销 pending_ 并将响应编码进 output_；若超 4 MiB 上限则关闭连接。
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

// 将拆包得到的完整请求帧封装为任务提交给 WorkerPool 执行。
// 分支逻辑：
//   1. Worker 内按 method 分发：system_ping 直接无锁响应；KV 操作加锁调用 kv->service.handle()。
//   2. 异常捕获分支：将内存不足或未知异常转化为对应的 RPC 错误状态码，绝不允许异常逃出 Worker 线程。
//   3. 执行完毕后通过 loop.post_task 将响应帧投递回原 I/O 线程执行 complete_response()。
bool submit_request(Frame request, const std::weak_ptr<Connection> &weak, net::EventLoop &loop,
                    ConnectionMap &connections, runtime::WorkerPool &workers,
                    ConnectionCounter &counter, const std::shared_ptr<KvState> &kv) {

    return workers.try_submit([request = std::move(request), weak, &loop, &connections, &counter,
                               kv]() mutable {
        // 回显原方法和 64 位请求编号；只有访问共享 KV 数据时才持锁。
        request.kind = Kind::response;
        try {
            if (request.method == Method::system_ping) {
                handle_ping(request);
            } else {
                std::lock_guard lock(kv->mutex);
                kv->service.handle(request);
            }
        } catch (const std::bad_alloc &) {
            request.status = Status::resource_exhausted;
            request.payload = "out of memory";
        } catch (...) {
            request.status = Status::internal;
            request.payload = "internal server error";
        }

        (void)loop.post_task([weak, &loop, &connections, &counter, response = std::move(request)] {
            complete_response(weak, loop, connections, counter, response);
        });
    });
}

// 客户端连接在所属 I/O 线程上的 epoll 事件主回调。
// 分支逻辑：
//   1. event.error()：Socket 发生底层错误（EPOLLERR），立即关闭连接。
//   2. 可读/对端关闭分支：连接未处于半关闭或停机排空时，调用 conn->read() 读数据、拆帧并提交给 Worker；
//      若帧格式非法、在途请求超限、Worker 队列满或 EOF 时残留半包（!ok），立即关闭连接。
//   3. 可写分支（conn->wants_write()）：将 output_ 中积压的响应数据非阻塞写入内核缓冲区。
//   4. 末尾调用 refresh_connection()：根据最新状态决定关闭连接、静默等待 Worker 还是重装 epoll 事件。
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
                return submit_request(std::move(request), weak, loop, connections, workers, counter, kv);
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

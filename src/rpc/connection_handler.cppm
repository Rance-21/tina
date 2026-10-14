module;
#include <sys/epoll.h>

export module rpc.connection_handler;

import std;

import net.socket;
import net.epoll;
import net.event_loop;
import runtime.worker_pool;
import rpc.connection;
import rpc.protocol;
import service.kv;

namespace rpc::detail {

// 所有 Worker 共享一个 KV Service。
export struct KvState {
    std::mutex mutex;
    service::KvService service;
};

namespace {

net::EpollInterest interests(const Connection &conn) {
    auto mask = static_cast<net::EpollInterest>(EPOLLONESHOT);

    if (!conn.peer_eof() && !conn.draining())
        mask = mask | net::EpollInterest::read | net::EpollInterest::peer_closed;

    if (conn.wants_write())
        mask = mask | net::EpollInterest::write;

    return mask;
}

} // namespace

// 只能由 Connection 所属的 I/O 线程调用。
export void refresh_connection(net::EventLoop &loop, ConnectionMap &connections,
                               ConnectionCounter &counter,
                               const std::shared_ptr<Connection> &conn) {
    if (conn->finished()) {
        close_connection(loop, connections, counter, conn->fd());
        return;
    }

    // 没有新数据要读，也没有需要发送的数据，等 Worker 回调即可。
    if ((conn->peer_eof() || conn->draining()) && !conn->wants_write())
        return;

    loop.modify_inst(conn->fd(), interests(*conn));
}

namespace {

// Worker 只回传结果，连接状态仍由所属 I/O 线程更新。
void complete_response(const std::weak_ptr<Connection> &weak, net::EventLoop &loop,
                       ConnectionMap &connections, ConnectionCounter &counter,
                       const Response &response) {
    auto conn = weak.lock();
    if (!conn)
        return;

    try {
        if (!conn->complete_response(response)) {
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

bool submit_request(Request request, const std::weak_ptr<Connection> &weak, net::EventLoop &loop,
                    ConnectionMap &connections, runtime::WorkerPool &workers,
                    ConnectionCounter &counter, const std::shared_ptr<KvState> &kv) {
    return workers.try_submit([request = std::move(request), weak, &loop, &connections, &counter,
                               kv]() mutable {
        Response response{request.id, Status::bad_request, "internal server error"};
        try {
            std::lock_guard lock(kv->mutex);
            response = kv->service.handle(request);
        } catch (...) {
            // 将错误响应返回客户端。
        }

        (void)loop.post_task([weak, &loop, &connections, &counter, response = std::move(response)] {
            complete_response(weak, loop, connections, counter, response);
        });
    });
}

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
            bool ok = conn->read([&](Request request) {
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

void register_connection(net::Socket socket, net::EventLoop &loop, ConnectionMap &connections,
                         runtime::WorkerPool &workers, ConnectionCounter &counter,
                         const std::shared_ptr<KvState> &kv) {
    auto conn = std::make_shared<Connection>(std::move(socket));
    int fd = conn->fd();
    std::weak_ptr<Connection> weak = conn;

    connections.emplace(fd, conn);
    try {
        loop.add_event(fd, interests(*conn),
                       [conn, weak, &loop, &connections, &workers, &counter,
                        kv](net::EpollEvent event) {
                           handle_connection_event(conn, weak, loop, connections, workers, counter,
                                                   kv, event);
                       });
    } catch (...) {
        connections.erase(fd);
        throw;
    }
    counter.opened();
}

} // namespace

// 管理线程投递连接；引用的资源必须存活到 Worker 和 I/O 线程回收完成。
export bool post_connection(net::Socket socket, net::EventLoop &loop, ConnectionMap &connections,
                            runtime::WorkerPool &workers, ConnectionCounter &counter,
                            std::shared_ptr<KvState> kv) {
    return loop.post_task([socket = std::move(socket), &loop, &connections, &workers, &counter,
                           kv = std::move(kv)]() mutable {
        register_connection(std::move(socket), loop, connections, workers, counter, kv);
    });
}

} // namespace rpc::detail

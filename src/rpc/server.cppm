module;
#include <sys/epoll.h>

export module rpc.server;

import std;

import net.socket;
import net.epoll;
import net.event_loop;
import net.event_loop_thread_pool;

import runtime.config;
import runtime.worker_pool;
import runtime.log;

import rpc.connection;
import rpc.protocol;
import service.kv;

namespace {

// 所有 Worker 共享一个 KV Service。先用 Mutex
struct KvState {
    std::mutex mutex;
    service::KvService service;
};

// EPOLLONESHOT：每处理一次事件后，需要主动重新激活。
net::EpollInterest interests(const rpc::Connection &conn) {
    auto mask = static_cast<net::EpollInterest>(EPOLLONESHOT);

    if (!conn.peer_eof()) {
        mask = mask | net::EpollInterest::read | net::EpollInterest::peer_closed;
    }

    if (conn.wants_write())
        mask = mask | net::EpollInterest::write;

    return mask;
}

// 根据当前 Connection 状态，更新 epoll 监听。
// 只能在连接所属的 I/O 线程中调用。
void refresh(net::EventLoop &loop, const std::shared_ptr<rpc::Connection> &conn) {
    if (conn->finished()) {
        loop.unwatch(conn->fd());
        return;
    }

    // 已经 EOF，但仍有 Worker 在执行任务。
    // 暂不重新激活 FD，等待 Worker 结果通过 post() 返回。
    if (conn->peer_eof() && !conn->wants_write())
        return;

    loop.modify(conn->fd(), interests(*conn));
}

} // namespace

export namespace rpc {

void run_server(std::string_view port) {
    runtime::ServerConfig config;
    config.validate();

    runtime::log::init();

    auto listener = net::Socket::listen_tcp(port);
    listener.set_non_blocking();

    // Main Reactor：只负责接受新连接。
    net::Epoll accept_epoll;
    accept_epoll.add(listener.native_handle(), net::EpollInterest::read);

    auto kv = std::make_shared<KvState>();

    // 析构顺序：先停止 WorkerPool，再停止 I/O 线程。
    net::EventLoopThreadPool io_pool(config.io_threads);

    runtime::WorkerPool workers(config.worker_threads, config.max_pending_tasks);

    runtime::log::info("tina listening on port " + std::string(port));

    while (true) {
        for (const auto event : accept_epoll.wait()) {
            if (event.fd != listener.native_handle())
                continue;

            if (event.error() || event.hangup())
                throw std::runtime_error("listener error");

            if (!event.readable())
                continue;

            // 每轮最多接受 64 个连接，避免长期占用主线程。
            for (int i = 0; i < 64; ++i) {
                auto client = listener.try_accept();

                if (!client)
                    break;

                auto &loop = io_pool.next_loop();

                // 把新 Socket 的所有权移交给目标 I/O 线程。
                bool accepted = loop.post([socket = std::move(*client), &loop, &workers,
                                           kv]() mutable {
                    auto conn = std::make_shared<Connection>(std::move(socket));

                    const int fd = conn->fd();

                    // Worker 不拥有 Connection。
                    // 如果连接提前关闭，weak_ptr 会失效。
                    std::weak_ptr<Connection> weak = conn;

                    loop.watch(
                        fd, interests(*conn),

                        [conn, weak, &loop, &workers, kv](net::EpollEvent event) {
                            if (event.error()) {
                                loop.unwatch(conn->fd());
                                return;
                            }

                            try {
                                // 1. 读取 Socket 并提交完整的 RPC 请求。
                                if (!conn->peer_eof() &&
                                    (event.readable() || event.peer_closed() || event.hangup())) {

                                    bool ok = conn->read([&](Request request) -> bool {
                                        // 2. 业务任务进入 WorkerPool。
                                        return workers.try_submit([request = std::move(request),
                                                                   weak, &loop, kv]() mutable {
                                            Response response{request.id, Status::bad_request,
                                                              "internal server error"};

                                            try {
                                                std::lock_guard lock(kv->mutex);

                                                response = kv->service.handle(request);
                                            } catch (...) {
                                                // 保留上面的错误响应。
                                            }

                                            // 3. 业务完成，结果回到原 I/O 线程。
                                            (void)loop.post([weak, &loop,
                                                             response =
                                                                 std::move(response)]() mutable {
                                                auto connection = weak.lock();

                                                // 连接已关闭，直接丢弃结果。
                                                if (!connection)
                                                    return;

                                                try {
                                                    if (!connection->complete_response(response)) {
                                                        loop.unwatch(connection->fd());
                                                        return;
                                                    }

                                                    // 4. 先尝试非阻塞发送。
                                                    if (connection->wants_write())
                                                        connection->write();

                                                    refresh(loop, connection);

                                                } catch (...) {
                                                    loop.unwatch(connection->fd());
                                                }
                                            });
                                        });
                                    });

                                    if (!ok) {
                                        loop.unwatch(conn->fd());
                                        return;
                                    }
                                }

                                // 原来就有待发送数据时继续尝试发送。
                                if (conn->wants_write())
                                    conn->write();

                                // 5. 重新激活 EPOLLONESHOT。
                                refresh(loop, conn);

                            } catch (...) {
                                loop.unwatch(conn->fd());
                            }
                        });
                });

                if (!accepted)
                    runtime::log::warn("I/O EventLoop rejected connection");
            }
        }
    }
}

} // namespace rpc

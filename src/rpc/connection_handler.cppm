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

import broker.protocol;
import broker.service;

namespace rpc::detail {

// 所有 Worker 共享一个 KV Service。
export struct KvState {
    std::mutex mutex;
    service::KvService service;
};

// 服务端共享的业务状态。
// KV 有自己的锁，Broker 的锁由各 Partition 管理。
// 不允许在这里增加覆盖全部业务操作的全局锁。
export struct ServiceState {
    KvState kv;
    broker::BrokerService broker;
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

// Broker 业务分发，输入已经是一条完整的 RPC Request，payload 仍是二进制内容。
Response handle_broker_request(const Request &request, broker::BrokerService &service) {
    switch (request.method) {
    case Method::publish: {
        auto decoded = broker::protocol::decode_publish(request.payload);

        if (!decoded)
            return {request.id, Status::bad_request, "invalid PUBLISH payload"};

        auto result = service.publish(*decoded);

        return {request.id, Status::ok, broker::protocol::encode_publish_result(result)};
    }

    case Method::fetch: {
        auto decoded = broker::protocol::decode_fetch(request.payload);

        if (!decoded)
            return {request.id, Status::bad_request, "invalid FETCH payload"};

        auto result = service.fetch(*decoded);

        return {request.id, Status::ok, broker::protocol::encode_fetch_result(result)};
    }

    case Method::commit_offset: {
        auto decoded = broker::protocol::decode_commit_offset(request.payload);

        if (!decoded)
            return {request.id, Status::bad_request, "invalid COMMIT_OFFSET payload"};

        service.commit_offset(*decoded);

        // COMMIT_OFFSET 成功响应不需要 Payload。
        return {request.id, Status::ok, ""};
    }

    default:
        return {request.id, Status::bad_request, "unknown broker method"};
    }
}

// 这里只负责选择业务模块。
constexpr std::uint8_t service_mask = 0xF0;
Response dispatch_request(const Request &request, ServiceState &services) {
    const auto group = static_cast<std::uint8_t>(request.method) & service_mask;

    switch (group) {
    case 0x00: {
        std::lock_guard lock(services.kv.mutex);
        return services.kv.service.handle(request);
    }

    case 0x10:
        return handle_broker_request(request, services.broker);

    default:
        return {request.id, Status::bad_request, "unknown service"};
    }
}

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
                    ConnectionCounter &counter, const std::shared_ptr<ServiceState> &services) {

    return workers.try_submit([request = std::move(request), weak, &loop, &connections, &counter,
                               services]() mutable {
        Response response{request.id, Status::io_error, "internal server error"};

        try {
            response = dispatch_request(request, *services);
        } catch (const std::out_of_range &) {
            // Topic 或 Partition 不存在。
            response = {request.id, Status::not_found, "topic or partition not found"};
        } catch (const std::invalid_argument &) {
            // Offset 越界、非法参数、消费进度倒退等。
            response = {request.id, Status::bad_request, "invalid request"};
        } catch (const std::length_error &) {
            // Partition 满、Consumer 数量超限、FETCH 预算不足等。
            response = {request.id, Status::resource_exhausted, "broker resource limit exceeded"};
        } catch (const std::bad_alloc &) {
            response = {request.id, Status::resource_exhausted, "out of memory"};
        } catch (...) {
            // 不允许一个业务异常逃出 Worker，影响线程池。
            // 以后可以增加服务端错误日志与指标。
        }

        (void)loop.post_task([weak, &loop, &connections, &counter, response = std::move(response)] {
            complete_response(weak, loop, connections, counter, response);
        });
    });
}

void handle_connection_event(const std::shared_ptr<Connection> &conn,
                             const std::weak_ptr<Connection> &weak, net::EventLoop &loop,
                             ConnectionMap &connections, runtime::WorkerPool &workers,
                             ConnectionCounter &counter,
                             const std::shared_ptr<ServiceState> &services, net::EpollEvent event) {
    if (event.error()) {
        close_connection(loop, connections, counter, conn->fd());
        return;
    }

    try {
        if (!conn->peer_eof() && !conn->draining() &&
            (event.readable() || event.peer_closed() || event.hangup())) {
            bool ok = conn->read([&](Request request) {
                return submit_request(std::move(request), weak, loop, connections, workers, counter,
                                      services);
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
                         const std::shared_ptr<ServiceState> &services) {
    auto conn = std::make_shared<Connection>(std::move(socket));
    int fd = conn->fd();
    std::weak_ptr<Connection> weak = conn;

    connections.emplace(fd, conn);
    try {
        loop.add_event(
            fd, interests(*conn),
            [conn, weak, &loop, &connections, &workers, &counter, services](net::EpollEvent event) {
                handle_connection_event(conn, weak, loop, connections, workers, counter, services,
                                        event);
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
                            std::shared_ptr<ServiceState> services) {
    return loop.post_task([socket = std::move(socket), &loop, &connections, &workers, &counter,
                           services = std::move(services)]() mutable {
        register_connection(std::move(socket), loop, connections, workers, counter, services);
    });
}

} // namespace rpc::detail

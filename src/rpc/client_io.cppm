module;
#include <unistd.h>

export module rpc.client:io;

import :types;

import std;
import net.socket;
import net.buffer;
import net.epoll;
import net.event_loop;
import ide.protocol;

namespace rpc {

void RpcClient::State::start(const std::shared_ptr<State> &state) {
    // Socket 和 timerfd 注册到同一个 EventLoop。
    state->loop.add_event(state->socket->native_handle(),
                          net::EpollInterest::read | net::EpollInterest::write |
                              net::EpollInterest::peer_closed,
                          [state](net::EpollEvent event) {
                              state->on_event(event);
                          });

    state->loop.add_event(state->timer_fd, net::EpollInterest::read,
                          [state](net::EpollEvent event) {
                              state->on_timer(event);
                          });
}

void RpcClient::State::on_event(net::EpollEvent event) {
    if (closed)
        return;

    try {
        if (!connected) {
            if (!event.writable() && !event.error() && !event.hangup() && !event.peer_closed())
                return;

            socket->check_connect_result();
            connected = true;
        }

        if (event.error())
            throw std::runtime_error("client socket error");

        if (event.readable() || event.peer_closed() || event.hangup())
            receive();

        if (event.writable() || !output.empty())
            send_pending();

        refresh();
    } catch (...) {
        fail(std::current_exception());
    }
}

void RpcClient::State::refresh() {
    auto interest = net::EpollInterest::read | net::EpollInterest::peer_closed;

    // Socket 在大部分时间里都是可写的。如果一直监听 EPOLLOUT，即使没有数据需要发送，epoll
    // 也会不断返回可写事件，造成不必要的唤醒
    if (!connected || !output.empty())
        interest = interest | net::EpollInterest::write;

    loop.modify_inst(socket->native_handle(), interest);
}

// 网络连接失败或客户端关闭，所有尚未完成的请求都失败。
void RpcClient::State::fail(std::exception_ptr error) noexcept {
    if (closed)
        return;

    closed = true;

    if (socket) {
        try {
            loop.del_event(socket->native_handle());
        } catch (...) {
        }

        socket.reset();
    }

    if (timer_fd >= 0) {
        try {
            loop.del_event(timer_fd);
        } catch (...) {
        }

        ::close(timer_fd);
        timer_fd = -1;
    }

    for (auto &[id, request] : pendings) {
        try {
            request.promise->set_exception(error);
        } catch (...) {
        }
    }

    pendings.clear();
    deadlines.clear();
    expired_ids.clear();
}

// 由 EventLoop::post() 提交过来。
// Deadline 在调用线程计算，不在这里重新开始计时。
void RpcClient::State::send(Method method, std::string payload, Clock::time_point deadline,
                            Promise promise) {
    if (closed || payload.size() > max_frame_length - min_frame_length || pendings.size() >= max_inflight ||
        output.readable_bytes() + payload.size() + frame_header_size > max_output) {
        promise->set_exception(std::make_exception_ptr(
            std::runtime_error("RPC client closed or request limit exceeded")));
        return;
    }

    if (Clock::now() >= deadline) {
        promise->set_exception(std::make_exception_ptr(TimeoutError{}));
        return;
    }

    std::uint64_t id;

    do {
        id = next_id++;

        if (next_id == 0)
            next_id = 1;
    } while (id == 0 || pendings.contains(id) || expired_ids.contains(id));

    const bool earliest = deadlines.empty() || DeadlineEntry{deadline, id} < *deadlines.begin();

    pendings.emplace(id, Pending{method, promise, deadline});
    deadlines.emplace(deadline, id);

    encode_frame({.kind = Kind::request, .request_id = id, .method = method,
                  .status = Status::ok, .payload = std::move(payload)}, output);

    // 仅在最早的截止时间改变时更新定时器。
    if (earliest)
        update_timer();

    if (connected)
        send_pending();

    refresh();
}

void RpcClient::State::send_pending() {
    for (int i = 0; i < 8 && !output.empty(); ++i) {
        auto n = socket->try_send(output.data(), output.readable_bytes());

        if (!n || *n == 0)
            break;

        output.retrieve(*n);
    }
}

void RpcClient::State::receive() {
    std::array<char, 8192> chunk{};

    for (int i = 0; i < 8; ++i) {
        auto n = socket->try_recv(chunk.data(), chunk.size());

        if (!n)
            break;

        if (*n == 0)
            throw std::runtime_error("server closed connection");

        input.append(chunk.data(), *n);
        decode();
    }
}

// 解析响应，并判断对应请求是否已经超时。
void RpcClient::State::decode() {
    while (auto response = decode_frame(input)) {
        if (response->kind != Kind::response)
            throw std::runtime_error("expected RPC response");
        const auto id = response->request_id;
        auto it = pendings.find(id);

        if (it == pendings.end()) {
            // 已经超时的请求可能晚些时候才返回。
            // 识别后直接丢弃，不影响其他 Future。
            if (expired_ids.erase(id) == 0)
                throw std::runtime_error("unknown response id");

            continue;
        }

        if (response->method != it->second.method)
            throw std::runtime_error("RPC response method mismatch");
        auto promise = it->second.promise;
        const auto deadline = it->second.deadline;

        const bool earliest = !deadlines.empty() && deadlines.begin()->second == id;

        deadlines.erase(DeadlineEntry{deadline, id});
        pendings.erase(it);

        if (Clock::now() >= deadline) {
            // 即使 timerfd 事件还没有执行，
            // 已经过期的响应仍然算超时。

            promise->set_exception(std::make_exception_ptr(TimeoutError{}));
        } else {
            promise->set_value(std::move(*response));
        }

        // 仅当最近的 Deadline 改变时才修改 timerfd。
        if (earliest)
            update_timer();
    }
}

} // namespace rpc

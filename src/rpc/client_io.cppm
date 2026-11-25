module;
#include <unistd.h>

export module rpc.client:io;

import :types;

import std;
import net.socket;
import net.buffer;
import net.epoll;
import net.event_loop;
import rpc.protocol;

namespace rpc {

// 在客户端专属 I/O 线程启动阶段，将非阻塞 Socket 和共享 timerfd 注册到同一个 EventLoop。
// 初始注册包含 EpollInterest::write，是因为非阻塞 connect() 三次握手完成需要通过可写事件感知。
void RpcClient::State::start(const std::shared_ptr<State> &state) {
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

// 客户端 Socket 的 epoll 事件主回调（运行于客户端 I/O 线程）。
// 分支逻辑：
//   1. !connected（非阻塞 connect 尚未完成）：等待首次可写或错误事件，调用 check_connect_result()
//      通过 getsockopt(SO_ERROR) 检查三次握手是否成功；成功则标记 connected = true。
//   2. event.error()：连接发生错误，抛异常进入 catch 块统一调用 fail() 终止所有在途请求。
//   3. 可读/对端关闭分支：调用 receive() 读取服务端响应并唤醒对应 Future。
//   4. 可写/存在待发数据分支：调用 send_pending() 把 output 中积压的请求刷入内核缓冲区。
//   5. 末尾调用 refresh()：根据连接是否已建立及 output 是否排空，动态增删 EPOLLOUT 监听。
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

// 根据客户端当前状态动态更新 Socket 的 epoll 关注事件。
// 分支逻辑：
//   - 常驻监听：read | peer_closed（随时接收服务端响应或感知服务端断开）。
//   - 按需监听 write（!connected || !output.empty()）：
//     仅当非阻塞 connect 仍在握手中，或 send_pending() 遇到 EAGAIN 导致 output 还有残留数据时，
//     才挂载 EpollInterest::write；一旦握手完成且 output 排空，立即摘除 write 防止 epoll 空转。
void RpcClient::State::refresh() {
    auto interest = net::EpollInterest::read | net::EpollInterest::peer_closed;

    if (!connected || !output.empty())
        interest = interest | net::EpollInterest::write;

    loop.modify_event(socket->native_handle(), interest);
}

// 网络故障或客户端析构时，注销并关闭 Socket 与 timerfd，并向所有尚未完成的 Promise 注入异常。
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

// 由 async_call() 通过 post_task 投递到客户端 I/O 线程执行：分配请求 ID、编码请求帧并尝试发送。
// 分支逻辑：
//   1. 容量与状态校验：若已关闭、单帧超限、在途请求达 max_inflight (64) 或输出缓冲超 4 MiB，直接令 Promise 失败。
//   2. 超时预检：若排队到 I/O 线程时已超过调用方设定的 deadline，直接返回 TimeoutError。
//   3. ID 分配与编码：跳过正在使用或已超时未回收的 ID，记录到 pendings 与有序集合 deadlines，并将请求编码进 output。
//   4. 定时器更新（earliest）：仅当新请求成为最早到期的请求时才调用 update_timer() 重置 timerfd。
//   5. 立即发送与刷新：若已连接则先调 send_pending() 尝试当场发完，再调 refresh() 按需决定是否监听 EPOLLOUT。
void RpcClient::State::send(Method method, std::string payload, Clock::time_point deadline,
                            Promise promise) {
    if (closed || payload.size() > max_frame_length - min_frame_length ||
        pendings.size() >= max_inflight ||
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

    Frame req{.kind = Kind::request,
              .request_id = id,
              .method = method,
              .status = Status::ok,
              .payload = std::move(payload)};
    (void)output.enqueue(req);

    // 仅在最早的截止时间改变时更新定时器。
    if (earliest)
        update_timer();

    if (connected)
        send_pending();

    refresh();
}

// 非阻塞将 output 队列中的请求数据通过 writev 写入 Socket；遇 EAGAIN 时中断，留待下次 EPOLLOUT 继续发送。
void RpcClient::State::send_pending() {
    output.write(*socket);
}

// 非阻塞从 Socket 直接读入 input 可写区（单次事件最多 64 KiB），每读一块就调用 handle_responses() 拆包分发。
// 分支逻辑：!n 表示内核缓冲区已读空（EAGAIN）；*n == 0 表示服务端关闭连接，抛异常触发 fail()。
void RpcClient::State::receive() {
    for (int i = 0; i < 8; ++i) {
        input.ensure_writable(8192);
        auto n = socket->try_recv(input.writable_data(), input.writable_bytes());

        if (!n)
            break;

        if (*n == 0)
            throw std::runtime_error("server closed connection");

        input.commit_write(*n);
        handle_responses();
    }
}

// 从 input 缓冲区循环零拷贝拆出完整响应视图，按需获取 payload 所有权后再调用 retrieve() 推进读游标。
void RpcClient::State::handle_responses() {
    while (auto response = decode_frame(input)) {
        const std::size_t frame_bytes = response->wire_bytes();
        if (response->kind == Kind::event) {
            if (event_handler) {
                Frame ev{
                    .length = static_cast<std::uint32_t>(min_frame_length + response->payload.size()),
                    .kind = Kind::event,
                    .request_id = 0,
                    .method = response->method,
                    .status = response->status,
                    .payload = std::string(response->payload),
                };
                input.retrieve(frame_bytes);
                try {
                    event_handler(std::move(ev));
                } catch (...) {
                }
            } else {
                input.retrieve(frame_bytes);
            }
            continue;
        }

        if (response->kind != Kind::response)
            throw std::runtime_error("expected RPC response");
        const auto id = response->request_id;
        auto it = pendings.find(id);

        if (it == pendings.end()) {
            // 已经超时的请求直接丢弃视图，不分配 payload。
            input.retrieve(frame_bytes);
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
            input.retrieve(frame_bytes);
            promise->set_exception(std::make_exception_ptr(TimeoutError{}));
        } else {
            Frame owned{
                .length = static_cast<std::uint32_t>(min_frame_length + response->payload.size()),
                .kind = Kind::response,
                .request_id = id,
                .method = response->method,
                .status = response->status,
                .payload = std::string(response->payload),
            };
            input.retrieve(frame_bytes);
            promise->set_value(std::move(owned));
        }

        // 仅当最近的 Deadline 改变时才修改 timerfd。
        if (earliest)
            update_timer();
    }
}

} // namespace rpc

export module ide.server_terminal;

export import :types;
export import :session;
export import :io;

import std;
import net.epoll;
import net.event_loop;
import service.terminal;
import ide.server_protocol;

namespace server {

// ========================================================================
// 1. 处理 0x0303 (terminal_state) 请求：启动或关闭 Shell（在 Worker 线程调用）
// ========================================================================
// Payload 约定（[u32 len][data]）：
//   - 启动 Shell：data 为空、"start" 或 4 字节 0；
//   - 关闭 Shell：data 为 4 字节大端 terminal_id（> 0）。
// 成功响应：Frame.status = Status::ok，Payload = [u32 len=4][u32 terminal_id]。
void TerminalManager::handle_terminal_state(Frame &frame, int conn_fd) {
    auto req_id = decode_terminal_state_id(frame.payload);
    if (!req_id) {
        frame.status = Status::invalid_argument;
        frame.payload = encode_terminal_state("invalid terminal_state payload");
        return;
    }

    if (*req_id == 0) {
        start_shell(frame, conn_fd);
    } else {
        close_shell(frame, conn_fd, *req_id);
    }
}

// ========================================================================
// 2. 处理 0x0301 (terminal_input) 请求：在所属 I/O 线程内联解码并挂入 EPOLLOUT 发送缓冲
// ========================================================================
void TerminalManager::handle_terminal_input(std::string_view payload, Frame &response,
                                            int conn_fd) {
    auto req = decode_terminal_payload_view(payload);
    if (!req || req->terminal_id == 0) {
        response.status = Status::invalid_argument;
        response.payload = encode_terminal_input(0, "invalid terminal_input payload");
        return;
    }
    const std::uint32_t terminal_id = req->terminal_id;
    auto session = find_session(terminal_id, conn_fd);
    if (!session || session->closed || session->master_fd < 0) {
        response.status = Status::not_found;
        response.payload = encode_terminal_input(terminal_id, "terminal not found");
        return;
    }

    session->running_command = true;
    session->write_buffer.append(req->data.data(), req->data.size());
    session->write_buffer.append("\r", 1);
    try {
        loop_.modify_event(session->master_fd, net::EpollInterest::read | net::EpollInterest::write);
    } catch (...) {
    }

    response.status = Status::ok;
    response.payload = encode_terminal_input(terminal_id, "OK");
}

// ========================================================================
// 3. 处理 0x0302 (terminal_on_tab) 请求：Tab 命令补全（在 Worker 线程执行）
// ========================================================================
// 流程：
//   1. 标记 tab_completing = true，暂停 I/O 线程对该终端的常规输出转发；
//   2. 调用 service::terminal::complete_tab_on_pty() 执行清空 Readline、写入输入+\t、
//      读取 PTY 字节并通过单行光标模拟器解析出补全后的纯净命令；
//   3. 恢复 tab_completing = false 并直接在当前 Frame 返回。
void TerminalManager::handle_terminal_on_tab(Frame &frame, int conn_fd) {
    // 零拷贝解析出 terminal_id 与指向 frame.payload 内部的输入切片
    auto req = decode_terminal_payload_view(frame.payload);
    if (!req || req->terminal_id == 0) {
        frame.status = Status::invalid_argument;
        frame.payload = encode_terminal_on_tab(0, "invalid terminal_on_tab payload");
        return;
    }

    const std::uint32_t terminal_id = req->terminal_id;
    auto session = find_session(terminal_id, conn_fd);
    if (!session) {
        frame.status = Status::not_found;
        frame.payload = encode_terminal_on_tab(terminal_id, "terminal not found");
        return;
    }

    std::lock_guard tab_lock(session->tab_mutex);
    session->tab_completing.store(true, std::memory_order_relaxed);
    struct ResetGuard {
        std::atomic<bool> &flag;
        ~ResetGuard() {
            flag.store(false, std::memory_order_relaxed);
        }
    } guard{session->tab_completing};

    auto completed = service::terminal::complete_tab_on_pty(session->master_fd, req->data);
    if (!completed) {
        frame.status = Status::internal;
        frame.payload = encode_terminal_on_tab(terminal_id, req->data);
        return;
    }

    frame.status = Status::ok;
    frame.payload = encode_terminal_on_tab(terminal_id, *completed);
}

// 统一分发需在 Worker 线程执行的终端 RPC 请求（0x0302 Tab 补全、0x0303 启停会话）。
void TerminalManager::handle_request(Frame &frame, int conn_fd) {
    switch (frame.method) {
    case Method::terminal_on_tab:
        handle_terminal_on_tab(frame, conn_fd);
        return;
    case Method::terminal_state:
        handle_terminal_state(frame, conn_fd);
        return;
    default:
        frame.status = Status::unsupported;
        frame.payload = "unsupported method";
        return;
    }
}

}

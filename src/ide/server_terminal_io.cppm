module;
#include <errno.h>
#include <unistd.h>

export module ide.server_terminal:io;

import :types;

import std;
import net.epoll;
import net.event_loop;
import service.terminal;
import ide.server_protocol;

namespace server {

// ========================================================================
// I/O 线程 Epoll 事件回调（处理 master_fd 的 EPOLLOUT 命令写入与 EPOLLIN 输出读取）
// ========================================================================
void TerminalManager::on_pty_event(const std::shared_ptr<Session> &session, net::EpollEvent event) {
    if (session->closed || session->master_fd < 0)
        return;

    // 1. 处理可写事件（EPOLLOUT）：将 write_buffer 中的命令写入 master_fd，写完立即撤销
    // EPOLLOUT。
    if (event.writable() && !session->write_buffer.empty()) {
        while (!session->write_buffer.empty()) {
            ssize_t n = ::write(session->master_fd, session->write_buffer.data(),
                                session->write_buffer.readable_bytes());
            if (n > 0) {
                session->write_buffer.retrieve(static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            break; // EAGAIN：等待下次 EPOLLOUT
        }

        if (session->write_buffer.empty()) {
            try {
                loop_.modify_event(session->master_fd, net::EpollInterest::read);
            } catch (...) {
            }
        }
    }

    // 2. 处理可读事件（EPOLLIN）：若 Worker 正在做 Tab 补全则暂不抢读 master_fd。
    if (session->tab_completing.load(std::memory_order_relaxed))
        return;

    bool child_exited = false;
    if (event.readable() || event.hangup() || event.error()) {
        std::array<char, 8192> buf{};
        for (int i = 0; i < 8; ++i) {
            if (session->tab_completing.load(std::memory_order_relaxed))
                break;

            ssize_t n = ::read(session->master_fd, buf.data(), buf.size());
            if (n > 0) {
                session->read_carry.append(buf.data(), static_cast<std::size_t>(n));
                process_pty_output(*session, false);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;

            // Linux PTY 特性：当子进程退出关闭全部 slave_fd 时，read(master_fd) 返回 0 或 -1
            // (errno == EIO)。
            child_exited = true;
            break;
        }
    }

    if (child_exited) {
        process_pty_output(*session, true);
        {
            std::lock_guard lock(mutex_);
            sessions_.erase(session->terminal_id);
        }
        cleanup_session(*session, true);
        send_state_event(session->conn_fd, session->terminal_id);
    }
}

// 从 read_carry 中识别并剥离 PROMPT_COMMAND 结束标记（\033]777;__TINA_DONE__\007）：
//   - 普通输出通过 terminal_output (0x0381) 事件推送；
//   - 遇到结束标记说明命令已执行完毕，通过 terminal_state (0x0303) 事件推送通知客户端。
void TerminalManager::process_pty_output(Session &session, bool flush_all) {
    while (!session.read_carry.empty()) {
        const std::size_t pos = session.read_carry.find(service::terminal::prompt_done_marker);
        if (pos != std::string::npos) {
            if (pos > 0)
                send_output_chunks(session.conn_fd, session.terminal_id,
                                   std::string_view(session.read_carry).substr(0, pos));

            session.read_carry.erase(0, pos + service::terminal::prompt_done_marker.size());
            if (std::exchange(session.running_command, false))
                send_state_event(session.conn_fd, session.terminal_id);
            continue;
        }

        if (flush_all) {
            send_output_chunks(session.conn_fd, session.terminal_id, session.read_carry);
            session.read_carry.clear();
            break;
        }

        // 保留尾部可能与 prompt_done_marker 前缀发生半包重叠的少量字节（不超过标记长度 - 1）
        const std::size_t emit_len = service::terminal::safe_output_prefix_len(session.read_carry);
        if (emit_len > 0) {
            send_output_chunks(session.conn_fd, session.terminal_id,
                               std::string_view(session.read_carry).substr(0, emit_len));
            session.read_carry.erase(0, emit_len);
        }
        break;
    }
}

// 按 max_terminal_output_bytes 分块推送 0x0381 (terminal_output) 事件。
void TerminalManager::send_output_chunks(int conn_fd, std::uint32_t terminal_id,
                                         std::string_view text) {
    if (!sender_ || text.empty())
        return;

    for (std::size_t offset = 0; offset < text.size();) {
        const std::size_t chunk_len =
            std::min<std::size_t>(text.size() - offset, max_terminal_output_bytes);
        sender_(conn_fd, make_terminal_output_event(terminal_id, text.substr(offset, chunk_len)));
        offset += chunk_len;
    }
}

// 推送 0x0303 (terminal_state) 事件（通知客户端命令执行完毕或 Shell 退出）。
void TerminalManager::send_state_event(int conn_fd, std::uint32_t terminal_id) {
    if (sender_)
        sender_(conn_fd, make_terminal_state_event(terminal_id));
}

}

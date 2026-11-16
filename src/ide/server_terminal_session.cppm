module;
#include <sys/types.h>

export module ide.server_terminal:session;

import :types;

import std;
import net.epoll;
import net.event_loop;
import service.terminal;
import ide.server_protocol;

namespace server {

// 停机或析构时清理本 I/O 线程名下的所有终端会话与子进程（在所属 I/O 线程调用）。
void TerminalManager::stop() noexcept {
    std::vector<std::shared_ptr<Session>> all_sessions;
    {
        std::lock_guard lock(mutex_);
        if (stopped_)
            return;
        stopped_ = true;
        for (auto &[_, session] : sessions_)
            all_sessions.push_back(session);
        sessions_.clear();
    }

    for (auto &session : all_sessions)
        cleanup_session(*session, true);
}

// 当某条客户端 TCP 连接断开时，自动清理该连接创建的全部 Shell 会话。
void TerminalManager::close_for_connection(int conn_fd) noexcept {
    std::vector<std::shared_ptr<Session>> to_close;
    {
        std::lock_guard lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->second->conn_fd == conn_fd) {
                to_close.push_back(it->second);
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto &session : to_close)
        cleanup_session(*session, true);
}

// 在 Worker 线程中通过 service::terminal::spawn_bash_pty() 启动 Bash，并把 master_fd 挂载到
// EventLoop。
void TerminalManager::start_shell(Frame &frame, int conn_fd) {
    int master_fd = -1;
    pid_t pid = -1;
    std::string err = service::terminal::spawn_bash_pty(master_fd, pid);
    if (!err.empty()) {
        frame.status = (err == "forkpty failed") ? Status::resource_exhausted : Status::internal;
        frame.payload = encode_terminal_state(err);
        return;
    }

    auto session = std::make_shared<Session>();
    session->conn_fd = conn_fd;
    session->master_fd = master_fd;
    session->child_pid = pid;

    std::uint32_t terminal_id = 0;
    {
        // 给 sessions_ 加锁
        std::lock_guard lock(mutex_);
        if (stopped_) {
            service::terminal::terminate_pty_child(session->master_fd, session->child_pid);
            frame.status = Status::unavailable;
            frame.payload = encode_terminal_state("terminal manager stopped");
            return;
        }
        terminal_id = next_id_++;
        session->terminal_id = terminal_id;
        sessions_.emplace(terminal_id, session);
    }

    // 将 master_fd 跨线程投递到所属连接的 EventLoop 上监听可读事件（EPOLLIN）。
    auto registered = std::make_shared<std::promise<bool>>();
    auto future = registered->get_future();
    std::weak_ptr<Session> weak_session = session;
    std::weak_ptr<TerminalManager> weak = shared_from_this();

    if (!loop_.post_task([&loop = loop_, weak, weak_session, registered]() {
            auto s = weak_session.lock();
            if (!s || s->closed.load() || s->master_fd < 0) {
                registered->set_value(false);
                return;
            }
            try {
                // epoll 回调仅持有 weak_session，不强引用 Session
                loop.add_event(s->master_fd, net::EpollInterest::read,
                               [weak, weak_session](net::EpollEvent ev) {
                                   auto sess = weak_session.lock();
                                   if (!sess)
                                       return;
                                   if (auto mgr = weak.lock())
                                       mgr->on_pty_event(sess, ev);
                               });
                registered->set_value(true);
            } catch (...) {
                registered->set_value(false);
            }
        }) ||
        !future.get()) {
        {
            std::lock_guard lock(mutex_);
            sessions_.erase(terminal_id);
        }
        cleanup_session(*session, false);
        frame.status = Status::internal;
        frame.payload = encode_terminal_state("failed to register pty fd to event loop");
        return;
    }

    frame.status = Status::ok;
    frame.payload = encode_terminal_state_id(terminal_id);
}

// 在 Worker 线程中关闭指定 terminal_id 的 Shell。
void TerminalManager::close_shell(Frame &frame, int conn_fd, std::uint32_t terminal_id) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(terminal_id);
        if (it != sessions_.end() && it->second->conn_fd == conn_fd) {
            session = it->second;
            sessions_.erase(it);
        }
    }

    if (!session) {
        frame.status = Status::not_found;
        frame.payload = encode_terminal_state_id(terminal_id);
        return;
    }

    session->closed.store(true);
    auto unhooked = std::make_shared<std::promise<void>>();
    auto future = unhooked->get_future();

    if (loop_.post_task([&loop = loop_, fd = session->master_fd, unhooked]() {
            try {
                loop.del_event(fd);
            } catch (...) {
            }
            unhooked->set_value();
        })) {
        future.wait();
    }

    cleanup_session(*session, false);
    frame.status = Status::ok;
    frame.payload = encode_terminal_state_id(terminal_id);
}

// 释放 master_fd 并终止、回收 Shell 子进程。
void TerminalManager::cleanup_session(Session &session, bool on_io_thread) noexcept {
    session.closed.store(true);
    if (on_io_thread && session.master_fd >= 0) {
        try {
            loop_.del_event(session.master_fd);
        } catch (...) {
        }
    }
    service::terminal::terminate_pty_child(session.master_fd, session.child_pid);
}

}

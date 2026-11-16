module;
#include <errno.h>
#include <sys/inotify.h>
#include <unistd.h>

export module service.dir_watcher;

export import :types;

import std;
import net.epoll;
import net.event_loop;
import runtime.worker_pool;
import rpc.connection;
import ide.protocol;
import ide.server;
import service.file_watcher;
import service.workspace_cache;

namespace service {

// epoll 到 inotify-fd 的 handler 函数
void DirWatcher::on_event(net::EpollEvent event) {
    if (stopped_ || inotify_fd_ < 0)
        return;

    if (event.error() || event.hangup()) {
        stop();
        return;
    }

    alignas(inotify_event) std::array<char, 4096> buffer{};
    std::unordered_set<int> changed_wds;
    std::unordered_set<std::uint32_t> tmp_move_cookies;
    bool overflow = false;

    while (true) {
        const ssize_t n = ::read(inotify_fd_, buffer.data(), buffer.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break; // EAGAIN：本轮内核事件已读空
        }
        if (n == 0)
            break;

        std::size_t offset = 0;
        while (offset + sizeof(inotify_event) <= static_cast<std::size_t>(n)) {
            const auto *ev = reinterpret_cast<const inotify_event *>(buffer.data() + offset);
            offset += sizeof(inotify_event) + ev->len;

            // 内核队列满、可能丢事件了
            if ((ev->mask & IN_Q_OVERFLOW) != 0) {
                overflow = true;
                continue;
            }

            // 内核通知这个 wd 已经失效（比如目录被删或被卸载），内核通知的，不需要 remove 内核
            if ((ev->mask & IN_IGNORED) != 0) {
                remove_watch(ev->wd, false, true);
                changed_wds.erase(ev->wd);
                continue;
            }

            if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                remove_watch(ev->wd, true, true);
                changed_wds.erase(ev->wd);
                continue;
            }

            std::string_view entry_name;
            if (ev->len > 0) {
                std::string_view raw(ev->name, ev->len);
                entry_name = raw.substr(0, raw.find('\0'));
            }

            // 忽略服务端 write_file 原子替换过程中创建的 .tmp.XXXXXX 临时文件及其配对 rename
            // 事件。
            if (!entry_name.empty() && entry_name.find(".tmp.") != std::string_view::npos) {
                if ((ev->mask & IN_MOVED_FROM) != 0 && ev->cookie != 0)
                    tmp_move_cookies.insert(ev->cookie);
                continue;
            }
            if ((ev->mask & IN_MOVED_TO) != 0 && ev->cookie != 0 &&
                tmp_move_cookies.contains(ev->cookie)) {
                continue;
            }

            auto wit = watches_.find(ev->wd);
            if (wit == watches_.end())
                continue;

            if (cache_ != nullptr)
                cache_->invalidate_dir(wit->second.rel_path);
            // 若被删除或移走的是子目录，级联清理该子目录树缓存并解除对应 FileWatcher 监听。
            if (!entry_name.empty() && (ev->mask & IN_ISDIR) != 0 &&
                (ev->mask & (IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)) != 0) {
                evict_dir_and_files(join_rel_path(wit->second.rel_path, entry_name), true);
            }
            changed_wds.insert(ev->wd);
        }
    }

    // 丢事件，无法更新 cache ，所有删除重来（单 inotify 对应多 path）
    if (overflow) {
        if (cache_ != nullptr)
            cache_->clear_dirs();
        for (const auto &[wd, _] : watches_)
            changed_wds.insert(wd);
    }

    for (int wd : changed_wds)
        schedule_refresh(wd);
}

// 把目录扫描扔到 Worker 线程
void DirWatcher::schedule_refresh(int wd) {
    auto it = watches_.find(wd);
    if (it == watches_.end())
        return;

    WatchEntry &entry = it->second;
    if (!has_subscribers(entry)) {
        remove_watch(wd, true);
        return;
    }

    if (entry.refreshing) {
        entry.dirty = true;
        return;
    }

    entry.refreshing = true;
    entry.dirty = false;
    const std::string rel_path = entry.rel_path;
    std::weak_ptr<DirWatcher> weak = shared_from_this();

    if (!workers_.try_submit([weak, wd, rel_path]() {
            auto event_frame = server::build_workspace_list_dir_event(rel_path);
            if (auto self = weak.lock()) {
                (void)self->loop_.post_task(
                    [weak, wd, event_frame = std::move(event_frame)]() mutable {
                        if (auto watcher = weak.lock())
                            watcher->complete_refresh(wd, std::move(event_frame));
                    });
            }
        })) {
        entry.refreshing = false;
    }
}

void DirWatcher::complete_refresh(int wd, std::optional<Frame> event_frame) {
    if (stopped_)
        return;

    auto it = watches_.find(wd);
    if (it == watches_.end())
        return;

    WatchEntry &entry = it->second;
    if (!has_subscribers(entry)) {
        remove_watch(wd, true);
        return;
    }

    if (event_frame) {
        const bool broadcast = entry.subscribers.size() > 1;
        for (auto sub_it = entry.subscribers.begin(); sub_it != entry.subscribers.end();) {
            auto conn = sub_it->second.lock();
            if (!conn || conn->peer_eof() || conn->draining()) {
                sub_it = entry.subscribers.erase(sub_it);
                continue;
            }
            ++sub_it;

            try {
                if (broadcast)
                    event_frame->share_payload();
                const bool enqueued = broadcast ? conn->enqueue_event(*event_frame)
                                                : conn->enqueue_event(std::move(*event_frame));
                if (!enqueued) {
                    rpc::close_connection(loop_, connections_, counter_, conn->fd());
                    continue;
                }
                if (conn->wants_write())
                    conn->write();
                if (refresh_conn_)
                    refresh_conn_(conn);
            } catch (...) {
                rpc::close_connection(loop_, connections_, counter_, conn->fd());
            }
        }
    }

    if (!has_subscribers(entry)) {
        remove_watch(wd, true);
        return;
    }

    if (entry.dirty) {
        entry.refreshing = false;
        entry.dirty = false;
        schedule_refresh(wd);
    } else {
        entry.refreshing = false;
    }
}

}

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

    if (!changed_wds.empty()) {
        // 本轮 inotify 可能同时命中多个目录变更，使用批量守卫合并唤醒沉睡 Blocking Worker。
        runtime::WorkerPool::SubmitBatchGuard wake_batch(workers_, io_id_);
        for (int wd : changed_wds)
            schedule_refresh(wd);
    }
}

// 把目录扫描扔到 Worker 线程（通过单指针捕获 [ctx] 命中 24B SBO，消除两层闭包堆分配）
void DirWatcher::schedule_refresh(int wd) {
    auto it = watches_.find(wd);
    if (it == watches_.end())
        return;

    WatchEntry &entry = it->second;
    prune_disconnected_subscribers(entry);
    if (entry.subscribers.empty()) {
        remove_watch(wd, true);
        return;
    }

    if (entry.refreshing) {
        entry.dirty = true;
        return;
    }

    entry.refreshing = true;
    entry.dirty = false;
    RefreshTaskContext *ctx = acquire_refresh_ctx(wd, entry.rel_path);

    auto worker_lambda = [ctx]() noexcept {
        try {
            ctx->event_frame = server::build_workspace_list_dir_event(ctx->rel_path);
        } catch (...) {
            ctx->event_frame.reset();
        }

        if (auto self = ctx->weak_watcher.lock()) {
            auto complete_lambda = [ctx]() noexcept {
                if (auto watcher = ctx->weak_watcher.lock()) {
                    watcher->complete_refresh(ctx);
                } else {
                    delete ctx;
                }
            };
            static_assert(sizeof(complete_lambda) <= 24 && alignof(decltype(complete_lambda)) <= 8 &&
                          std::is_nothrow_move_constructible_v<decltype(complete_lambda)>,
                          "DirWatcher completion task must fit in std::move_only_function 24B SBO");

            if (self->loop_.post_task(std::move(complete_lambda)))
                return;
        }
        // 仅当 DirWatcher 已销毁或 EventLoop 已停止拒绝新任务时，在 Worker 线程兜底释放 ctx。
        delete ctx;
    };
    static_assert(sizeof(worker_lambda) <= 24 && alignof(decltype(worker_lambda)) <= 8 &&
                  std::is_nothrow_move_constructible_v<decltype(worker_lambda)>,
                  "DirWatcher worker task must fit in std::move_only_function 24B SBO");

    if (!workers_.try_submit(io_id_, runtime::TaskClass::blocking, std::move(worker_lambda))) {
        release_refresh_ctx(ctx);
        entry.refreshing = false;
    }
}

void DirWatcher::complete_refresh(RefreshTaskContext *ctx) {
    const int wd = ctx->wd;
    std::optional<Frame> event_frame = std::move(ctx->event_frame);
    release_refresh_ctx(ctx);

    if (stopped_)
        return;

    auto it = watches_.find(wd);
    if (it == watches_.end())
        return;

    WatchEntry &entry = it->second;
    // 先排除已断开或停读的订阅者。只有多个有效接收方才需要共享正文。
    prune_disconnected_subscribers(entry);
    if (event_frame) {
        const bool broadcast = entry.subscribers.size() > 1;
        std::erase_if(entry.subscribers, [&](const auto &subscriber) {
            // 同一 I/O 线程内没有其他回调插入，刚校验过的节点在轮到它时一定存活。
            auto *conn = connections_.find(subscriber.first, subscriber.second);
            return !connections_.send_async_frame(*conn, *event_frame, broadcast);
        });
    }
    if (entry.subscribers.empty()) {
        remove_watch(wd, true);
        return;
    }

    entry.refreshing = false;
    if (entry.dirty)
        schedule_refresh(wd);
}

}

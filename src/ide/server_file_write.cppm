module;
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

export module ide.server:write;

import std;
import service.file;
import service.file_watcher;
import service.workspace_cache;
import ide.server_protocol;

export namespace server {

// 处理 0x0204 (workspace_write_file)：
// 带版本校验与临时文件原子替换的文件保存流程：
//   1. 获取该文件路径的路径锁（防止多个 Worker 并发写同一文件）。
//   2. 读取原文件当前 revision，与客户端期望的 revision 比对：
//      - 若不同：返回 Status::conflict 及当前最新的 [u64 currentRevision]。
//   3. 若相同：
//      创建同目录临时文件 -> 写入全部内容并 fsync -> 再次检查原文件 revision ->
//      确认未变化后 rename 原子替换原文件 -> 取得新文件 revision -> 释放锁并返回 OK + [u64
//      newRevision]。
void handle_workspace_write_file(Frame &frame) {
    auto req = decode_workspace_write_file_view(frame.payload);
    if (!req) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_write_file payload";
        return;
    }

    auto full_path = service::file::resolve_workspace_path(req->path);
    if (!full_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace file path";
        return;
    }

    // 1. 先锁住文件（路径锁）。
    std::lock_guard lock(service::file::path_mutex_for(*full_path));

    // 2. 获取原文件当前 revision 并与客户端期望值比对。
    struct stat st_before{};
    if (::lstat(full_path->c_str(), &st_before) != 0) {
        service::file::set_errno_error(frame, errno, "failed to stat target file");
        return;
    }
    if (!S_ISREG(st_before.st_mode)) {
        frame.status = Status::invalid_argument;
        frame.payload = "target is not a regular file";
        return;
    }

    const auto current_rev = service::file::file_revision(st_before);
    if (current_rev != req->expected_revision) {
        frame.status = Status::conflict;
        encode_workspace_write_file_into(current_rev, frame.payload);
        return;
    }

    // 3. 在同一目录下创建临时文件（保证后续 rename 在同一挂载点内原子完成）。
    std::string tmp_path = *full_path + ".tmp.XXXXXX";
    service::file::ScopedFd tmp_fd(::mkostemp(tmp_path.data(), O_CLOEXEC));
    if (!tmp_fd.valid()) {
        service::file::set_errno_error(frame, errno, "failed to create temporary file");
        return;
    }

    // 保持与原文件一致的权限位。
    (void)::fchmod(tmp_fd.get(), st_before.st_mode & 0777);

    // 4. 写入全部内容并刷盘。
    if (!service::file::write_all(tmp_fd.get(), req->content) || ::fsync(tmp_fd.get()) != 0) {
        const int err = errno;
        tmp_fd.close_now();
        (void)::unlink(tmp_path.c_str());
        service::file::set_errno_error(frame, err, "failed to write temporary file");
        return;
    }
    // rename 会更新 ctime。保留已 fsync 数据的 inode、长度和 mtime，避免外部
    // 在 rename 后改写/替换路径时，把请求中的旧正文配上另一个文件的新 revision。
    auto &cache = service::global_workspace_cache();
    struct stat written{};
    const bool can_fill = req->content.size() <= cache.max_cacheable_file_bytes() &&
                          ::fstat(tmp_fd.get(), &written) == 0;
    tmp_fd.close_now();

    // 5. 再次检查原文件 revision（防止写临时文件期间外部进程修改了原文件）。
    struct stat st_recheck{};
    if (::lstat(full_path->c_str(), &st_recheck) != 0) {
        const int err = errno;
        (void)::unlink(tmp_path.c_str());
        service::file::set_errno_error(frame, err, "failed to re-stat target file");
        return;
    }

    const auto recheck_rev = service::file::file_revision(st_recheck);
    if (recheck_rev != req->expected_revision) {
        (void)::unlink(tmp_path.c_str());
        frame.status = Status::conflict;
        encode_workspace_write_file_into(recheck_rev, frame.payload);
        return;
    }

    // 6. 确认未变化后执行 rename，随即撤销旧 inode 监听、使旧版本和在途回填失效。
    auto &file_watcher = service::global_file_watcher();
    if (::rename(tmp_path.c_str(), full_path->c_str()) != 0) {
        const int err = errno;
        (void)::unlink(tmp_path.c_str());
        service::file::set_errno_error(frame, err, "failed to rename temporary file");
        return;
    }

    file_watcher.unwatch_file(req->path);
    const auto epoch = cache.invalidate_file(req->path);
    // 7. 取得替换后的新文件 revision，并为新 inode 注册监听后同步刷新缓存。
    struct stat st_new{};
    service::file::ScopedFd installed(
        can_fill ? ::open(full_path->c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC) : -1);
    const int stat_result = installed.valid() ? ::fstat(installed.get(), &st_new)
                                              : ::lstat(full_path->c_str(), &st_new);
    if (stat_result != 0) {
        service::file::set_errno_error(frame, errno, "failed to stat replaced file");
        return;
    }

    const auto new_rev = service::file::file_revision(st_new);
    if (installed.valid() && st_new.st_dev == written.st_dev && st_new.st_ino == written.st_ino &&
        st_new.st_size == written.st_size && st_new.st_mtim.tv_sec == written.st_mtim.tv_sec &&
        st_new.st_mtim.tv_nsec == written.st_mtim.tv_nsec) {
        // rename 已成功；容量不足、epoch 过期或缓存分配失败都不改变写盘结果。
        try {
            auto cached = std::make_shared<ReadFileResponse>(
                ReadFileResponse{.file_size = static_cast<std::uint64_t>(st_new.st_size),
                                 .revision = new_rev,
                                 .content = std::string(req->content)});
            file_watcher.cache_file_if_current(req->path, *full_path, installed.get(),
                                               std::move(cached), epoch);
        } catch (const std::bad_alloc &) {
            // 仅回填失败，磁盘已完成原子替换。
        }
    }

    // 8. 函数退出自动释放路径锁，返回 OK 及 [u64 newRevision]。
    frame.status = Status::ok;
    encode_workspace_write_file_into(new_rev, frame.payload);
}

} // namespace server

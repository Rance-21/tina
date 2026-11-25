module;
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

namespace {

// 在同目录下通过 openat 独占创建 <rel_path>.tmp.XXXXXX 临时文件，栈上生成相对路径，零堆分配。
service::file::ScopedFd create_temp_sibling(int root_fd, std::string_view rel_path,
                                            char (&tmp_path)[PATH_MAX]) noexcept {
    constexpr std::string_view mid = ".tmp.";
    constexpr std::size_t suffix_len = 6;
    if (rel_path.size() + mid.size() + suffix_len >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return {};
    }

    std::memcpy(tmp_path, rel_path.data(), rel_path.size());
    std::memcpy(tmp_path + rel_path.size(), mid.data(), mid.size());
    char *digits = tmp_path + rel_path.size() + mid.size();
    digits[suffix_len] = '\0';

    static constexpr char kChars[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    thread_local std::uint64_t state = [] {
        auto s = static_cast<std::uint64_t>(
                     std::chrono::steady_clock::now().time_since_epoch().count()) ^
                 0x9E3779B97F4A7C15ULL;
        return s != 0 ? s : 1ULL;
    }();

    for (int attempt = 0; attempt < 32; ++attempt) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        std::uint64_t v = state * 2685821657736338717ULL;
        for (std::size_t i = 0; i < suffix_len; ++i) {
            digits[i] = kChars[v % 62];
            v /= 62;
        }
        const int fd = ::openat(root_fd, tmp_path,
                                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0)
            return service::file::ScopedFd(fd);
        if (errno != EEXIST)
            return {};
    }
    errno = EEXIST;
    return {};
}

} // namespace

export namespace server {

// 处理 0x0204 (workspace_write_file)：
// 带版本校验与临时文件原子替换的文件保存流程（全程基于工作区根目录 FD 与 *at 系统调用）。
void handle_workspace_write_file(Frame &frame) {
    auto req = decode_workspace_write_file_view(frame.payload);
    if (!req) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_write_file payload";
        return;
    }

    service::file::RelPath rel(req->path);
    if (!rel) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace file path";
        return;
    }

    const int root_fd = service::file::workspace_root_fd();
    if (root_fd < 0) {
        const char *home = ::getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            frame.status = Status::invalid_argument;
            frame.payload = "invalid workspace file path";
        } else {
            service::file::set_errno_error(frame, ENOENT, "failed to stat target file");
        }
        return;
    }

    const std::string_view rel_path = rel.view();

    // 1. 先锁住文件（按规范化工作区相对路径分片加锁）。
    std::lock_guard lock(service::file::path_mutex_for(rel_path));

    // 2. 获取原文件当前 revision 并与客户端期望值比对。
    struct stat st_before{};
    if (::fstatat(root_fd, rel.c_str(), &st_before, AT_SYMLINK_NOFOLLOW) != 0) {
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

    // 3. 在同一目录下创建临时文件（保证后续 renameat 在同一挂载点内原子完成）。
    char tmp_path[PATH_MAX];
    service::file::ScopedFd tmp_fd = create_temp_sibling(root_fd, rel_path, tmp_path);
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
        (void)::unlinkat(root_fd, tmp_path, 0);
        service::file::set_errno_error(frame, err, "failed to write temporary file");
        return;
    }
    // renameat 会更新 ctime。保留已 fsync 数据的 inode、长度和 mtime，避免外部
    // 在 renameat 后改写/替换路径时，把请求中的旧正文配上另一个文件的新 revision。
    auto &cache = service::global_workspace_cache();
    struct stat written{};
    const bool can_fill = req->content.size() <= cache.max_cacheable_file_bytes() &&
                          ::fstat(tmp_fd.get(), &written) == 0;
    tmp_fd.close_now();

    // 5. 再次检查原文件 revision（防止写临时文件期间外部进程修改了原文件）。
    struct stat st_recheck{};
    if (::fstatat(root_fd, rel.c_str(), &st_recheck, AT_SYMLINK_NOFOLLOW) != 0) {
        const int err = errno;
        (void)::unlinkat(root_fd, tmp_path, 0);
        service::file::set_errno_error(frame, err, "failed to re-stat target file");
        return;
    }

    const auto recheck_rev = service::file::file_revision(st_recheck);
    if (recheck_rev != req->expected_revision) {
        (void)::unlinkat(root_fd, tmp_path, 0);
        frame.status = Status::conflict;
        encode_workspace_write_file_into(recheck_rev, frame.payload);
        return;
    }

    // 6. 确认未变化后执行 renameat，随即撤销旧 inode 监听、使旧版本和在途回填失效。
    auto &file_watcher = service::global_file_watcher();
    if (::renameat(root_fd, tmp_path, root_fd, rel.c_str()) != 0) {
        const int err = errno;
        (void)::unlinkat(root_fd, tmp_path, 0);
        service::file::set_errno_error(frame, err, "failed to rename temporary file");
        return;
    }

    file_watcher.unwatch_file(rel_path);
    const auto epoch = cache.invalidate_file(rel_path);
    // 7. 取得替换后的新文件 revision，并为新 inode 注册监听后同步刷新缓存。
    struct stat st_new{};
    service::file::ScopedFd installed(
        can_fill ? ::openat(root_fd, rel.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC) : -1);
    const int stat_result = installed.valid()
                                ? ::fstat(installed.get(), &st_new)
                                : ::fstatat(root_fd, rel.c_str(), &st_new, AT_SYMLINK_NOFOLLOW);
    if (stat_result != 0) {
        service::file::set_errno_error(frame, errno, "failed to stat replaced file");
        return;
    }

    const auto new_rev = service::file::file_revision(st_new);
    if (installed.valid() && st_new.st_dev == written.st_dev && st_new.st_ino == written.st_ino &&
        st_new.st_size == written.st_size && st_new.st_mtim.tv_sec == written.st_mtim.tv_sec &&
        st_new.st_mtim.tv_nsec == written.st_mtim.tv_nsec) {
        // renameat 已成功；容量不足、epoch 过期或缓存分配失败都不改变写盘结果。
        try {
            auto cached = std::make_shared<ReadFileResponse>(
                ReadFileResponse{.file_size = static_cast<std::uint64_t>(st_new.st_size),
                                 .revision = new_rev,
                                 .content = std::string(req->content)});
            file_watcher.cache_file_if_current(rel_path, root_fd, rel.c_str(), installed.get(),
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

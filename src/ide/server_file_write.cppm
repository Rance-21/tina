module;
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

export module ide.server:write;

import std;
import service.file;
import service.file_watcher;
import service.workspace_cache;
import ide.server_protocol;

namespace {

// 在同目录下通过 openat 独占创建 <rel_path>.tmp.<tag> 临时文件（使用独立的线程局部缓冲）。
const char *create_temp_sibling(int root_fd, std::string_view rel_path,
                                service::file::ScopedFd &out_fd) noexcept {
    constexpr std::string_view mid = ".tmp.";
    if (rel_path.size() + mid.size() + 32 >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return nullptr;
    }

    thread_local char tmp_path[PATH_MAX];
    thread_local std::uint64_t seq = 0;
    std::memcpy(tmp_path, rel_path.data(), rel_path.size());
    std::memcpy(tmp_path + rel_path.size(), mid.data(), mid.size());
    char *suffix = tmp_path + rel_path.size() + mid.size();

    const auto tid_tag = reinterpret_cast<std::uintptr_t>(&seq) >> 4;
    for (int i = 0; i < 16; ++i) {
        auto [p1, _1] = std::to_chars(suffix, tmp_path + PATH_MAX - 1, tid_tag, 36);
        *p1++ = '_';
        auto [p2, _2] = std::to_chars(p1, tmp_path + PATH_MAX - 1, ++seq, 36);
        *p2 = '\0';
        const int fd = ::openat(root_fd, tmp_path,
                                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            out_fd = service::file::ScopedFd(fd);
            return tmp_path;
        }
        if (errno != EEXIST)
            return nullptr;
    }
    errno = EEXIST;
    return nullptr;
}

} // namespace

export namespace server {

void handle_workspace_write_file(Frame &frame) {
    auto req = decode_workspace_write_file_view(frame.payload);
    if (!req) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_write_file payload";
        return;
    }

    const char *c_path = service::file::c_rel_path(req->path);
    if (!c_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace file path";
        return;
    }
    const int root_fd = service::file::workspace_root_fd();
    if (root_fd < 0) {
        service::file::set_errno_error(frame, ENOENT, "failed to stat target file");
        return;
    }

    const std::string_view rel_path(c_path, req->path.size());
    std::lock_guard lock(service::file::path_mutex_for(rel_path));

    struct stat st_before{};
    if (::fstatat(root_fd, c_path, &st_before, AT_SYMLINK_NOFOLLOW) != 0) {
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

    service::file::ScopedFd tmp_fd;
    const char *tmp_path = create_temp_sibling(root_fd, rel_path, tmp_fd);
    if (!tmp_path) {
        service::file::set_errno_error(frame, errno, "failed to create temporary file");
        return;
    }

    (void)::fchmod(tmp_fd.get(), st_before.st_mode & 0777);

    if (!service::file::write_all(tmp_fd.get(), req->content) || ::fsync(tmp_fd.get()) != 0) {
        const int err = errno;
        tmp_fd.close_now();
        (void)::unlinkat(root_fd, tmp_path, 0);
        service::file::set_errno_error(frame, err, "failed to write temporary file");
        return;
    }

    auto &cache = service::global_workspace_cache();
    struct stat written{};
    const bool can_fill = req->content.size() <= cache.max_cacheable_file_bytes() &&
                          ::fstat(tmp_fd.get(), &written) == 0;
    tmp_fd.close_now();

    struct stat st_recheck{};
    if (::fstatat(root_fd, c_path, &st_recheck, AT_SYMLINK_NOFOLLOW) != 0) {
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

    auto &file_watcher = service::global_file_watcher();
    if (::renameat(root_fd, tmp_path, root_fd, c_path) != 0) {
        const int err = errno;
        (void)::unlinkat(root_fd, tmp_path, 0);
        service::file::set_errno_error(frame, err, "failed to rename temporary file");
        return;
    }

    file_watcher.unwatch_file(rel_path);
    const auto epoch = cache.invalidate_file(rel_path);

    struct stat st_new{};
    service::file::ScopedFd installed(
        can_fill ? ::openat(root_fd, c_path, O_PATH | O_NOFOLLOW | O_CLOEXEC) : -1);
    const int stat_result = installed.valid()
                                ? ::fstat(installed.get(), &st_new)
                                : ::fstatat(root_fd, c_path, &st_new, AT_SYMLINK_NOFOLLOW);
    if (stat_result != 0) {
        service::file::set_errno_error(frame, errno, "failed to stat replaced file");
        return;
    }

    const auto new_rev = service::file::file_revision(st_new);
    if (installed.valid() && st_new.st_dev == written.st_dev && st_new.st_ino == written.st_ino &&
        st_new.st_size == written.st_size && st_new.st_mtim.tv_sec == written.st_mtim.tv_sec &&
        st_new.st_mtim.tv_nsec == written.st_mtim.tv_nsec) {
        try {
            auto cached = std::make_shared<ReadFileResponse>(
                ReadFileResponse{.file_size = static_cast<std::uint64_t>(st_new.st_size),
                                 .revision = new_rev,
                                 .content = std::string(req->content)});
            file_watcher.cache_file_if_current(rel_path, root_fd, c_path, installed.get(),
                                               std::move(cached), epoch);
        } catch (const std::bad_alloc &) {
        }
    }

    frame.status = Status::ok;
    encode_workspace_write_file_into(new_rev, frame.payload);
}

} // namespace server

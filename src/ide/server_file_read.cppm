module;
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module ide.server:read;

import std;
import service.file;
import service.file_watcher;
import service.workspace_cache;
import ide.server_protocol;

export namespace server {

namespace detail {

// 从已缓存的完整文件响应中按 [offset, max_bytes] 零拷贝切片出响应内容。
inline std::string_view slice_file_content(const ReadFileResponse &cached, std::uint64_t offset,
                                           std::uint32_t max_bytes) noexcept {
    if (offset >= cached.file_size || offset >= cached.content.size())
        return {};

    const auto available = std::min<std::uint64_t>(
        cached.file_size - offset, cached.content.size() - static_cast<std::size_t>(offset));
    const auto to_read = static_cast<std::size_t>(std::min<std::uint64_t>(available, max_bytes));
    return std::string_view(cached.content).substr(static_cast<std::size_t>(offset), to_read);
}

// 从已打开的 fd 指定偏移读取最多 to_read 字节并写入 out + out_offset 处（处理 EINTR 与短读）。
// 调用方可通过 out_offset 预留协议头字节，避免读取后再二次拷贝拼接。
inline bool pread_range_at(int fd, std::uint64_t offset, std::size_t to_read, std::string &out,
                           std::size_t out_offset, int &out_errno) {
    out.resize(out_offset + to_read);
    std::size_t total_read = 0;
    while (total_read < to_read) {
        ssize_t n = ::pread(fd, out.data() + out_offset + total_read, to_read - total_read,
                            static_cast<off_t>(offset + total_read));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            out_errno = errno;
            return false;
        }
        if (n == 0)
            break; // 遇到文件末尾（例如读取途中文件被截断）
        total_read += static_cast<std::size_t>(n);
    }
    out.resize(out_offset + total_read);
    return true;
}

} // namespace detail

// 处理 0x0203 (workspace_read_file)：
// 按 [offset, maxBytes] 分段读取 UTF-8 文件内容。
// 优先命中内存缓存（ReadFileResponse），命中时直接切片编码返回，不进入 Linux 文件系统；
// 未命中时采用乐观并发控制（pread 前后 fstat 校验 revision），文件不超过缓存上限时整文件写入缓存。
void handle_workspace_read_file(Frame &frame) {
    auto req = decode_workspace_read_file_view(frame.payload);
    if (!req) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_read_file payload";
        return;
    }

    const std::string_view rel_path = req->path;
    const std::uint64_t offset = req->offset;
    const std::uint32_t max_bytes = req->max_bytes;

    if (!service::file::is_valid_workspace_rel_path(rel_path) ||
        !service::file::has_projects_root_env()) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace file path";
        return;
    }

    auto &cache = service::global_workspace_cache();
    if (auto cached = cache.get_file(rel_path)) {
        const auto slice = detail::slice_file_content(*cached, offset, max_bytes);
        frame.status = Status::ok;
        encode_workspace_read_file(frame, std::move(cached), slice);
        return;
    }

    auto full_path = service::file::resolve_workspace_path(rel_path);
    if (!full_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace file path";
        return;
    }

    service::file::ScopedFd fd(::open(full_path->c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) {
        service::file::set_errno_error(frame, errno, "failed to open file for reading");
        return;
    }

    auto &file_watcher = service::global_file_watcher();
    bool watched = false;

    // 每次读取前后比对 revision，若读取期间文件恰好被并发修改则重试。
    while (true) {
        struct stat st_before{};
        if (::fstat(fd.get(), &st_before) != 0) {
            service::file::set_errno_error(frame, errno, "failed to stat file before read");
            return;
        }
        if (!S_ISREG(st_before.st_mode)) {
            frame.status = Status::invalid_argument;
            frame.payload = "target is not a regular file";
            return;
        }

        const auto file_size = static_cast<std::uint64_t>(st_before.st_size);
        const auto rev_before = service::file::file_revision(st_before);
        const bool cacheable = file_size <= cache.max_cacheable_file_bytes();

        // 若目标可缓存，先向 FileWatcher 注册监听并采样版本号，保证读盘期间的并发修改可被感知。
        std::uint64_t epoch = 0;
        if (cacheable) {
            if (!watched) {
                file_watcher.watch_file(rel_path, *full_path);
                watched = true;
            }
            epoch = cache.file_epoch(rel_path);
        }

        std::string content;
        if (cacheable) {
            int err = 0;
            if (file_size > 0 &&
                !detail::pread_range_at(fd.get(), 0, static_cast<std::size_t>(file_size), content,
                                        0, err)) {
                service::file::set_errno_error(frame, err, "failed to pread file");
                return;
            }
        } else {
            // 超大文件不进缓存：头部预留 16 字节直读内容，校验通过后原地回填头并移动给 frame.payload。
            const std::size_t to_read =
                (offset < file_size) ? static_cast<std::size_t>(
                                           std::min<std::uint64_t>(file_size - offset, max_bytes))
                                     : 0;
            int err = 0;
            if (!detail::pread_range_at(fd.get(), offset, to_read, content, 16, err)) {
                service::file::set_errno_error(frame, err, "failed to pread file");
                return;
            }
        }

        struct stat st_after{};
        if (::fstat(fd.get(), &st_after) != 0) {
            service::file::set_errno_error(frame, errno, "failed to stat file after read");
            return;
        }

        const auto rev_after = service::file::file_revision(st_after);
        if (rev_before == rev_after) {
            if (cacheable) {
                auto cached = std::make_shared<ReadFileResponse>(ReadFileResponse{
                    .file_size = file_size,
                    .revision = rev_after,
                    .content = std::move(content),
                });
                cache.put_file(rel_path, cached, epoch);

                const auto slice = detail::slice_file_content(*cached, offset, max_bytes);
                frame.status = Status::ok;
                encode_workspace_read_file(frame, std::move(cached), slice);
                return;
            }

            write_be(content.data(), file_size);
            write_be(content.data() + 8, rev_after);
            frame.status = Status::ok;
            frame.payload = std::move(content);
            return;
        }
    }
}

}

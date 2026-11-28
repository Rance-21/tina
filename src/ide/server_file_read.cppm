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

// 从已缓存的完整文件响应中按 [offset, max_bytes] 零拷贝切片出响应内容。
inline std::string_view slice_file_content(const ReadFileResponse &cached, std::uint64_t offset,
                                           std::uint32_t max_bytes) noexcept {
    // 元数据和实际缓存字节数共同限制可读范围，offset 超过任一末尾即为 EOF。
    const auto end = std::min<std::uint64_t>(cached.file_size, cached.content.size());
    if (offset >= end)
        return {};
    return {cached.content.data() + offset, std::min<std::uint64_t>(end - offset, max_bytes)};
}

// 处理 0x0203 (workspace_read_file)：
// 按 [offset, maxBytes] 分段读取 UTF-8 文件内容。
// 优先命中内存缓存（ReadFileResponse），命中时直接切片编码返回，不进入 Linux 文件系统；
// 未命中时采用乐观并发控制（pread 前后校验 revision），文件不超过缓存上限时整文件写入缓存。
void handle_workspace_read_file(Frame &frame) {
    auto req = decode_workspace_read_file_view(frame.payload);
    if (!req) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_read_file payload";
        return;
    }

    const std::uint64_t offset = req->offset;
    const std::uint32_t max_bytes = req->max_bytes;

    // 非缓存读会改写请求 payload；后续统一借用线程局部缓冲中的 c_path / rel_path，重试仍然有效。
    const char *c_path = service::file::c_rel_path(req->path);
    if (!c_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace file path";
        return;
    }
    const int root_fd = service::file::workspace_root_fd();
    if (root_fd < 0) {
        service::file::set_errno_error(frame, ENOENT, "failed to open file for reading");
        return;
    }

    const std::string_view rel_path(c_path, req->path.size());
    auto &cache = service::global_workspace_cache();
    std::uint64_t epoch;
    bool admit = true;
    if (auto cached = cache.get_file(rel_path, &epoch, &admit)) {
        const auto slice = slice_file_content(*cached, offset, max_bytes);
        frame.kind = Kind::response;
        frame.status = Status::ok;
        encode_workspace_read_file(frame, std::move(cached), slice);
        return;
    }

    service::file::ScopedFd fd(::openat(root_fd, c_path, O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) {
        service::file::set_errno_error(frame, errno, "failed to open file for reading");
        return;
    }

    auto &file_watcher = service::global_file_watcher();
    std::shared_ptr<ReadFileResponse> complete;
    bool try_full_file = true;

    // revision 变化时重试，沿用已有正文空间。这里仅准备读盘结果，尚未占用缓存槽位。
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
        bool cacheable = try_full_file && admit && file_size <= cache.max_cacheable_file_bytes();
        if (cacheable && !complete) {
            try {
                complete = std::make_shared<ReadFileResponse>();
            } catch (const std::bad_alloc &) {
                cacheable = false;
            }
        }
        auto &content = cacheable ? complete->content : frame.payload;
        if (!cacheable)
            content.clear();

        // 可缓存文件从头读；超限文件只读请求范围，并直接写进最终响应的 16 字节头之后。
        // resize_and_overwrite 只分配空间，避免先将整段内存清零再由 pread 覆盖。
        const auto start = cacheable ? 0 : std::min(offset, file_size);
        const auto length = static_cast<std::size_t>(
            cacheable ? file_size : std::min<std::uint64_t>(max_bytes, file_size - start));
        const std::size_t header = cacheable ? 0 : 16;
        int read_error = 0;
        try {
            content.resize_and_overwrite(header + length, [&](char *data, std::size_t) noexcept {
                if (!cacheable) {
                    write_be(data, file_size);
                    write_be(data + 8, rev_before);
                }

                std::size_t done = 0;
                while (done < length) {
                    const auto n = ::pread(fd.get(), data + header + done, length - done,
                                           static_cast<off_t>(start + done));
                    if (n > 0)
                        done += static_cast<std::size_t>(n);
                    else if (n == 0)
                        break; // 外部截短文件时可能提前遇到 EOF，后续仍要复核 revision。
                    else if (errno != EINTR) {
                        read_error = errno;
                        break;
                    }
                }
                return header + done; // 只发布已经写过的字节。
            });
        } catch (const std::bad_alloc &) {
            if (!cacheable)
                throw;
            // 整文件缓冲区是回填开销，只为当前请求分配较小切片再试一次。
            complete.reset();
            try_full_file = false;
            continue;
        }
        if (read_error) {
            service::file::set_errno_error(frame, read_error, "failed to pread file");
            return;
        }

        if (cacheable) {
            complete->file_size = file_size;
            complete->revision = rev_before;
            // add_watch 后按路径复核了同一 inode 的 revision，已覆盖 pread 的整个区间。
            // 正常回填只做一次读后检查；随后文件再被修改，由 inotify 使缓存失效。
            if (auto pin = file_watcher.cache_file_if_current(rel_path, root_fd, c_path,
                                                              fd.get(), complete, epoch)) {
                const auto slice = slice_file_content(*pin, offset, max_bytes);
                frame.status = Status::ok;
                encode_workspace_read_file(frame, std::move(pin), slice);
                return;
            }
        }

        // 没有发布缓存时，响应仍按这次 open 到的 fd 校验。路径即使已被 rename，
        // 也可以返回旧 fd 上的一致快照；只有这个 inode 在读取中变化才重试。
        struct stat st_after{};
        if (::fstat(fd.get(), &st_after) != 0) {
            service::file::set_errno_error(frame, errno, "failed to stat file after read");
            return;
        }
        if (rev_before != service::file::file_revision(st_after))
            continue;

        frame.status = Status::ok;
        if (cacheable) {
            const auto slice = slice_file_content(*complete, offset, max_bytes);
            encode_workspace_read_file(frame, std::move(complete), slice);
        } else {
            frame.clear_external_payload();
        }
        return;
    }
}

} // namespace server

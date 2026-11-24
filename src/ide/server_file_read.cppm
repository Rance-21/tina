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
    if (offset >= cached.file_size || offset >= cached.content.size())
        return {};

    const auto available = std::min<std::uint64_t>(
        cached.file_size - offset, cached.content.size() - static_cast<std::size_t>(offset));
    const auto to_read = static_cast<std::size_t>(std::min<std::uint64_t>(available, max_bytes));
    return std::string_view(cached.content).substr(static_cast<std::size_t>(offset), to_read);
}

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

    // I/O 线程已处理热命中。到 Worker 后一次完成路径校验和拼接；
    // 即使别的 Worker 刚刚回填了缓存，下面的复查仍能省掉 open / pread。
    auto full_path = service::file::resolve_workspace_path(rel_path);
    if (!full_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace file path";
        return;
    }

    auto &cache = service::global_workspace_cache();
    if (auto cached = cache.get_file(rel_path)) {
        const auto slice = slice_file_content(*cached, offset, max_bytes);
        frame.kind = Kind::response;
        frame.status = Status::ok;
        encode_workspace_read_file(frame, std::move(cached), slice);
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
    std::string content; // revision 变化时重试，可沿用上一轮的缓冲区。
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

        // 可缓存文件从头读；超限文件只读请求范围，并直接写进最终响应的 16 字节头之后。
        // resize_and_overwrite 只分配空间，避免先将整段内存清零再由 pread 覆盖。
        const auto start = cacheable ? 0 : std::min(offset, file_size);
        const auto length = static_cast<std::size_t>(
            cacheable ? file_size : std::min<std::uint64_t>(max_bytes, file_size - start));
        const std::size_t header = cacheable ? 0 : 16;
        int read_error = 0;
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
        if (read_error) {
            service::file::set_errno_error(frame, read_error, "failed to pread file");
            return;
        }

        struct stat st_after{};
        if (::fstat(fd.get(), &st_after) != 0) {
            service::file::set_errno_error(frame, errno, "failed to stat file after read");
            return;
        }

        const auto rev_after = service::file::file_revision(st_after);
        if (rev_before != rev_after)
            continue;

        frame.status = Status::ok;
        if (cacheable) {
            auto cached = std::make_shared<ReadFileResponse>(ReadFileResponse{
                .file_size = file_size, .revision = rev_after, .content = std::move(content)});
            cache.put_file(rel_path, cached, epoch);
            const auto slice = slice_file_content(*cached, offset, max_bytes);
            encode_workspace_read_file(frame, std::move(cached), slice);
        } else {
            frame.payload = std::move(content);
        }
        return;
    }
}

}

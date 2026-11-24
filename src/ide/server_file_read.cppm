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

// 从 WorkspaceCache 文件缓存中尝试命中并直接编码响应（与 Worker 慢路径共用同一份缓存切片逻辑）：
//   - 先通过 get_file() 获取不可变缓存条目（shared_ptr<const ReadFileResponse>），并切片出本次请求范围；
//   - 若切片大小超出 max_slice_bytes（例如 I/O 线程快路径设置的 32 KiB 阈值），则放弃快路径返回 false，
//     交由 WorkerPool 处理，避免大响应写回及发送队列背压阻塞同一 I/O 线程上的其他连接；
//   - 命中且在阈值内时：直接原地改写 frame 为响应帧，仅编码 16B 元数据头进 frame.payload，
//     大块文件内容通过 frame.payload_owner + external_payload 零拷贝挂载后返回 true。
inline bool respond_from_file_cache(Frame &frame, std::string_view rel_path, std::uint64_t offset,
                                    std::uint32_t max_bytes, std::size_t max_slice_bytes) {
    if (auto cached = service::global_workspace_cache().get_file(rel_path)) {
        const auto slice = slice_file_content(*cached, offset, max_bytes);
        if (slice.size() > max_slice_bytes)
            return false;

        frame.kind = Kind::response;
        frame.status = Status::ok;
        encode_workspace_read_file(frame, std::move(cached), slice);
        return true;
    }
    return false;
}

} // namespace detail

// I/O 线程快路径专用入口（针对 0x0203 workspace_read_file）：
// 在把请求提交给 WorkerPool 之前先试探一次内存缓存，仅在同时满足以下条件时由当前 I/O 线程直接消化：
//   1. 请求格式与工作区相对路径合法（非法请求仍交由 Worker 统一返回错误帧，保持错误路径简单）；
//   2. 目标文件已命中 WorkspaceCache（完全无需 open/fstat/pread 等磁盘系统调用）；
//   3. 切片大小 <= max_slice_bytes（阈值以内由 I/O 线程直接入队 ::writev()，省去两次线程切换与 eventfd 唤醒）。
// 注意：返回 false 时绝不修改 frame 的内容与状态，保证后续仍可原封不动投递给 WorkerPool。
bool try_handle_cached_read_file(Frame &frame, std::size_t max_slice_bytes) {
    auto req = decode_workspace_read_file_view(frame.payload);
    if (!req || !service::file::is_valid_workspace_rel_path(req->path) ||
        !service::file::has_projects_root_env()) {
        return false;
    }
    return detail::respond_from_file_cache(frame, req->path, req->offset, req->max_bytes,
                                           max_slice_bytes);
}

// 缓存探测与分流结果：单次查询同时服务于 I/O 线程内联（<= 32 KiB）与 Normal Worker 大缓存搬运（> 32 KiB）。
struct CachedReadProbe {
    enum class Outcome : std::uint8_t {
        inline_handled, // 命中缓存且切片 <= 阈值，已原地编码为响应帧
        normal_cached,  // 命中缓存但切片 > 阈值，已持有只读快照，交由 Normal Worker 准备响应
        blocking_miss,  // 缓存未命中，需交由 Blocking Worker 走磁盘读盘路径
        invalid_request // 请求参数或相对路径非法，交由 Normal Worker 快速返回参数错误（不碰磁盘）
    };

    Outcome outcome{Outcome::blocking_miss};
    std::shared_ptr<const ReadFileResponse> cached;
    std::size_t slice_offset{0};
    std::size_t slice_size{0};
};

// 【作用】在 I/O 线程对 0x0203 (workspace_read_file) 做一次轻量缓存探测并决定分流去向。
// 【原理】
//   1. 先零拷贝解码并校验相对路径：非法直接返回 invalid_request（不改 frame，交 Normal 回错）；
//   2. 查内存缓存未命中：返回 blocking_miss 交 Blocking Worker 读盘；
//   3. 命中缓存且切片大小 <= max_inline_bytes（32 KiB）：原地把 frame 编码为响应并挂载只读快照，返回 inline_handled；
//   4. 命中缓存但切片 > 32 KiB：不改 frame，直接把只读 shared_ptr 快照和切片偏移装入结果返回 normal_cached，
//      使 Normal Worker 不必二次加锁查表，也不会因为中途 LRU 淘汰而被迫在 Normal 线程读盘。
CachedReadProbe probe_cached_read_file(Frame &frame, std::size_t max_inline_bytes) {
    CachedReadProbe res;
    auto req = decode_workspace_read_file_view(frame.payload);
    if (!req || !service::file::is_valid_workspace_rel_path(req->path) ||
        !service::file::has_projects_root_env()) {
        res.outcome = CachedReadProbe::Outcome::invalid_request;
        return res;
    }

    auto cached = service::global_workspace_cache().get_file(req->path);
    if (!cached) {
        res.outcome = CachedReadProbe::Outcome::blocking_miss;
        return res;
    }

    const auto slice = detail::slice_file_content(*cached, req->offset, req->max_bytes);
    if (max_inline_bytes > 0 && slice.size() <= max_inline_bytes) {
        frame.kind = Kind::response;
        frame.status = Status::ok;
        encode_workspace_read_file(frame, std::move(cached), slice);
        res.outcome = CachedReadProbe::Outcome::inline_handled;
        return res;
    }

    const std::size_t slice_offset =
        slice.empty() ? 0 : static_cast<std::size_t>(slice.data() - cached->content.data());
    res.outcome = CachedReadProbe::Outcome::normal_cached;
    res.cached = std::move(cached);
    res.slice_offset = slice_offset;
    res.slice_size = slice.size();
    return res;
}

// 【作用】由 Normal Worker 使用提交阶段捕获的只读文件缓存快照完成 > 32 KiB 响应帧组装。
// 【原理】直接按 [slice_offset, slice_size] 从不可变 cached->content 切出 string_view，
//         写入 16 字节元数据头并将 cached 挂入 frame.payload_owner 保活，全程零拷贝、零磁盘 I/O。
void finish_cached_read_file(Frame &frame, std::shared_ptr<const ReadFileResponse> cached,
                             std::size_t slice_offset, std::size_t slice_size) {
    const std::string_view slice =
        (slice_size == 0)
            ? std::string_view{}
            : std::string_view(cached->content).substr(slice_offset, slice_size);
    frame.status = Status::ok;
    encode_workspace_read_file(frame, std::move(cached), slice);
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

    if (!service::file::is_valid_workspace_rel_path(rel_path) ||
        !service::file::has_projects_root_env()) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace file path";
        return;
    }

    auto &cache = service::global_workspace_cache();
    if (detail::respond_from_file_cache(frame, rel_path, offset, max_bytes,
                                        std::numeric_limits<std::size_t>::max())) {
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

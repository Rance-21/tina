module;
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

export module ide.server;

import std;
import service.file;
export import ide.server_protocol;

export namespace server {

using service::file::file_revision;
using service::file::path_mutex_for;
using service::file::projects_root_dir;
using service::file::read_directory_entries;
using service::file::resolve_workspace_path;
using service::file::ScopedDir;
using service::file::ScopedFd;
using service::file::set_errno_error;
using service::file::write_all;

// 解析客户端相对目录路径为服务端绝对路径（供 IO 线程注册 inotify 使用）。
std::optional<std::string> resolve_workspace_dir_path(std::string_view rel_path) {
    return resolve_workspace_path(rel_path);
}

// 当 inotify 监听到目录变化时，由 Worker 线程重新扫描目录并构造 Kind::event 帧。
// 事件负载格式与 workspace_list_dir 响应完全一致：[u16 dirPathLen][dirPath bytes][entries...]。
std::optional<Frame> build_workspace_list_dir_event(std::string_view rel_path) {
    auto target_path = resolve_workspace_path(rel_path);
    if (!target_path)
        return std::nullopt;

    std::vector<DirEntry> entries;
    if (read_directory_entries(*target_path, entries) != 0)
        return std::nullopt;

    auto payload = encode_workspace_list_dir(rel_path, entries);
    if (payload.size() > max_frame_length - min_frame_length)
        return std::nullopt;

    return Frame{
        .length = 0,
        .kind = Kind::event,
        .request_id = 0,
        .method = Method::workspace_list_dir,
        .status = Status::ok,
        .payload = std::move(payload),
    };
}

// ============================================================================
// Workspace 模块服务端业务处理函数
// ============================================================================

// 处理 0x0201 (workspace_list_projects)：
// 列出 ~/projects 下的所有一级文件夹，响应格式为流式拼接的 [u16 nameLen][name bytes]。
void handle_workspace_list_projects(Frame &frame) {
    if (!decode_workspace_list_projects(frame.payload)) {
        frame.status = Status::invalid_argument;
        frame.payload = "workspace_list_projects requires an empty payload";
        return;
    }

    auto root = projects_root_dir();
    if (!root) {
        frame.status = Status::internal;
        frame.payload = "HOME environment variable is not set";
        return;
    }

    ScopedDir dir(::opendir(root->c_str()));
    if (!dir) {
        set_errno_error(frame, errno, "failed to open ~/projects");
        return;
    }

    const int dfd = ::dirfd(dir.get());
    std::vector<std::string> projects;

    while (true) {
        errno = 0;
        dirent *entry = ::readdir(dir.get());
        if (entry == nullptr) {
            if (errno != 0) {
                set_errno_error(frame, errno, "failed to read ~/projects directory");
                return;
            }
            break;
        }

        std::string_view name(entry->d_name);
        if (name == "." || name == "..")
            continue;

        bool is_dir = false;
        if (entry->d_type == DT_DIR) {
            is_dir = true;
        } else if (entry->d_type == DT_UNKNOWN) {
            // 部分文件系统在 readdir 中不填充 d_type，退回 fstatat 检查。
            struct stat st{};
            if (::fstatat(dfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode))
                is_dir = true;
        }

        if (is_dir && name.size() <= std::numeric_limits<std::uint16_t>::max())
            projects.emplace_back(name);
    }

    std::ranges::sort(projects);
    frame.status = Status::ok;
    frame.payload = encode_workspace_list_projects(projects);
}

// 处理 0x0202 (workspace_list_dir)：
// 枚举 ~/projects/<path> 下的一级文件与文件夹（深度为 1，不递归展开子目录）。
// 响应头部为 [u16 dirPathLen][dirPath bytes]，随后每个条目编码为 [u8 type][u16 nameLen][name bytes]，
// 其中：type = 0（普通文件）、type = 1（目录）、type = 2（符号链接）。
// 若成功且 watched_dir 非空，则将标准化后的目录相对路径写入 *watched_dir 供 IO 线程注册 inotify。
void handle_workspace_list_dir(Frame &frame, std::string *watched_dir = nullptr) {
    auto req = decode_workspace_list_dir(frame.payload);
    if (!req) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_list_dir payload";
        return;
    }

    auto target_path = resolve_workspace_path(req->path);
    if (!target_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace directory path";
        return;
    }

    std::vector<DirEntry> entries;
    const int rc = read_directory_entries(*target_path, entries);
    if (rc > 0) {
        set_errno_error(frame, rc, "failed to open target directory");
        return;
    }
    if (rc < 0) {
        set_errno_error(frame, -rc, "failed to read directory entries");
        return;
    }

    frame.status = Status::ok;
    frame.payload = encode_workspace_list_dir(req->path, entries);
    if (watched_dir != nullptr)
        *watched_dir = std::move(req->path);
}

// 处理 0x0203 (workspace_read_file)：
// 按 [offset, maxBytes] 分段读取 UTF-8 文件内容。
// 采用乐观并发控制：每次 pread 读取前后各做一次 fstat 计算 revision，
// 若前后 revision 一致则返回 [u64 fileSize][u64 revision][bytes...]；若不一致则自动重试。
void handle_workspace_read_file(Frame &frame) {
    auto req = decode_workspace_read_file(frame.payload);
    if (!req) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_read_file payload";
        return;
    }

    auto full_path = resolve_workspace_path(req->path);
    if (!full_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace file path";
        return;
    }

    ScopedFd fd(::open(full_path->c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) {
        set_errno_error(frame, errno, "failed to open file for reading");
        return;
    }

    // 每次读取前后比对 revision，若读取期间文件恰好被并发修改则重试。
    while (true) {
        struct stat st_before{};
        if (::fstat(fd.get(), &st_before) != 0) {
            set_errno_error(frame, errno, "failed to stat file before read");
            return;
        }
        if (!S_ISREG(st_before.st_mode)) {
            frame.status = Status::invalid_argument;
            frame.payload = "target is not a regular file";
            return;
        }

        const auto file_size = static_cast<std::uint64_t>(st_before.st_size);
        const auto rev_before = file_revision(st_before);

        std::string content;
        if (req->offset < file_size) {
            const auto readable = file_size - req->offset;
            const auto to_read =
                static_cast<std::size_t>(std::min<std::uint64_t>(readable, req->max_bytes));
            content.resize(to_read);

            std::size_t total_read = 0;
            while (total_read < to_read) {
                ssize_t n = ::pread(fd.get(), content.data() + total_read, to_read - total_read,
                                    static_cast<off_t>(req->offset + total_read));
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    set_errno_error(frame, errno, "failed to pread file");
                    return;
                }
                if (n == 0)
                    break; // 遇到文件末尾（例如读取途中文件被截断）
                total_read += static_cast<std::size_t>(n);
            }
            content.resize(total_read);
        }

        struct stat st_after{};
        if (::fstat(fd.get(), &st_after) != 0) {
            set_errno_error(frame, errno, "failed to stat file after read");
            return;
        }

        const auto rev_after = file_revision(st_after);
        if (rev_before == rev_after) {
            frame.status = Status::ok;
            frame.payload = encode_workspace_read_file(ReadFileResponse{
                .file_size = file_size,
                .revision = rev_after,
                .content = std::move(content),
            });
            return;
        }
    }
}

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
    auto req = decode_workspace_write_file(frame.payload);
    if (!req) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_write_file payload";
        return;
    }

    auto full_path = resolve_workspace_path(req->path);
    if (!full_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace file path";
        return;
    }

    // 1. 先锁住文件（路径锁）。
    std::lock_guard lock(path_mutex_for(*full_path));

    // 2. 获取原文件当前 revision 并与客户端期望值比对。
    struct stat st_before{};
    if (::lstat(full_path->c_str(), &st_before) != 0) {
        set_errno_error(frame, errno, "failed to stat target file");
        return;
    }
    if (!S_ISREG(st_before.st_mode)) {
        frame.status = Status::invalid_argument;
        frame.payload = "target is not a regular file";
        return;
    }

    const auto current_rev = file_revision(st_before);
    if (current_rev != req->expected_revision) {
        frame.status = Status::conflict;
        frame.payload = encode_workspace_write_file(current_rev);
        return;
    }

    // 3. 在同一目录下创建临时文件（保证后续 rename 在同一挂载点内原子完成）。
    std::string tmp_path = *full_path + ".tmp.XXXXXX";
    ScopedFd tmp_fd(::mkostemp(tmp_path.data(), O_CLOEXEC));
    if (!tmp_fd.valid()) {
        set_errno_error(frame, errno, "failed to create temporary file");
        return;
    }

    // 保持与原文件一致的权限位。
    (void)::fchmod(tmp_fd.get(), st_before.st_mode & 0777);

    // 4. 写入全部内容并刷盘。
    if (!write_all(tmp_fd.get(), req->content) || ::fsync(tmp_fd.get()) != 0) {
        const int err = errno;
        tmp_fd.close_now();
        (void)::unlink(tmp_path.c_str());
        set_errno_error(frame, err, "failed to write temporary file");
        return;
    }
    tmp_fd.close_now();

    // 5. 再次检查原文件 revision（防止写临时文件期间外部进程修改了原文件）。
    struct stat st_recheck{};
    if (::lstat(full_path->c_str(), &st_recheck) != 0) {
        const int err = errno;
        (void)::unlink(tmp_path.c_str());
        set_errno_error(frame, err, "failed to re-stat target file");
        return;
    }

    const auto recheck_rev = file_revision(st_recheck);
    if (recheck_rev != req->expected_revision) {
        (void)::unlink(tmp_path.c_str());
        frame.status = Status::conflict;
        frame.payload = encode_workspace_write_file(recheck_rev);
        return;
    }

    // 6. 确认未变化后，执行 rename 原子替换原文件。
    if (::rename(tmp_path.c_str(), full_path->c_str()) != 0) {
        const int err = errno;
        (void)::unlink(tmp_path.c_str());
        set_errno_error(frame, err, "failed to rename temporary file");
        return;
    }

    // 7. 取得替换后的新文件 revision。
    struct stat st_new{};
    if (::lstat(full_path->c_str(), &st_new) != 0) {
        set_errno_error(frame, errno, "failed to stat replaced file");
        return;
    }

    const auto new_rev = file_revision(st_new);

    // 8. 函数退出自动释放路径锁，返回 OK 及 [u64 newRevision]。
    frame.status = Status::ok;
    frame.payload = encode_workspace_write_file(new_rev);
}

// 处理 0x0205 (workspace_close_dir)：
// 客户端折叠或关闭指定目录时调用，请求与响应负载均为 [u16 pathLength][path bytes]（len + data）。
// 校验相对目录路径合法后，将目录相对路径写入 *unwatched_dir，供所属 I/O 线程在 DirWatcher 中执行 unwatch 释放 inotify 监听。
void handle_workspace_close_dir(Frame &frame, std::string *unwatched_dir = nullptr) {
    auto req = decode_workspace_close_dir(frame.payload);
    if (!req) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_close_dir payload";
        return;
    }

    auto target_path = resolve_workspace_path(req->path);
    if (!target_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace directory path";
        return;
    }

    frame.status = Status::ok;
    frame.payload = encode_workspace_close_dir(req->path);
    if (unwatched_dir != nullptr)
        *unwatched_dir = std::move(req->path);
}

// Workspace 模块请求统一分发入口（对应 MethodGroup::workspace 0x0200）。
// watched_dir 用于回传 list_dir 需新增监听或 close_dir 需取消监听的目录相对路径（若额外提供了 unwatched_dir 则优先写入 unwatched_dir）。
void handle_workspace_request(Frame &frame, std::string *watched_dir = nullptr,
                              std::string *unwatched_dir = nullptr) {
    switch (frame.method) {
    case Method::workspace_list_projects:
        handle_workspace_list_projects(frame);
        return;

    case Method::workspace_list_dir:
        handle_workspace_list_dir(frame, watched_dir);
        return;

    case Method::workspace_read_file:
        handle_workspace_read_file(frame);
        return;

    case Method::workspace_write_file:
        handle_workspace_write_file(frame);
        return;

    case Method::workspace_close_dir:
        handle_workspace_close_dir(frame, unwatched_dir != nullptr ? unwatched_dir : watched_dir);
        return;

    default:
        frame.status = Status::unsupported;
        frame.payload = "unsupported method";
        return;
    }
}

} // namespace server

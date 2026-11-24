module;
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

export module ide.server:directory;

import std;
import service.file;
import service.workspace_cache;
import ide.server_protocol;

export namespace server {

// 当 inotify 监听到目录变化时，由 Worker 线程重新扫描目录并构造 Kind::event 帧，同时刷新缓存。
// 事件负载格式与 workspace_list_dir 响应完全一致：[u16 dirPathLen][dirPath bytes][entries...]。
std::optional<Frame> build_workspace_list_dir_event(std::string_view rel_path) {
    auto target_path = service::file::resolve_workspace_path(rel_path);
    if (!target_path)
        return std::nullopt;

    auto &cache = service::global_workspace_cache();
    const auto epoch = cache.dir_epoch(rel_path);

    auto entries = std::make_shared<std::vector<DirEntry>>();
    if (service::file::read_directory_entries(*target_path, *entries) != 0)
        return std::nullopt;

    cache.put_dir(rel_path, entries, epoch);

    Frame frame{
        .length = 0,
        .kind = Kind::event,
        .request_id = 0,
        .method = Method::workspace_list_dir,
        .status = Status::ok,
        .payload = {},
    };
    encode_workspace_list_dir_into(rel_path, *entries, frame.payload);
    if (frame.payload.size() > max_frame_length - min_frame_length)
        return std::nullopt;
    return frame;
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

    auto root = service::file::projects_root_dir();
    if (!root) {
        frame.status = Status::internal;
        frame.payload = "HOME environment variable is not set";
        return;
    }

    service::file::ScopedDir dir(::opendir(root->c_str()));
    if (!dir) {
        service::file::set_errno_error(frame, errno, "failed to open ~/projects");
        return;
    }

    const int dfd = ::dirfd(dir.get());
    std::vector<std::string> projects;

    while (true) {
        errno = 0;
        dirent *entry = ::readdir(dir.get());
        if (entry == nullptr) {
            if (errno != 0) {
                service::file::set_errno_error(frame, errno, "failed to read ~/projects directory");
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
    encode_workspace_list_projects_into(projects, frame.payload);
}

// 处理 0x0202 (workspace_list_dir)：
// 枚举 ~/projects/<path> 下的一级文件与文件夹（深度为 1，不递归展开子目录）。
// 优先命中内存缓存（std::vector<DirEntry>），命中时完全不进入 Linux 文件系统；
// 未命中时扫描目录并将结果按失效版本号写入缓存。
// 若调用方传入 watched_dir，成功时将请求里的相对路径保存在其中，供 I/O 线程注册 inotify。
void handle_workspace_list_dir(Frame &frame, std::string *watched_dir = nullptr) {
    auto path = decode_workspace_list_dir_view(frame.payload);
    if (!path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_list_dir payload";
        return;
    }

    const std::string_view rel_path = *path;
    if (!service::file::is_valid_workspace_rel_path(rel_path) ||
        !service::file::has_projects_root_env()) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace directory path";
        return;
    }

    auto &cache = service::global_workspace_cache();
    auto entries = cache.get_dir(rel_path);
    if (!entries) {
        auto target_path = service::file::resolve_workspace_path(rel_path);
        if (!target_path) {
            frame.status = Status::invalid_argument;
            frame.payload = "invalid workspace directory path";
            return;
        }

        const auto epoch = cache.dir_epoch(rel_path);
        auto scanned = std::make_shared<std::vector<DirEntry>>();
        const int rc = service::file::read_directory_entries(*target_path, *scanned);
        if (rc != 0) {
            service::file::set_errno_error(frame, std::abs(rc),
                rc > 0 ? "failed to open target directory" : "failed to read directory entries");
            return;
        }
        entries = std::move(scanned);
        cache.put_dir(rel_path, entries, epoch);
    }

    // rel_path 借用请求的 payload；原地编码响应前，须先把路径保存到独立的字符串。
    // 有订阅者时直接使用回包节点的字符串，没有时只需保存到本次编码结束。
    std::string local_path;
    auto &response_path = watched_dir ? *watched_dir : local_path;
    response_path.assign(rel_path);
    frame.status = Status::ok;
    encode_workspace_list_dir_into(response_path, *entries, frame.payload);
}

// 处理 0x0205 (workspace_close_dir)：
// 客户端折叠或关闭指定目录时调用，请求与响应负载均为 [u16 pathLength][path bytes]（len + data）。
// 校验相对目录路径合法后，将目录相对路径写入 unwatched_dir，供所属 I/O 线程在 DirWatcher 中执行
// unwatch 释放 inotify 监听。
void handle_workspace_close_dir(Frame &frame, std::string &unwatched_dir) {
    bool canonical_u16 = false;
    auto req_path = decode_workspace_close_dir_view(frame.payload, &canonical_u16);
    if (!req_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_close_dir payload";
        return;
    }

    auto target_path = service::file::resolve_workspace_path(*req_path);
    if (!target_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace directory path";
        return;
    }

    frame.status = Status::ok;
    unwatched_dir.assign(*req_path);
    if (!canonical_u16)
        encode_workspace_close_dir_into(unwatched_dir, frame.payload);
}

}

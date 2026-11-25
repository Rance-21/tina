module;
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

export module ide.server:directory;

import std;
import service.file;
import service.workspace_cache;
import ide.server_protocol;

export namespace server {

// 当 inotify 监听到目录变化时，由 Worker 线程重新扫描目录并构造 Kind::event 帧，同时刷新缓存。
// 事件负载格式与 workspace_list_dir 响应完全一致：[u16 dirPathLen][dirPath bytes][entries...]。
std::optional<Frame> build_workspace_list_dir_event(std::string_view rel_path) {
    const int root_fd = service::file::workspace_root_fd();
    service::file::RelPath rel(rel_path);
    if (root_fd < 0 || !rel)
        return std::nullopt;

    auto &cache = service::global_workspace_cache();
    const auto epoch = cache.dir_epoch(rel.view());

    auto encoded = std::make_shared<std::string>();
    if (service::file::read_directory_payload(root_fd, rel, *encoded) != 0)
        return std::nullopt;
    if (encoded->size() > max_frame_length - min_frame_length)
        return std::nullopt;

    Frame frame{
        .length = 0,
        .kind = Kind::event,
        .request_id = 0,
        .method = Method::workspace_list_dir,
        .status = Status::ok,
        .payload = {},
    };
    if (auto pin = cache.put_dir(rel.view(), encoded, epoch))
        encode_workspace_list_dir(frame, std::move(pin));
    else
        encode_workspace_list_dir(frame, std::move(encoded));
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

    const int root_fd = service::file::workspace_root_fd();
    if (root_fd < 0) {
        const char *home = ::getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            frame.status = Status::internal;
            frame.payload = "HOME environment variable is not set";
        } else {
            service::file::set_errno_error(frame, ENOENT, "failed to open ~/projects");
        }
        return;
    }

    const int raw_fd = ::openat(root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (raw_fd < 0) {
        service::file::set_errno_error(frame, errno, "failed to open ~/projects");
        return;
    }
    DIR *raw_dir = ::fdopendir(raw_fd);
    if (!raw_dir) {
        const int err = errno;
        ::close(raw_fd);
        service::file::set_errno_error(frame, err, "failed to open ~/projects");
        return;
    }
    service::file::ScopedDir dir(raw_dir);

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
// 优先命中编码后负载缓存，命中时直接挂载外置切片零拷贝返回；
// 未命中时基于 openat 扫描编码并将不可变 Payload 按失效版本号写入缓存。
void handle_workspace_list_dir(Frame &frame, std::string *watched_dir = nullptr) {
    auto path = decode_workspace_list_dir_view(frame.payload);
    if (!path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_list_dir payload";
        return;
    }

    service::file::RelPath rel(*path);
    if (!rel) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace directory path";
        return;
    }

    const int root_fd = service::file::workspace_root_fd();
    if (root_fd < 0) {
        const char *home = ::getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            frame.status = Status::invalid_argument;
            frame.payload = "invalid workspace directory path";
        } else {
            service::file::set_errno_error(frame, ENOENT, "failed to open target directory");
        }
        return;
    }

    auto &cache = service::global_workspace_cache();
    std::uint64_t epoch;
    if (auto cached = cache.get_dir(rel.view(), &epoch)) {
        if (watched_dir)
            watched_dir->assign(rel.view());
        frame.status = Status::ok;
        encode_workspace_list_dir(frame, std::move(cached));
        return;
    }

    auto encoded = std::make_shared<std::string>();
    const int rc = service::file::read_directory_payload(root_fd, rel, *encoded);
    if (rc != 0) {
        service::file::set_errno_error(
            frame, std::abs(rc),
            rc > 0 ? "failed to open target directory" : "failed to read directory entries");
        return;
    }
    if (encoded->size() > max_frame_length - min_frame_length) {
        frame.status = Status::resource_exhausted;
        frame.payload = "directory listing exceeds maximum frame length";
        return;
    }

    if (watched_dir)
        watched_dir->assign(rel.view());
    frame.status = Status::ok;
    if (auto pin = cache.put_dir(rel.view(), encoded, epoch))
        encode_workspace_list_dir(frame, std::move(pin));
    else
        encode_workspace_list_dir(frame, std::move(encoded));
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

    if (!service::file::is_valid_workspace_rel_path(*req_path) ||
        (service::file::workspace_root_fd() < 0 &&
         (::getenv("HOME") == nullptr || ::getenv("HOME")[0] == '\0'))) {
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

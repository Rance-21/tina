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

namespace detail {

// 将目录结果写入响应帧，同时安全处理 rel_path（可能是 frame.payload 的切片视图）。
inline void write_list_dir_response(Frame &frame, std::string_view rel_path,
                                    const std::vector<DirEntry> &entries,
                                    std::string *watched_dir) {
    frame.status = Status::ok;
    if (watched_dir != nullptr) {
        watched_dir->assign(rel_path);
        encode_workspace_list_dir_into(*watched_dir, entries, frame.payload);
    } else {
        std::string dir_copy(rel_path);
        encode_workspace_list_dir_into(dir_copy, entries, frame.payload);
    }
}

}

// 解析客户端相对目录路径为服务端绝对路径（供 IO 线程注册 inotify 使用）。
std::optional<std::string> inline resolve_workspace_dir_path(std::string_view rel_path) {
    return service::file::resolve_workspace_path(rel_path);
}

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
// 若成功且 watched_dir 非空，则将标准化后的目录相对路径写入 *watched_dir 供 IO 线程注册 inotify。
void handle_workspace_list_dir(Frame &frame, std::string *watched_dir = nullptr) {
    auto req = decode_workspace_list_dir_view(frame.payload);
    if (!req) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_list_dir payload";
        return;
    }

    const std::string_view rel_path = req->path;
    if (!service::file::is_valid_workspace_rel_path(rel_path) ||
        !service::file::has_projects_root_env()) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace directory path";
        return;
    }

    auto &cache = service::global_workspace_cache();
    if (auto cached = cache.get_dir(rel_path)) {
        detail::write_list_dir_response(frame, rel_path, *cached, watched_dir);
        return;
    }

    auto target_path = service::file::resolve_workspace_path(rel_path);
    if (!target_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace directory path";
        return;
    }

    const auto epoch = cache.dir_epoch(rel_path);
    auto entries = std::make_shared<std::vector<DirEntry>>();
    const int rc = service::file::read_directory_entries(*target_path, *entries);
    if (rc > 0) {
        service::file::set_errno_error(frame, rc, "failed to open target directory");
        return;
    }
    if (rc < 0) {
        service::file::set_errno_error(frame, -rc, "failed to read directory entries");
        return;
    }

    cache.put_dir(rel_path, entries, epoch);
    detail::write_list_dir_response(frame, rel_path, *entries, watched_dir);
}

// 0x0202 (workspace_list_dir) 缓存探测结果：
// 区分 I/O 线程直接内联（<= 32 KiB）、Normal Worker 编码大目录快照、以及阻塞扫盘。
struct CachedListDirProbe {
    enum class Outcome : std::uint8_t {
        inline_handled,
        normal_cached,
        blocking_miss,
        invalid_request
    };

    Outcome outcome{Outcome::blocking_miss};
    std::shared_ptr<const std::vector<DirEntry>> cached;
    std::string dir_path;
};

// 【作用】在 I/O 线程对 0x0202 (workspace_list_dir) 做一次缓存探测并决定走 Inline、Normal 还是 Blocking。
// 【原理】
//   1. 零拷贝解码目录路径并查 WorkspaceCache 目录分片；未命中返回 blocking_miss 交 Blocking 扫盘；
//   2. 命中后先一遍累加预估线上编码字节数并检查 u16 边界：
//      - 总大小 <= max_inline_bytes（32 KiB）：原地编码进 frame 并把目录路径填入 watched_dir_out，
//        返回 inline_handled 由当前 I/O 线程直接发出并挂上 DirWatcher；
//      - 超过 32 KiB：把只读 shared_ptr 目录快照转交给 Normal Worker 编码，避免大目录序列化拖慢 EventLoop。
CachedListDirProbe probe_cached_list_dir(Frame &frame, std::size_t max_inline_bytes,
                                         std::string &watched_dir_out) {
    CachedListDirProbe res;
    auto req = decode_workspace_list_dir_view(frame.payload);
    if (!req || !service::file::is_valid_workspace_rel_path(req->path) ||
        !service::file::has_projects_root_env()) {
        res.outcome = CachedListDirProbe::Outcome::invalid_request;
        return res;
    }

    auto cached = service::global_workspace_cache().get_dir(req->path);
    if (!cached) {
        res.outcome = CachedListDirProbe::Outcome::blocking_miss;
        return res;
    }

    // 预估线上响应大小并校检 u16 边界，避免对超大目录在内联路径中产生大块拷贝或异常。
    bool u16_valid = req->path.size() <= std::numeric_limits<std::uint16_t>::max();
    std::size_t total_bytes = 2 + req->path.size();
    for (const auto &entry : *cached) {
        if (entry.name.empty() || entry.name.size() > std::numeric_limits<std::uint16_t>::max()) {
            u16_valid = false;
            break;
        }
        total_bytes += 1 + 2 + entry.name.size();
    }

    if (u16_valid && total_bytes <= max_inline_bytes) {
        frame.kind = Kind::response;
        detail::write_list_dir_response(frame, req->path, *cached, &watched_dir_out);
        res.outcome = CachedListDirProbe::Outcome::inline_handled;
        return res;
    }

    res.outcome = CachedListDirProbe::Outcome::normal_cached;
    res.cached = std::move(cached);
    res.dir_path = std::string(req->path);
    return res;
}

// 处理 0x0205 (workspace_close_dir)：
// 客户端折叠或关闭指定目录时调用，请求与响应负载均为 [u16 pathLength][path bytes]（len + data）。
// 校验相对目录路径合法后，将目录相对路径写入 *unwatched_dir，供所属 I/O 线程在 DirWatcher 中执行
// unwatch 释放 inotify 监听。
void handle_workspace_close_dir(Frame &frame, std::string *unwatched_dir = nullptr) {
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
    if (unwatched_dir != nullptr) {
        unwatched_dir->assign(*req_path);
        if (!canonical_u16)
            encode_workspace_close_dir_into(*unwatched_dir, frame.payload);
    } else if (!canonical_u16) {
        std::string dir_copy(*req_path);
        encode_workspace_close_dir_into(dir_copy, frame.payload);
    }
}

}

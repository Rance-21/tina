module;
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module ide.server:directory;

import std;
import service.file;
import service.workspace_cache;
import ide.server_protocol;

export namespace server {

// 当 inotify 监听到目录变化时，由 Worker 线程重新扫描目录并构造 Kind::event 帧，同时刷新缓存。
std::optional<Frame> build_workspace_list_dir_event(std::string_view rel_path) {
    const int root_fd = service::file::workspace_root_fd();
    const char *c_path = service::file::c_rel_path(rel_path);
    if (root_fd < 0 || !c_path)
        return std::nullopt;

    auto &cache = service::global_workspace_cache();
    const auto epoch = cache.dir_epoch(rel_path);

    auto encoded = std::make_shared<std::string>();
    if (service::file::read_directory_payload(root_fd, c_path, rel_path, *encoded) != 0 ||
        encoded->size() > max_frame_length - min_frame_length)
        return std::nullopt;

    Frame frame{
        .length = 0,
        .kind = Kind::event,
        .request_id = 0,
        .method = Method::workspace_list_dir,
        .status = Status::ok,
        .payload = {},
    };
    if (auto pin = cache.put_dir(rel_path, encoded, epoch))
        encode_workspace_list_dir(frame, std::move(pin));
    else
        encode_workspace_list_dir(frame, std::move(encoded));
    return frame;
}

void handle_workspace_list_projects(Frame &frame) {
    if (!decode_workspace_list_projects(frame.payload)) {
        frame.status = Status::invalid_argument;
        frame.payload = "workspace_list_projects requires an empty payload";
        return;
    }

    const int root_fd = service::file::workspace_root_fd();
    const int raw_fd =
        root_fd >= 0 ? ::openat(root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    if (raw_fd < 0) {
        service::file::set_errno_error(frame, root_fd < 0 ? ENOENT : errno,
                                       "failed to open ~/projects");
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

void handle_workspace_list_dir(Frame &frame) {
    auto path = decode_workspace_list_dir_view(frame.payload);
    if (!path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_list_dir payload";
        return;
    }

    const char *c_path = service::file::c_rel_path(*path);
    if (!c_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace directory path";
        return;
    }
    const int root_fd = service::file::workspace_root_fd();
    if (root_fd < 0) {
        service::file::set_errno_error(frame, ENOENT, "failed to open target directory");
        return;
    }

    const std::string_view rel_path(c_path, path->size());
    auto &cache = service::global_workspace_cache();
    std::uint64_t epoch;
    if (auto cached = cache.get_dir(rel_path, &epoch)) {
        frame.status = Status::ok;
        encode_workspace_list_dir(frame, std::move(cached));
        return;
    }

    auto encoded = std::make_shared<std::string>();
    const int rc = service::file::read_directory_payload(root_fd, c_path, rel_path, *encoded);
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

    frame.status = Status::ok;
    if (auto pin = cache.put_dir(rel_path, encoded, epoch))
        encode_workspace_list_dir(frame, std::move(pin));
    else
        encode_workspace_list_dir(frame, std::move(encoded));
}

void handle_workspace_close_dir(Frame &frame) {
    bool canonical_u16 = false;
    auto req_path = decode_workspace_close_dir_view(frame.payload, &canonical_u16);
    if (!req_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace_close_dir payload";
        return;
    }

    if (!service::file::is_valid_workspace_rel_path(*req_path) ||
        service::file::workspace_root_fd() < 0) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid workspace directory path";
        return;
    }

    frame.status = Status::ok;
    if (!canonical_u16)
        encode_workspace_close_dir_into(std::string(*req_path), frame.payload);
}

} // namespace server

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
import ide.protocol;

namespace {

// 1. 文件版本指纹（Revision）计算
// 使用 64 位 FNV-1a 哈希生成版本指纹，只要文件内容或元数据发生改变，指纹就会变化。
std::uint64_t file_revision(const struct stat &st) noexcept {
    constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;

    std::uint64_t hash = offset_basis;

    // 每个字段固定按低字节到高字节编码为 8 字节，避免结构体 padding 和字节序差异。
    auto add = [&](std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= static_cast<std::uint8_t>(value);
            hash *= prime;
            value >>= 8;
        }
    };

    add(static_cast<std::uint64_t>(st.st_dev));
    add(static_cast<std::uint64_t>(st.st_ino));
    add(static_cast<std::uint64_t>(st.st_size));

    add(static_cast<std::uint64_t>(st.st_mtim.tv_sec));
    add(static_cast<std::uint64_t>(st.st_mtim.tv_nsec));

    add(static_cast<std::uint64_t>(st.st_ctim.tv_sec));
    add(static_cast<std::uint64_t>(st.st_ctim.tv_nsec));

    return hash;
}

// 2. Linux 原生资源 RAII 包装与错误映射
// 自动关闭 Linux 文件描述符（fd）的 RAII 守卫。
class ScopedFd {
  public:
    explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {
    }

    ~ScopedFd() {
        if (fd_ >= 0)
            ::close(fd_);
    }

    ScopedFd(const ScopedFd &) = delete;
    ScopedFd &operator=(const ScopedFd &) = delete;

    [[nodiscard]] inline int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] inline bool valid() const noexcept {
        return fd_ >= 0;
    }

    void close_now() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

  private:
    int fd_{-1};
};

// 自动关闭 Linux 目录流（DIR*）的 RAII 守卫。
class ScopedDir {
  public:
    explicit ScopedDir(DIR *dir = nullptr) noexcept : dir_(dir) {
    }

    ~ScopedDir() {
        if (dir_ != nullptr)
            ::closedir(dir_);
    }

    ScopedDir(const ScopedDir &) = delete;
    ScopedDir &operator=(const ScopedDir &) = delete;

    [[nodiscard]] inline DIR *get() const noexcept {
        return dir_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return dir_ != nullptr;
    }

  private:
    DIR *dir_{nullptr};
};

// 将 Linux errno 映射为 RPC Status 状态码。
void set_errno_error(Frame &frame, int err, std::string_view context) {
    switch (err) {
    case ENOENT:
        frame.status = Status::not_found;
        break;
    case EACCES:
    case EPERM:
        frame.status = Status::permission_denied;
        break;
    case EINVAL:
    case ENAMETOOLONG:
    case ENOTDIR:
    case EISDIR:
        frame.status = Status::invalid_argument;
        break;
    case ENOSPC:
    case EMFILE:
    case ENFILE:
    case ENOMEM:
        frame.status = Status::resource_exhausted;
        break;
    default:
        frame.status = Status::internal;
        break;
    }
    frame.payload = std::string(context);
}

// 3. 工作区根目录（~/projects）与相对路径安全校验
// 获取 ~/projects 根目录路径。
std::optional<std::string> projects_root_dir() {
    const char *home = ::getenv("HOME");
    if (home == nullptr || home[0] == '\0')
        return std::nullopt;

    std::string root(home);
    if (root.back() == '/')
        root.pop_back();
    root.append("/projects");
    return root;
}

// 校验客户端传入的相对于 ~/projects 的路径（如 "tina" 或 "tina/src/main.cpp"），
// 严格禁止以 "/" 开头、禁止空路径段 "//"、禁止 "." 与 ".." 越界逃逸。
std::optional<std::string> resolve_workspace_path(std::string_view rel_path) {
    if (rel_path.empty() || rel_path.front() == '/' || rel_path.back() == '/' ||
        rel_path.find('\0') != std::string_view::npos)
        return std::nullopt;

    std::size_t start = 0;
    while (start < rel_path.size()) {
        std::size_t slash = rel_path.find('/', start);
        std::string_view part = (slash == std::string_view::npos)
                                    ? rel_path.substr(start)
                                    : rel_path.substr(start, slash - start);

        if (part.empty() || part == "." || part == "..")
            return std::nullopt;

        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }

    auto root = projects_root_dir();
    if (!root)
        return std::nullopt;

    *root += '/';
    root->append(rel_path);
    return root;
}

// ============================================================================
// 4. 路径锁（Striped Path Mutex）
// ============================================================================

// 使用 64 个分片互斥锁按完整文件路径哈希加锁：
// 同一个文件路径的并发写操作会命中同一把锁严格串行，不同文件则可并发执行。
std::mutex &path_mutex_for(std::string_view full_path) noexcept {
    static std::array<std::mutex, 64> mutex_stripes{};
    const std::size_t index = std::hash<std::string_view>{}(full_path) & (mutex_stripes.size() - 1);
    return mutex_stripes[index];
}

// 向文件描述符完整写入指定字节流（处理部分写与 EINTR）。
bool write_all(int fd, std::string_view data) noexcept {
    std::size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

// ============================================================================
// Workspace 模块服务端业务处理函数
// ============================================================================

// 处理 0x0011 (workspace_list_projects)：
// 列出 ~/projects 下的所有一级文件夹，响应格式为流式拼接的 [u16 nameLen][name bytes]。
export void handle_workspace_list_projects(Frame &frame) {
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

// 处理 0x0013 (workspace_list_dir)：
// 枚举 ~/projects/<path> 下的一级文件与文件夹（深度为 1，不递归展开子目录）。
// 每个条目编码为 [u8 type][u16 nameLen][name bytes]，其中：
//   type = 0（普通文件）、type = 1（目录）、type = 2（符号链接）。
export void handle_workspace_list_dir(Frame &frame) {
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

    ScopedDir dir(::opendir(target_path->c_str()));
    if (!dir) {
        set_errno_error(frame, errno, "failed to open target directory");
        return;
    }

    const int dfd = ::dirfd(dir.get());
    std::vector<DirEntry> entries;

    while (true) {
        errno = 0;
        dirent *entry = ::readdir(dir.get());
        if (entry == nullptr) {
            if (errno != 0) {
                set_errno_error(frame, errno, "failed to read directory entries");
                return;
            }
            break;
        }

        std::string_view name(entry->d_name);
        if (name == "." || name == ".." || name.size() > std::numeric_limits<std::uint16_t>::max())
            continue;

        std::optional<DirEntryType> type;
        switch (entry->d_type) {
        case DT_REG:
            type = DirEntryType::file;
            break;
        case DT_DIR:
            type = DirEntryType::directory;
            break;
        case DT_LNK:
            type = DirEntryType::symlink;
            break;
        case DT_UNKNOWN: {
            struct stat st{};
            if (::fstatat(dfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISREG(st.st_mode))
                    type = DirEntryType::file;
                else if (S_ISDIR(st.st_mode))
                    type = DirEntryType::directory;
                else if (S_ISLNK(st.st_mode))
                    type = DirEntryType::symlink;
            }
            break;
        }
        default:
            break;
        }

        if (type)
            entries.push_back(DirEntry{.type = *type, .name = std::string(name)});
    }

    std::ranges::sort(entries, [](const DirEntry &a, const DirEntry &b) {
        return a.name < b.name;
    });

    frame.status = Status::ok;
    frame.payload = encode_workspace_list_dir(entries);
}

// 处理 0x0014 (workspace_read_file)：
// 按 [offset, maxBytes] 分段读取 UTF-8 文件内容。
// 采用乐观并发控制：每次 pread 读取前后各做一次 fstat 计算 revision，
// 若前后 revision 一致则返回 [u64 fileSize][u64 revision][bytes...]；若不一致则自动重试。
export void handle_workspace_read_file(Frame &frame) {
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

// 处理 0x0015 (workspace_write_file)：
// 带版本校验与临时文件原子替换的文件保存流程：
//   1. 获取该文件路径的路径锁（防止多个 Worker 并发写同一文件）。
//   2. 读取原文件当前 revision，与客户端期望的 revision 比对：
//      - 若不同：返回 Status::conflict 及当前最新的 [u64 currentRevision]。
//   3. 若相同：
//      创建同目录临时文件 -> 写入全部内容并 fsync -> 再次检查原文件 revision ->
//      确认未变化后 rename 原子替换原文件 -> 取得新文件 revision -> 释放锁并返回 OK + [u64
//      newRevision]。
export void handle_workspace_write_file(Frame &frame) {
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

// Workspace 模块请求统一分发入口（对应 Method 掩码分组 0x0010）。
export void handle_workspace_request(Frame &frame) {
    switch (frame.method) {
    case Method::workspace_list_projects:
        handle_workspace_list_projects(frame);
        return;

    case Method::workspace_list_dir:
        handle_workspace_list_dir(frame);
        return;

    case Method::workspace_read_file:
        handle_workspace_read_file(frame);
        return;

    case Method::workspace_write_file:
        handle_workspace_write_file(frame);
        return;

    default:
        frame.status = Status::unsupported;
        frame.payload = "unsupported method";
        return;
    }
}

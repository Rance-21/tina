module;
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

export module service.file;

import std;
import ide.protocol;

export namespace service::file {

// 1. 文件版本指纹（Revision）计算
// 使用 64 位 FNV-1a 哈希生成版本指纹，只要文件内容或元数据发生改变，指纹就会变化。
inline std::uint64_t file_revision(const struct stat &st) noexcept {
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
inline void set_errno_error(Frame &frame, int err, std::string_view context) {
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
inline std::optional<std::string> projects_root_dir() {
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
inline std::optional<std::string> resolve_workspace_path(std::string_view rel_path) {
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

// 枚举指定完整目录路径下的一级子项（深度为 1，不递归展开）。
// 成功返回 0，失败返回对应的 errno（> 0 表示 open 失败，< 0 表示 readdir 失败的 -errno）。
inline int read_directory_entries(const std::string &target_path, std::vector<DirEntry> &entries) {
    ScopedDir dir(::opendir(target_path.c_str()));
    if (!dir)
        return errno != 0 ? errno : EIO;

    const int dfd = ::dirfd(dir.get());

    while (true) {
        errno = 0;
        dirent *entry = ::readdir(dir.get());
        if (entry == nullptr) {
            if (errno != 0)
                return -errno;
            break;
        }

        std::string_view name(entry->d_name);
        if (name == "." || name == ".." || name.size() > std::numeric_limits<std::uint16_t>::max())
            continue;

        // 忽略服务端原子写文件期间产生的同目录临时文件。
        if (name.find(".tmp.") != std::string_view::npos)
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
    return 0;
}

// ============================================================================
// 4. 路径锁（Striped Path Mutex）
// ============================================================================

// 使用 64 个分片互斥锁按完整文件路径哈希加锁：
// 同一个文件路径的并发写操作会命中同一把锁严格串行，不同文件则可并发执行。
inline std::mutex &path_mutex_for(std::string_view full_path) noexcept {
    static std::array<std::mutex, 64> mutex_stripes{};
    const std::size_t index = std::hash<std::string_view>{}(full_path) & (mutex_stripes.size() - 1);
    return mutex_stripes[index];
}

// 向文件描述符完整写入指定字节流（处理部分写与 EINTR）。
inline bool write_all(int fd, std::string_view data) noexcept {
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

} // namespace service::file

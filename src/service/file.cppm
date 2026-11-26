module;
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

export module service.file;

import std;
import ide.protocol;

namespace service::file::detail {

int workspace_root_fd = -2;

} // namespace service::file::detail

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
    ScopedFd() noexcept = default;
    explicit ScopedFd(int fd) noexcept : fd_(fd) {
    }

    ~ScopedFd() {
        if (fd_ >= 0)
            ::close(fd_);
    }

    ScopedFd(ScopedFd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {
    }
    ScopedFd &operator=(ScopedFd &&other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
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

// 3. 工作区根目录（~/projects）FD 管理与相对路径校验
// 预先打开并持有 ~/projects 目录 FD（服务启动时调用一次；独立单测首次访问时按需打开）。
int init_workspace_root() noexcept {
    static ScopedFd holder;
    holder.close_now();
    detail::workspace_root_fd = -1;

    const char *home = ::getenv("HOME");
    if (home == nullptr || home[0] == '\0')
        return -1;

    std::string_view base(home);
    while (base.size() > 1 && base.ends_with('/'))
        base.remove_suffix(1);
    constexpr std::string_view projects = "/projects";
    if (base.size() + projects.size() >= PATH_MAX)
        return -1;

    char root[PATH_MAX];
    std::memcpy(root, base.data(), base.size());
    std::memcpy(root + base.size(), projects.data(), projects.size());
    root[base.size() + projects.size()] = '\0';

    holder = ScopedFd(::open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    detail::workspace_root_fd = holder.get();
    return detail::workspace_root_fd;
}

[[nodiscard]] inline int workspace_root_fd() noexcept {
    if (detail::workspace_root_fd != -2) [[likely]]
        return detail::workspace_root_fd;
    return init_workspace_root();
}

// 仅做相对路径合法性校验（零堆分配）：严格禁止以 "/" 开头或结尾、空段 "//"、"." 与 ".." 逃逸。
inline bool is_valid_workspace_rel_path(std::string_view rel_path) noexcept {
    if (rel_path.empty() || rel_path.size() >= PATH_MAX || rel_path.front() == '/' ||
        rel_path.back() == '/' || rel_path.find('\0') != std::string_view::npos)
        return false;

    std::size_t start = 0;
    while (start < rel_path.size()) {
        std::size_t slash = rel_path.find('/', start);
        std::string_view part = (slash == std::string_view::npos)
                                    ? rel_path.substr(start)
                                    : rel_path.substr(start, slash - start);

        if (part.empty() || part == "." || part == "..")
            return false;

        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    return true;
}

// 校验相对路径并写入线程局部缓冲区补齐 '\0'，供 openat 系列系统调用直接使用。
[[nodiscard]] const char *c_rel_path(std::string_view rel_path) noexcept {
    if (!is_valid_workspace_rel_path(rel_path))
        return nullptr;
    thread_local char buf[PATH_MAX];
    std::memcpy(buf, rel_path.data(), rel_path.size());
    buf[rel_path.size()] = '\0';
    return buf;
}

// 基于 openat 枚举一级子项并直接按协议格式编码进 out：
// [u16 dirPathLen][dirPath bytes] + 按名字升序排列的 [u8 type][u16 nameLen][name bytes]...
int read_directory_payload(int root_fd, const char *c_path, std::string_view rel_path,
                           std::string &out) {
    const int raw_fd = ::openat(root_fd, c_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (raw_fd < 0)
        return errno != 0 ? errno : EIO;

    DIR *raw_dir = ::fdopendir(raw_fd);
    if (!raw_dir) {
        const int err = errno != 0 ? errno : EIO;
        ::close(raw_fd);
        return err;
    }
    ScopedDir dir(raw_dir);
    const int dfd = ::dirfd(dir.get());

    struct EntrySpan {
        std::uint32_t offset;
        std::uint16_t len;
        DirEntryType type;
    };
    std::vector<EntrySpan> entries;
    std::string names;

    while (true) {
        errno = 0;
        dirent *entry = ::readdir(dir.get());
        if (entry == nullptr) {
            if (errno != 0)
                return -errno;
            break;
        }

        std::string_view name(entry->d_name);
        if (name.empty() || name == "." || name == ".." ||
            name.size() > std::numeric_limits<std::uint16_t>::max())
            continue;

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

        if (type) {
            entries.push_back(EntrySpan{
                .offset = static_cast<std::uint32_t>(names.size()),
                .len = static_cast<std::uint16_t>(name.size()),
                .type = *type,
            });
            names.append(name);
        }
    }

    std::ranges::sort(entries, [&](const EntrySpan &a, const EntrySpan &b) {
        return std::string_view(names.data() + a.offset, a.len) <
               std::string_view(names.data() + b.offset, b.len);
    });

    const std::size_t total_size = 2 + rel_path.size() + entries.size() * 3 + names.size();
    out.clear();
    out.resize_and_overwrite(total_size, [&](char *dst, std::size_t) noexcept {
        write_be(dst, static_cast<std::uint16_t>(rel_path.size()));
        std::memcpy(dst + 2, rel_path.data(), rel_path.size());
        char *next = dst + 2 + rel_path.size();
        for (const auto &e : entries) {
            *next++ = static_cast<char>(e.type);
            write_be(next, e.len);
            std::memcpy(next + 2, names.data() + e.offset, e.len);
            next += 2 + e.len;
        }
        return total_size;
    });
    return 0;
}

// ============================================================================
// 4. 路径锁
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

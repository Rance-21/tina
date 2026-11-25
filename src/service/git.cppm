module;
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

export module service.git;

export import :process;
export import :protocol;

import std;
import service.file;

export namespace service::git {

// ============================================================================
// 2. 仓库目录 FD 解析与 Git 分支名安全校验
// ============================================================================

// 将客户端传入的仓库相对路径解析为目录 FD（避免拼接绝对路径，供子进程 fchdir 使用）：
//   - 若 rel_path 非空，在栈上校验并通过 openat(root_fd, ..., O_DIRECTORY) 打开目标目录；
//   - 若 rel_path 为空，优先检查工作区根目录是否存在 .git，否则若当前目录存在 .git 则打开 "."。
service::file::ScopedFd open_git_repo_dir(std::string_view rel_path, int &err_out) noexcept {
    err_out = 0;
    const int root_fd = service::file::workspace_root_fd();

    auto open_checked = [&](int dirfd, const char *path) noexcept {
        service::file::ScopedFd dir(::openat(dirfd, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!dir.valid())
            err_out = errno != 0 ? errno : ENOENT;
        return dir;
    };

    if (!rel_path.empty()) {
        service::file::RelPath rel(rel_path);
        if (!rel) {
            err_out = EINVAL;
            return {};
        }
        if (root_fd < 0) {
            const char *home = ::getenv("HOME");
            err_out = (home == nullptr || home[0] == '\0') ? EINVAL : ENOENT;
            return {};
        }
        return open_checked(root_fd, rel.c_str());
    }

    if (root_fd >= 0 && ::faccessat(root_fd, ".git", F_OK, 0) == 0)
        return open_checked(root_fd, ".");
    if (::access(".git", F_OK) == 0)
        return open_checked(AT_FDCWD, ".");
    if (root_fd >= 0)
        return open_checked(root_fd, ".");

    const char *home = ::getenv("HOME");
    if (home != nullptr && home[0] != '\0')
        return open_checked(AT_FDCWD, home);
    return open_checked(AT_FDCWD, "/");
}

// 校验客户端传入的 Git 分支/引用名称是否合法：
//   - 严禁以 '-' 开头，防止把分支名伪造为 git 命令行选项（Argument Injection，如 --output=...）；
//   - 严禁包含控制字符、空格、NUL、".."、"~", "^", ":", "?", "*", "[", "\\" 或 "@{"。
bool valid_git_ref(std::string_view ref) noexcept {
    if (ref.empty() || ref.front() == '-' || ref.front() == '/' || ref.back() == '/' ||
        ref.back() == '.' || ref.ends_with(".lock"))
        return false;

    if (ref.find("..") != std::string_view::npos || ref.find("@{") != std::string_view::npos ||
        ref.find("//") != std::string_view::npos)
        return false;

    for (unsigned char ch : ref) {
        if (ch <= 0x20 || ch == 0x7f)
            return false;
        switch (ch) {
        case '~':
        case '^':
        case ':':
        case '?':
        case '*':
        case '[':
        case '\\':
            return false;
        default:
            break;
        }
    }
    return true;
}

} // namespace service::git

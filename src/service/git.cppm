module;
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

export module service.git;

export import :process;
export import :protocol;

import std;
import service.file;

export namespace service::git {

// 将客户端传入的仓库相对路径解析为目录 FD（供子进程 fchdir 使用）：
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
        const char *c_path = service::file::c_rel_path(rel_path);
        if (!c_path) {
            err_out = EINVAL;
            return {};
        }
        if (root_fd < 0) {
            err_out = ENOENT;
            return {};
        }
        return open_checked(root_fd, c_path);
    }

    if (root_fd >= 0 && ::faccessat(root_fd, ".git", F_OK, 0) == 0)
        return open_checked(root_fd, ".");
    if (::access(".git", F_OK) == 0)
        return open_checked(AT_FDCWD, ".");
    if (root_fd >= 0)
        return open_checked(root_fd, ".");
    return open_checked(AT_FDCWD, ".");
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

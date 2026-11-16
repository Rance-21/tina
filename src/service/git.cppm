module;
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

export module service.git;

export import :process;
export import :protocol;

import std;
import service.file;
import service.terminal;

export namespace service::git {

// ============================================================================
// 2. 仓库路径解析与 Git 分支名安全校验（非热路径短函数不盲目 inline，保护 L1 I-Cache）
// ============================================================================

// 将客户端传入的仓库相对路径解析为服务端绝对目录路径：
//   - 若 rel_path 非空（如 "tina" 或 "demo"），调用 service::file::resolve_workspace_path
//     解析为 ~/projects/<rel_path>，严格拦截 ".." 路径逃逸；
//   - 若 rel_path 为空，优先检查 ~/projects，若其本身不是 Git 仓库而当前工作目录 "." 下存在
//   .git，则回退到 "."。
std::optional<std::string> resolve_git_repo_path(std::string_view rel_path, int &err_out) {
    err_out = 0;
    std::string target;

    if (!rel_path.empty()) {
        auto resolved = service::file::resolve_workspace_path(rel_path);
        if (!resolved) {
            err_out = EINVAL;
            return std::nullopt;
        }
        target = std::move(*resolved);
    } else {
        std::string cwd = service::terminal::default_shell_cwd();
        cwd.append("/.git");
        if (::access(cwd.c_str(), F_OK) == 0) {
            cwd.resize(cwd.size() - 5); // 剥离末尾 "/.git"，原地复用 cwd 内存
            target = std::move(cwd);
        } else if (::access(".git", F_OK) == 0) {
            target = ".";
        } else {
            cwd.resize(cwd.size() - 5);
            target = std::move(cwd);
        }
    }

    struct stat st{};
    if (::stat(target.c_str(), &st) != 0) {
        err_out = errno != 0 ? errno : ENOENT;
        return std::nullopt;
    }
    if (!S_ISDIR(st.st_mode)) {
        err_out = ENOTDIR;
        return std::nullopt;
    }
    return target;
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

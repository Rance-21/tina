module;
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

export module service.git;

import std;
import net.buffer;
import ide.protocol;
import service.file;
import service.terminal;

export namespace service::git {

using service::file::ScopedFd;

// Git 子进程单次执行默认超时时间（毫秒）与最大允许输出字节数（与 RPC 单帧负载上限对齐）。
inline constexpr int default_git_timeout_ms = 10000;
inline constexpr std::size_t max_git_output_bytes = max_frame_length - min_frame_length;

// ============================================================================
// 1. Git 子进程执行结果（直接复用 net::Buffer 承载 stdout 与 stderr，零额外拷贝）
// ============================================================================
struct GitExecResult {
    int exit_code{-1};           // 子进程退出码（0 表示成功，非 0 表示失败）
    bool timed_out{false};       // 是否因超时被强制终止
    bool output_overflow{false}; // 输出是否超过单帧最大长度限制
    net::Buffer out_buf{8192};   // 标准输出缓冲（stdout）
    net::Buffer err_buf{1024};   // 标准错误缓冲（stderr）

    // 简短高频访问器保留 inline，直接返回指向 net::Buffer 内部连续内存的 string_view。
    [[nodiscard]] inline std::string_view stdout_view() const noexcept {
        return out_buf.empty() ? std::string_view{}
                               : std::string_view(out_buf.data(), out_buf.readable_bytes());
    }

    [[nodiscard]] inline std::string_view stderr_view() const noexcept {
        return err_buf.empty() ? std::string_view{}
                               : std::string_view(err_buf.data(), err_buf.readable_bytes());
    }
};

// ============================================================================
// 2. 仓库路径解析与 Git 分支名安全校验（非热路径短函数不盲目 inline，保护 L1 I-Cache）
// ============================================================================

// 将客户端传入的仓库相对路径解析为服务端绝对目录路径：
//   - 若 rel_path 非空（如 "tina" 或 "demo"），调用 service::file::resolve_workspace_path
//     解析为 ~/projects/<rel_path>，严格拦截 ".." 路径逃逸；
//   - 若 rel_path 为空，优先检查 ~/projects，若其本身不是 Git 仓库而当前工作目录 "." 下存在 .git，则回退到 "."。
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

// ============================================================================
// 3. Worker 线程通过 Linux pipe2 + fork + dup2 + poll 执行 Git 命令
// ============================================================================
// 核心机制说明：
//   1. 调用方直接传入栈上或编译期静态构造好的 `const char *const *argv`（以 nullptr 结尾），
//      彻底省去任何 std::vector<std::string> 堆分配与指针数组二次拷贝。
//   2. 为什么要用两个管道（stdout 管道 + stderr 管道）配合 poll() 同时读取？
//      Linux 内核管道默认缓冲区约 64 KB。若子进程同时向 stdout 和 stderr 写数据，而父进程只单路阻塞读 stdout，
//      一旦 stderr 管道写满，子进程阻塞在 write(stderr) 上，父进程阻塞在 read(stdout) 上，将引发死锁。
//   3. 函数体包含系统调用与循环逻辑，不加 inline（C++23 模块中非模板函数天然具有模块链接属性，无需靠 inline 规避 ODR，
//      避免大函数内联膨胀污染 CPU L1i 指令缓存）。
GitExecResult run_git_command(const char *repo_dir, const char *const *argv,
                              int timeout_ms = default_git_timeout_ms) {
    GitExecResult result;

    // 1. 创建 stdout 与 stderr 两条非阻塞管道：pipe[0] 为读端（父进程用），pipe[1] 为写端（子进程用）。
    int out_pipe[2]{-1, -1};
    int err_pipe[2]{-1, -1};
    if (::pipe2(out_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        result.err_buf.append("pipe2 stdout failed", 19);
        return result;
    }
    ScopedFd out_read(out_pipe[0]);
    ScopedFd out_write(out_pipe[1]);

    if (::pipe2(err_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        result.err_buf.append("pipe2 stderr failed", 19);
        return result;
    }
    ScopedFd err_read(err_pipe[0]);
    ScopedFd err_write(err_pipe[1]);

    // 2. 派生子进程执行 git 命令（argv 已由调用方在编译期或栈上直接备好，零拷贝直传 execvp）。
    pid_t pid = ::fork();
    if (pid < 0) {
        result.err_buf.append("fork failed", 11);
        return result;
    }

    if (pid == 0) {
        // ================= 子进程分支 =================
        int dev_null = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (dev_null >= 0) {
            (void)::dup2(dev_null, STDIN_FILENO);
            if (dev_null > STDERR_FILENO)
                ::close(dev_null);
        }

        // dup2() 将子进程的 STDOUT_FILENO(1) 与 STDERR_FILENO(2) 重定向到管道写端；
        // 新描述符 1 和 2 默认清除 FD_CLOEXEC，execvp 后保持打开，而原 pipe fd 因带 O_CLOEXEC 被内核自动关闭。
        if (::dup2(out_write.get(), STDOUT_FILENO) < 0 ||
            ::dup2(err_write.get(), STDERR_FILENO) < 0) {
            ::_exit(126);
        }

        if (::chdir(repo_dir) != 0) {
            const char msg[] = "failed to chdir to git repository\n";
            (void)::write(STDERR_FILENO, msg, sizeof(msg) - 1);
            ::_exit(126);
        }

        ::setenv("GIT_TERMINAL_PROMPT", "0", 1);
        ::setenv("LC_ALL", "C.UTF-8", 1);

        // POSIX execvp 签名为 char *const argv[]（历史原因未加 const），但标准保证不会修改参数字符串内容。
        ::execvp("git", const_cast<char *const *>(argv));

        const char msg[] = "failed to execvp git\n";
        (void)::write(STDERR_FILENO, msg, sizeof(msg) - 1);
        ::_exit(127);
    }

    // ================= 父进程分支（Worker 线程） =================
    // 父进程立即关闭两个管道的写端，确保子进程退出时父进程 read() 能立即收到 0 (EOF)。
    out_write.close_now();
    err_write.close_now();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::array<char, 8192> chunk{};

    auto drain_pipe = [&](ScopedFd &pipe_fd, net::Buffer &target_buf) {
        while (pipe_fd.valid()) {
            ssize_t n = ::read(pipe_fd.get(), chunk.data(), chunk.size());
            if (n > 0) {
                if (target_buf.readable_bytes() + static_cast<std::size_t>(n) >
                    max_git_output_bytes) {
                    result.output_overflow = true;
                    pipe_fd.close_now();
                    return;
                }
                target_buf.append(chunk.data(), static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                pipe_fd.close_now();
                return;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;

            pipe_fd.close_now();
            return;
        }
    };

    // 3. 循环调用 poll() 同步多路复用读取 stdout 和 stderr，直到两个管道全部到达 EOF。
    while (out_read.valid() || err_read.valid()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.timed_out = true;
            break;
        }

        const auto remaining_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int wait_ms = static_cast<int>(std::max<std::int64_t>(1, remaining_ms));

        std::array<pollfd, 2> pfds{{
            {.fd = out_read.get(), .events = POLLIN, .revents = 0},
            {.fd = err_read.get(), .events = POLLIN, .revents = 0},
        }};

        int rc = ::poll(pfds.data(), pfds.size(), wait_ms);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (rc == 0) {
            result.timed_out = true;
            break;
        }

        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))
            drain_pipe(out_read, result.out_buf);

        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))
            drain_pipe(err_read, result.err_buf);

        if (result.output_overflow)
            break;
    }

    out_read.close_now();
    err_read.close_now();

    if (result.timed_out || result.output_overflow)
        (void)::kill(pid, SIGKILL);

    // 4. 回收子进程并提取退出状态码。
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            break;
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

// ============================================================================
// 4. Git 输出单遍直编器（从 net::Buffer 的 string_view 直接写入 Frame::payload，零中间结构体/字符串拷贝）
// ============================================================================

// 单遍解析 `git status --porcelain=v1 -z --untracked-files=all` 的输出，
// 并直接编码追加进响应 `out`（即 Frame::payload）：
//   - 不构造中间 std::vector<GitStatusEntry>，不分配任何临时 std::string；
//   - 每条记录 `<X><Y> <PATH>\0` 直接切片后编码为 `[u8 X][u8 Y][u16 path_len][path bytes]`；
//   - 遇到重命名（R）或复制（C）时，自动跳过紧随其后的 `<ORIG_PATH>\0` 字段。
void encode_status_porcelain_z_into(std::string_view raw, std::string &out) {
    out.clear();
    out.reserve(raw.size());

    std::size_t pos = 0;
    while (pos < raw.size()) {
        const std::size_t nul = raw.find('\0', pos);
        if (nul == std::string_view::npos)
            break;

        std::string_view token = raw.substr(pos, nul - pos);
        pos = nul + 1;

        if (token.size() < 4 || token.starts_with("## ") || token[2] != ' ')
            continue;

        const char x = token[0];
        const char y = token[1];
        std::string_view path = token.substr(3);

        // 若是重命名（R）或复制（C），紧接着的下一个 NUL 字段是原路径（orig_path），将其跳过保持流同步。
        if (x == 'R' || x == 'C' || y == 'R' || y == 'C') {
            const std::size_t orig_nul = raw.find('\0', pos);
            pos = (orig_nul != std::string_view::npos) ? (orig_nul + 1) : raw.size();
        }

        if (!path.empty() && path.size() <= std::numeric_limits<std::uint16_t>::max()) {
            append_be(out, static_cast<std::uint8_t>(x));
            append_be(out, static_cast<std::uint8_t>(y));
            append_be(out, static_cast<std::uint16_t>(path.size()));
            out.append(path);
        }
    }
}

// 单遍解析 `git log --all --topo-order -n 100 -z --pretty=format:%h%x1f%p%x1f%D%x1f%an%x1f%ct%x1f%s` 的输出，
// 并直接编码写入响应 `out`（即 Frame::payload）：
//   - 全程只用 string_view 在 net::Buffer 上原地切片，直接序列化进 out；
//   - 零 std::vector<GitCommit> 分配、零 std::vector<std::string> parents 分配、零临时 std::string 拷贝；
//   - 解析父提交列表时，先在 out 中预留 1 字节 parent_count 槽位，扫描完所有父哈希后再原地回填计数。
void encode_log_z_into(std::string_view raw, std::string &out) {
    out.clear();
    out.reserve(raw.size() + 256);

    std::size_t pos = 0;
    std::uint32_t commit_count = 0;

    while (pos < raw.size() && commit_count < git_log_limit) {
        const std::size_t nul = raw.find('\0', pos);
        std::string_view record =
            (nul == std::string_view::npos) ? raw.substr(pos) : raw.substr(pos, nul - pos);
        pos = (nul == std::string_view::npos) ? raw.size() : (nul + 1);

        while (!record.empty() && (record.front() == '\n' || record.front() == '\r'))
            record.remove_prefix(1);
        if (record.empty())
            continue;

        // 按 '\x1f' 切分 6 个字段视图：[0]hash [1]parents [2]refs [3]author [4]timestamp [5]subject
        std::array<std::string_view, 6> fields{};
        std::size_t f_pos = 0;
        bool valid = true;
        for (std::size_t i = 0; i < 5; ++i) {
            const std::size_t sep = record.find('\x1f', f_pos);
            if (sep == std::string_view::npos) {
                valid = false;
                break;
            }
            fields[i] = record.substr(f_pos, sep - f_pos);
            f_pos = sep + 1;
        }
        if (!valid)
            continue;
        fields[5] = record.substr(f_pos);

        const std::string_view hash = fields[0];
        if (hash.empty() || hash.size() > std::numeric_limits<std::uint8_t>::max())
            continue;

        // 1. 写入 [u8 hash_len][hash]
        append_be(out, static_cast<std::uint8_t>(hash.size()));
        out.append(hash);

        // 2. 先在 out 中占位 1 字节 parent_count，随后原地扫描空格分隔的父哈希直接写入 out，最后回填计数。
        const std::size_t parent_count_offset = out.size();
        out.push_back('\0');
        std::uint8_t parent_count = 0;

        const std::string_view parents_raw = fields[1];
        std::size_t p_start = 0;
        while (p_start < parents_raw.size()) {
            while (p_start < parents_raw.size() && parents_raw[p_start] == ' ')
                ++p_start;
            if (p_start >= parents_raw.size())
                break;

            const std::size_t p_end = parents_raw.find(' ', p_start);
            const std::string_view phash = (p_end == std::string_view::npos)
                                               ? parents_raw.substr(p_start)
                                               : parents_raw.substr(p_start, p_end - p_start);
            if (!phash.empty() && phash.size() <= std::numeric_limits<std::uint8_t>::max() &&
                parent_count < std::numeric_limits<std::uint8_t>::max()) {
                append_be(out, static_cast<std::uint8_t>(phash.size()));
                out.append(phash);
                ++parent_count;
            }
            if (p_end == std::string_view::npos)
                break;
            p_start = p_end + 1;
        }
        out[parent_count_offset] = static_cast<char>(parent_count);

        // 3. 写入 [u16 refs_len][refs] 与 [u16 author_len][author]（超出 u16 上限时直接截断视图）
        constexpr std::size_t u16_max = std::numeric_limits<std::uint16_t>::max();
        const std::string_view refs = fields[2].substr(0, std::min(fields[2].size(), u16_max));
        const std::string_view author = fields[3].substr(0, std::min(fields[3].size(), u16_max));
        const std::string_view subject = fields[5].substr(0, std::min(fields[5].size(), u16_max));

        append_be(out, static_cast<std::uint16_t>(refs.size()));
        out.append(refs);

        append_be(out, static_cast<std::uint16_t>(author.size()));
        out.append(author);

        // 4. 解析并写入 [u64 timestamp]
        std::uint64_t timestamp = 0;
        (void)std::from_chars(fields[4].data(), fields[4].data() + fields[4].size(), timestamp);
        append_be(out, timestamp);

        // 5. 写入 [u16 subject_len][subject]
        append_be(out, static_cast<std::uint16_t>(subject.size()));
        out.append(subject);

        ++commit_count;
    }
}

} // namespace service::git

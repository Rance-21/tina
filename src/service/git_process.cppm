module;
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

export module service.git:process;

import std;
import net.buffer.storage;
import rpc.protocol;
import service.file;

export namespace service::git {

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
                               : std::string_view(out_buf.readable_data(), out_buf.readable_bytes());
    }

    [[nodiscard]] inline std::string_view stderr_view() const noexcept {
        return err_buf.empty() ? std::string_view{}
                               : std::string_view(err_buf.readable_data(), err_buf.readable_bytes());
    }
};

// ============================================================================
// 3. Worker 线程通过 Linux pipe2 + fork + dup2 + poll 执行 Git 命令
// ============================================================================
GitExecResult run_git_command(int repo_fd, const char *const *argv,
                              int timeout_ms = default_git_timeout_ms) {
    GitExecResult result;

    // 1. 创建 stdout 与 stderr 两条非阻塞管道：pipe[0] 为读端（父进程用），pipe[1]
    // 为写端（子进程用）。
    int out_pipe[2]{-1, -1};
    int err_pipe[2]{-1, -1};
    if (::pipe2(out_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        result.err_buf.append("pipe2 stdout failed", 19);
        return result;
    }
    service::file::ScopedFd out_read(out_pipe[0]);
    service::file::ScopedFd out_write(out_pipe[1]);

    if (::pipe2(err_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        result.err_buf.append("pipe2 stderr failed", 19);
        return result;
    }
    service::file::ScopedFd err_read(err_pipe[0]);
    service::file::ScopedFd err_write(err_pipe[1]);

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
        // 新描述符 1 和 2 默认清除 FD_CLOEXEC，execvp 后保持打开，而原 pipe fd 因带 O_CLOEXEC
        // 被内核自动关闭。
        if (::dup2(out_write.get(), STDOUT_FILENO) < 0 ||
            ::dup2(err_write.get(), STDERR_FILENO) < 0) {
            ::_exit(126);
        }

        if (::fchdir(repo_fd) != 0) {
            const char msg[] = "failed to chdir to git repository\n";
            (void)::write(STDERR_FILENO, msg, sizeof(msg) - 1);
            ::_exit(126);
        }

        ::setenv("GIT_TERMINAL_PROMPT", "0", 1);
        ::setenv("LC_ALL", "C.UTF-8", 1);

        // POSIX execvp 签名为 char *const argv[]（历史原因未加
        // const），但标准保证不会修改参数字符串内容。
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

    auto drain_pipe = [&](service::file::ScopedFd &pipe_fd, net::Buffer &target_buf) {
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

}

module;
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

export module service.terminal;

import std;

export namespace service::terminal {

// Bash 每次执行完用户命令、准备重新显示提示符前，都会自动执行 PROMPT_COMMAND。
// 我们利用它向 PTY 输出一段不可见的私有 OSC 转义序列，供服务端精准识别“命令执行完毕”。
inline constexpr std::string_view prompt_done_marker = "\033]777;__TINA_DONE__\007";

// 获取默认工作目录（优先进入 ~/projects，若不存在则退回 $HOME）。
std::string default_shell_cwd() {
    const char *home = ::getenv("HOME");
    if (home == nullptr || home[0] == '\0')
        return "/";

    std::string projects(home);
    if (projects.back() == '/')
        projects.pop_back();
    projects.append("/projects");

    if (::access(projects.c_str(), R_OK | X_OK) == 0)
        return projects;
    return std::string(home);
}

// 将文件描述符设为非阻塞（O_NONBLOCK）与执行新程序时自动关闭（FD_CLOEXEC）。
// Linux 接口说明：
//   - fcntl(fd, F_GETFL) / fcntl(fd, F_SETFL) : 读取/修改文件状态标志（如 O_NONBLOCK 非阻塞模式）；
//   - fcntl(fd, F_SETFD, FD_CLOEXEC)          : 防止该 fd 泄漏给未来 fork+exec 出来的其他子进程。
bool set_nonblocking_cloexec(int fd) noexcept {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return false;

    int fd_flags = ::fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0)
        (void)::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    return true;
}

// 在 Worker 线程中向 fd 完整写入指定字节流（配合 poll 处理非阻塞 EAGAIN 与 EINTR）。
// Linux 接口说明：
//   - poll(&pfd, 1, timeout_ms) : 在指定毫秒数内等待单个 fd 变为可写（POLLOUT）或可读（POLLIN）。
inline bool write_all_timed(int fd, std::string_view data, int timeout_ms = 500) noexcept {
    std::size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
            int rc = ::poll(&pfd, 1, timeout_ms);
            if (rc <= 0)
                return false;
            continue;
        }
        return false;
    }
    return true;
}

// 排空 PTY Master 当前内核缓冲区中残留的回显字节。
inline void drain_pty_input(int fd, int wait_ms = 15) noexcept {
    std::array<char, 1024> buf{};
    while (true) {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        int rc = ::poll(&pfd, 1, wait_ms);
        if (rc <= 0 || (pfd.revents & POLLIN) == 0)
            break;

        while (true) {
            ssize_t n = ::read(fd, buf.data(), buf.size());
            if (n > 0)
                continue;
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        wait_ms = 5;
    }
}

// 解析 PTY 返回的复杂 Tab 补全字节流，还原出纯净的命令行字符串。
// 为什么 PTY 返回的不是单纯字符串？
//   Bash 的 Readline 行编辑器为了在真实终端上控制光标和局部重绘，会混入大量控制字符与 ANSI 转义码：
//   1. '\r' (0x0D 回车)      : 光标回到当前行首（常用于整行重绘）；
//   2. '\b' (0x08 退格)      : 光标左移一格（常配合空格覆盖删除字符，或回退修改后缀）；
//   3. '\a' (0x07 响铃)      : 存在多个候选或无匹配时 Readline 发出响铃警告；
//   4. "\x1b[K" (CSI 序列)   : 清除从当前光标到行尾的内容；
//   5. "\x1b[nC" / "\x1b[nD" : 光标右移 / 左移 n 格；
//   6. "\x1b]...\x07" (OSC)  : 终端标题或 PROMPT_COMMAND 标记。
// 这里通过一个轻量级“单行光标模拟器”逐字节执行上述终端控制动作，最终得到的就是屏幕上显示的真实命令。
inline std::string parse_pty_tab_completion(std::string_view user_input, std::string_view raw_pty) {
    std::string line;
    std::size_t cursor = 0;

    std::size_t i = 0;
    while (i < raw_pty.size()) {
        const unsigned char ch = static_cast<unsigned char>(raw_pty[i]);

        // 1. 处理 ESC (0x1B) 开头的 ANSI 转义序列
        if (ch == 0x1b) {
            ++i;
            if (i >= raw_pty.size())
                break;

            if (raw_pty[i] == '[') {
                // CSI 序列：\x1b[ 参数字节(0x30-0x3F) 中间字节(0x20-0x2F) 结束字母(0x40-0x7E)
                ++i;
                std::size_t param_start = i;
                while (i < raw_pty.size() && static_cast<unsigned char>(raw_pty[i]) >= 0x30 &&
                       static_cast<unsigned char>(raw_pty[i]) <= 0x3f) {
                    ++i;
                }
                std::string_view params = raw_pty.substr(param_start, i - param_start);
                while (i < raw_pty.size() && static_cast<unsigned char>(raw_pty[i]) >= 0x20 &&
                       static_cast<unsigned char>(raw_pty[i]) <= 0x2f) {
                    ++i;
                }
                if (i < raw_pty.size()) {
                    char final_byte = raw_pty[i++];
                    std::size_t count = 1;
                    if (!params.empty()) {
                        std::size_t parsed = 0;
                        for (char c : params) {
                            if (c >= '0' && c <= '9')
                                parsed = parsed * 10 + static_cast<std::size_t>(c - '0');
                            else
                                break;
                        }
                        if (parsed > 0)
                            count = parsed;
                    }
                    if (final_byte == 'K') {
                        // \x1b[K：清除光标到行尾
                        if (cursor < line.size())
                            line.resize(cursor);
                    } else if (final_byte == 'C') {
                        // \x1b[nC：光标右移
                        cursor = std::min(line.size(), cursor + count);
                    } else if (final_byte == 'D') {
                        // \x1b[nD：光标左移
                        cursor = (cursor > count) ? (cursor - count) : 0;
                    }
                }
                continue;
            }

            if (raw_pty[i] == ']') {
                // OSC 序列：\x1b] ... 以 BEL(\x07) 或 ST(\x1b\\) 结束
                ++i;
                while (i < raw_pty.size()) {
                    if (raw_pty[i] == '\x07') {
                        ++i;
                        break;
                    }
                    if (raw_pty[i] == '\x1b' && i + 1 < raw_pty.size() && raw_pty[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    ++i;
                }
                continue;
            }

            // 其他双字节 ESC 序列直接跳过
            ++i;
            continue;
        }

        // 2. 处理常规控制字符
        if (ch == '\r') {
            cursor = 0;
            ++i;
            continue;
        }
        if (ch == '\n') {
            // 若 Readline 因多候选打印了换行列表，最终重绘的命令行位于最后一行
            line.clear();
            cursor = 0;
            ++i;
            continue;
        }
        if (ch == '\b' || ch == 0x7f) {
            if (cursor > 0)
                --cursor;
            ++i;
            continue;
        }
        if (ch < 0x20) {
            // 忽略响铃 '\a' (0x07) 等其他不可见控制字符
            ++i;
            continue;
        }

        // 3. 普通可打印字符（含 UTF-8 多字节序列）：在当前光标处写入并推进光标
        if (cursor < line.size()) {
            line[cursor] = static_cast<char>(ch);
        } else {
            line.push_back(static_cast<char>(ch));
        }
        ++cursor;
        ++i;
    }

    if (line.empty())
        return std::string(user_input);
    return line;
}

// 在给定 PTY master_fd 上执行一次完整的 Readline Tab 补全交互：
//   1. 发送 "\x05\x15"（Ctrl+E 移至行尾 + Ctrl+U 删除整行）清除当前 Readline 行缓冲，并排空回显；
//   2. 向 PTY Master 写入“Android 发来的字符串 + \t”；
//   3. 用 poll() 读取 Bash Readline 返回的补全字节流，通过光标模拟器解析出纯净的补全命令；
//   4. 再次发送 "\x05\x15" 清空 Readline 行缓冲，防止后续发送完整命令时内容重复叠加。
std::optional<std::string> complete_tab_on_pty(int fd, std::string_view input) noexcept {
    // 第一步：发送 Ctrl+E (\x05) + Ctrl+U (\x15) 清除当前 PTY Readline 缓冲，并排空回显。
    if (!write_all_timed(fd, "\x05\x15"))
        return std::nullopt;
    drain_pty_input(fd, 15);

    // 第二步：直接把 string_view 切片与 '\t' 写入 PTY。
    if (!write_all_timed(fd, input) || !write_all_timed(fd, "\t"))
        return std::nullopt;

    // 第三步：读取 PTY 返回的回显与补全结果。
    // 首次等待最多 60ms 让 Bash 计算补全，一旦收到数据则继续以 15ms 短窗口收齐剩余字节。
    std::string raw_reply;
    std::array<char, 2048> buf{};
    int wait_ms = 60;
    while (raw_reply.size() < 16384) {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        int rc = ::poll(&pfd, 1, wait_ms);
        if (rc <= 0 || (pfd.revents & POLLIN) == 0)
            break;

        while (true) {
            ssize_t n = ::read(fd, buf.data(), buf.size());
            if (n > 0) {
                raw_reply.append(buf.data(), static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        wait_ms = 15;
    }

    // 第四步：再次清除 Readline 当前行并排空回显，防止后续发送完整命令时内容重复叠加。
    (void)write_all_timed(fd, "\x05\x15");
    drain_pty_input(fd, 15);

    // 第五步：解析复杂的 PTY 转义序列，提取补全后的命令字符串。
    return parse_pty_tab_completion(input, raw_reply);
}

// 等待 Bash 启动时的第一条 PROMPT_COMMAND 标记输出并排空（最多等待 250ms）。
void wait_initial_prompt(int master_fd) noexcept {
    std::string startup_buf;
    std::array<char, 1024> chunk{};
    for (int elapsed = 0; elapsed < 250; elapsed += 25) {
        pollfd pfd{.fd = master_fd, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, 25) <= 0 || (pfd.revents & POLLIN) == 0)
            continue;

        while (true) {
            ssize_t n = ::read(master_fd, chunk.data(), chunk.size());
            if (n > 0) {
                startup_buf.append(chunk.data(), static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        if (startup_buf.find(prompt_done_marker) != std::string::npos)
            break;
    }
}

// 通过 pipe2() + forkpty() 启动交互式 Bash 子进程：
// 成功时返回空字符串，并将非阻塞 master_fd 与子进程 pid 写入输出参数；失败时返回错误原因描述。
inline std::string spawn_bash_pty(int &master_fd_out, pid_t &pid_out) {
    // 1. 创建带 O_CLOEXEC 的错误通知管道（用于检测子进程 execvp 是否失败）：
    //    - pipefd[0] 为读端（父进程持有），pipefd[1] 为写端（子进程持有）；
    //    - O_CLOEXEC 保证一旦子进程 execvp("bash") 成功，内核会自动关闭 pipefd[1]，
    //      父进程 read(pipefd[0]) 立即返回 0 (EOF)；若 execvp 失败，子进程会将 errno 写入
    //      pipefd[1]。
    int err_pipe[2]{-1, -1};
    if (::pipe2(err_pipe, O_CLOEXEC) != 0)
        return "pipe2 failed";

    // 2. 设置伪终端初始窗口大小（24 行 x 80 列）。
    winsize ws{
        .ws_row = 24,
        .ws_col = 80,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };

    // 3. 调用 forkpty() 创建伪终端主从设备对并派生 Shell 子进程：
    //    - 父进程得到 master_fd（主设备描述符），可通过读写 master_fd 与 Shell 交互；
    //    - 子进程在内部自动完成 setsid() 新建会话、绑定 slave 为控制终端，
    //      并将 stdin(0)/stdout(1)/stderr(2) 重定向到伪终端从设备。
    int master_fd = -1;
    pid_t pid = ::forkpty(&master_fd, nullptr, nullptr, &ws);
    if (pid < 0) {
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return "forkpty failed";
    }

    if (pid == 0) {
        // ================= 子进程分支 =================
        ::close(err_pipe[0]);

        // 关闭从设备上可能产生额外控制序列的 ECHOCTL（避免 Ctrl+U 显示为 ^U）。
        termios tio{};
        if (::tcgetattr(STDIN_FILENO, &tio) == 0) {
            tio.c_lflag &= ~static_cast<tcflag_t>(ECHOCTL);
            (void)::tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        }

        const std::string cwd = default_shell_cwd();
        (void)::chdir(cwd.c_str());

        // 配置 Shell 环境变量：
        //   - PS1="" 清空提示符，避免每次输出夹杂冗长的 [user@host dir]$ 提示符；
        //   - PROMPT_COMMAND 在每条命令执行完毕后先关闭 Readline 括号粘贴模式，再打印完成标记。
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("PS1", "", 1);
        ::setenv("PROMPT_COMMAND",
                 "bind 'set enable-bracketed-paste off' >/dev/null 2>&1; "
                 "printf '\\033]777;__TINA_DONE__\\007'",
                 1);

        char arg0[] = "bash";
        char arg1[] = "--norc";
        char arg2[] = "--noprofile";
        char arg3[] = "-i";
        char *const argv[] = {arg0, arg1, arg2, arg3, nullptr};
        ::execvp("bash", argv);

        // 若走到这里说明 execvp 失败：向管道写入 errno 并调用 _exit 立即退出子进程。
        int err = errno;
        (void)::write(err_pipe[1], &err, sizeof(err));
        ::_exit(127);
    }

    // ================= 父进程分支（Worker 线程） =================
    ::close(err_pipe[1]);

    int child_err = 0;
    ssize_t n = 0;
    while ((n = ::read(err_pipe[0], &child_err, sizeof(child_err))) < 0 && errno == EINTR) {
    }
    ::close(err_pipe[0]);

    if (n > 0) {
        // 子进程 execvp 失败：关闭 master_fd 并 waitpid 回收子进程。
        ::close(master_fd);
        (void)::waitpid(pid, nullptr, 0);
        return "failed to exec bash";
    }

    if (!set_nonblocking_cloexec(master_fd)) {
        ::close(master_fd);
        ::kill(pid, SIGKILL);
        (void)::waitpid(pid, nullptr, 0);
        return "failed to set nonblocking pty";
    }

    // 等待 Bash 完成首次启动并排空初始 PROMPT_COMMAND 标记，确保返回给客户端时 Shell 已就绪。
    wait_initial_prompt(master_fd);

    master_fd_out = master_fd;
    pid_out = pid;
    return {};
}

// 关闭 master_fd 并向 Shell 子进程发送 SIGHUP/SIGKILL 后 waitpid 回收，防止产生僵尸进程。
inline void terminate_pty_child(int &master_fd, pid_t &child_pid) noexcept {
    if (master_fd >= 0) {
        ::close(master_fd);
        master_fd = -1;
    }

    if (child_pid > 0) {
        // 向子进程发送 SIGHUP（终端挂断信号），随后回收子进程避免变成僵尸进程。
        ::kill(child_pid, SIGHUP);
        int status = 0;
        if (::waitpid(child_pid, &status, WNOHANG) == 0) {
            ::kill(child_pid, SIGKILL);
            (void)::waitpid(child_pid, &status, 0);
        }
        child_pid = -1;
    }
}

// 从 read_carry 中计算当前可安全发送的普通输出长度（保留尾部可能与 prompt_done_marker
// 重叠的前缀字节）。
inline std::size_t safe_output_prefix_len(std::string_view read_carry) noexcept {
    std::size_t keep = 0;
    const std::size_t max_keep = std::min(read_carry.size(), prompt_done_marker.size() - 1);
    for (std::size_t k = max_keep; k > 0; --k) {
        if (read_carry.substr(read_carry.size() - k) == prompt_done_marker.substr(0, k)) {
            keep = k;
            break;
        }
    }
    return read_carry.size() - keep;
}

} // namespace service::terminal

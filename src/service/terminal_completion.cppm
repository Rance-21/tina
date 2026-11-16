module;
#include <errno.h>
#include <poll.h>
#include <unistd.h>

export module service.terminal:completion;

import std;

export namespace service::terminal {

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

}

export module ide.server_git;

import std;
import service.file;
import service.git;
export import ide.server_protocol;

namespace {

// 去除错误消息首尾空白字符，直接写入 frame.payload。
void assign_trimmed_error(std::string &out, std::string_view text, std::string_view fallback) {
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
            text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
            text.back() == '\n')) {
        text.remove_suffix(1);
    }
    out.assign(text.empty() ? fallback : text);
}

// 将 Git 子进程执行失败结果映射为对应的 RPC Status 与错误描述。
void set_git_exec_error(Frame &frame, const service::git::GitExecResult &res,
                        std::string_view fallback) {
    if (res.timed_out) {
        frame.status = Status::timeout;
        frame.payload = "git command timed out";
        return;
    }
    if (res.output_overflow) {
        frame.status = Status::resource_exhausted;
        frame.payload = "git command output exceeds maximum frame length";
        return;
    }

    const std::string_view err = res.stderr_view();
    if (err.find("not a git repository") != std::string_view::npos ||
        err.find("unknown revision") != std::string_view::npos ||
        err.find("ambiguous argument") != std::string_view::npos ||
        err.find("bad revision") != std::string_view::npos) {
        frame.status = Status::invalid_argument;
    } else {
        frame.status = Status::internal;
    }
    assign_trimmed_error(frame.payload, err, fallback);
}

} // namespace

export namespace server {

// ============================================================================
// Git 模块服务端业务处理函数（在 Worker 线程中执行，非内联以保持 L1 I-Cache 紧凑）
// ============================================================================

// 1. 处理 0x0401 (git_status) 请求：
//    直接以编译期静态 argv 数组启动 `git status --porcelain=v1 -z --untracked-files=all`，
//    再由 encode_status_porcelain_z_into 从 net::Buffer 零中间拷贝直编入 frame.payload。
void handle_git_status(Frame &frame) {
    auto rel_path = decode_git_path_request_view(frame.payload);
    if (!rel_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid git_status payload";
        return;
    }

    int err = 0;
    auto repo_dir = service::git::resolve_git_repo_path(*rel_path, err);
    if (!repo_dir) {
        service::file::set_errno_error(frame, err, "invalid git repository path");
        return;
    }

    static constexpr std::array<const char *, 9> argv{
        "git",
        "--no-pager",
        "-c",
        "core.quotepath=false",
        "status",
        "--porcelain=v1",
        "-z",
        "--untracked-files=all",
        nullptr,
    };

    auto res = service::git::run_git_command(repo_dir->c_str(), argv.data());
    if (res.timed_out || res.output_overflow || res.exit_code != 0) {
        set_git_exec_error(frame, res, "git status failed");
        return;
    }

    service::git::encode_status_porcelain_z_into(res.stdout_view(), frame.payload);
    if (frame.payload.size() > max_frame_length - min_frame_length) {
        frame.status = Status::resource_exhausted;
        frame.payload = "git_status response exceeds maximum frame length";
        return;
    }

    frame.status = Status::ok;
}

// 2. 处理 0x0402 (git_diff) 请求：
//    - 若两个分支名相同（如都是 "main"），对比工作区（worktree）与当前 HEAD；
//    - 若两个分支名不同，原地复用请求 frame.payload 内存补齐 '\0' 构造栈上指针数组执行 `git diff <base> <target>`；
//    - 输出直接从 net::Buffer assign 到 frame.payload，无临时 string 拷贝。
void handle_git_diff(Frame &frame) {
    auto req = decode_git_diff_request_view(frame.payload);
    if (!req || !service::git::valid_git_ref(req->base_branch) ||
        !service::git::valid_git_ref(req->target_branch)) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid git_diff payload or branch name";
        return;
    }

    int err = 0;
    auto repo_dir = service::git::resolve_git_repo_path(req->path, err);
    if (!repo_dir) {
        service::file::set_errno_error(frame, err, "invalid git repository path");
        return;
    }

    service::git::GitExecResult res;
    if (req->base_branch == req->target_branch) {
        static constexpr std::array<const char *, 7> head_argv{
            "git", "--no-pager", "-c", "core.quotepath=false", "diff", "HEAD", nullptr,
        };
        res = service::git::run_git_command(repo_dir->c_str(), head_argv.data());

        if (res.exit_code != 0 && !res.timed_out && !res.output_overflow &&
            res.stderr_view().find("HEAD") != std::string_view::npos) {
            static constexpr std::array<const char *, 6> wt_argv{
                "git", "--no-pager", "-c", "core.quotepath=false", "diff", nullptr,
            };
            res = service::git::run_git_command(repo_dir->c_str(), wt_argv.data());
        }
    } else {
        // 原地复用 frame.payload 缓冲区为 base_branch 补齐结尾 '\0'（借用前面已读完的 2B base_len 末字节），
        // target_branch 位于 std::string 末尾天然以 '\0' 结尾，从而实现零堆分配构造 execvp 参数。
        const std::size_t base_offset =
            static_cast<std::size_t>(req->base_branch.data() - frame.payload.data());
        const std::size_t base_size = req->base_branch.size();
        char *base_cstr = frame.payload.data() + base_offset - 1;
        std::memmove(base_cstr, frame.payload.data() + base_offset, base_size);
        base_cstr[base_size] = '\0';
        const char *target_cstr = req->target_branch.data();

        const std::array<const char *, 8> branch_argv{
            "git",
            "--no-pager",
            "-c",
            "core.quotepath=false",
            "diff",
            base_cstr,
            target_cstr,
            nullptr,
        };
        res = service::git::run_git_command(repo_dir->c_str(), branch_argv.data());
    }

    if (res.timed_out || res.output_overflow || res.exit_code != 0) {
        set_git_exec_error(frame, res, "git diff failed");
        return;
    }

    frame.status = Status::ok;
    frame.payload.assign(res.stdout_view());
}

// 3. 处理 0x0403 (git_log) 请求：
//    以编译期静态 argv 数组执行 `git log --all --topo-order -n 100 -z ...`，
//    由 encode_log_z_into 直接从 net::Buffer 单遍切片编码进 frame.payload，消除一切中间 vector/string 分配。
void handle_git_log(Frame &frame) {
    auto rel_path = decode_git_path_request_view(frame.payload);
    if (!rel_path) {
        frame.status = Status::invalid_argument;
        frame.payload = "invalid git_log payload";
        return;
    }

    int err = 0;
    auto repo_dir = service::git::resolve_git_repo_path(*rel_path, err);
    if (!repo_dir) {
        service::file::set_errno_error(frame, err, "invalid git repository path");
        return;
    }

    static constexpr std::array<const char *, 12> argv{
        "git",
        "--no-pager",
        "-c",
        "core.quotepath=false",
        "log",
        "--all",
        "--topo-order",
        "-n",
        "100",
        "-z",
        "--pretty=format:%h%x1f%p%x1f%D%x1f%an%x1f%ct%x1f%s",
        nullptr,
    };

    auto res = service::git::run_git_command(repo_dir->c_str(), argv.data());
    if (res.timed_out || res.output_overflow) {
        set_git_exec_error(frame, res, "git log failed");
        return;
    }

    if (res.exit_code != 0) {
        if (res.stderr_view().find("does not have any commits yet") != std::string_view::npos) {
            frame.status = Status::ok;
            frame.payload.clear();
            return;
        }
        set_git_exec_error(frame, res, "git log failed");
        return;
    }

    service::git::encode_log_z_into(res.stdout_view(), frame.payload);
    if (frame.payload.size() > max_frame_length - min_frame_length) {
        frame.status = Status::resource_exhausted;
        frame.payload = "git_log response exceeds maximum frame length";
        return;
    }

    frame.status = Status::ok;
}

// Git 模块请求统一分发入口（对应 MethodGroup::git 0x0400）。
void handle_git_request(Frame &frame) {
    switch (frame.method) {
    case Method::git_status:
        handle_git_status(frame);
        return;

    case Method::git_diff:
        handle_git_diff(frame);
        return;

    case Method::git_log:
        handle_git_log(frame);
        return;

    default:
        frame.status = Status::unsupported;
        frame.payload = "unsupported method";
        return;
    }
}

} // namespace server

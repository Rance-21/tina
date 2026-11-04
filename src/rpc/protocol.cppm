export module rpc.protocol;

import std;

export namespace rpc {

// KV 继续使用原有的 9 字节帧头和状态码，供命令行及并发测试使用。
enum class Method : std::uint8_t {
    ping = 1,
    set = 2,
    get = 3,
    del = 4
};

enum class Status : std::uint8_t {
    ok = 0,
    not_found = 1,
    bad_request = 2,
    conflict = 3,
    incomplete = 4,
    io_error = 5,
    resource_exhausted = 6
};

struct Request {
    std::uint32_t id;
    Method method;
    std::string payload;
};

struct Response {
    std::uint32_t request_id;
    Status status;
    std::string payload;
};

constexpr std::uint32_t max_payload_size = 1024 * 1024;

} // namespace rpc

export namespace rpc::workbench {

enum class Kind : std::uint8_t {
    request = 0x01,  // Android 向服务端发起请求。
    response = 0x02, // 服务端返回这次请求的结果。
    event = 0x03     // 服务端主动推送终端或 Agent 事件。
};

enum class Method : std::uint16_t {
    system_ping = 0x0001,             // 请求：检查连通性。

    workspace_list_projects = 0x0011, // 请求：列出已登记的项目。
    workspace_open = 0x0012,          // 请求：打开项目。
    workspace_list_dir = 0x0013,      // 请求：枚举一级目录。
    workspace_read_file = 0x0014,     // 请求：读取 UTF-8 文本，可按字节范围分段。
    workspace_write_file = 0x0015,    // 请求：保存完整的 UTF-8 文本文件。

    terminal_input = 0x0021,          // 请求：向默认 Shell 输入文本。
    terminal_resize = 0x0022,         // 请求：调整默认终端的行列数。
    terminal_output = 0x0281,         // 事件：推送终端输出。
    terminal_state = 0x0282,          // 事件：通知 Shell 启动、退出或重启。

    git_status = 0x0031,              // 请求：查询分支及文件状态。
    git_diff = 0x0032,                // 请求：读取指定文件的 Unified Diff。

    agent_list = 0x0401,              // 请求：列出 Agent 会话。
    agent_start = 0x0402,             // 请求：按配置启动会话。
    agent_input = 0x0403,             // 请求：向会话输入文本。
    agent_cancel = 0x0404,            // 请求：中断当前任务，尽量保留进程以便继续对话。
    agent_close = 0x0405,             // 请求：结束会话并释放进程和 PTY。
    agent_output = 0x0481,            // 事件：推送指定会话的输出。
    agent_state = 0x0482              // 事件：通知会话状态变化。
};

enum class Status : std::uint16_t {
    ok = 0,                 // 成功。
    invalid_argument = 1,   // 参数不合法。
    unauthenticated = 2,    // 尚未认证。
    permission_denied = 3,  // 无权限。
    not_found = 4,          // 目标不存在。
    already_exists = 5,     // 目标已存在。
    conflict = 6,           // 状态冲突。
    resource_exhausted = 7, // 资源或队列达到上限。
    unsupported = 8,        // 不支持该操作。
    canceled = 9,           // 操作已取消。
    internal = 10,          // 内部错误。
    unavailable = 11,       // 服务不可用。
    timeout = 12            // 操作超时。
};

inline constexpr std::uint32_t min_frame_length = 13; // 空 Payload 时 Length 的值。
inline constexpr std::uint32_t frame_header_size = 4 + min_frame_length;
inline constexpr std::uint32_t max_frame_length = 1024 * 1024; // 限制 Length 字段的值。
inline constexpr std::uint32_t max_file_read_bytes = 256 * 1024;
inline constexpr std::uint32_t max_terminal_output_bytes = 64 * 1024;

// 线上按以下字段顺序以大端序逐项读写，不能用 sizeof(Frame) 或复制结构体内存。
// 固定帧头共 17 字节。Length = 13 + Payload 字节数，不含 Length 自身的 4 字节。
// 收包时应先验证 min_frame_length <= Length <= max_frame_length，再分配缓冲区。
struct Frame {
    std::uint32_t length;
    Kind kind;
    std::uint64_t request_id; // 请求非零；每个请求恰有一个响应，允许乱序，按原 ID 匹配；事件为零。
    Method method;
    Status status;            // 仅响应携带结果；请求与事件必须为零。
    std::string payload;      // UTF-8 文本，具体 JSON 结构由 Kind 和 Method 决定。
};

// 工作台帧的编码与解码入口预留给后续实现。
void encode_frame(const Frame &, std::string & /* 输出字节 */) {
}

void decode_frame(std::string_view /* 完整帧字节 */, Frame &) {
}

// 失败原因由状态码标识；错误 JSON 可附带供人阅读的说明。
struct ErrorPayload {
    std::optional<std::string> message;
};

struct AcceptedResponse {
    bool accepted;
};

namespace system {

struct PingRequest {}; // 请求的 Payload 为零字节。
struct PingResponse {
    bool ok;
};

} // namespace system

namespace workspace {

struct ListProjectsRequest {}; // 请求的 Payload 为 JSON 对象 {}。
struct Project {
    std::string workspaceId;
    std::string name;
    std::string path;
};
struct ListProjectsResponse {
    std::vector<Project> projects;
};

struct OpenRequest {
    std::string workspaceId;
};
struct OpenResponse {
    std::string workspaceId;
    std::string name;
};

struct ListDirRequest {
    std::string workspaceId;
    std::string path;
};
struct DirEntry {
    std::string name;
    std::string path;
    std::string type;                  // "file" 或 "directory"。
    std::optional<std::uint64_t> size; // 文件字节数，目录可省略此字段。
};
struct ListDirResponse {
    std::vector<DirEntry> entries; // 只返回当前一级，客户端展开子目录时再请求。
};

struct ReadFileRequest {
    std::string workspaceId;
    std::string path;
    std::uint64_t offset{}; // 从文件开头计算的 UTF-8 字节偏移。
    std::uint32_t maxBytes{max_file_read_bytes};
};
struct ReadFileResponse {
    std::uint64_t offset;
    std::uint64_t nextOffset; // 下次读取的字节偏移，必须位于 UTF-8 字符边界。
    std::uint64_t totalBytes;
    bool eof;
    std::string content;      // 本段只包含完整的 UTF-8 字符。
};

struct WriteFileRequest {
    std::string workspaceId;
    std::string path;
    std::string content;
};
struct WriteFileResponse {
    bool saved;
};

} // namespace workspace

namespace terminal {

struct InputRequest {
    std::string text;
};
using InputResponse = AcceptedResponse;

struct ResizeRequest {
    std::uint32_t cols;
    std::uint32_t rows;
};
using ResizeResponse = AcceptedResponse;

struct OutputEvent {
    std::uint64_t sequence;       // 标识默认终端的输出顺序。
    std::string text;
    std::optional<bool> snapshot; // 重连时可设为 true 表示近期输出快照，普通输出可省略。
};
struct StateEvent {
    std::string state; // 默认 Shell 的状态，如 "running"。
};

} // namespace terminal

namespace git {

struct StatusRequest {
    std::string workspaceId;
};
struct FileStatus {
    std::string path;
    std::string indexStatus;    // 暂存区状态，保留表示无变化的空格。
    std::string worktreeStatus; // 工作区状态。
    bool untracked;
};
struct StatusResponse {
    std::string branch;
    std::vector<FileStatus> files;
};

// scope 的 JSON 取值：worktree 比较工作区与暂存区，staged 比较暂存区与 HEAD。
enum class DiffScope { worktree, staged };
struct DiffRequest {
    std::string workspaceId;
    std::string path;
    DiffScope scope;
};
struct DiffResponse {
    std::string path;
    DiffScope scope;
    std::string diff; // Unified Diff 文本，未跟踪文件按新增文件处理。
};

} // namespace git

namespace agent {

// 对应 JSON 中同名的小写状态字符串。
enum class State { starting, running, busy, idle, exited, failed, closed };

struct ListRequest {}; // 请求的 Payload 为 JSON 对象 {}。
struct Session {
    std::string sessionId;
    std::string name;
    State state;
};
struct ListResponse {
    std::vector<Session> sessions;
};

struct StartRequest {
    std::string profileId;
    std::string workspaceId;
};
struct StartResponse {
    std::string sessionId;
    State state;
};

struct InputRequest {
    std::string sessionId;
    std::string text;
};
using InputResponse = AcceptedResponse;

struct CancelRequest {
    std::string sessionId;
};
using CancelResponse = AcceptedResponse;

struct CloseRequest {
    std::string sessionId;
};
struct CloseResponse {
    bool closed;
};

struct OutputEvent {
    std::string sessionId;
    std::uint64_t sequence; // 标识该会话的输出顺序。
    std::string text;
};
struct StateEvent {
    std::string sessionId;
    State state;
};

} // namespace agent
} // namespace rpc::workbench

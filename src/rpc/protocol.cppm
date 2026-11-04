export module rpc.protocol;

import std;

export enum class Kind : std::uint8_t {
    request = 0x01,  // Android 向服务端发起请求。
    response = 0x02, // 服务端返回这次请求的结果。
    event = 0x03     // 服务端主动推送终端或 Agent 事件。
};

export enum class Method : std::uint16_t {
    system_ping = 0x0001, // 请求：检查连通性。

    kv_set = 0x0002, // 请求：以首个换行分隔键和值。
    kv_get = 0x0003, // 请求：按键读取值。
    kv_del = 0x0004, // 请求：删除指定键。

    workspace_list_projects = 0x0011, // 请求：列出已登记的项目。
    workspace_open = 0x0012,          // 请求：打开项目。
    workspace_list_dir = 0x0013,      // 请求：枚举一级目录。
    workspace_read_file = 0x0014,     // 请求：读取 UTF-8 文本，可按字节范围分段。
    workspace_write_file = 0x0015,    // 请求：保存完整的 UTF-8 文本文件。

    terminal_input = 0x0021,  // 请求：向默认 Shell 输入文本。
    terminal_resize = 0x0022, // 请求：调整默认终端的行列数。
    terminal_output = 0x0281, // 事件：推送终端输出。
    terminal_state = 0x0282,  // 事件：通知 Shell 启动、退出或重启。

    git_status = 0x0031, // 请求：查询分支及文件状态。
    git_diff = 0x0032,   // 请求：读取指定文件的 Unified Diff。

    agent_list = 0x0401,   // 请求：列出 Agent 会话。
    agent_start = 0x0402,  // 请求：按配置启动会话。
    agent_input = 0x0403,  // 请求：向会话输入文本。
    agent_cancel = 0x0404, // 请求：中断当前任务，尽量保留进程以便继续对话。
    agent_close = 0x0405,  // 请求：结束会话并释放进程和 PTY。
    agent_output = 0x0481, // 事件：推送指定会话的输出。
    agent_state = 0x0482   // 事件：通知会话状态变化。
};

export enum class Status : std::uint16_t {
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

export inline constexpr std::uint32_t min_frame_length = 13; // 空 Payload 时 Length 的值。
export inline constexpr std::uint32_t frame_header_size = 4 + min_frame_length;
export inline constexpr std::uint32_t max_frame_length = 1024 * 1024; // 限制 Length 字段的值。
export inline constexpr std::uint32_t max_file_read_bytes = 256 * 1024;
export inline constexpr std::uint32_t max_terminal_output_bytes = 64 * 1024;

// 线上按以下字段顺序以大端序逐项读写，不能用 sizeof(Frame) 或复制结构体内存。
// 固定帧头共 17 字节。Length = 13 + Payload 字节数，不含 Length 自身的 4 字节。
// 收包时应先验证 min_frame_length <= Length <= max_frame_length，再分配缓冲区。
export struct Frame {
    std::uint32_t length{}; // 解码后为线上 Length；编码时按 payload 的字节数计算。
    Kind kind;
    std::uint64_t request_id; // 请求非零；每个请求恰有一个响应，允许乱序，按原 ID 匹配；事件为零。
    Method method;
    Status status;       // 仅响应携带结果；请求与事件必须为零。
    std::string payload; // 原样传输的文本，具体格式由 Method 决定。
};

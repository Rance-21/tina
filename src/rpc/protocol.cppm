export module rpc.protocol;

import std;

export enum class Kind : std::uint8_t {
    request = 0x01,  // Android 向服务端发起请求。
    response = 0x02, // 服务端返回这次请求的结果。
    event = 0x03     // 服务端主动推送目录变更、终端或 Agent 事件。
};

// ============================================================================
// 16 位 Method 位域编码设计：
//   Bits [15:8] (0xFF00) : 业务模块分组（MethodGroup），右移 8 位即可得到连续分组索引 0..5
//   Bit  [7]    (0x0080) : 纯服务端推送事件标志位（method_event_flag，1 表示不可作为客户端 Request
//   调用） Bits [6:0]  (0x007F) : 模块内操作序号（从 1 开始连续编号，利好编译器生成紧凑跳转表）
// ============================================================================
export inline constexpr std::uint16_t method_group_shift = 8;
export inline constexpr std::uint16_t method_group_mask = 0xFF00;
export inline constexpr std::uint16_t method_event_flag = 0x0080;
export inline constexpr std::uint16_t method_op_mask = 0x007F;

export enum class MethodGroup : std::uint16_t {
    system = 0x0000,    // 0x00xx：系统基础协议
    kv = 0x0100,        // 0x01xx：KV 内存存储
    workspace = 0x0200, // 0x02xx：工作区项目与文件读写
    terminal = 0x0300,  // 0x03xx：终端 PTY 会话与补全
    git = 0x0400,       // 0x04xx：Git 状态、Diff 与提交历史
    agent = 0x0500      // 0x05xx：AI Agent 会话管理与事件流
};

export [[nodiscard]] constexpr inline std::uint16_t
make_method_code(MethodGroup group, std::uint8_t op, bool event_only = false) noexcept {
    return static_cast<std::uint16_t>(std::to_underlying(group) |
                                      (event_only ? method_event_flag : 0) | (op & method_op_mask));
}

export enum class Method : std::uint16_t {
    // 0x00xx: System 模块
    system_ping = make_method_code(MethodGroup::system, 1), // 0x0001 请求：检查连通性。

    // 0x01xx: KV 模块
    kv_set = make_method_code(MethodGroup::kv, 1), // 0x0101 请求：以首个换行分隔键和值。
    kv_get = make_method_code(MethodGroup::kv, 2), // 0x0102 请求：按键读取值。
    kv_del = make_method_code(MethodGroup::kv, 3), // 0x0103 请求：删除指定键。

    // 0x02xx: Workspace 模块
    workspace_list_projects =
        make_method_code(MethodGroup::workspace, 1), // 0x0201 请求：列出已登记的项目。
    workspace_list_dir = make_method_code(MethodGroup::workspace,
                                          2), // 0x0202 请求/事件：枚举一级目录或推送目录变更。
    workspace_read_file = make_method_code(MethodGroup::workspace,
                                           3), // 0x0203 请求：读取 UTF-8 文本，可按字节范围分段。
    workspace_write_file =
        make_method_code(MethodGroup::workspace, 4), // 0x0204 请求：保存完整的 UTF-8 文本文件。

    // 0x03xx: Terminal 模块
    terminal_input =
        make_method_code(MethodGroup::terminal, 1), // 0x0301 请求：向终端输入命令文本。
    terminal_on_tab =
        make_method_code(MethodGroup::terminal, 2), // 0x0302 请求：触发 Tab 命令补全。
    terminal_state =
        make_method_code(MethodGroup::terminal,
                         3), // 0x0303 请求/事件：启动或关闭 Shell；通知命令结束或 Shell 退出。
    terminal_output =
        make_method_code(MethodGroup::terminal, 1, true), // 0x0381 事件：推送终端输出。

    // 0x04xx: Git 模块
    git_status = make_method_code(MethodGroup::git, 1), // 0x0401 请求：查询分支及文件状态。
    git_diff = make_method_code(MethodGroup::git, 2), // 0x0402 请求：读取指定文件的 Unified Diff。
    git_log = make_method_code(MethodGroup::git, 3),  // 0x0403 请求：查询提交历史记录（Git Log）。

    // 0x05xx: Agent 模块
    agent_list = make_method_code(MethodGroup::agent, 1),  // 0x0501 请求：列出 Agent 会话。
    agent_start = make_method_code(MethodGroup::agent, 2), // 0x0502 请求：按配置启动会话。
    agent_input = make_method_code(MethodGroup::agent, 3), // 0x0503 请求：向会话输入文本。
    agent_cancel = make_method_code(MethodGroup::agent,
                                    4), // 0x0504 请求：中断当前任务，尽量保留进程以便继续对话。
    agent_close =
        make_method_code(MethodGroup::agent, 5), // 0x0505 请求：结束会话并释放进程和 PTY。
    agent_output =
        make_method_code(MethodGroup::agent, 1, true),          // 0x0581 事件：推送指定会话的输出。
    agent_state = make_method_code(MethodGroup::agent, 2, true) // 0x0582 事件：通知会话状态变化。
};

// 单指令位与提取 Method 所属模块分组（保留高 8 位，如 0x0300）。
export [[nodiscard]] constexpr MethodGroup method_group(Method method) noexcept {
    return static_cast<MethodGroup>(std::to_underlying(method) & method_group_mask);
}

// 单指令右移提取连续分组索引（0..5），适合作为紧凑跳转表或数组下标。
export [[nodiscard]] constexpr std::uint8_t method_group_index(Method method) noexcept {
    return static_cast<std::uint8_t>(std::to_underlying(method) >> method_group_shift);
}

// 单指令位与提取模块内操作序号（1..127）。
export [[nodiscard]] constexpr std::uint8_t method_op(Method method) noexcept {
    return static_cast<std::uint8_t>(std::to_underlying(method) & method_op_mask);
}

// 单指令位测试判断该 Method 是否为仅限服务端主动推送的 Event 方法（Bit 7 == 1）。
export [[nodiscard]] constexpr bool is_event_only_method(Method method) noexcept {
    return (std::to_underlying(method) & method_event_flag) != 0;
}

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

// 瞬态状态码位掩码（conflict | resource_exhausted | internal | unavailable | timeout）。
export inline constexpr std::uint32_t transient_status_mask =
    (1u << std::to_underlying(Status::conflict)) |
    (1u << std::to_underlying(Status::resource_exhausted)) |
    (1u << std::to_underlying(Status::internal)) | (1u << std::to_underlying(Status::unavailable)) |
    (1u << std::to_underlying(Status::timeout));

export [[nodiscard]] constexpr bool is_transient_status(Status status) noexcept {
    const auto raw = std::to_underlying(status);
    return raw <= std::to_underlying(Status::timeout) && ((transient_status_mask >> raw) & 1u) != 0;
}

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
    std::string payload; // 原样传输的负载字节流，具体格式由 Method 决定。
};

export module ide.server_protocol;

import std;
export import ide.protocol;

namespace {

// 三种工作区请求的路径前缀相同：[u16 pathLength][path bytes]。
// 视图借用请求帧，业务处理完之前须保持 payload 有效；路径是否允许访问由业务层判断。
std::optional<std::string_view> read_workspace_path(PayloadReader &reader) noexcept {
    auto length = reader.read_int<std::uint16_t>();
    if (!length || *length == 0)
        return std::nullopt;
    auto path = reader.read_bytes(*length);
    if (!path || path->find('\0') != std::string_view::npos)
        return std::nullopt;
    return path;
}

} // namespace

export namespace server {

// ============================================================================
// Workspace 模块服务端 Decode 函数（解析客户端发来的 Request Payload）
// ============================================================================

// 解码 0x0201 (workspace_list_projects) 请求负载：
// 协议规定请求负载为空，若非空则视为非法参数。
bool decode_workspace_list_projects(std::string_view payload) {
    return payload.empty();
}

// 0x0203 (workspace_read_file) 零拷贝视图，直接引用 Frame::payload 切片。
struct ReadFileRequestView {
    std::string_view path;
    std::uint64_t offset{};
    std::uint32_t max_bytes{};
};

// list_dir 的完整请求只有路径，不允许尾随字节。
std::optional<std::string_view> decode_workspace_list_dir_view(std::string_view payload) noexcept {
    PayloadReader reader(payload);
    auto path = read_workspace_path(reader);
    return reader.finished() ? path : std::nullopt;
}

// 零拷贝解码 0x0203 (workspace_read_file) 请求负载：
// 格式：[u16 pathLength][path bytes][u64 offset][u32 maxBytes]
std::optional<ReadFileRequestView>
decode_workspace_read_file_view(std::string_view payload) noexcept {
    PayloadReader reader(payload);
    auto path = read_workspace_path(reader);
    auto offset = reader.read_int<std::uint64_t>();
    auto max_bytes = reader.read_int<std::uint32_t>();

    if (!path || !offset || !max_bytes || !reader.finished())
        return std::nullopt;

    if (*max_bytes == 0 || *max_bytes > max_file_read_bytes)
        return std::nullopt;

    return ReadFileRequestView{
        .path = *path,
        .offset = *offset,
        .max_bytes = *max_bytes,
    };
}

// write_file 的路径和正文都借用 Worker 独占的请求帧。冲突返回和超大文件写盘
// 都不需要复制正文；只有成功写入并进入文件缓存时，缓存才需要拥有独立的字节。
struct WriteFileRequestView {
    std::string_view path;
    std::uint64_t expected_revision;
    std::string_view content;
};

std::optional<WriteFileRequestView>
decode_workspace_write_file_view(std::string_view payload) noexcept {
    PayloadReader reader(payload);
    auto path = read_workspace_path(reader);
    auto revision = reader.read_int<std::uint64_t>();
    if (!path || !revision)
        return std::nullopt;
    return WriteFileRequestView{*path, *revision, reader.remaining_bytes()};
}

// 零拷贝解码 0x0205 (workspace_close_dir) 请求负载：
// 格式：[u16 pathLength][path bytes]（即 len + data，同时兼容 4 字节大端 len 编码）。
std::optional<std::string_view> decode_workspace_close_dir_view(std::string_view payload) noexcept {
    PayloadReader reader(payload);
    auto path_len = reader.read_int<std::uint16_t>();
    if (!path_len)
        return std::nullopt;

    std::optional<std::string_view> path;
    if (*path_len > 0) {
        path = reader.read_bytes(*path_len);
    } else if (reader.remaining_size() >= 2) {
        auto low_len = reader.read_int<std::uint16_t>();
        if (low_len && *low_len > 0)
            path = reader.read_bytes(*low_len);
    }

    if (!path || !reader.finished() || path->find('\0') != std::string_view::npos)
        return std::nullopt;
    return *path;
}

// ============================================================================
// Workspace 模块服务端 Encode 函数（构造返回给客户端的 Response / Event Payload）
// ============================================================================

// 原地编码 0x0201 (workspace_list_projects) 响应负载进 out（复用已有缓冲区，避免临时字符串）。
// 每个项目编码为 [u16 nameLen][name bytes]，多个项目直接顺序流式拼接，不设数量头。
void encode_workspace_list_projects_into(const std::vector<std::string> &projects,
                                         std::string &out) {
    std::size_t total_size = 0;
    for (const auto &name : projects) {
        if (name.empty() || name.size() > std::numeric_limits<std::uint16_t>::max())
            throw std::invalid_argument("project name exceeds u16 length limit");
        total_size += 2 + name.size();
    }

    out.clear();
    out.reserve(total_size);
    for (const auto &name : projects) {
        append_be(out, static_cast<std::uint16_t>(name.size()));
        out.append(name);
    }
}

// 零拷贝封装已编码完成的 0x0202 (workspace_list_dir) 响应或目录变更 Event 负载，
// 直接挂载外置切片与所有者（缓存槽位 Pin 或独立正文），由 writev 聚集发送。
void encode_workspace_list_dir(Frame &frame, auto cached) noexcept {
    frame.payload.clear();
    frame.external_payload = *cached;
    frame.payload_owner = std::move(cached);
}

// 原地编码 0x0203 (workspace_read_file) 响应负载进 out：
// 格式：[u64 fileSize][u64 revision][content bytes...]
void encode_workspace_read_file_into(std::uint64_t file_size, std::uint64_t revision,
                                     std::string_view content, std::string &out) {
    out.clear();
    out.reserve(16 + content.size());
    append_be(out, file_size);
    append_be(out, revision);
    out.append(content);
}

// 只编码 16 字节文件头；移动槽位 Pin 或独立读盘结果，保活内容切片，直到 I/O 线程发送完毕。
void encode_workspace_read_file(Frame &frame, auto cached,
                                std::string_view content) {
    frame.payload.resize(16);
    write_be(frame.payload.data(), cached->file_size);
    write_be(frame.payload.data() + 8, cached->revision);
    if (content.empty()) {
        frame.clear_external_payload();
    } else {
        frame.payload_owner = std::move(cached);
        frame.external_payload = content;
    }
}

// 原地编码 0x0204 (workspace_write_file) 响应负载进 out：
// 格式：[u64 revision]（写入成功时为 newRevision；版本冲突时为当前实际的 currentRevision）。
void encode_workspace_write_file_into(std::uint64_t revision, std::string &out) {
    out.resize(8);
    write_be(out.data(), revision);
}

// 原地编码 0x0205 (workspace_close_dir) 响应负载进 out：
// 格式：[u16 pathLength][path bytes]（len + data，返回已关闭并取消 inotify 监听的目录相对路径）。
void encode_workspace_close_dir_into(std::string_view dir_path, std::string &out) {
    if (dir_path.empty() || dir_path.size() > std::numeric_limits<std::uint16_t>::max() ||
        dir_path.find('\0') != std::string_view::npos)
        throw std::invalid_argument("invalid workspace_close_dir path");

    out.clear();
    out.reserve(2 + dir_path.size());
    append_be(out, static_cast<std::uint16_t>(dir_path.size()));
    out.append(dir_path);
}

// 构造 0x0381 (terminal_output) 服务端推送 Event 帧。
inline Frame make_terminal_output_event(std::uint32_t terminal_id, std::string_view text) {
    return Frame{
        .length = 0,
        .kind = Kind::event,
        .request_id = 0,
        .method = Method::terminal_output,
        .status = Status::ok,
        .payload = encode_terminal_output(terminal_id, text),
    };
}

// 构造 0x0303 (terminal_state) 服务端推送 Event 帧（通知命令执行完毕或 Shell 退出）。
inline Frame make_terminal_state_event(std::uint32_t terminal_id) {
    return Frame{
        .length = 0,
        .kind = Kind::event,
        .request_id = 0,
        .method = Method::terminal_state,
        .status = Status::ok,
        .payload = encode_terminal_state_id(terminal_id),
    };
}

} // namespace server

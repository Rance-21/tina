export module ide.server_protocol;

import std;
export import ide.protocol;

export namespace server {

// ============================================================================
// Workspace 模块服务端 Decode 函数（解析客户端发来的 Request Payload）
// ============================================================================

// 解码 0x0201 (workspace_list_projects) 请求负载：
// 协议规定请求负载为空，若非空则视为非法参数。
bool decode_workspace_list_projects(std::string_view payload) {
    return payload.empty();
}

// 解码 0x0202 (workspace_list_dir) 请求负载：
// 格式：[u16 pathLength][path bytes]
std::optional<ListDirRequest> decode_workspace_list_dir(std::string_view payload) {
    PayloadReader reader(payload);
    auto path_len = reader.read_int<std::uint16_t>();
    if (!path_len || *path_len == 0)
        return std::nullopt;

    auto path = reader.read_bytes(*path_len);
    if (!path || !reader.finished() || path->find('\0') != std::string_view::npos)
        return std::nullopt;

    return ListDirRequest{.path = std::string(*path)};
}

// 解码 0x0203 (workspace_read_file) 请求负载：
// 格式：[u16 pathLength][path bytes][u64 offset][u32 maxBytes]
std::optional<ReadFileRequest> decode_workspace_read_file(std::string_view payload) {
    PayloadReader reader(payload);
    auto path_len = reader.read_int<std::uint16_t>();
    if (!path_len || *path_len == 0)
        return std::nullopt;

    auto path = reader.read_bytes(*path_len);
    auto offset = reader.read_int<std::uint64_t>();
    auto max_bytes = reader.read_int<std::uint32_t>();

    if (!path || !offset || !max_bytes || !reader.finished() ||
        path->find('\0') != std::string_view::npos)
        return std::nullopt;

    if (*max_bytes == 0 || *max_bytes > max_file_read_bytes)
        return std::nullopt;

    return ReadFileRequest{
        .path = std::string(*path),
        .offset = *offset,
        .max_bytes = *max_bytes,
    };
}

// 解码 0x0204 (workspace_write_file) 请求负载：
// 格式：[u16 pathLen][path bytes][u64 revision][content bytes...]
std::optional<WriteFileRequest> decode_workspace_write_file(std::string_view payload) {
    PayloadReader reader(payload);
    auto path_len = reader.read_int<std::uint16_t>();
    if (!path_len || *path_len == 0)
        return std::nullopt;

    auto path = reader.read_bytes(*path_len);
    auto revision = reader.read_int<std::uint64_t>();
    if (!path || !revision || path->find('\0') != std::string_view::npos)
        return std::nullopt;

    auto content = reader.remaining_bytes();
    return WriteFileRequest{
        .path = std::string(*path),
        .expected_revision = *revision,
        .content = std::string(content),
    };
}

// ============================================================================
// Workspace 模块服务端 Encode 函数（构造返回给客户端的 Response / Event Payload）
// ============================================================================

// 编码 0x0201 (workspace_list_projects) 响应负载：
// 每个项目编码为 [u16 nameLen][name bytes]，多个项目直接顺序流式拼接，不设数量头。
std::string encode_workspace_list_projects(const std::vector<std::string> &projects) {
    std::size_t total_size = 0;
    for (const auto &name : projects) {
        if (name.empty() || name.size() > std::numeric_limits<std::uint16_t>::max())
            throw std::invalid_argument("project name exceeds u16 length limit");
        total_size += 2 + name.size();
    }

    std::string out;
    out.reserve(total_size);
    for (const auto &name : projects) {
        append_be(out, static_cast<std::uint16_t>(name.size()));
        out.append(name);
    }
    return out;
}

// 编码 0x0202 (workspace_list_dir) 响应与目录变化 Event 负载：
// 头部为所属目录相对路径 [u16 dirPathLen][dirPath bytes]，
// 随后每个文件/文件夹编码为 [u8 type][u16 nameLen][name bytes]，流式紧密拼接。
std::string encode_workspace_list_dir(std::string_view dir_path,
                                      const std::vector<DirEntry> &entries) {
    if (dir_path.empty() || dir_path.size() > std::numeric_limits<std::uint16_t>::max())
        throw std::invalid_argument("directory path exceeds u16 length limit");

    std::size_t total_size = 2 + dir_path.size();
    for (const auto &entry : entries) {
        if (entry.name.empty() || entry.name.size() > std::numeric_limits<std::uint16_t>::max())
            throw std::invalid_argument("dir entry name exceeds u16 length limit");
        total_size += 1 + 2 + entry.name.size();
    }

    std::string out;
    out.reserve(total_size);
    append_be(out, static_cast<std::uint16_t>(dir_path.size()));
    out.append(dir_path);

    for (const auto &entry : entries) {
        append_be(out, std::to_underlying(entry.type));
        append_be(out, static_cast<std::uint16_t>(entry.name.size()));
        out.append(entry.name);
    }
    return out;
}

std::string encode_workspace_list_dir(const ListDirResponse &response) {
    return encode_workspace_list_dir(response.path, response.entries);
}

// 编码 0x0203 (workspace_read_file) 响应负载：
// 格式：[u64 fileSize][u64 revision][content bytes...]
std::string encode_workspace_read_file(const ReadFileResponse &response) {
    std::string out;
    out.reserve(16 + response.content.size());
    append_be(out, response.file_size);
    append_be(out, response.revision);
    out.append(response.content);
    return out;
}

// 编码 0x0204 (workspace_write_file) 响应负载：
// 格式：[u64 revision]（写入成功时为 newRevision；版本冲突时为当前实际的 currentRevision）。
std::string encode_workspace_write_file(std::uint64_t revision) {
    std::string out;
    out.reserve(8);
    append_be(out, revision);
    return out;
}

using ::decode_terminal_input;
using ::decode_terminal_on_tab;
using ::decode_terminal_output;
using ::decode_terminal_payload;
using ::decode_terminal_payload_view;
using ::decode_terminal_state;
using ::decode_terminal_state_id;
using ::decode_terminal_state_view;
using ::encode_terminal_input;
using ::encode_terminal_on_tab;
using ::encode_terminal_output;
using ::encode_terminal_payload;
using ::encode_terminal_state;
using ::encode_terminal_state_id;

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

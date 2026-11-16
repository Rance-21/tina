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

// 0x0202 (workspace_list_dir) 零拷贝视图，直接引用 Frame::payload 切片。
struct ListDirRequestView {
    std::string_view path;
};

// 0x0203 (workspace_read_file) 零拷贝视图，直接引用 Frame::payload 切片。
struct ReadFileRequestView {
    std::string_view path;
    std::uint64_t offset{};
    std::uint32_t max_bytes{};
};

// 零拷贝解码 0x0202 (workspace_list_dir) 请求负载：
// 格式：[u16 pathLength][path bytes]
std::optional<ListDirRequestView> decode_workspace_list_dir_view(std::string_view payload) noexcept {
    PayloadReader reader(payload);
    auto path_len = reader.read_int<std::uint16_t>();
    if (!path_len || *path_len == 0)
        return std::nullopt;

    auto path = reader.read_bytes(*path_len);
    if (!path || !reader.finished() || path->find('\0') != std::string_view::npos)
        return std::nullopt;

    return ListDirRequestView{.path = *path};
}

// 零拷贝解码 0x0203 (workspace_read_file) 请求负载：
// 格式：[u16 pathLength][path bytes][u64 offset][u32 maxBytes]
std::optional<ReadFileRequestView>
decode_workspace_read_file_view(std::string_view payload) noexcept {
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

    return ReadFileRequestView{
        .path = *path,
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

// 零拷贝解码 0x0205 (workspace_close_dir) 请求负载：
// 格式：[u16 pathLength][path bytes]（即 len + data，同时兼容 4 字节大端 len 编码）。
// is_canonical_u16 非空时返回是否已满足标准 2 字节长度头，便于服务端原样零拷贝回传响应。
std::optional<std::string_view>
decode_workspace_close_dir_view(std::string_view payload, bool *is_canonical_u16 = nullptr) noexcept {
    PayloadReader reader(payload);
    auto path_len = reader.read_int<std::uint16_t>();
    if (!path_len)
        return std::nullopt;

    bool canonical = false;
    std::optional<std::string_view> path;
    if (*path_len > 0) {
        path = reader.read_bytes(*path_len);
        canonical = true;
    } else if (reader.remaining_size() >= 2) {
        auto low_len = reader.read_int<std::uint16_t>();
        if (low_len && *low_len > 0)
            path = reader.read_bytes(*low_len);
    }

    if (!path || !reader.finished() || path->find('\0') != std::string_view::npos)
        return std::nullopt;

    if (is_canonical_u16 != nullptr)
        *is_canonical_u16 = canonical;
    return *path;
}

// ============================================================================
// Workspace 模块服务端 Encode 函数（构造返回给客户端的 Response / Event Payload）
// ============================================================================

// 原地编码 0x0201 (workspace_list_projects) 响应负载进 out（复用已有缓冲区，避免临时字符串）。
// 每个项目编码为 [u16 nameLen][name bytes]，多个项目直接顺序流式拼接，不设数量头。
void encode_workspace_list_projects_into(const std::vector<std::string> &projects, std::string &out) {
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

// 原地编码 0x0202 (workspace_list_dir) 响应与目录变化 Event 负载进 out。
// 头部为所属目录相对路径 [u16 dirPathLen][dirPath bytes]，
// 随后每个文件/文件夹编码为 [u8 type][u16 nameLen][name bytes]，流式紧密拼接。
void encode_workspace_list_dir_into(std::string_view dir_path, const std::vector<DirEntry> &entries,
                                    std::string &out) {
    if (dir_path.empty() || dir_path.size() > std::numeric_limits<std::uint16_t>::max())
        throw std::invalid_argument("directory path exceeds u16 length limit");

    std::size_t total_size = 2 + dir_path.size();
    for (const auto &entry : entries) {
        if (entry.name.empty() || entry.name.size() > std::numeric_limits<std::uint16_t>::max())
            throw std::invalid_argument("dir entry name exceeds u16 length limit");
        total_size += 1 + 2 + entry.name.size();
    }

    out.clear();
    out.reserve(total_size);
    append_be(out, static_cast<std::uint16_t>(dir_path.size()));
    out.append(dir_path);

    for (const auto &entry : entries) {
        append_be(out, std::to_underlying(entry.type));
        append_be(out, static_cast<std::uint16_t>(entry.name.size()));
        out.append(entry.name);
    }
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

using ::decode_git_diff_request;
using ::decode_git_diff_request_view;
using ::decode_git_log_request;
using ::decode_git_path_request_view;
using ::decode_git_status_request;
using ::encode_git_diff_response;
using ::encode_git_log_response;
using ::encode_git_status_response;

} // namespace server

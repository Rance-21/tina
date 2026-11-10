export module ide.client_protocol;

import std;
export import ide.protocol;

namespace {

void validate_non_empty_u16_string(std::string_view text, const char *what) {
    if (text.empty() || text.size() > std::numeric_limits<std::uint16_t>::max() ||
        text.find('\0') != std::string_view::npos)
        throw std::invalid_argument(what);
}

} // namespace

export namespace client {

// ============================================================================
// Workspace 模块客户端 Encode 函数（构造发往服务端的 Request Payload）
// ============================================================================

// 编码 0x0011 (workspace_list_projects) 请求负载：
// 请求负载为空，仅列出 ~/projects 下的所有一级文件夹。
std::string encode_workspace_list_projects() {
    return {};
}

// 编码 0x0013 (workspace_list_dir) 请求负载：
// 格式：[u16 pathLength][path bytes]
std::string encode_workspace_list_dir(std::string_view path) {
    validate_non_empty_u16_string(path, "invalid workspace_list_dir path");

    std::string out;
    out.reserve(2 + path.size());
    append_be(out, static_cast<std::uint16_t>(path.size()));
    out.append(path);
    return out;
}

std::string encode_workspace_list_dir(const ListDirRequest &req) {
    return encode_workspace_list_dir(req.path);
}

// 编码 0x0014 (workspace_read_file) 请求负载：
// 格式：[u16 pathLength][path bytes][u64 offset][u32 maxBytes]
std::string encode_workspace_read_file(std::string_view path, std::uint64_t offset,
                                       std::uint32_t max_bytes) {
    validate_non_empty_u16_string(path, "invalid workspace_read_file path");
    if (max_bytes == 0 || max_bytes > max_file_read_bytes)
        throw std::invalid_argument("invalid workspace_read_file max_bytes");

    std::string out;
    out.reserve(2 + path.size() + 8 + 4);
    append_be(out, static_cast<std::uint16_t>(path.size()));
    out.append(path);
    append_be(out, offset);
    append_be(out, max_bytes);
    return out;
}

std::string encode_workspace_read_file(const ReadFileRequest &req) {
    return encode_workspace_read_file(req.path, req.offset, req.max_bytes);
}

// 编码 0x0015 (workspace_write_file) 请求负载：
// 格式：[u16 pathLen][path bytes][u64 revision][content bytes...]
std::string encode_workspace_write_file(std::string_view path, std::uint64_t expected_revision,
                                        std::string_view content) {
    validate_non_empty_u16_string(path, "invalid workspace_write_file path");

    std::string out;
    out.reserve(2 + path.size() + 8 + content.size());
    append_be(out, static_cast<std::uint16_t>(path.size()));
    out.append(path);
    append_be(out, expected_revision);
    out.append(content);
    return out;
}

std::string encode_workspace_write_file(const WriteFileRequest &req) {
    return encode_workspace_write_file(req.path, req.expected_revision, req.content);
}

// ============================================================================
// Workspace 模块客户端 Decode 函数（解析服务端返回的 Response / Event Payload）
// ============================================================================

// 解码 0x0011 (workspace_list_projects) 响应负载：
// 格式：流式拼接的 [u16 nameLen][name bytes]...
std::optional<std::vector<std::string>> decode_workspace_list_projects(std::string_view payload) {
    PayloadReader reader(payload);
    std::vector<std::string> projects;

    while (!reader.finished()) {
        auto name_len = reader.read_int<std::uint16_t>();
        if (!name_len || *name_len == 0)
            return std::nullopt;

        auto name = reader.read_bytes(*name_len);
        if (!name || name->find('\0') != std::string_view::npos)
            return std::nullopt;

        projects.emplace_back(*name);
    }

    return projects;
}

// 解码 0x0013 (workspace_list_dir) 响应与目录变化 Event 负载：
// 格式：[u16 dirPathLen][dirPath bytes] + 流式拼接的 [u8 type][u16 nameLen][name bytes]...
// 其中 type = 0（普通文件）、1（目录）、2（符号链接）。
std::optional<ListDirResponse> decode_workspace_list_dir(std::string_view payload) {
    PayloadReader reader(payload);
    auto path_len = reader.read_int<std::uint16_t>();
    if (!path_len || *path_len == 0)
        return std::nullopt;

    auto dir_path = reader.read_bytes(*path_len);
    if (!dir_path || dir_path->find('\0') != std::string_view::npos)
        return std::nullopt;

    ListDirResponse response{.path = std::string(*dir_path), .entries = {}};

    while (!reader.finished()) {
        auto raw_type = reader.read_int<std::uint8_t>();
        auto name_len = reader.read_int<std::uint16_t>();
        if (!raw_type || *raw_type > std::to_underlying(DirEntryType::symlink) || !name_len ||
            *name_len == 0)
            return std::nullopt;

        auto name = reader.read_bytes(*name_len);
        if (!name || name->find('\0') != std::string_view::npos)
            return std::nullopt;

        response.entries.push_back(DirEntry{
            .type = static_cast<DirEntryType>(*raw_type),
            .name = std::string(*name),
        });
    }

    return response;
}

// 解码 0x0014 (workspace_read_file) 响应负载：
// 格式：[u64 fileSize][u64 revision][UTF-8 bytes...]
std::optional<ReadFileResponse> decode_workspace_read_file(std::string_view payload) {
    PayloadReader reader(payload);
    auto file_size = reader.read_int<std::uint64_t>();
    auto revision = reader.read_int<std::uint64_t>();
    if (!file_size || !revision)
        return std::nullopt;

    return ReadFileResponse{
        .file_size = *file_size,
        .revision = *revision,
        .content = std::string(reader.remaining_bytes()),
    };
}

// 解码 0x0015 (workspace_write_file) 响应负载：
// 格式：[u64 newRevision]（写入成功时为新 revision，冲突 Status::conflict 时为服务端当前最新 revision）。
std::optional<std::uint64_t> decode_workspace_write_file(std::string_view payload) {
    PayloadReader reader(payload);
    auto revision = reader.read_int<std::uint64_t>();
    if (!revision || !reader.finished())
        return std::nullopt;

    return *revision;
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

} // namespace client

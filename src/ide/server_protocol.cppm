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
// is_canonical_u16 非空时返回是否已满足标准 2 字节长度头，便于服务端原样零拷贝回传响应。
std::optional<std::string_view>
decode_workspace_close_dir_view(std::string_view payload,
                                bool *is_canonical_u16 = nullptr) noexcept {
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

// 原地编码 0x0202 (workspace_list_dir) 响应与目录变化 Event 负载进 out。
// 头部为所属目录相对路径 [u16 dirPathLen][dirPath bytes]，
// 随后每个文件/文件夹编码为 [u8 type][u16 nameLen][name bytes]，流式紧密拼接。
// 给 I/O 分类器传入内联预算，给普通编码器传入无限预算。预算不足或字段非法时返回
// false，out 仍保留原请求，Worker 可以继续编码或按原来的错误路径处理。
bool try_encode_list_dir(std::string_view dir_path, const std::vector<DirEntry> &entries,
                         std::string &out, std::size_t budget) {
    constexpr auto max_name = std::numeric_limits<std::uint16_t>::max();
    if (dir_path.empty() || dir_path.size() > max_name)
        return false;

    std::size_t size = 2 + dir_path.size();
    // 每个合法目录项至少占四字节（类型、长度、一个名字节）。条目数已超预算时，
    // I/O 线程可以直接交给 Worker，连名字都无需遍历。
    if (size > budget || entries.size() > (budget - size) / 4)
        return false;
    for (const auto &entry : entries) {
        if (entry.name.empty() || entry.name.size() > max_name)
            return false;
        size += 3 + entry.name.size();
    }
    if (size > budget)
        return false;

    // 上面已算出精确长度。逐项直接写入最终位置，每个名字只拷贝一次，
    // 避免对短小目录项反复扩展 string、检查容量并写结尾的 NUL。
    out.clear(); // 扩容时也不用复制原请求里的路径前缀。
    out.resize_and_overwrite(size, [&](char *data, std::size_t) noexcept {
        auto write_name = [](char *dst, std::string_view name) {
            write_be(dst, static_cast<std::uint16_t>(name.size()));
            std::memcpy(dst + 2, name.data(), name.size());
            return dst + 2 + name.size();
        };
        char *next = write_name(data, dir_path);
        for (const auto &entry : entries) {
            *next++ = static_cast<char>(entry.type);
            next = write_name(next, entry.name);
        }
        return size;
    });
    return true;
}

void encode_workspace_list_dir_into(std::string_view dir_path, const std::vector<DirEntry> &entries,
                                    std::string &out) {
    if (try_encode_list_dir(dir_path, entries, out, std::numeric_limits<std::size_t>::max()))
        return;
    if (dir_path.empty() || dir_path.size() > std::numeric_limits<std::uint16_t>::max())
        throw std::invalid_argument("directory path exceeds u16 length limit");
    throw std::invalid_argument("dir entry name exceeds u16 length limit");
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

// 只编码 16 字节文件头；复用缓存已有的控制块保活内容切片，直到 I/O 线程发送完毕。
void encode_workspace_read_file(Frame &frame, std::shared_ptr<const ReadFileResponse> cached,
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

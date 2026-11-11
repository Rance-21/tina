export module ide.protocol;

import std;
import net.buffer;
export import rpc.protocol;

// 使用 memcpy + std::byteswap 实现单指令大端序整数字节序转换，消除逐字节循环移位开销，
// 同时保证不依赖机器原生字节序或非对齐指针强转。
export template <std::unsigned_integral T>
[[nodiscard]] inline T read_be(const char *bytes) noexcept {
    if constexpr (sizeof(T) == 1) {
        return static_cast<T>(static_cast<unsigned char>(bytes[0]));
    } else {
        T value{};
        std::memcpy(&value, bytes, sizeof(T));
        if constexpr (std::endian::native == std::endian::little)
            value = std::byteswap(value);
        return value;
    }
}

export template <std::unsigned_integral T> inline void write_be(char *bytes, T value) noexcept {
    if constexpr (sizeof(T) == 1) {
        bytes[0] = static_cast<char>(value);
    } else {
        if constexpr (std::endian::native == std::endian::little)
            value = std::byteswap(value);
        std::memcpy(bytes, &value, sizeof(T));
    }
}

// 向字符串末尾追加大端序无符号整数（支持 u8 / u16 / u32 / u64）。
export template <std::unsigned_integral T> inline void append_be(std::string &out, T value) {
    if constexpr (sizeof(T) == 1) {
        out.push_back(static_cast<char>(value));
    } else {
        if constexpr (std::endian::native == std::endian::little)
            value = std::byteswap(value);
        out.append(reinterpret_cast<const char *>(&value), sizeof(T));
    }
}

// 按大端序顺序切片解析二进制 Payload 的辅助读取器。
export class PayloadReader {
  public:
    explicit PayloadReader(std::string_view input) : input_(input) {
    }

    // 读取一个大端序无符号整数 T（如 uint8_t / uint16_t / uint32_t / uint64_t）。
    template <typename T> std::optional<T> read_int() {
        if (sizeof(T) > remaining_size())
            return std::nullopt;

        T value = read_be<T>(input_.data() + pos_);
        pos_ += sizeof(T);
        return value;
    }

    // 读取指定字节数的切片（零拷贝返回 string_view）。
    std::optional<std::string_view> read_bytes(std::size_t count) {
        if (count > remaining_size())
            return std::nullopt;

        auto slice = input_.substr(pos_, count);
        pos_ += count;
        return slice;
    }

    // 读取剩余的所有字节。
    [[nodiscard]] std::string_view remaining_bytes() const noexcept {
        return input_.substr(pos_);
    }

    [[nodiscard]] std::size_t remaining_size() const noexcept {
        return input_.size() - pos_;
    }

    [[nodiscard]] bool finished() const noexcept {
        return pos_ == input_.size();
    }

  private:
    std::string_view input_;
    std::size_t pos_{0};
};

static bool valid_header(const Frame &frame) {
    switch (frame.kind) {
    case Kind::request:
        return frame.request_id != 0 && frame.status == Status::ok;
    case Kind::response:
        return frame.request_id != 0 && frame.status <= Status::timeout;
    case Kind::event:
        return frame.request_id == 0 && frame.status == Status::ok;
    }
    return false;
}

// 客户端和服务端共用同一帧头。Length 不含自己的 4 字节，按当前负载重新计算。
export void encode_frame(const Frame &frame, net::Buffer &out) {
    if (frame.payload.size() > max_frame_length - min_frame_length || !valid_header(frame))
        throw std::invalid_argument("invalid RPC frame");

    std::array<char, frame_header_size> header;
    write_be(header.data(), min_frame_length + static_cast<std::uint32_t>(frame.payload.size()));

    header[4] = static_cast<char>(frame.kind);

    write_be(header.data() + 5, frame.request_id);
    write_be(header.data() + 13, std::to_underlying(frame.method));
    write_be(header.data() + 15, std::to_underlying(frame.status));

    out.append(header.data(), header.size());
    out.append(frame.payload.data(), frame.payload.size());
}

// 每次只取一帧。半包留在输入缓冲区；非法长度立即报错，不按外部长度预分配。
export std::optional<Frame> decode_frame(net::Buffer &in) {
    if (in.readable_bytes() < 4)
        return std::nullopt;

    const char *bytes = in.data();
    const auto length = read_be<std::uint32_t>(bytes);

    if (length < min_frame_length || length > max_frame_length)
        throw std::invalid_argument("invalid RPC frame length");

    if (in.readable_bytes() < 4 + length)
        return std::nullopt;

    Frame frame{.length = length,
                .kind = static_cast<Kind>(bytes[4]),
                .request_id = read_be<std::uint64_t>(bytes + 5),
                .method = static_cast<Method>(read_be<std::uint16_t>(bytes + 13)),
                .status = static_cast<Status>(read_be<std::uint16_t>(bytes + 15)),
                .payload = {}};

    if (!valid_header(frame))
        throw std::invalid_argument("invalid RPC frame header");

    frame.payload.assign(bytes + frame_header_size, length - min_frame_length);
    in.retrieve(4 + length);
    return frame;
}

// ping 的请求负载为空，成功响应是四个文本字节 PONG。
export void handle_ping(Frame &frame) {
    if (frame.payload.empty()) {
        frame.payload = "PONG";
    } else {
        frame.status = Status::invalid_argument;
        frame.payload = "ping requires an empty payload";
    }
}

// ============================================================================
// Workspace 模块协议公共数据结构（客户端与服务端共用）
// ============================================================================

// workspace_list_dir 响应/事件中的目录项类型编码（1 字节 u8）。
export enum class DirEntryType : std::uint8_t {
    file = 0,      // 普通文件
    directory = 1, // 目录
    symlink = 2    // 符号链接
};

// workspace_list_dir 单个文件或子目录项。
export struct DirEntry {
    DirEntryType type;
    std::string name;
};

// 0x0202 (workspace_list_dir) 请求参数。
export struct ListDirRequest {
    std::string path; // 相对于 ~/projects 的路径（如 "tina" 或 "tina/src"）
};

// 0x0202 (workspace_list_dir) 响应与目录变化 Event 的解码结果。
export struct ListDirResponse {
    std::string path;              // 所属目录相对路径（如 "tina" 或 "tina/src"）
    std::vector<DirEntry> entries; // 当前目录下的一级子项列表（深度为 1）
};

// 0x0203 (workspace_read_file) 请求参数。
export struct ReadFileRequest {
    std::string path;          // 相对于 ~/projects 的文件路径
    std::uint64_t offset{};    // 读取起始字节偏移
    std::uint32_t max_bytes{}; // 本次最多读取的字节数
};

// 0x0203 (workspace_read_file) 响应结果。
export struct ReadFileResponse {
    std::uint64_t file_size{}; // 文件当前总字节数
    std::uint64_t revision{};  // 文件当前 64 位版本指纹
    std::string content;       // 本次读取到的 UTF-8 字节内容
};

// 0x0204 (workspace_write_file) 请求参数。
export struct WriteFileRequest {
    std::string path;                  // 相对于 ~/projects 的文件路径
    std::uint64_t expected_revision{}; // 客户端期望的原文件版本指纹
    std::string content;               // 待保存的完整 UTF-8 文本内容
};

// ============================================================================
// Terminal 模块协议公共数据结构与编解码函数
// ============================================================================
// 帧内 Payload 编码规则：
//   1. terminal_state (0x0303)               : [u32 len][data (len bytes)]
//   2. terminal_input / on_tab / output (其余): [u32 len][u32 terminal_id][data (len bytes)]

// 携带 terminal_id 与文本/字节数据的通用终端消息结构（用于 input / on_tab / output）。
export struct TerminalMessage {
    std::uint32_t terminal_id{};
    std::string data;
};

// 零拷贝切片视图版本的终端消息结构（直接引用 Frame::payload 内存，不分配新 std::string）。
export struct TerminalMessageView {
    std::uint32_t terminal_id{};
    std::string_view data;
};

// 编码非 state 的终端负载：[u32 len][u32 terminal_id][data]
export inline std::string encode_terminal_payload(std::uint32_t terminal_id,
                                                  std::string_view data) {
    if (data.size() > max_frame_length - min_frame_length - 8)
        throw std::invalid_argument("terminal payload exceeds maximum frame length");

    std::string out;
    out.reserve(8 + data.size());
    append_be(out, static_cast<std::uint32_t>(data.size()));
    append_be(out, terminal_id);
    out.append(data);
    return out;
}

export inline std::string encode_terminal_payload(const TerminalMessage &msg) {
    return encode_terminal_payload(msg.terminal_id, msg.data);
}

// 零拷贝解码非 state 的终端负载：[u32 len][u32 terminal_id][data]
export inline std::optional<TerminalMessageView>
decode_terminal_payload_view(std::string_view payload) noexcept {
    PayloadReader reader(payload);
    auto len = reader.read_int<std::uint32_t>();
    auto terminal_id = reader.read_int<std::uint32_t>();
    if (!len || !terminal_id)
        return std::nullopt;

    auto data = reader.read_bytes(*len);
    if (!data || !reader.finished())
        return std::nullopt;

    return TerminalMessageView{
        .terminal_id = *terminal_id,
        .data = *data,
    };
}

// 解码非 state 的终端负载：[u32 len][u32 terminal_id][data]
export inline std::optional<TerminalMessage> decode_terminal_payload(std::string_view payload) {
    auto view = decode_terminal_payload_view(payload);
    if (!view)
        return std::nullopt;

    return TerminalMessage{
        .terminal_id = view->terminal_id,
        .data = std::string(view->data),
    };
}

// 0x0301 (terminal_input)：[u32 len][u32 terminal_id][data]
export inline std::string encode_terminal_input(std::uint32_t terminal_id,
                                                std::string_view command) {
    return encode_terminal_payload(terminal_id, command);
}

export inline std::optional<TerminalMessage> decode_terminal_input(std::string_view payload) {
    return decode_terminal_payload(payload);
}

// 0x0302 (terminal_on_tab)：[u32 len][u32 terminal_id][data]
export inline std::string encode_terminal_on_tab(std::uint32_t terminal_id,
                                                 std::string_view input) {
    return encode_terminal_payload(terminal_id, input);
}

export inline std::optional<TerminalMessage> decode_terminal_on_tab(std::string_view payload) {
    return decode_terminal_payload(payload);
}

// 0x0381 (terminal_output)：[u32 len][u32 terminal_id][data]
export inline std::string encode_terminal_output(std::uint32_t terminal_id,
                                                 std::string_view output) {
    return encode_terminal_payload(terminal_id, output);
}

export inline std::optional<TerminalMessage> decode_terminal_output(std::string_view payload) {
    return decode_terminal_payload(payload);
}

// 0x0303 (terminal_state)：[u32 len][data]
export inline std::string encode_terminal_state(std::string_view data) {
    if (data.size() > max_frame_length - min_frame_length - 4)
        throw std::invalid_argument("terminal_state payload exceeds maximum frame length");

    std::string out;
    out.reserve(4 + data.size());
    append_be(out, static_cast<std::uint32_t>(data.size()));
    out.append(data);
    return out;
}

// 零拷贝解码 0x0303 (terminal_state)：[u32 len][data]
export inline std::optional<std::string_view>
decode_terminal_state_view(std::string_view payload) noexcept {
    PayloadReader reader(payload);
    auto len = reader.read_int<std::uint32_t>();
    if (!len)
        return std::nullopt;

    auto data = reader.read_bytes(*len);
    if (!data || !reader.finished())
        return std::nullopt;

    return *data;
}

// 解码 0x0303 (terminal_state)：[u32 len][data]
export inline std::optional<std::string> decode_terminal_state(std::string_view payload) {
    auto view = decode_terminal_state_view(payload);
    if (!view)
        return std::nullopt;

    return std::string(*view);
}

// 将 32 位大端 terminal_id 作为 data 封装进 terminal_state 的 [u32 len=4][u32 terminal_id] 负载中。
// 约定：
//   - Request 中 terminal_id == 0（或 data 为空 / "start"）表示请求启动新 Shell；
//   - Request 中 terminal_id > 0 表示请求关闭指定 terminal_id 的 Shell；
//   - Response / Event 中携带对应的 4 字节大端 terminal_id。
export inline std::string encode_terminal_state_id(std::uint32_t terminal_id) {
    std::string out;
    out.reserve(8);
    append_be(out, static_cast<std::uint32_t>(sizeof(std::uint32_t)));
    append_be(out, terminal_id);
    return out;
}

// 从 terminal_state 负载 [u32 len][data] 中零拷贝解析出 terminal_id：
//   - 若 data 为空或 "start"：返回 0（代表启动新 Shell）；
//   - 若 data 恰为 4 字节：按大端序 u32 解析 terminal_id。
export inline std::optional<std::uint32_t> decode_terminal_state_id(std::string_view payload) {
    auto data = decode_terminal_state_view(payload);
    if (!data)
        return std::nullopt;

    if (data->empty() || *data == "start")
        return std::uint32_t{0};

    if (data->size() == sizeof(std::uint32_t))
        return read_be<std::uint32_t>(data->data());

    return std::nullopt;
}

// ============================================================================
// Git 模块协议公共数据结构与编解码函数（0x0401 / 0x0402 / 0x0403）
// ============================================================================
// 设计原则：
//   - 紧凑高效，不携带任何校验码（CRC）或冗余外层长度/条数头（Frame 已携带总 Length）；
//   - 服务端解码 Request 优先使用 string_view 零拷贝视图，避免在 Frame::payload 之上二次分配 std::string；
//   - 仅对单行轻量包装函数使用 inline，包含循环或复杂分支的编解码函数不加 inline，避免代码膨胀污染 L1 I-Cache。

// git_log 默认最大拉取提交数量。
export inline constexpr std::uint32_t git_log_limit = 100;

// 0x0401 (git_status) 请求参数：
// path 为相对于 ~/projects 的仓库路径（如 "tina" 或 "demo"）；为空时代表服务端默认工作目录。
export struct GitStatusRequest {
    std::string path;
};

// 0x0401 (git_status) 单个文件状态项（对应 git status --porcelain=v1 -z 的 XY 与路径）。
export struct GitStatusEntry {
    char index_status{' '};
    char worktree_status{' '};
    std::string path;
};

// 0x0402 (git_diff) 请求参数（拥有所有权版本与零拷贝视图版本）。
export struct GitDiffRequest {
    std::string path;
    std::string base_branch;
    std::string target_branch;
};

export struct GitDiffRequestView {
    std::string_view path;
    std::string_view base_branch;
    std::string_view target_branch;
};

// 0x0403 (git_log) 请求参数。
export struct GitLogRequest {
    std::string path;
};

// 0x0403 (git_log) 单条提交记录（用于客户端还原 Git 提交历史图 DAG）。
export struct GitCommit {
    std::string hash; // 提交短哈希（如 "20f0df3"）
    std::vector<std::string>
        parents;               // 父提交短哈希列表（普通提交 1 个，Merge >= 2 个，根提交 0 个）
    std::string refs;          // 分支与标签装饰（如 "HEAD -> main, origin/main"）
    std::string author;        // 作者名
    std::uint64_t timestamp{}; // 提交时间戳（Unix 秒）
    std::string subject;       // 提交单行标题摘要
};

// ----------------------------------------------------------------------------
// 1. 0x0401 (git_status) & 0x0403 (git_log) 共用单路径请求编解码
//    Request  Payload : [path bytes...]（直接作为帧负载，无多余长度头）
//    Response Payload : 流式拼接 [u8 index_status][u8 worktree_status][u16 path_len][path bytes]...
// ----------------------------------------------------------------------------
export std::string encode_git_status_request(std::string_view path = {}) {
    if (path.size() > std::numeric_limits<std::uint16_t>::max() ||
        path.find('\0') != std::string_view::npos)
        throw std::invalid_argument("invalid git request path");
    return std::string(path);
}

export inline std::string encode_git_status_request(const GitStatusRequest &req) {
    return encode_git_status_request(req.path);
}

// 零拷贝校验并返回指向 Frame::payload 内部的仓库路径视图（供服务端直接消费，不分配新 std::string）。
export inline std::optional<std::string_view>
decode_git_path_request_view(std::string_view payload) noexcept {
    if (payload.size() > std::numeric_limits<std::uint16_t>::max() ||
        payload.find('\0') != std::string_view::npos)
        return std::nullopt;
    return payload;
}

export inline std::optional<GitStatusRequest> decode_git_status_request(std::string_view payload) {
    auto view = decode_git_path_request_view(payload);
    if (!view)
        return std::nullopt;
    return GitStatusRequest{.path = std::string(*view)};
}

export std::string encode_git_status_response(std::span<const GitStatusEntry> entries) {
    std::size_t total_size = 0;
    for (const auto &entry : entries) {
        if (entry.path.empty() || entry.path.size() > std::numeric_limits<std::uint16_t>::max() ||
            entry.path.find('\0') != std::string::npos)
            throw std::invalid_argument("invalid git_status entry path");
        total_size += 1 + 1 + 2 + entry.path.size();
    }

    std::string out;
    out.reserve(total_size);
    for (const auto &entry : entries) {
        append_be(out, static_cast<std::uint8_t>(entry.index_status));
        append_be(out, static_cast<std::uint8_t>(entry.worktree_status));
        append_be(out, static_cast<std::uint16_t>(entry.path.size()));
        out.append(entry.path);
    }
    return out;
}

export std::optional<std::vector<GitStatusEntry>>
decode_git_status_response(std::string_view payload) {
    PayloadReader reader(payload);
    std::vector<GitStatusEntry> entries;

    while (!reader.finished()) {
        auto x = reader.read_int<std::uint8_t>();
        auto y = reader.read_int<std::uint8_t>();
        auto path_len = reader.read_int<std::uint16_t>();
        if (!x || !y || !path_len || *path_len == 0)
            return std::nullopt;

        auto path = reader.read_bytes(*path_len);
        if (!path || path->find('\0') != std::string_view::npos)
            return std::nullopt;

        entries.push_back(GitStatusEntry{
            .index_status = static_cast<char>(*x),
            .worktree_status = static_cast<char>(*y),
            .path = std::string(*path),
        });
    }
    return entries;
}

// ----------------------------------------------------------------------------
// 2. 0x0402 (git_diff) 编解码
//    Request  Payload : [u16 path_len][path][u16 base_len][base_branch][target_branch (剩余字节)]
//    Response Payload : [Unified Diff 原始 UTF-8 字节流...]（无多余长度头）
// ----------------------------------------------------------------------------
export std::string encode_git_diff_request(std::string_view path, std::string_view base_branch,
                                           std::string_view target_branch) {
    if (path.size() > std::numeric_limits<std::uint16_t>::max() ||
        path.find('\0') != std::string_view::npos)
        throw std::invalid_argument("invalid git_diff path");

    if (base_branch.empty() || base_branch.size() > std::numeric_limits<std::uint16_t>::max() ||
        base_branch.find('\0') != std::string_view::npos || target_branch.empty() ||
        target_branch.size() > std::numeric_limits<std::uint16_t>::max() ||
        target_branch.find('\0') != std::string_view::npos)
        throw std::invalid_argument("invalid git_diff branch");

    std::string out;
    out.reserve(2 + path.size() + 2 + base_branch.size() + target_branch.size());
    append_be(out, static_cast<std::uint16_t>(path.size()));
    out.append(path);
    append_be(out, static_cast<std::uint16_t>(base_branch.size()));
    out.append(base_branch);
    out.append(target_branch);
    return out;
}

export inline std::string encode_git_diff_request(std::string_view base_branch,
                                                  std::string_view target_branch) {
    return encode_git_diff_request({}, base_branch, target_branch);
}

export inline std::string encode_git_diff_request(const GitDiffRequest &req) {
    return encode_git_diff_request(req.path, req.base_branch, req.target_branch);
}

// 零拷贝解码 0x0402 (git_diff) 请求负载：直接返回指向 Frame::payload 的切片视图，零堆内存分配。
export std::optional<GitDiffRequestView>
decode_git_diff_request_view(std::string_view payload) noexcept {
    PayloadReader reader(payload);
    auto path_len = reader.read_int<std::uint16_t>();
    if (!path_len)
        return std::nullopt;

    auto path = reader.read_bytes(*path_len);
    auto base_len = reader.read_int<std::uint16_t>();
    if (!path || !base_len || *base_len == 0)
        return std::nullopt;

    auto base_branch = reader.read_bytes(*base_len);
    if (!base_branch)
        return std::nullopt;

    auto target_branch = reader.remaining_bytes();
    if (target_branch.empty() || target_branch.size() > std::numeric_limits<std::uint16_t>::max() ||
        path->find('\0') != std::string_view::npos ||
        base_branch->find('\0') != std::string_view::npos ||
        target_branch.find('\0') != std::string_view::npos)
        return std::nullopt;

    return GitDiffRequestView{
        .path = *path,
        .base_branch = *base_branch,
        .target_branch = target_branch,
    };
}

export std::optional<GitDiffRequest> decode_git_diff_request(std::string_view payload) {
    auto view = decode_git_diff_request_view(payload);
    if (!view)
        return std::nullopt;

    return GitDiffRequest{
        .path = std::string(view->path),
        .base_branch = std::string(view->base_branch),
        .target_branch = std::string(view->target_branch),
    };
}

export inline std::string encode_git_diff_response(std::string_view diff_output) {
    return std::string(diff_output);
}

export inline std::string_view decode_git_diff_response_view(std::string_view payload) noexcept {
    return payload;
}

export inline std::string decode_git_diff_response(std::string_view payload) {
    return std::string(payload);
}

// ----------------------------------------------------------------------------
// 3. 0x0403 (git_log) 编解码
//    Request  Payload : [path bytes...]（直接作为帧负载，无多余长度头）
//    Response Payload : 流式拼接每条 Commit：
//                       [u8 hash_len][hash]
//                       [u8 parent_count] + ([u8 parent_len][parent_hash]) * parent_count
//                       [u16 refs_len][refs]
//                       [u16 author_len][author]
//                       [u64 timestamp]
//                       [u16 subject_len][subject]
// ----------------------------------------------------------------------------
export inline std::string encode_git_log_request(std::string_view path = {}) {
    return encode_git_status_request(path);
}

export inline std::string encode_git_log_request(const GitLogRequest &req) {
    return encode_git_status_request(req.path);
}

export inline std::optional<GitLogRequest> decode_git_log_request(std::string_view payload) {
    auto view = decode_git_path_request_view(payload);
    if (!view)
        return std::nullopt;
    return GitLogRequest{.path = std::string(*view)};
}

export std::string encode_git_log_response(std::span<const GitCommit> commits) {
    std::size_t total_size = 0;
    for (const auto &c : commits) {
        if (c.hash.empty() || c.hash.size() > std::numeric_limits<std::uint8_t>::max() ||
            c.parents.size() > std::numeric_limits<std::uint8_t>::max() ||
            c.refs.size() > std::numeric_limits<std::uint16_t>::max() ||
            c.author.size() > std::numeric_limits<std::uint16_t>::max() ||
            c.subject.size() > std::numeric_limits<std::uint16_t>::max())
            throw std::invalid_argument("git_log commit field exceeds size limit");

        total_size += 1 + c.hash.size() + 1;
        for (const auto &p : c.parents) {
            if (p.empty() || p.size() > std::numeric_limits<std::uint8_t>::max())
                throw std::invalid_argument("git_log parent hash exceeds u8 limit");
            total_size += 1 + p.size();
        }
        total_size += 2 + c.refs.size() + 2 + c.author.size() + 8 + 2 + c.subject.size();
    }

    std::string out;
    out.reserve(total_size);
    for (const auto &c : commits) {
        append_be(out, static_cast<std::uint8_t>(c.hash.size()));
        out.append(c.hash);

        append_be(out, static_cast<std::uint8_t>(c.parents.size()));
        for (const auto &p : c.parents) {
            append_be(out, static_cast<std::uint8_t>(p.size()));
            out.append(p);
        }

        append_be(out, static_cast<std::uint16_t>(c.refs.size()));
        out.append(c.refs);

        append_be(out, static_cast<std::uint16_t>(c.author.size()));
        out.append(c.author);

        append_be(out, c.timestamp);

        append_be(out, static_cast<std::uint16_t>(c.subject.size()));
        out.append(c.subject);
    }
    return out;
}

export std::optional<std::vector<GitCommit>>
decode_git_log_response(std::string_view payload) {
    PayloadReader reader(payload);
    std::vector<GitCommit> commits;

    while (!reader.finished()) {
        auto hash_len = reader.read_int<std::uint8_t>();
        if (!hash_len || *hash_len == 0)
            return std::nullopt;
        auto hash = reader.read_bytes(*hash_len);
        auto parent_count = reader.read_int<std::uint8_t>();
        if (!hash || !parent_count)
            return std::nullopt;

        std::vector<std::string> parents;
        parents.reserve(*parent_count);
        for (std::uint8_t i = 0; i < *parent_count; ++i) {
            auto plen = reader.read_int<std::uint8_t>();
            if (!plen || *plen == 0)
                return std::nullopt;
            auto phash = reader.read_bytes(*plen);
            if (!phash)
                return std::nullopt;
            parents.emplace_back(*phash);
        }

        auto refs_len = reader.read_int<std::uint16_t>();
        if (!refs_len)
            return std::nullopt;
        auto refs = reader.read_bytes(*refs_len);

        auto author_len = reader.read_int<std::uint16_t>();
        if (!refs || !author_len)
            return std::nullopt;
        auto author = reader.read_bytes(*author_len);

        auto timestamp = reader.read_int<std::uint64_t>();
        auto subject_len = reader.read_int<std::uint16_t>();
        if (!author || !timestamp || !subject_len)
            return std::nullopt;
        auto subject = reader.read_bytes(*subject_len);
        if (!subject)
            return std::nullopt;

        commits.push_back(GitCommit{
            .hash = std::string(*hash),
            .parents = std::move(parents),
            .refs = std::string(*refs),
            .author = std::string(*author),
            .timestamp = *timestamp,
            .subject = std::string(*subject),
        });
    }
    return commits;
}

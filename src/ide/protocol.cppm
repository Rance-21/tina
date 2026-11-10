export module ide.protocol;

import std;
import net.buffer;
export import rpc.protocol;

// 从高字节到低字节逐项读写，帧头与负载整数不依赖机器的字节序或结构体对齐。
export template <typename T> T read_be(const char *bytes) {
    T value{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        if constexpr (sizeof(T) > 1)
            value = static_cast<T>(value << 8);
        value = static_cast<T>(value | static_cast<unsigned char>(bytes[i]));
    }
    return value;
}

export template <typename T> void write_be(char *bytes, T value) {
    for (std::size_t i = sizeof(T); i > 0; --i) {
        bytes[i - 1] = static_cast<char>(value & 0xff);
        if constexpr (sizeof(T) > 1)
            value >>= 8;
    }
}

// 向字符串末尾追加大端序无符号整数（支持 u8 / u16 / u32 / u64）。
export template <typename T> void append_be(std::string &out, T value) {
    std::array<char, sizeof(T)> bytes{};
    write_be(bytes.data(), value);
    out.append(bytes.data(), bytes.size());
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

// 0x0013 (workspace_list_dir) 请求参数。
export struct ListDirRequest {
    std::string path; // 相对于 ~/projects 的路径（如 "tina" 或 "tina/src"）
};

// 0x0013 (workspace_list_dir) 响应与目录变化 Event 的解码结果。
export struct ListDirResponse {
    std::string path;              // 所属目录相对路径（如 "tina" 或 "tina/src"）
    std::vector<DirEntry> entries; // 当前目录下的一级子项列表（深度为 1）
};

// 0x0014 (workspace_read_file) 请求参数。
export struct ReadFileRequest {
    std::string path;         // 相对于 ~/projects 的文件路径
    std::uint64_t offset{};   // 读取起始字节偏移
    std::uint32_t max_bytes{}; // 本次最多读取的字节数
};

// 0x0014 (workspace_read_file) 响应结果。
export struct ReadFileResponse {
    std::uint64_t file_size{}; // 文件当前总字节数
    std::uint64_t revision{};  // 文件当前 64 位版本指纹
    std::string content;       // 本次读取到的 UTF-8 字节内容
};

// 0x0015 (workspace_write_file) 请求参数。
export struct WriteFileRequest {
    std::string path;                  // 相对于 ~/projects 的文件路径
    std::uint64_t expected_revision{}; // 客户端期望的原文件版本指纹
    std::string content;               // 待保存的完整 UTF-8 文本内容
};

// ============================================================================
// Terminal 模块协议公共数据结构与编解码函数
// ============================================================================
// 帧内 Payload 编码规则：
//   1. terminal_state (0x0282)               : [u32 len][data (len bytes)]
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

// 0x0021 (terminal_input)：[u32 len][u32 terminal_id][data]
export inline std::string encode_terminal_input(std::uint32_t terminal_id,
                                                std::string_view command) {
    return encode_terminal_payload(terminal_id, command);
}

export inline std::optional<TerminalMessage> decode_terminal_input(std::string_view payload) {
    return decode_terminal_payload(payload);
}

// 0x0022 (terminal_on_tab)：[u32 len][u32 terminal_id][data]
export inline std::string encode_terminal_on_tab(std::uint32_t terminal_id,
                                                 std::string_view input) {
    return encode_terminal_payload(terminal_id, input);
}

export inline std::optional<TerminalMessage> decode_terminal_on_tab(std::string_view payload) {
    return decode_terminal_payload(payload);
}

// 0x0281 (terminal_output)：[u32 len][u32 terminal_id][data]
export inline std::string encode_terminal_output(std::uint32_t terminal_id,
                                                 std::string_view output) {
    return encode_terminal_payload(terminal_id, output);
}

export inline std::optional<TerminalMessage> decode_terminal_output(std::string_view payload) {
    return decode_terminal_payload(payload);
}

// 0x0282 (terminal_state)：[u32 len][data]
export inline std::string encode_terminal_state(std::string_view data) {
    if (data.size() > max_frame_length - min_frame_length - 4)
        throw std::invalid_argument("terminal_state payload exceeds maximum frame length");

    std::string out;
    out.reserve(4 + data.size());
    append_be(out, static_cast<std::uint32_t>(data.size()));
    out.append(data);
    return out;
}

// 零拷贝解码 0x0282 (terminal_state)：[u32 len][data]
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

// 解码 0x0282 (terminal_state)：[u32 len][data]
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

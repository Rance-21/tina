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

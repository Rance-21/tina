export module ide.protocol;

import std;
import net.buffer;
export import rpc.protocol;

// 从高字节到低字节逐项读写，帧头不依赖机器的字节序或结构体对齐。
template <typename T> static T read_be(const char *bytes) {
    T value{};
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value = static_cast<T>((value << 8) | static_cast<unsigned char>(bytes[i]));
    return value;
}

template <typename T> static void write_be(char *bytes, T value) {
    for (std::size_t i = sizeof(T); i > 0; --i) {
        bytes[i - 1] = static_cast<char>(value & 0xff);
        value >>= 8;
    }
}

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

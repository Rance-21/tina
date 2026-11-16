export module rpc.frame;

import std;
import net.buffer.storage;
export import rpc.protocol;
export import rpc.payload;

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

// 将 Frame 的 5 个头部字段编码为 17 字节大端序固定帧头写入 dst。
// 注意：这里只写 17 字节帧头，绝对不拷贝 frame.payload；
// frame.payload 通过 std::move 直接交给发送队列，由 ::writev() 将帧头和负载一起聚集发送。
export void encode_frame_header(const Frame &frame, char *dst) {
    constexpr std::size_t max_payload = max_frame_length - min_frame_length;
    const std::size_t payload_size = frame.payload_size();

    if (frame.external_payload.size() > max_payload ||
        frame.payload.size() > max_payload - frame.external_payload.size() || !valid_header(frame))
        throw std::invalid_argument("invalid RPC frame");

    // 17 字节帧头布局：
    // [0..3]  : Length (4B, 不含自身的 4 字节 = 13B 剩余帧头 + Payload 长度)
    // [4]     : Kind (1B, request / response / event)
    // [5..12] : Request ID (8B)
    // [13..14]: Method (2B)
    // [15..16]: Status (2B)
    write_be(dst, min_frame_length + static_cast<std::uint32_t>(payload_size));
    dst[4] = static_cast<char>(frame.kind);
    write_be(dst + 5, frame.request_id);
    write_be(dst + 13, std::to_underlying(frame.method));
    write_be(dst + 15, std::to_underlying(frame.status));
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

    // 完整的 payload
    frame.payload.assign(bytes + frame_header_size, length - min_frame_length);
    in.retrieve(4 + length);
    return frame;
}

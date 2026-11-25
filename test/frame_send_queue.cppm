#include <sys/uio.h>

import std;
import net.buffer;
import rpc.protocol;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

template <typename Exception, typename Fn> void require_throws(Fn &&fn) {
    try {
        fn();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error("expected exception was not thrown");
}

Frame request(std::uint64_t id, std::string payload = {}) {
    return Frame{.length = 0,
                 .kind = Kind::request,
                 .request_id = id,
                 .method = Method::system_ping,
                 .status = Status::ok,
                 .payload = std::move(payload)};
}

struct RecordingSocket {
    std::size_t limit{std::numeric_limits<std::size_t>::max()};
    bool blocked{false};
    const char *expected_slice{nullptr};
    mutable std::string bytes;
    mutable int calls{0};
    mutable bool saw_slice{false};

    std::optional<std::size_t> try_writev(const iovec *iov, int count) const {
        ++calls;
        require(count > 0 && count <= 64, "invalid writev batch size");
        for (int i = 0; i < count; ++i)
            require(iov[i].iov_len > 0, "writev batch contains an empty slice");
        if (blocked)
            return std::nullopt;

        std::size_t written = 0;
        for (int i = 0; i < count && written < limit; ++i) {
            if (iov[i].iov_base == expected_slice)
                saw_slice = true;
            const auto size = std::min(iov[i].iov_len, limit - written);
            bytes.append(static_cast<const char *>(iov[i].iov_base), size);
            written += size;
        }
        return written;
    }
};

void check_header_and_limits() {
    net::FrameSendQueue queue;
    auto frame = request(1, "ab");
    const std::size_t size = frame_header_size + frame.payload.size();
    require(!queue.enqueue(frame, size - 1), "queue accepted an oversized batch");
    require(frame.payload == "ab" && queue.empty(), "rejected enqueue changed state");
    require(queue.enqueue(frame, size), "queue rejected an exact capacity fit");
    frame.payload = "changed after enqueue";

    auto next = request(2);
    require(!queue.enqueue(next, size), "queue ignored already queued bytes");
    RecordingSocket socket;
    socket.blocked = true;
    queue.write(socket);
    require(queue.readable_bytes() == size && socket.calls == 1,
            "EAGAIN changed the queue or retried immediately");
    socket.blocked = false;
    socket.limit = 0;
    queue.write(socket);
    require(queue.readable_bytes() == size && socket.calls == 2,
            "zero-byte write changed the queue or retried immediately");

    socket.limit = 5;
    queue.write(socket);
    require(queue.readable_bytes() == size - 5 && socket.calls == 3,
            "short header write did not stop at the correct offset");
    socket.limit = frame_header_size - 5 + 1;
    queue.write(socket);
    require(queue.readable_bytes() == 1 && socket.calls == 4,
            "short payload write did not stop at the correct offset");
    queue.write(socket);
    require(queue.empty(), "completed frame remained queued");

    const std::array<char, 17> header{0, 0, 0, 15, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0};
    require(socket.bytes == std::string(header.data(), header.size()) + "ab",
            "encoded frame differs from the wire format");
    const int calls = socket.calls;
    queue.write(socket);
    require(socket.calls == calls, "empty queue attempted a write");

    auto invalid = request(0, "invalid");
    require_throws<std::invalid_argument>([&] {
        queue.enqueue(invalid);
    });
    require(invalid.payload == "invalid" && queue.empty(), "invalid frame changed state");
    auto oversized = request(3, std::string(max_frame_length - min_frame_length + 1, 'x'));
    require_throws<std::invalid_argument>([&] {
        queue.enqueue(oversized);
    });
    require(queue.empty(), "oversized frame remained queued");

    auto external = std::make_shared<const std::string>(max_frame_length - min_frame_length, 'x');
    auto combined = request(4, "x");
    combined.external_payload = *external;
    combined.payload_owner = external;
    require_throws<std::invalid_argument>([&] {
        queue.enqueue(combined);
    });
    require(queue.empty() && combined.payload_owner == external,
            "oversized combined payload changed state");
}

void check_batch_writes() {
    for (std::size_t limit : {1, 4, 16, 17, 18, 64, 1024, 65536}) {
        net::FrameSendQueue queue;
        std::vector<Frame> expected;
        std::size_t total = 0;
        for (std::uint64_t id = 1; id <= 100; ++id) {
            auto frame = request(id, id % 3 == 0 ? std::string{} : std::string(id % 23, 'x'));
            if (id % 4 == 0) {
                auto owner = std::make_shared<const std::string>(id % 31 + 1, 'y');
                frame.external_payload = *owner;
                frame.payload_owner = std::move(owner);
            }
            total += frame_header_size + frame.payload_size();
            expected.push_back(request(id, frame.payload + std::string(frame.external_payload)));
            if (id % 2 == 0) {
                const Frame &readonly = frame;
                require(queue.enqueue(readonly), "const enqueue failed");
                require(frame.payload + std::string(frame.external_payload) ==
                            expected.back().payload,
                        "const enqueue consumed input");
            } else {
                require(queue.enqueue(frame), "mutable enqueue failed");
            }
        }

        RecordingSocket socket{.limit = limit};
        while (!queue.empty()) {
            const auto before = queue.readable_bytes();
            queue.write(socket);
            require(queue.readable_bytes() < before, "write made no progress");
            require(queue.readable_bytes() + socket.bytes.size() == total,
                    "queued byte accounting changed");
        }

        net::Buffer input(0);
        std::size_t index = 0;
        for (char byte : socket.bytes) {
            input.append(&byte, 1);
            if (auto decoded = decode_frame(input)) {
                require(index < expected.size(), "decoded an extra frame");
                const auto &frame = expected[index++];
                require(decoded->request_id == frame.request_id && decoded->kind == frame.kind &&
                            decoded->method == frame.method && decoded->status == frame.status &&
                            decoded->payload == frame.payload,
                        "fragmented frame differs from the enqueued frame");
                input.retrieve(decoded->wire_bytes());
            }
        }
        require(index == expected.size() && input.empty(), "frame stream was not fully consumed");
    }
}

void check_external_payload_lifetime() {
    net::FrameSendQueue queue;
    auto owner = std::make_shared<const std::string>(4096, 'z');
    std::weak_ptr<const std::string> lifetime = owner;
    const auto slice = std::string_view(*owner).substr(17, 1024);
    auto frame = request(1, "prefix");
    frame.external_payload = slice;
    frame.payload_owner = std::move(owner);
    const auto total = frame_header_size + frame.payload_size();
    require(!queue.enqueue(frame, total - 1), "external slice was omitted from the queue limit");
    require(frame.payload_owner && frame.external_payload == slice, "rejection consumed the owner");
    require(queue.enqueue(frame), "external frame enqueue failed");
    require(!frame.payload_owner && frame.external_payload.empty(),
            "enqueue did not move the owner");
    require(!lifetime.expired(), "queued data lost its owner");

    RecordingSocket socket{.limit = frame_header_size + 6 + 3, .expected_slice = slice.data()};
    queue.write(socket);
    require(socket.saw_slice, "queue copied the external slice");
    require(!lifetime.expired() && queue.readable_bytes() == slice.size() - 3,
            "short write released the owner or lost its cursor");
    socket.blocked = true;
    queue.write(socket);
    require(!lifetime.expired(), "EAGAIN released the owner");
    socket.blocked = false;
    socket.limit = 4096;
    queue.write(socket);
    require(queue.empty() && lifetime.expired(), "completed send retained the owner");

    net::Buffer input;
    input.append(socket.bytes.data(), socket.bytes.size());
    auto decoded = decode_frame(input);
    require(decoded &&
                decoded->payload.data() == input.readable_data() + frame_header_size &&
                decoded->payload == "prefix" + std::string(1024, 'z'),
            "external frame changed on the wire or copied payload");
    input.retrieve(decoded->wire_bytes());
    require(input.empty(), "decoded frame was not retrieved");
}

void check_broadcast_lifetime() {
    auto event = request(0, std::string(8192, 'e'));
    event.kind = Kind::event;
    event.share_payload();
    const char *data = event.external_payload.data();
    std::weak_ptr<const void> lifetime = event.payload_owner;
    net::FrameSendQueue fast;
    net::FrameSendQueue slow;
    const Frame &shared = event;
    require(fast.enqueue(shared) && slow.enqueue(shared), "broadcast enqueue failed");
    event = {};

    RecordingSocket fast_socket{.expected_slice = data};
    fast.write(fast_socket);
    require(fast.empty() && !lifetime.expired(),
            "fast subscriber released the slow subscriber's data");
    require(fast_socket.saw_slice, "fast subscriber copied the broadcast");
    RecordingSocket slow_socket{.limit = frame_header_size + 5, .expected_slice = data};
    slow.write(slow_socket);
    require(slow_socket.saw_slice && !lifetime.expired(), "slow subscriber lost shared storage");
    slow_socket.limit = 16384;
    slow.write(slow_socket);
    require(slow.empty() && lifetime.expired(),
            "broadcast remained alive after all subscribers sent");
    require(fast_socket.bytes == slow_socket.bytes, "subscribers received different events");

    event = request(0, std::string(1024, 'd'));
    event.kind = Kind::event;
    event.share_payload();
    lifetime = event.payload_owner;
    {
        net::FrameSendQueue disconnected;
        require(disconnected.enqueue(event), "disconnect enqueue failed");
        require(!lifetime.expired(), "pending event lost its owner");
    }
    require(lifetime.expired(), "disconnected subscriber retained the event");
}

void check_ring_wrap_and_compaction() {
    // 1. 先在单次 writev 批次（<=32 帧）内发送前 19 帧并将第 20 帧停在半包偏移处（head_ = 19, front_offset_ > 0），
    //    再连续入队 100 帧触发跨越槽位 63->0 回绕及回绕态下的 64->128 扩容。
    net::FrameSendQueue queue;
    std::size_t first_20_bytes = 0;
    for (std::uint64_t id = 1; id <= 20; ++id) {
        auto f = request(id, "p" + std::to_string(id));
        first_20_bytes += frame_header_size + f.payload.size();
        require(queue.enqueue(f), "initial wrap enqueue failed");
    }
    RecordingSocket socket{.limit = first_20_bytes - 2};
    queue.write(socket);
    require(queue.readable_bytes() == 2, "partial 20th frame offset mismatch");

    for (std::uint64_t id = 21; id <= 120; ++id)
        require(queue.enqueue(request(id, "p" + std::to_string(id))), "wrapped grow enqueue failed");
    socket.limit = std::numeric_limits<std::size_t>::max();
    queue.write(socket);
    require(queue.empty(), "wrapped grow drain failed");

    // 2. 将全部 120 帧字节流灌入 Buffer，越过 4096B 紧缩阈值，验证先消费视图、再 retrieve 的零拷贝正确性。
    net::Buffer input(1024);
    input.append(socket.bytes.data(), socket.bytes.size());
    for (std::uint64_t id = 1; id <= 120; ++id) {
        auto view = decode_frame(input);
        require(view.has_value(), "missing wrapped frame");
        require(view->payload.data() == input.readable_data() + frame_header_size,
                "decode_frame did not reference Buffer storage directly");
        require(view->request_id == id && view->payload == "p" + std::to_string(id),
                "wrapped frame order or payload corrupted across ring grow / buffer compaction");
        input.retrieve(view->wire_bytes());
    }
    require(input.empty(), "wrapped frame stream left unread bytes");
}

void check_buffer_reuse() {
    net::Buffer buffer(8192);
    std::string data(8192, 'a');
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>(i % 251);
    buffer.append(data.data(), data.size());
    buffer.retrieve(4095);
    require(buffer.writable_bytes() == 0, "buffer compacted before the threshold");
    buffer.retrieve(1);
    require(buffer.writable_bytes() == 4096, "buffer did not compact at the threshold");
    require(std::string_view(buffer.readable_data(), buffer.readable_bytes()) ==
                std::string_view(data).substr(4096),
            "compaction changed unread bytes");

    buffer.ensure_writable(8192);
    const auto remaining = buffer.readable_bytes();
    require_throws<std::out_of_range>([&] {
        buffer.retrieve(remaining + 1);
    });
    require_throws<std::out_of_range>([&] {
        buffer.commit_write(buffer.writable_bytes() + 1);
    });
    require(buffer.readable_bytes() == remaining, "rejected operation changed readable bytes");
    buffer.retrieve(remaining);
    require(buffer.empty() && buffer.readable_data() == nullptr, "drained buffer was not reset");
    const auto capacity = buffer.writable_bytes();
    std::memcpy(buffer.begin_write(3), "abc", 3);
    buffer.clear();
    require(buffer.empty() && buffer.writable_bytes() == capacity, "clear discarded storage");
    std::memcpy(buffer.writable_data(), "xyz", 3);
    buffer.commit_write(3);
    require(std::string_view(buffer.readable_data(), buffer.readable_bytes()) == "xyz",
            "direct write after clear changed data");
}

}

int main() {
    try {
        check_header_and_limits();
        check_batch_writes();
        check_external_payload_lifetime();
        check_broadcast_lifetime();
        check_ring_wrap_and_compaction();
        check_buffer_reuse();
        std::cout << "frame send queue test passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

export module rpc.connection;

import std;
import net.buffer;
import net.socket;
import rpc.protocol;

export namespace rpc {

class Connection {
  public:
    explicit Connection(net::Socket socket) : socket_(std::move(socket)) {
    }

    [[nodiscard]]
    int fd() const noexcept {
        return socket_.native_handle();
    }

    [[nodiscard]]
    bool wants_write() const noexcept {
        return !output_.empty();
    }

    [[nodiscard]]
    bool peer_eof() const noexcept {
        return peer_eof_;
    }

    [[nodiscard]]
    bool finished() const noexcept {
        return peer_eof_ && pending_ == 0 && output_.empty();
    }

    // 读取 Socket，解析请求并提交给线程池。
    // submit(Request) 返回 false 表示拒绝任务，此时关闭连接。
    template <typename Submit> bool read(Submit &&submit) {
        std::array<char, 8192> chunk{};

        // 每次事件最多读 64 KiB，避免一个连接长期占用 I/O 线程。
        for (int i = 0; i < 8; ++i) {
            auto n = socket_.try_recv(chunk.data(), chunk.size());

            if (!n)
                return true;

            if (*n == 0) {
                peer_eof_ = true;

                // EOF 时不允许残留不完整的 RPC Frame。
                return input_.empty();
            }

            input_.append(chunk.data(), *n);

            if (!decode(submit))
                return false;
        }

        return true;
    }

    // Worker 返回 Response 后，由所属 I/O 线程调用。
    bool complete_response(const Response &response) {
        if (pending_ == 0)
            return false;

        --pending_;
        return encode_response(response);
    }

    // 非阻塞发送输出缓冲区中的数据。
    void write() {
        for (int i = 0; i < 8 && wants_write(); ++i) {
            auto n = socket_.try_send(output_.data(), output_.readable_bytes());

            // EAGAIN：等待下一次 EPOLLOUT。
            if (!n || *n == 0)
                break;

            output_.retrieve(*n);
        }
    }

  private:
    static std::uint32_t get_u32(const char *p) noexcept {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
               static_cast<std::uint32_t>(static_cast<unsigned char>(p[3]));
    }

    static void put_u32(net::Buffer &out, std::uint32_t value) {
        std::array<char, 4> bytes{static_cast<char>(value >> 24), static_cast<char>(value >> 16),
                                  static_cast<char>(value >> 8), static_cast<char>(value)};

        out.append(bytes.data(), bytes.size());
    }

    // 解析完整帧，不完整的数据留到下一次读取。
    template <typename Submit> bool decode(Submit &submit) {
        std::size_t consumed = 0;

        while (input_.readable_bytes() - consumed >= 4) {
            const char *frame = input_.data() + consumed;
            const std::uint32_t length = get_u32(frame);

            if (length < 5 || length > max_payload_size + 5)
                return false;

            const std::size_t frame_size = static_cast<std::size_t>(length) + 4;

            if (input_.readable_bytes() - consumed < frame_size)
                break;

            // 限制单连接同时运行的 RPC 数量。
            if (pending_ >= max_inflight)
                return false;

            Request request{.id = get_u32(frame + 4),
                            .method = static_cast<Method>(static_cast<unsigned char>(frame[8])),
                            .payload = std::string(frame + 9, length - 5)};

            if (!submit(std::move(request)))
                return false;

            ++pending_;
            consumed += frame_size;
        }

        if (consumed != 0)
            input_.retrieve(consumed);

        return true;
    }

    // 将 Response 编码到输出缓冲区。
    bool encode_response(const Response &response) {
        if (response.payload.size() > max_payload_size)
            return false;

        const std::size_t frame_size = response.payload.size() + 9;

        if (output_.readable_bytes() + frame_size > max_output)
            return false;

        put_u32(output_, static_cast<std::uint32_t>(response.payload.size() + 5));

        put_u32(output_, response.request_id);

        char status = static_cast<char>(response.status);
        output_.append(&status, 1);

        output_.append(response.payload.data(), response.payload.size());

        return true;
    }

    static constexpr std::size_t max_inflight = 64;
    static constexpr std::size_t max_output = 4 * 1024 * 1024;

    net::Socket socket_;

    net::Buffer input_;
    net::Buffer output_;

    // 已提交、但尚未收到业务执行结果的 RPC 数量。
    std::size_t pending_{0};

    bool peer_eof_{false};
};

} // namespace rpc

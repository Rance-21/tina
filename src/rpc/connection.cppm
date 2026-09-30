export module rpc.connection;

import std;

import net.buffer;
import net.socket;
import rpc.protocol;
import service.kv;

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

    // 对方已经 EOF，并且所有 Response 都发完了，
    // 这个 Connection 才真正结束。
    [[nodiscard]]
    bool finished() const noexcept {
        return peer_eof_ && !wants_write();
    }

    // 读取当前 socket 上已经到达的数据。
    // false 表示连接应被关闭。
    bool read(service::KvService &kv) {
        std::array<char, 8192> chunk{};

        // 一次 event 最多处理 64 KB，
        // 防止单个高流量连接长期霸占 EventLoop。
        for (int i = 0; i < 8; ++i) {
            auto n = socket_.try_recv(chunk.data(), chunk.size());

            // EAGAIN：当前已经没有更多数据。
            if (!n) {
                return true;
            }

            // recv == 0：对方关闭发送方向。
            if (*n == 0) {
                peer_eof_ = true;

                // 如果还有半条 RPC，协议不完整。
                return input_.empty();
            }

            input_.append(chunk.data(), *n);

            if (!decode(kv)) {
                return false;
            }
        }

        return true;
    }

    // 尽量发送 output_ 中尚未发送的数据。
    void write() {
        for (int i = 0; i < 8 && wants_write(); ++i) {

            auto n = socket_.try_send(output_.data(), output_.readable_bytes());

            // send buffer 满了，等待 EPOLLOUT。
            if (!n || *n == 0) {
                break;
            }

            output_.retrieve(*n);
        }
    }

  private:
    // 网络大端 4 bytes -> 本地 uint32_t。
    static std::uint32_t get_u32(const char *p) noexcept {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) << 24) |

               (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 16) |

               (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 8) |

               static_cast<std::uint32_t>(static_cast<unsigned char>(p[3]));
    }

    // 本地 uint32_t -> 网络大端 4 bytes。
    static void put_u32(net::Buffer &out, std::uint32_t value) {
        std::array<char, 4> bytes{static_cast<char>(value >> 24), static_cast<char>(value >> 16),
                                  static_cast<char>(value >> 8), static_cast<char>(value)};

        out.append(bytes.data(), bytes.size());
    }

    // 把 Response 编码为 wire format，并加入 output buffer。
    bool encode_response(const Response &response) {
        if (response.payload.size() > max_payload_size) {
            return false;
        }

        constexpr std::size_t max_output = 4 * 1024 * 1024;

        // Response:
        // [length:4]
        // [request_id:4]
        // [status:1]
        // [payload:N]
        // 总共 N + 9 bytes。
        const std::size_t frame_size = response.payload.size() + 9;

        // 慢客户端不能无限堆积 Response。
        if (output_.readable_bytes() + frame_size > max_output) {
            return false;
        }

        put_u32(output_, static_cast<std::uint32_t>(response.payload.size() + 5));
        put_u32(output_, response.request_id);

        const char status = static_cast<char>(response.status);

        output_.append(&status, 1);
        output_.append(response.payload.data(), response.payload.size());

        return true;
    }

    // 尽可能解析 input_ 中的完整 RPC frames。
    // 不完整的最后一帧会留在 Buffer，
    // 等下一次 EPOLLIN 再继续。
    bool decode(service::KvService &kv) {
        std::size_t consumed = 0;

        while (input_.readable_bytes() - consumed >= 4) {
            const char *frame = input_.data() + consumed;

            const std::uint32_t length = get_u32(frame);

            // length =
            // request_id(4)
            // + method(1)
            // + payload
            if (length < 5 || length > max_payload_size + 5) {
                return false;
            }

            const std::size_t frame_size = static_cast<std::size_t>(length) + 4;

            // 半包：完整 frame 还没有到齐。
            if (input_.readable_bytes() - consumed < frame_size) {
                break;
            }

            Request request{.id = get_u32(frame + 4),
                            .method = static_cast<Method>(static_cast<unsigned char>(frame[8])),
                            .payload = std::string(frame + 9, length - 5)};

            Response response = kv.handle(request);

            if (!encode_response(response)) {
                return false;
            }

            consumed += frame_size;
        }

        // 一次性消费所有已经成功处理的完整 frame。
        if (consumed != 0) {
            input_.retrieve(consumed);
        }

        return true;
    }

    net::Socket socket_;

    net::Buffer input_;
    net::Buffer output_;

    bool peer_eof_{false};
};

} // namespace rpc
export module rpc.connection;

import std;
import net.socket;
import rpc.protocol;
import service.kv;

export namespace rpc {

// One event loop owns this connection and all its buffers.
class Connection {
  public:
    explicit Connection(net::Socket socket) : socket_(std::move(socket)) {}

    [[nodiscard]] int fd() const noexcept { return socket_.native_handle(); }
    [[nodiscard]] bool wants_write() const noexcept { return sent_ < output_.size(); }
    [[nodiscard]] bool finished() const noexcept { return peer_eof_ && !wants_write(); }

    // false means the peer closed or this connection exceeded its buffer limit.
    bool read(service::KvService &kv) {
        std::array<char, 8192> chunk{};
        // Bound work per event; level-triggered epoll will report remaining data.
        for (int i = 0; i < 8; ++i) {
            auto n = socket_.try_recv(chunk.data(), chunk.size());
            if (!n) return true;
            if (*n == 0) {
                peer_eof_ = true;
                return input_.empty(); // discard a truncated frame
            }
            input_.insert(input_.end(), chunk.data(), chunk.data() + *n);
            if (!decode(kv)) return false;
        }
        return true;
    }

    void write() {
        for (int i = 0; i < 8 && wants_write(); ++i) {
            auto n = socket_.try_send(output_.data() + sent_, output_.size() - sent_);
            if (!n || *n == 0) break;
            sent_ += *n;
        }
        if (!wants_write()) {
            output_.clear();
            sent_ = 0;
        }
    }

    void mark_peer_closed() noexcept { peer_eof_ = true; }

  private:
    static std::uint32_t get_u32(const char *p) noexcept {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
               static_cast<std::uint32_t>(static_cast<unsigned char>(p[3]));
    }

    static void put_u32(std::vector<char> &out, std::uint32_t n) {
        out.push_back(static_cast<char>(n >> 24));
        out.push_back(static_cast<char>(n >> 16));
        out.push_back(static_cast<char>(n >> 8));
        out.push_back(static_cast<char>(n));
    }

    bool decode(service::KvService &kv) {
        std::size_t consumed = 0;
        while (input_.size() - consumed >= 4) {
            const char *frame = input_.data() + consumed;
            auto length = get_u32(frame);
            if (length < 5 || length > max_payload_size + 5) return false;
            if (input_.size() - consumed < static_cast<std::size_t>(length) + 4) break;

            Request request{get_u32(frame + 4), static_cast<Method>(static_cast<unsigned char>(frame[8])),
                            std::string(frame + 9, length - 5)};
            Response response = kv.handle(request);
            if (response.payload.size() > max_payload_size) return false;
            // Backpressure: bound memory retained for a peer that does not read.
            constexpr std::size_t max_output = 4 * 1024 * 1024;
            if (output_.size() - sent_ + response.payload.size() + 9 > max_output) return false;
            if (sent_ != 0) {
                output_.erase(output_.begin(), output_.begin() + static_cast<std::ptrdiff_t>(sent_));
                sent_ = 0;
            }
            put_u32(output_, static_cast<std::uint32_t>(response.payload.size() + 5));
            put_u32(output_, response.request_id);
            output_.push_back(static_cast<char>(response.status));
            output_.insert(output_.end(), response.payload.begin(), response.payload.end());
            consumed += static_cast<std::size_t>(length) + 4;
        }
        if (consumed != 0) input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(consumed));
        return true;
    }

    net::Socket socket_;
    std::vector<char> input_;
    std::vector<char> output_;
    std::size_t sent_{0};
    bool peer_eof_{false};
};

} // namespace rpc

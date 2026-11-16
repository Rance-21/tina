module;
#include <sys/uio.h>

export module net.buffer.frame_send_queue;

import std;
import rpc.frame;

export namespace net {

// 基于 ::writev() 的帧发送队列（服务端 Connection、客户端 RpcClient 与压测端共用）。
// 核心设计：
//   - 每帧拆分为“17 字节固定帧头（内联数组）”和“直接 std::move 接管的 payload 字符串”，
//     彻底省去在用户态分配连续大 Buffer 并把 payload 再 memcpy 拷贝一遍的开销。
//   - 发送时通过 ::writev() 在单次系统调用中将多帧的“17B 帧头 + Payload”一起聚集写入内核。
class FrameSendQueue {
  public:
    [[nodiscard]] inline bool empty() const noexcept {
        return frames_.empty();
    }

    [[nodiscard]] inline std::size_t readable_bytes() const noexcept {
        return queued_bytes_;
    }

    // 入队一帧（直接移动接管 frame.payload，零拷贝）。
    // 若超出 max_bytes 上限则返回 false。
    bool enqueue(Frame &frame, std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
        const std::size_t frame_size = frame_header_size + frame.payload_size();
        if (queued_bytes_ > max_bytes || frame_size > max_bytes - queued_bytes_)
            return false;

        Item item{};
        encode_frame_header(frame, item.header.data()); // 1. 编码 17 字节固定帧头
        item.payload = std::move(frame.payload);        // 2. 直接移动接管 payload，不拷贝字节
        item.payload_owner = std::move(frame.payload_owner);
        item.external_payload = std::exchange(frame.external_payload, {});
        frames_.push_back(std::move(item));
        queued_bytes_ += frame_size;
        return true;
    }

    // 入队只读帧（如向多个订阅者广播同一个 Event 帧）。
    bool enqueue(const Frame &frame,
                 std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
        const std::size_t frame_size = frame_header_size + frame.payload_size();
        if (queued_bytes_ > max_bytes || frame_size > max_bytes - queued_bytes_)
            return false;

        Item item{};
        encode_frame_header(frame, item.header.data());
        item.payload = frame.payload;
        item.payload_owner = frame.payload_owner;
        item.external_payload = frame.external_payload;
        frames_.push_back(std::move(item));
        queued_bytes_ += frame_size;
        return true;
    }

    // 使用 ::writev() 将队列中待发的“17B 帧头”和“Payload”聚集写入非阻塞 Socket。
    template <typename Socket> void write(const Socket &socket) {
        constexpr int kMaxIov = 64;
        std::array<struct iovec, kMaxIov> iov;

        while (!frames_.empty()) {
            int iov_count = 0;
            std::size_t batch_bytes = 0;

            // 第一步：遍历队列，把每帧还没发完的“17B 帧头”和“Payload”依次挂进 iov 数组。
            for (auto &item : frames_) {
                // 1. 若 17B 帧头还没发完，把剩余帧头作为一段 iovec
                if (item.sent < frame_header_size) {
                    const std::size_t len = frame_header_size - item.sent;
                    iov[iov_count++] = {item.header.data() + item.sent, len};
                    batch_bytes += len;
                    if (iov_count == kMaxIov)
                        break;
                }

                // 2. 若有 Payload，把剩余未发的 Payload 作为下一段 iovec
                if (!item.payload.empty() && item.sent < frame_header_size + item.payload.size()) {
                    const std::size_t offset =
                        (item.sent > frame_header_size) ? (item.sent - frame_header_size) : 0;
                    const std::size_t len = item.payload.size() - offset;
                    iov[iov_count++] = {item.payload.data() + offset, len};
                    batch_bytes += len;
                    if (iov_count == kMaxIov)
                        break;
                }

                if (!item.external_payload.empty()) {
                    const std::size_t prefix_size = frame_header_size + item.payload.size();
                    const std::size_t offset =
                        item.sent > prefix_size ? item.sent - prefix_size : 0;
                    const std::size_t len = item.external_payload.size() - offset;
                    iov[iov_count++] = {const_cast<char *>(item.external_payload.data() + offset),
                                        len};
                    batch_bytes += len;
                    if (iov_count == kMaxIov)
                        break;
                }
            }

            // 第二步：单次 ::writev() 系统调用将所有分散数据块一起写入内核。
            auto n = socket.try_writev(iov.data(), iov_count);
            if (!n || *n == 0)
                break; // EAGAIN：内核发送缓冲区已满，等待下次 EPOLLOUT

            // 第三步：根据实际发出的字节数 *n，从队头弹出已发完的帧，或推进部分写游标。
            std::size_t rem = *n;
            queued_bytes_ -= rem;

            while (rem > 0 && !frames_.empty()) {
                auto &front = frames_.front();
                const std::size_t frame_left =
                    (frame_header_size + front.payload.size() + front.external_payload.size()) -
                    front.sent;

                if (rem >= frame_left) {
                    rem -= frame_left;
                    frames_.pop_front(); // 整帧已发完，弹出并释放其 payload 内存
                } else {
                    front.sent += rem; // 部分写（只发了一半）：推进游标，留待下次继续发
                    rem = 0;
                }
            }

            // 若本次未写完当前批次，说明内核发送缓冲区已满，直接退出避免多调一次必触发 EAGAIN 的系统调用。
            if (*n < batch_bytes)
                break;
        }
    }

  private:
    struct Item {
        std::array<char, frame_header_size> header{};
        std::string payload;
        std::shared_ptr<const void> payload_owner;
        std::string_view external_payload;
        std::size_t sent{0}; // 当前帧已发送的总字节数（范围：0 .. 17 + payload.size()）
        // 有外置切片时，上界还包含 external_payload.size()；所有切片均由 payload_owner 保活。
    };

    std::deque<Item> frames_;
    std::size_t queued_bytes_{0};
};

}

module;
#include <sys/uio.h>

export module net.buffer.frame_send_queue;

import std;
import rpc.protocol;

export namespace net {

// 基于 ::writev() 的帧发送队列（服务端 Connection、客户端 RpcClient 与压测端共用）。
// 队列分别保留帧头、内联正文和可选的外置切片。writev 一次最多发送 64 段，
// 无需把缓存中的文件正文拷贝到连续发送缓冲区。切片的 owner 保留到整帧发完。
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
        return append_frame(frame, max_bytes);
    }

    // 广播保留原帧，多个发送队列各自持有同一正文的引用。
    bool enqueue(const Frame &frame,
                 std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
        return append_frame(frame, max_bytes);
    }

    // 使用 ::writev() 将队列中待发的“17B 帧头”和“Payload”聚集写入非阻塞 Socket。
    template <typename Socket> void write(const Socket &socket) {
        constexpr int kMaxIov = 64;
        std::array<struct iovec, kMaxIov> iov;

        while (!frames_.empty()) {
            int iov_count = 0;
            std::size_t batch_bytes = 0;

            // 只有队头可能发过一部分。skip 先跨过完整段，再截去首个未发段的前缀；
            // 此后它为零，可以直接把本帧和后续帧的剩余段交给 writev。
            std::size_t skip = front_offset_;

            auto append_segment = [&](std::string_view bytes) {
                if (skip >= bytes.size()) {
                    skip -= bytes.size();
                    return true;
                }

                bytes.remove_prefix(skip);
                skip = 0;
                iov[iov_count++] = {const_cast<char *>(bytes.data()), bytes.size()};
                batch_bytes += bytes.size();
                return iov_count < kMaxIov;
            };

            for (const auto &item : frames_) {
                if (!append_segment({item.header.data(), item.header.size()}) ||
                    !append_segment(item.payload) || !append_segment(item.external_payload))
                    break;
            }

            // 第二步：单次 ::writev() 系统调用将所有分散数据块一起写入内核。
            auto n = socket.try_writev(iov.data(), iov_count);
            if (!n || *n == 0)
                break; // EAGAIN：内核发送缓冲区已满，等待下次 EPOLLOUT

            // 第三步：根据实际发出的字节数 *n，从队头弹出已发完的帧，或推进部分写游标。
            std::size_t rem = *n;
            queued_bytes_ -= rem;

            while (rem > 0) {
                const auto &front = frames_.front();
                const auto left = frame_header_size + front.payload.size() +
                                  front.external_payload.size() - front_offset_;
                if (rem < left) {
                    front_offset_ += rem;
                    break;
                }
                rem -= left;
                front_offset_ = 0;
                frames_.pop_front();
            }

            // 若本次未写完当前批次，说明内核发送缓冲区已满，直接退出避免多调一次必触发 EAGAIN
            // 的系统调用。
            if (*n < batch_bytes)
                break;
        }
    }

  private:
    struct Item {
        std::array<char, frame_header_size> header;
        std::string payload;
        std::shared_ptr<const void> payload_owner;
        std::string_view external_payload;

        template <typename F> explicit Item(F &frame) {
            // 先验证帧头，异常时 deque 不增加元素，调用方仍持有原帧。
            encode_frame_header(frame, header.data());
            external_payload = frame.external_payload;
            if constexpr (std::is_const_v<F>) {
                payload = frame.payload;
                payload_owner = frame.payload_owner;
            } else {
                payload = std::move(frame.payload);
                payload_owner = std::move(frame.payload_owner);
                frame.external_payload = {};
            }
        }
    };

    template <typename F> bool append_frame(F &frame, std::size_t max_bytes) {
        const auto size = frame_header_size + frame.payload_size();
        if (size > max_bytes - queued_bytes_)
            return false;

        frames_.emplace_back(frame);
        queued_bytes_ += size;
        return true;
    }

    std::deque<Item> frames_;
    std::size_t front_offset_{0};
    std::size_t queued_bytes_{0};
};

} // namespace net

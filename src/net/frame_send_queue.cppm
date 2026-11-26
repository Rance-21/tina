module;
#include <sys/uio.h>

export module net.buffer.frame_send_queue;

import std;
import rpc.protocol;

export namespace net {

// 基于 ::writev() 的单线程环形帧发送队列（服务端 Connection、客户端 RpcClient 与压测端共用）。
// 常态使用内嵌 64 槽位环形数组承载流水线（零分配），超容量时按 2 的幂扩容；切片 owner 保活至整帧发完。
class FrameSendQueue {
  public:
    FrameSendQueue() noexcept {
        slots_ = inline_slots_.data();
    }
    FrameSendQueue(const FrameSendQueue &) = delete;
    FrameSendQueue &operator=(const FrameSendQueue &) = delete;

    [[nodiscard]] inline bool empty() const noexcept {
        return head_ == tail_;
    }

    [[nodiscard]] inline std::size_t readable_bytes() const noexcept {
        return queued_bytes_;
    }

    template <typename F>
        requires std::same_as<std::remove_cvref_t<F>, Frame>
    bool enqueue(F &&frame, std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
        const auto size = frame_header_size + frame.payload_size();
        if (size > max_bytes - queued_bytes_)
            return false;

        if (tail_ - head_ == capacity_)
            grow();

        Item &slot = slots_[tail_ & mask_];
        encode_frame_header(frame, slot.header.data());
        if constexpr (std::is_const_v<std::remove_reference_t<F>>) {
            slot.payload = frame.payload;
            slot.payload_owner = frame.payload_owner;
            slot.external_payload = frame.external_payload;
        } else {
            if (frame.payload.empty())
                slot.payload.clear();
            else
                slot.payload = std::move(frame.payload);
            slot.payload_owner = std::move(frame.payload_owner);
            slot.external_payload = std::exchange(frame.external_payload, {});
        }

        ++tail_;
        queued_bytes_ += size;
        return true;
    }

    template <typename Socket> void write(const Socket &socket) {
        constexpr int kMaxIov = 64;
        std::array<struct iovec, kMaxIov> iov;

        while (!empty()) {
            int iov_count = 0;
            std::size_t batch_bytes = 0;
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

            for (std::size_t cur = head_; cur != tail_; ++cur) {
                const Item &item = slots_[cur & mask_];
                if (!append_segment({item.header.data(), item.header.size()}) ||
                    !append_segment(item.payload) || !append_segment(item.external_payload))
                    break;
            }

            auto n = socket.try_writev(iov.data(), iov_count);
            if (!n || *n == 0)
                break;

            std::size_t rem = *n;
            queued_bytes_ -= rem;

            while (rem > 0) {
                Item &front = slots_[head_ & mask_];
                const auto left = frame_header_size + front.payload.size() +
                                  front.external_payload.size() - front_offset_;
                if (rem < left) {
                    front_offset_ += rem;
                    break;
                }
                rem -= left;
                front_offset_ = 0;
                front.reset();
                ++head_;
            }

            if (head_ == tail_) {
                head_ = 0;
                tail_ = 0;
            }

            if (*n < batch_bytes)
                break;
        }
    }

  private:
    static constexpr std::size_t kInlineCapacity = 64;

    struct Item {
        std::array<char, frame_header_size> header;
        std::string payload;
        PayloadOwner payload_owner;
        std::string_view external_payload;

        void reset() noexcept {
            if (payload.capacity() > 128 * 1024)
                std::string{}.swap(payload);
            else
                payload.clear();
            payload_owner.reset();
            external_payload = {};
        }
    };

    void grow() {
        const std::size_t count = tail_ - head_;
        const std::size_t new_cap = capacity_ * 2;
        auto next = std::make_unique<Item[]>(new_cap);
        for (std::size_t i = 0; i < count; ++i)
            next[i] = std::move(slots_[(head_ + i) & mask_]);
        heap_slots_ = std::move(next);
        slots_ = heap_slots_.get();
        head_ = 0;
        tail_ = count;
        capacity_ = new_cap;
        mask_ = new_cap - 1;
    }

    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t capacity_{kInlineCapacity};
    std::size_t mask_{kInlineCapacity - 1};
    std::size_t front_offset_{0};
    std::size_t queued_bytes_{0};
    Item *slots_{nullptr};
    std::unique_ptr<Item[]> heap_slots_;
    std::array<Item, kInlineCapacity> inline_slots_;
};

} // namespace net

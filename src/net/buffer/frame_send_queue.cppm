module;
#include <sys/uio.h>

export module net.buffer.frame_send_queue;

import std;
import rpc.protocol;

export namespace net {

// 基于 ::writev() 的单线程环形帧发送队列（服务端 Connection、客户端 RpcClient 与压测端共用）。
// 默认使用内嵌 64 槽位环形数组承载常规并发流水线（零堆分配），仅在超容量积压时按 2 的幂扩容。
// 队列分别保留帧头、内联正文和可选的外置切片；切片的 owner 保留到整帧发完。
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

    // 入队一帧：非常量帧直接移动接管 payload 与 owner；常量帧（广播）复制正文与共享 owner。
    // 超出 max_bytes 上限时返回 false 且不修改原帧。
    template <typename F>
        requires std::same_as<std::remove_cvref_t<F>, Frame>
    bool enqueue(F &&frame, std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
        const auto size = frame_header_size + frame.payload_size();
        if (size > max_bytes - queued_bytes_)
            return false;

        std::array<char, frame_header_size> temp_header;
        const bool full = (tail_ - head_ == capacity_);
        if (full) {
            encode_frame_header(frame, temp_header.data());
            grow();
        }

        Item &slot = slots_[tail_ & mask_];
        if (full)
            slot.header = temp_header;
        else
            encode_frame_header(frame, slot.header.data());

        if constexpr (std::is_const_v<std::remove_reference_t<F>>) {
            slot.payload = frame.payload;
            slot.payload_owner = frame.payload_owner;
            slot.external_payload = frame.external_payload;
        } else {
            slot.payload = std::move(frame.payload);
            slot.payload_owner = std::move(frame.payload_owner);
            slot.external_payload = std::exchange(frame.external_payload, {});
        }

        ++tail_;
        queued_bytes_ += size;
        return true;
    }

    // 使用 ::writev() 将队列中待发的“17B 帧头”和“Payload”聚集写入非阻塞 Socket。
    template <typename Socket> void write(const Socket &socket) {
        constexpr int kMaxIov = 64;
        std::array<struct iovec, kMaxIov> iov;

        while (!empty()) {
            int iov_count = 0;
            std::size_t batch_bytes = 0;

            // 仅队头帧可能存在部分写偏移；后续帧全部从头直接拼入 iovec。
            {
                const Item &front = slots_[head_ & mask_];
                std::size_t skip = front_offset_;
                auto push_front = [&](std::string_view seg) {
                    if (skip >= seg.size()) {
                        skip -= seg.size();
                        return;
                    }
                    seg.remove_prefix(skip);
                    skip = 0;
                    iov[iov_count++] = {const_cast<char *>(seg.data()), seg.size()};
                    batch_bytes += seg.size();
                };
                push_front({front.header.data(), front.header.size()});
                push_front(front.payload);
                push_front(front.external_payload);
            }

            for (std::size_t cur = head_ + 1; cur != tail_ && iov_count < kMaxIov; ++cur) {
                const Item &item = slots_[cur & mask_];
                iov[iov_count++] = {const_cast<char *>(item.header.data()), item.header.size()};
                batch_bytes += item.header.size();
                if (!item.payload.empty() && iov_count < kMaxIov) {
                    iov[iov_count++] = {const_cast<char *>(item.payload.data()),
                                        item.payload.size()};
                    batch_bytes += item.payload.size();
                }
                if (!item.external_payload.empty() && iov_count < kMaxIov) {
                    iov[iov_count++] = {const_cast<char *>(item.external_payload.data()),
                                        item.external_payload.size()};
                    batch_bytes += item.external_payload.size();
                }
            }

            auto n = socket.try_writev(iov.data(), iov_count);
            if (!n || *n == 0)
                break; // EAGAIN：内核发送缓冲区已满，等待下次 EPOLLOUT

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

            // 若本次未写完当前批次，说明内核发送缓冲区已满，直接退出避免多调一次必触发 EAGAIN
            // 的系统调用。
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
            if (payload.capacity() > 4096)
                std::string{}.swap(payload);
            else
                payload.clear();
            payload_owner.reset();
            external_payload = {};
        }
    };

    void grow() {
        const std::size_t new_cap = capacity_ * 2;
        auto next = std::make_unique<Item[]>(new_cap);
        const std::size_t count = tail_ - head_;
        for (std::size_t i = 0; i < count; ++i)
            next[i] = std::move(slots_[(head_ + i) & mask_]);
        if (slots_ == inline_slots_.data()) {
            for (auto &s : inline_slots_)
                s = {};
        }
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

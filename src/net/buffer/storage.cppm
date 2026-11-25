export module net.buffer.storage;

import std;

export namespace net {

class Buffer {
  public:
    explicit Buffer(std::size_t initial_bytes = 8192) : storage_(initial_bytes) {
    }

    [[nodiscard]] inline std::size_t readable_bytes() const noexcept {
        return write_index_ - read_index_;
    }

    [[nodiscard]] inline std::size_t writable_bytes() const noexcept {
        return storage_.size() - write_index_;
    }

    [[nodiscard]] inline bool empty() const noexcept {
        return readable_bytes() == 0;
    }

    // 返回当前尚未消费的可读数据起点。
    [[nodiscard]] inline const char *data() const noexcept {
        if (empty()) {
            return nullptr;
        }

        return storage_.data() + read_index_;
    }

    // 返回当前尾部可写区域的起点（配合 ensure_writable + commit_write 供 recv 等直接写入）。
    [[nodiscard]] inline char *writable_data() noexcept {
        return storage_.data() + write_index_;
    }

    // 只有 retrieve 会增大已消费前缀，它在那里按阈值紧缩。追加数据只会
    // 增大写游标，无法让前缀重新满足紧缩条件，尾部放不下时直接扩容即可。
    void ensure_writable(std::size_t bytes) {
        if (writable_bytes() >= bytes)
            return;
        if (bytes > storage_.max_size() - write_index_)
            throw std::length_error("buffer size exceeds maximum");
        storage_.resize(std::max(write_index_ + bytes, storage_.size() * 2));
    }

    // 当外部直接向 writable_data() 写入 bytes 字节后（如 socket.try_recv），推进写游标。
    inline void commit_write(std::size_t bytes) {
        if (bytes > writable_bytes()) {
            throw std::out_of_range("buffer commit_write exceeds writable bytes");
        }
        write_index_ += bytes;
    }

    // 在 Buffer 尾部直接切出 bytes 字节的可写空间并推进写游标，返回该区域首地址供调用方原地写入。
    [[nodiscard]] inline char *begin_write(std::size_t bytes) {
        ensure_writable(bytes);
        char *dst = writable_data();
        write_index_ += bytes;
        return dst;
    }

    // 在 Buffer 尾部追加数据。
    void append(const char *data, std::size_t bytes) {
        if (bytes == 0) {
            return;
        }

        char *dst = begin_write(bytes);
        std::memcpy(dst, data, bytes);
    }

    // 消费前 bytes 个可读字节。
    void retrieve(std::size_t bytes) {
        if (bytes > readable_bytes()) {
            throw std::out_of_range("buffer retrieve exceeds readable bytes");
        }

        read_index_ += bytes;

        // 全部读空时直接将读写游标归零，复用已分配的底层容量，避免后续重复扩容或清零。
        if (read_index_ == write_index_) {
            clear();
            return;
        }

        // 前面至少浪费 4 KB，并且已经占已写入区域的一半以上，才真正搬移剩余未读数据。
        if (read_index_ < 4096 || read_index_ < write_index_ / 2) {
            return;
        }

        const std::size_t readable = readable_bytes();
        std::memmove(storage_.data(), storage_.data() + read_index_, readable);
        read_index_ = 0;
        write_index_ = readable;
    }

    void clear() noexcept {
        read_index_ = 0;
        write_index_ = 0;
    }

  private:
    std::vector<char> storage_;
    std::size_t read_index_{0};
    std::size_t write_index_{0};
};

} // namespace net

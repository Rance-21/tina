export module net.buffer;
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

    // 确保 Buffer 尾部至少拥有 bytes 字节的连续可写空间：
    // 先尝试通过 maybe_compact() 回收前端已消费空间；若仍不足则扩容底层 storage_。
    void ensure_writable(std::size_t bytes) {
        if (writable_bytes() >= bytes) {
            return;
        }

        maybe_compact();

        if (writable_bytes() < bytes) {
            const std::size_t needed = write_index_ + bytes;
            storage_.resize(std::max(needed, storage_.size() * 2));
        }
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
            read_index_ = 0;
            write_index_ = 0;
            return;
        }

        maybe_compact();
    }

    void clear() noexcept {
        read_index_ = 0;
        write_index_ = 0;
    }

  private:
    void maybe_compact() noexcept {
        if (read_index_ == 0) {
            return;
        }

        if (read_index_ == write_index_) {
            read_index_ = 0;
            write_index_ = 0;
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

    std::vector<char> storage_;
    std::size_t read_index_{0};
    std::size_t write_index_{0};
};

} // namespace net

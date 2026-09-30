export module net.buffer;
import std;

export namespace net {

class Buffer {
  public:
    explicit Buffer(std::size_t reserve_bytes = 8192) {
        storage_.reserve(reserve_bytes);
    }

    [[nodiscard]]
    std::size_t readable_bytes() const noexcept {
        return storage_.size() - read_index_;
    }

    [[nodiscard]]
    bool empty() const noexcept {
        return readable_bytes() == 0;
    }

    // 返回当前尚未消费的数据起点。
    [[nodiscard]]
    const char *data() const noexcept {
        if (empty()) {
            return nullptr;
        }

        return storage_.data() + read_index_;
    }

    // 在 Buffer 尾部追加数据。
    void append(const char *data, std::size_t bytes) {
        if (bytes == 0) {
            return;
        }

        maybe_compact();

        storage_.insert(storage_.end(), data, data + bytes);
    }

    // 消费前 bytes 个可读字节。
    void retrieve(std::size_t bytes) {
        if (bytes > readable_bytes()) {
            throw std::out_of_range("buffer retrieve exceeds readable bytes");
        }

        read_index_ += bytes;

        if (read_index_ == storage_.size()) {
            storage_.clear();
            read_index_ = 0;
            return;
        }

        maybe_compact();
    }

    void clear() noexcept {
        storage_.clear();
        read_index_ = 0;
    }

  private:
    void maybe_compact() {
        if (read_index_ == 0) {
            return;
        }

        // 前面至少浪费 4 KB，并且已经占整个 storage 的一半以上，
        // 才真正移动剩余数据。
        if (read_index_ < 4096 || read_index_ < storage_.size() / 2) {
            return;
        }

        storage_.erase(storage_.begin(),
                       storage_.begin() + static_cast<std::ptrdiff_t>(read_index_));

        read_index_ = 0;
    }

    std::vector<char> storage_;
    std::size_t read_index_{0};
};

} // namespace net
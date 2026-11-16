export module rpc.payload;

import std;

// 使用 memcpy + std::byteswap 实现单指令大端序整数字节序转换，消除逐字节循环移位开销，
// 同时保证不依赖机器原生字节序或非对齐指针强转。
export template <std::unsigned_integral T>
[[nodiscard]] inline T read_be(const char *bytes) noexcept {
    if constexpr (sizeof(T) == 1) {
        return static_cast<T>(static_cast<unsigned char>(bytes[0]));
    } else {
        T value{};
        std::memcpy(&value, bytes, sizeof(T));
        if constexpr (std::endian::native == std::endian::little)
            value = std::byteswap(value);
        return value;
    }
}

export template <std::unsigned_integral T> inline void write_be(char *bytes, T value) noexcept {
    if constexpr (sizeof(T) == 1) {
        bytes[0] = static_cast<char>(value);
    } else {
        if constexpr (std::endian::native == std::endian::little)
            value = std::byteswap(value);
        std::memcpy(bytes, &value, sizeof(T));
    }
}

// 向字符串末尾追加大端序无符号整数（支持 u8 / u16 / u32 / u64）。
export template <std::unsigned_integral T> inline void append_be(std::string &out, T value) {
    if constexpr (sizeof(T) == 1) {
        out.push_back(static_cast<char>(value));
    } else {
        if constexpr (std::endian::native == std::endian::little)
            value = std::byteswap(value);
        out.append(reinterpret_cast<const char *>(&value), sizeof(T));
    }
}

// 按大端序顺序切片解析二进制 Payload 的辅助读取器。
export class PayloadReader {
  public:
    explicit PayloadReader(std::string_view input) : input_(input) {
    }

    // 读取一个大端序无符号整数 T（如 uint8_t / uint16_t / uint32_t / uint64_t）。
    template <typename T> std::optional<T> read_int() {
        if (sizeof(T) > remaining_size())
            return std::nullopt;

        T value = read_be<T>(input_.data() + pos_);
        pos_ += sizeof(T);
        return value;
    }

    // 读取指定字节数的切片（零拷贝返回 string_view）。
    std::optional<std::string_view> read_bytes(std::size_t count) {
        if (count > remaining_size())
            return std::nullopt;

        auto slice = input_.substr(pos_, count);
        pos_ += count;
        return slice;
    }

    // 读取剩余的所有字节。
    [[nodiscard]] std::string_view remaining_bytes() const noexcept {
        return input_.substr(pos_);
    }

    [[nodiscard]] std::size_t remaining_size() const noexcept {
        return input_.size() - pos_;
    }

    [[nodiscard]] bool finished() const noexcept {
        return pos_ == input_.size();
    }

  private:
    std::string_view input_;
    std::size_t pos_{0};
};

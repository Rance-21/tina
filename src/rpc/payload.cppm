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

// 发送队列只需要保活一段只读内存。缓存借用一个固定槽位；只有最后一个读者遇到
// retired 才进入回收慢路径。线程局部热表命中时可通过最低位标记零原子借用。
export class PayloadPin {
  public:
    struct Slot {
        static constexpr std::uint64_t retired = std::uint64_t{1} << 63;
        std::atomic<std::uint64_t> state{retired};
        std::shared_ptr<const void> value;
        void (*reclaim)(Slot &) noexcept;
    };

    PayloadPin() = default;
    explicit PayloadPin(Slot &slot) noexcept : raw_(reinterpret_cast<std::uintptr_t>(&slot)) {
    }
    PayloadPin(PayloadPin &&other) noexcept : raw_(std::exchange(other.raw_, 0)) {
    }
    PayloadPin &operator=(PayloadPin &&other) noexcept {
        PayloadPin moved(std::move(other));
        std::swap(raw_, moved.raw_);
        return *this;
    }
    ~PayloadPin() {
        if (raw_ && !(raw_ & 1)) {
            auto *s = reinterpret_cast<Slot *>(raw_);
            if (s->state.fetch_sub(1, std::memory_order_acq_rel) == Slot::retired + 1)
                s->reclaim(*s);
        }
    }

    explicit operator bool() const noexcept { return raw_ != 0; }
    bool operator==(std::nullptr_t) const noexcept { return !raw_; }
    const void *get() const noexcept { return raw_ ? slot()->value.get() : nullptr; }
    bool active() const noexcept {
        return raw_ && !(slot()->state.load(std::memory_order_acquire) & Slot::retired);
    }

    // 同一 I/O 线程在当轮同步 writev 期间临时借用，免除跨核引用计数增减。
    PayloadPin borrow() const noexcept {
        PayloadPin p;
        p.raw_ = raw_ ? (raw_ | 1) : 0;
        return p;
    }
    void promote_borrowed() noexcept {
        if (raw_ & 1) {
            raw_ &= ~std::uintptr_t{1};
            reinterpret_cast<Slot *>(raw_)->state.fetch_add(1, std::memory_order_relaxed);
        }
    }
    PayloadPin clone() const noexcept {
        auto *s = slot();
        if (!s)
            return {};
        s->state.fetch_add(1, std::memory_order_relaxed);
        return PayloadPin(*s);
    }
    std::weak_ptr<const void> weak() const noexcept {
        return raw_ ? slot()->value : std::weak_ptr<const void>{};
    }

  private:
    Slot *slot() const noexcept { return reinterpret_cast<Slot *>(raw_ & ~std::uintptr_t{1}); }
    std::uintptr_t raw_{};
};

// 一帧外置正文只有一种所有者。缓存切片移动 Pin；广播正文和未被缓存接纳的
// 读盘结果持有原对象。两条路径均能原样穿过 Worker、Frame 和部分写队列。
export class PayloadOwner {
  public:
    PayloadOwner() = default;
    template <class T> PayloadOwner(std::shared_ptr<T> value) noexcept : owner_(std::move(value)) {
    }
    PayloadOwner(PayloadPin pin) noexcept : owner_(std::move(pin)) {
    }
    PayloadOwner(PayloadOwner &&) noexcept = default;
    PayloadOwner &operator=(PayloadOwner &&) noexcept = default;
    PayloadOwner(const PayloadOwner &other) noexcept {
        if (auto *pin = std::get_if<PayloadPin>(&other.owner_))
            owner_.emplace<PayloadPin>(pin->clone());
        else
            owner_ = std::get<0>(other.owner_);
    }
    PayloadOwner &operator=(const PayloadOwner &other) noexcept {
        return *this = PayloadOwner(other);
    }

    void reset() noexcept {
        owner_.emplace<0>();
    }
    void promote_borrowed() noexcept {
        if (auto *pin = std::get_if<PayloadPin>(&owner_))
            pin->promote_borrowed();
    }
    const void *get() const noexcept {
        if (auto *pin = std::get_if<PayloadPin>(&owner_))
            return pin->get();
        return std::get<0>(owner_).get();
    }
    explicit operator bool() const noexcept {
        return get() != nullptr;
    }
    template <class T> bool operator==(const std::shared_ptr<T> &p) const noexcept {
        return get() == p.get();
    }
    operator std::weak_ptr<const void>() const noexcept {
        if (auto *pin = std::get_if<PayloadPin>(&owner_))
            return pin->weak();
        return std::get<0>(owner_);
    }

  private:
    std::variant<std::shared_ptr<const void>, PayloadPin> owner_;
};

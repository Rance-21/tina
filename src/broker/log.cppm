module;
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

export module broker.log;
import std;

export namespace broker {

// 单文件、单 Partition 的磁盘日志。
// 由 Partition 的 mutex 保护，本类自身不加锁。
// Record:
// magic:u32 version:u16 flags:u16 length:u32
// crc32c:u32 offset:u64 payload:bytes
class MessageLog {
  public:
    static constexpr std::size_t max_message_size = 256 * 1024;
    static constexpr std::size_t max_records = 65'536;
    static constexpr std::uint64_t max_file_size = 128ULL * 1024 * 1024;

    explicit MessageLog(const std::filesystem::path &path) {
        if (path.empty() || !path.has_filename())
            throw std::invalid_argument("invalid message log path");

        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path());

        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (fd_ < 0)
            fail_system("open message log");

        try {
            // 禁止不同进程同时写入同一个日志文件。
            if (::flock(fd_, LOCK_EX | LOCK_NB) < 0)
                fail_system("lock message log");

            recover();

            // 同步目录项，避免新建文件的路径在掉电后丢失。
            sync_parent_directories(path.parent_path());
        } catch (...) {
            ::close(fd_);
            fd_ = -1;
            throw;
        }
    }

    ~MessageLog() {
        if (fd_ >= 0)
            ::close(fd_);
    }

    MessageLog(const MessageLog &) = delete;
    MessageLog &operator=(const MessageLog &) = delete;

    [[nodiscard]]
    std::uint64_t next_offset() const noexcept {
        return static_cast<std::uint64_t>(index_.size());
    }

    [[nodiscard]]
    std::uint64_t size_bytes() const noexcept {
        return end_position_;
    }

    // FETCH 可先判断单条消息是否放得进响应，再决定是否 pread。
    [[nodiscard]]
    std::optional<std::uint32_t> payload_size(std::uint64_t offset) const {
        if (offset >= index_.size())
            return std::nullopt;

        return index_[static_cast<std::size_t>(offset)].length;
    }

    // 成功返回本次分配并持久化的 Offset。
    // 调用方必须在返回成功后才发送 PUBLISH ACK。
    [[nodiscard]]
    std::uint64_t append(std::string_view message) {
        if (poisoned_)
            throw std::runtime_error("message log requires reopening after I/O failure");

        if (message.size() > max_message_size)
            throw std::invalid_argument("message exceeds 256 KiB");

        if (index_.size() >= max_records ||
            header_size + message.size() > max_file_size - end_position_)
            throw std::length_error("message log is full");

        // 提前确保索引容量足够，避免写盘成功后
        // 更新内存索引时发生内存分配异常。
        index_.reserve(index_.size() + 1);

        const auto offset = next_offset();

        std::string record(header_size, '\0');

        put_be(record.data() + 0, magic, 4);
        put_be(record.data() + 4, version, 2);
        put_be(record.data() + 6, 0, 2); // flags
        put_be(record.data() + 8, message.size(), 4);
        put_be(record.data() + 16, offset, 8);

        record.append(message);

        // 校验和填写到 Header 的第 12~15 字节。
        put_be(record.data() + 12, checksum(record), 4);

        try {
            write_fully(fd_, record.data(), record.size(), end_position_);
            sync_file(fd_);
        } catch (...) {
            // 可能已经写入部分记录，
            // 也可能写完了但同步失败。
            // 当前进程不再继续追加，必须重新恢复。
            poisoned_ = true;
            throw;
        }

        index_.push_back({end_position_, static_cast<std::uint32_t>(message.size())});

        end_position_ += record.size();
        return offset;
    }

    // 按 Offset 读取，不存在时返回 nullopt。
    [[nodiscard]]
    std::optional<std::string> read(std::uint64_t offset) const {
        if (offset >= index_.size())
            return std::nullopt;

        const auto &entry = index_[static_cast<std::size_t>(offset)];

        std::string record(header_size + entry.length, '\0');
        read_fully(fd_, record.data(), record.size(), entry.position);

        // 读取时再次检查记录，避免使用已损坏的数据。
        if (get_be(record.data(), 4) != magic || get_be(record.data() + 4, 2) != version ||
            get_be(record.data() + 6, 2) != 0 || get_be(record.data() + 8, 4) != entry.length ||
            get_be(record.data() + 16, 8) != offset ||
            get_be(record.data() + 12, 4) != checksum(record))
            throw std::runtime_error("message log record corrupted during read");

        return record.substr(header_size);
    }

  private:
    struct IndexEntry {
        // 消息在日志文件中的字节位置。
        std::uint64_t position;
        std::uint32_t length;
    };

    static constexpr std::size_t header_size = 24;
    static constexpr std::uint32_t magic = 0x54494E41; // TINA
    static constexpr std::uint16_t version = 1;

    static void fail_system(const char *what) {
        throw std::system_error(errno, std::generic_category(), what);
    }

    // 不发送原生整数的内存布局，统一大端序。
    static void put_be(char *p, std::uint64_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i)
            p[i] = static_cast<char>(value >> ((bytes - 1 - i) * 8));
    }

    static std::uint64_t get_be(const char *p, unsigned bytes) {
        std::uint64_t value = 0;

        for (unsigned i = 0; i < bytes; ++i)
            value = (value << 8) | static_cast<unsigned char>(p[i]);

        return value;
    }

    // 软件 CRC32C (Castagnoli)。
    // 暂时不做 SSE4.2 硬件加速。
    static std::uint32_t crc_update(std::uint32_t crc, std::string_view bytes) {
        for (unsigned char byte : bytes) {
            crc ^= byte;

            for (int i = 0; i < 8; ++i)
                crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78U : 0U);
        }

        return crc;
    }

    static std::uint32_t checksum(std::string_view record) {
        std::uint32_t crc = 0xFFFFFFFFU;

        // Header 前 12 字节。
        crc = crc_update(crc, record.substr(0, 12));

        // 跳过 CRC 字段，计算 Offset + Payload。
        crc = crc_update(crc, record.substr(16));

        return ~crc;
    }

    // pwrite 可能只写出一部分，也可能被 EINTR 打断。
    static void write_fully(int fd, const char *p, std::size_t length, std::uint64_t pos) {
        std::size_t done = 0;
        while (done < length) {
            const auto n = ::pwrite(fd, p + done, length - done, static_cast<off_t>(pos + done));
            if (n < 0 && errno == EINTR)
                continue;

            if (n < 0)
                fail_system("pwrite message log");

            if (n == 0)
                throw std::runtime_error("pwrite made no progress");

            done += static_cast<std::size_t>(n);
        }
    }

    static void read_fully(int fd, char *p, std::size_t length, std::uint64_t pos) {
        std::size_t done = 0;
        while (done < length) {
            const auto n = ::pread(fd, p + done, length - done, static_cast<off_t>(pos + done));
            if (n < 0 && errno == EINTR)
                continue;

            if (n < 0)
                fail_system("pread message log");

            if (n == 0)
                throw std::runtime_error("unexpected EOF in message log");

            done += static_cast<std::size_t>(n);
        }
    }

    static void sync_parent_directories(std::filesystem::path dir) {
        if (dir.empty())
            dir = ".";

        while (true) {
            const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

            if (dfd < 0)
                fail_system("open message log directory");

            int rc;
            do {
                rc = ::fsync(dfd);
            } while (rc < 0 && errno == EINTR);

            const int saved_errno = errno;
            ::close(dfd);

            if (rc < 0)
                throw std::system_error(saved_errno, std::generic_category(),
                                        "fsync message log directory");

            const auto parent = dir.parent_path();

            if (parent == dir || (dir == "." && parent.empty()))
                break;

            dir = parent.empty() ? std::filesystem::path(".") : parent;
        }
    }

    static void sync_file(int fd) {
        while (::fdatasync(fd) < 0) {
            if (errno == EINTR)
                continue;

            fail_system("fdatasync message log");
        }
    }

    // 构造时扫描已有日志，恢复 Offset 索引。
    void recover() {
        struct stat st{};

        if (::fstat(fd_, &st) < 0)
            fail_system("fstat message log");

        if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > max_file_size)
            throw std::runtime_error("invalid message log size");

        const auto file_size = static_cast<std::uint64_t>(st.st_size);

        std::uint64_t pos = 0;

        while (pos < file_size) {
            // 不足一个完整 Header，属于不完整尾部。
            if (file_size - pos < header_size)
                break;

            std::string header(header_size, '\0');
            read_fully(fd_, header.data(), header_size, pos);

            const auto length = get_be(header.data() + 8, 4);
            const auto offset = get_be(header.data() + 16, 8);

            if (get_be(header.data(), 4) != magic || get_be(header.data() + 4, 2) != version ||
                get_be(header.data() + 6, 2) != 0 || length > max_message_size ||
                offset != index_.size() || index_.size() >= max_records)
                throw std::runtime_error("corrupt message log header at byte " +
                                         std::to_string(pos));

            // Header 完整但 Payload 不完整，截断尾部。
            if (length > file_size - pos - header_size)
                break;

            std::string record = std::move(header);
            record.resize(header_size + static_cast<std::size_t>(length));

            read_fully(fd_, record.data() + header_size, static_cast<std::size_t>(length),
                       pos + header_size);

            // CRC 错误不能直接当作正常尾部截断。
            if (get_be(record.data() + 12, 4) != checksum(record))
                throw std::runtime_error("CRC32C mismatch at byte " + std::to_string(pos));

            index_.push_back({pos, static_cast<std::uint32_t>(length)});

            pos += header_size + length;
        }

        // 只修复确认不完整的尾部记录。
        if (pos != file_size) {
            if (::ftruncate(fd_, static_cast<off_t>(pos)) < 0)
                fail_system("ftruncate incomplete log tail");

            sync_file(fd_);
        }

        end_position_ = pos;
    }

    int fd_{-1};
    std::vector<IndexEntry> index_;
    std::uint64_t end_position_{0};
    bool poisoned_{false};
};

} // namespace broker

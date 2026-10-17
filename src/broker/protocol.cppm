export module broker.protocol;

import std;
import rpc.protocol;

namespace {

// 整数统一采用大端序，可以考虑 SIMD
void put_number(std::string &out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = bytes; i > 0; --i)
        out.push_back(static_cast<char>(value >> ((i - 1) * 8)));
}

// 读取二进制 Payload 的辅助类。
class Reader {
  public:
    explicit Reader(std::string_view data) : data_(data) {
    }

    std::optional<std::uint64_t> number(unsigned bytes) {
        if (bytes > data_.size() - pos_)
            return std::nullopt;

        std::uint64_t value = 0;

        for (unsigned i = 0; i < bytes; ++i)
            value = (value << 8) | static_cast<unsigned char>(data_[pos_++]);

        return value;
    }

    std::optional<std::string_view> take(std::size_t bytes) {
        if (bytes > data_.size() - pos_)
            return std::nullopt;

        auto part = data_.substr(pos_, bytes);
        pos_ += bytes;
        return part;
    }

    std::size_t remaining() const noexcept {
        return data_.size() - pos_;
    }

    bool finished() const noexcept {
        return pos_ == data_.size();
    }

  private:
    std::string_view data_;
    std::size_t pos_{0};
};

// Topic / Consumer 都是标识符，不是文件路径。避免出现 "/"、NUL 等特殊字符。
bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 128 || name == "." || name == "..")
        return false;

    for (unsigned char c : name) {
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');

        const bool digit = c >= '0' && c <= '9';

        if (!alpha && !digit && c != '_' && c != '-' && c != '.')
            return false;
    }

    return true;
}

void put_name(std::string &out, std::string_view name) {
    if (!valid_name(name))
        throw std::invalid_argument("invalid broker name");

    put_number(out, name.size(), 2);
    out.append(name);
}

std::optional<std::string> read_name(Reader &reader) {
    auto length = reader.number(2);

    if (!length || *length > 128)
        return std::nullopt;

    auto bytes = reader.take(static_cast<std::size_t>(*length));

    if (!bytes || !valid_name(*bytes))
        return std::nullopt;

    return std::string(*bytes);
}

} // namespace

export namespace broker::protocol {

constexpr std::size_t max_message_size = 256 * 1024;

// 生产者发送一条消息
struct Publish {
    std::string topic;
    std::uint32_t partition{0};
    std::string message;
};

// 从指定 Offset 开始拉取消息。
// max_bytes 是整个成功响应 Payload 的字节预算。
struct Fetch {
    std::string topic;
    std::uint32_t partition{0};
    std::uint64_t offset{0};
    std::uint32_t max_bytes{0};
};

// 消费者提交下一次准备读取的位置。
// next_offset 不是最后一条已消费消息的 Offset。
struct CommitOffset {
    std::string consumer;
    std::string topic;
    std::uint32_t partition{0};
    std::uint64_t next_offset{0};
};

// PUBLISH 成功后，Broker 返回分配的 Offset。
struct PublishResult {
    std::uint64_t offset{0};
};

// FETCH 返回的单条记录。
struct Record {
    std::uint64_t offset{0};
    std::string message;
};

// FETCH 成功响应。
// records 为空时，next_offset 应等于请求的 offset。
struct FetchResult {
    std::uint64_t next_offset{0};
    std::vector<Record> records;
};

// PUBLISH
//
// [topic_len:u16]
// [topic:bytes]
// [partition:u32]
// [message_len:u32]
// [message:bytes]
std::string encode_publish(const Publish &request) {
    if (request.message.size() > max_message_size)
        throw std::invalid_argument("broker message too large");

    std::string out;

    put_name(out, request.topic);
    put_number(out, request.partition, 4);
    put_number(out, request.message.size(), 4);
    out.append(request.message);

    if (out.size() > rpc::max_payload_size)
        throw std::invalid_argument("RPC payload too large");

    return out;
}

std::optional<Publish> decode_publish(std::string_view payload) {
    if (payload.size() > rpc::max_payload_size)
        return std::nullopt;

    Reader reader(payload);

    auto topic = read_name(reader);
    auto partition = reader.number(4);
    auto size = reader.number(4);

    if (!topic || !partition || !size || *size > max_message_size)
        return std::nullopt;

    auto message = reader.take(static_cast<std::size_t>(*size));

    if (!message || !reader.finished())
        return std::nullopt;

    return Publish{std::move(*topic), static_cast<std::uint32_t>(*partition),
                   std::string(*message)};
}

// FETCH
// [topic_len:u16]
// [topic:bytes]
// [partition:u32]
// [offset:u64]
// [max_bytes:u32]
std::string encode_fetch(const Fetch &request) {
    // 空 FETCH 响应也需要 8 字节 next_offset 和 4 字节 record_count。
    if (request.max_bytes < 12 || request.max_bytes > rpc::max_payload_size)
        throw std::invalid_argument("invalid fetch byte budget");

    std::string out;

    put_name(out, request.topic);
    put_number(out, request.partition, 4);
    put_number(out, request.offset, 8);
    put_number(out, request.max_bytes, 4);

    return out;
}

std::optional<Fetch> decode_fetch(std::string_view payload) {
    Reader reader(payload);

    auto topic = read_name(reader);
    auto partition = reader.number(4);
    auto offset = reader.number(8);
    auto max_bytes = reader.number(4);

    if (!topic || !partition || !offset || !max_bytes || !reader.finished() || *max_bytes < 12 ||
        *max_bytes > rpc::max_payload_size)
        return std::nullopt;

    return Fetch{std::move(*topic), static_cast<std::uint32_t>(*partition), *offset,
                 static_cast<std::uint32_t>(*max_bytes)};
}

// COMMIT_OFFSET
// [consumer_len:u16]
// [consumer:bytes]
// [topic_len:u16]
// [topic:bytes]
// [partition:u32]
// [next_offset:u64]
std::string encode_commit_offset(const CommitOffset &request) {

    std::string out;

    put_name(out, request.consumer);
    put_name(out, request.topic);
    put_number(out, request.partition, 4);
    put_number(out, request.next_offset, 8);

    return out;
}

std::optional<CommitOffset> decode_commit_offset(std::string_view payload) {
    Reader reader(payload);

    auto consumer = read_name(reader);
    auto topic = read_name(reader);
    auto partition = reader.number(4);
    auto next_offset = reader.number(8);

    if (!consumer || !topic || !partition || !next_offset || !reader.finished())
        return std::nullopt;

    return CommitOffset{std::move(*consumer), std::move(*topic),
                        static_cast<std::uint32_t>(*partition), *next_offset};
}

// PUBLISH 成功响应
// [assigned_offset:u64]
std::string encode_publish_result(const PublishResult &result) {

    std::string out;
    put_number(out, result.offset, 8);
    return out;
}

std::optional<PublishResult> decode_publish_result(std::string_view payload) {
    Reader reader(payload);

    auto offset = reader.number(8);

    if (!offset || !reader.finished())
        return std::nullopt;

    return PublishResult{*offset};
}

// FETCH 成功响应
// [next_offset:u64]
// [count:u32]
// 重复 count 次：
//   [offset:u64]
//   [message_len:u32]
//   [message:bytes]
std::string encode_fetch_result(const FetchResult &result) {

    if (result.records.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("too many broker records");

    // 当前使用 Append-only Log：
    // 一个 Partition 的消息 Offset 必须连续递增。
    if (!result.records.empty()) {
        std::uint64_t expected = result.records.front().offset;

        for (const auto &record : result.records) {
            if (record.offset != expected || expected == std::numeric_limits<std::uint64_t>::max())
                throw std::invalid_argument("non-contiguous broker offsets");

            ++expected;
        }

        if (result.next_offset != expected)
            throw std::invalid_argument("invalid next offset");
    }

    std::string out;

    put_number(out, result.next_offset, 8);
    put_number(out, result.records.size(), 4);

    for (const auto &record : result.records) {
        // 每条消息不能超过 256 KiB；
        // 整个 RPC Payload 不能超过 1 MiB。
        if (record.message.size() > max_message_size ||
            12 + record.message.size() > rpc::max_payload_size - out.size())
            throw std::invalid_argument("fetch response exceeds RPC limit");

        put_number(out, record.offset, 8);
        put_number(out, record.message.size(), 4);
        out.append(record.message);
    }

    return out;
}

std::optional<FetchResult> decode_fetch_result(std::string_view payload) {
    if (payload.size() > rpc::max_payload_size)
        return std::nullopt;

    Reader reader(payload);

    auto next_offset = reader.number(8);
    auto count = reader.number(4);

    // 至少要有 count 个 12 字节的记录头。
    // 先检查，避免恶意 count 导致大量内存分配。
    if (!next_offset || !count || *count > reader.remaining() / 12)
        return std::nullopt;

    FetchResult result{.next_offset = *next_offset, .records = {}};

    result.records.reserve(static_cast<std::size_t>(*count));

    for (std::uint64_t i = 0; i < *count; ++i) {
        auto offset = reader.number(8);
        auto size = reader.number(4);

        if (!offset || !size || *size > max_message_size)
            return std::nullopt;

        auto message = reader.take(static_cast<std::size_t>(*size));

        if (!message)
            return std::nullopt;

        result.records.push_back(Record{*offset, std::string(*message)});
    }

    if (!reader.finished())
        return std::nullopt;

    // 检查返回结果的 Offset 连续性。
    if (!result.records.empty()) {
        std::uint64_t expected = result.records.front().offset;

        for (const auto &record : result.records) {
            if (record.offset != expected || expected == std::numeric_limits<std::uint64_t>::max())
                return std::nullopt;

            ++expected;
        }

        if (result.next_offset != expected)
            return std::nullopt;
    }

    return result;
}

// COMMIT_OFFSET 成功时使用空 Payload。失败通过 RPC Status 表达。

} // namespace broker::protocol
export module file.protocol;

import std;

namespace {

// 文件业务协议统一采用网络字节序（大端），不直接发送 C++ struct 的内存布局。
void put_number(std::string &out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = bytes; i != 0; --i)
        out.push_back(static_cast<char>(value >> ((i - 1) * 8)));
}

class Reader {
  public:
    explicit Reader(std::string_view input) : input_(input) {
    }

    std::optional<std::uint64_t> number(unsigned bytes) {
        if (bytes > input_.size() - pos_)
            return std::nullopt;

        std::uint64_t value = 0;
        for (unsigned i = 0; i < bytes; ++i)
            value = (value << 8) | static_cast<unsigned char>(input_[pos_++]);
        return value;
    }

    std::optional<std::string_view> take(std::size_t bytes) {
        if (bytes > input_.size() - pos_)
            return std::nullopt;

        const auto data = input_.substr(pos_, bytes);
        pos_ += bytes;
        return data;
    }

    [[nodiscard]] std::string_view remaining() const noexcept {
        return input_.substr(pos_);
    }

    [[nodiscard]] bool finished() const noexcept {
        return pos_ == input_.size();
    }

  private:
    std::string_view input_;
    std::size_t pos_{0};
};

bool valid_filename(std::string_view filename) {
    // 文件名仅作展示用元数据，服务器存储路径必须由 file_id 决定。
    return !filename.empty() && filename.size() <= 255 && filename != "." && filename != ".." &&
           filename.find_first_of("/\\\0", 0, 3) == std::string_view::npos;
}

} // namespace

export namespace file::protocol {

constexpr std::uint32_t chunk_size = 256 * 1024;

struct BeginUpload {
    std::uint64_t file_size;
    std::string filename;
};

struct PutChunk {
    std::uint64_t upload_id;
    std::uint32_t chunk_index;
    std::uint32_t checksum; // data 的 CRC32C；由 FileService 验证。
    std::string data;
};

struct CommitUpload {
    std::uint64_t upload_id;
};

struct GetRange {
    std::uint64_t file_id;
    std::uint64_t offset;
    std::uint32_t length;
};

// BEGIN_UPLOAD 请求：[file_size:u64][name_length:u16][filename:bytes]。
std::string encode_begin_upload(const BeginUpload &request) {
    if (!valid_filename(request.filename))
        throw std::invalid_argument("invalid file name");

    std::string out;
    out.reserve(10 + request.filename.size());
    put_number(out, request.file_size, 8);
    put_number(out, request.filename.size(), 2);
    out.append(request.filename);
    return out;
}

std::optional<BeginUpload> decode_begin_upload(std::string_view payload) {
    Reader reader(payload);
    auto file_size = reader.number(8);
    auto name_size = reader.number(2);
    if (!file_size || !name_size || *name_size > 255)
        return std::nullopt;

    auto name = reader.take(static_cast<std::size_t>(*name_size));
    if (!name || !reader.finished() || !valid_filename(*name))
        return std::nullopt;

    return BeginUpload{*file_size, std::string(*name)};
}

// PUT_CHUNK 请求：[upload_id:u64][chunk_index:u32][crc32c:u32][data:bytes]。
std::string encode_put_chunk(const PutChunk &request) {
    if (request.upload_id == 0 || request.data.empty() || request.data.size() > chunk_size)
        throw std::invalid_argument("invalid file chunk");

    std::string out;
    out.reserve(16 + request.data.size());
    put_number(out, request.upload_id, 8);
    put_number(out, request.chunk_index, 4);
    put_number(out, request.checksum, 4);
    out.append(request.data);
    return out;
}

std::optional<PutChunk> decode_put_chunk(std::string_view payload) {
    Reader reader(payload);
    auto upload_id = reader.number(8);
    auto chunk_index = reader.number(4);
    auto checksum = reader.number(4);
    if (!upload_id || *upload_id == 0 || !chunk_index || !checksum)
        return std::nullopt;

    auto data = reader.remaining();
    if (data.empty() || data.size() > chunk_size)
        return std::nullopt;

    return PutChunk{*upload_id, static_cast<std::uint32_t>(*chunk_index),
                    static_cast<std::uint32_t>(*checksum), std::string(data)};
}

// COMMIT_UPLOAD 请求：[upload_id:u64]。
std::string encode_commit_upload(const CommitUpload &request) {
    if (request.upload_id == 0)
        throw std::invalid_argument("invalid upload id");
    std::string out;
    put_number(out, request.upload_id, 8);
    return out;
}

std::optional<CommitUpload> decode_commit_upload(std::string_view payload) {
    Reader reader(payload);
    auto upload_id = reader.number(8);
    if (!upload_id || *upload_id == 0 || !reader.finished())
        return std::nullopt;
    return CommitUpload{*upload_id};
}

// GET_RANGE 请求：[file_id:u64][offset:u64][length:u32]。
std::string encode_get_range(const GetRange &request) {
    if (request.file_id == 0 || request.length == 0 || request.length > chunk_size ||
        request.offset > std::numeric_limits<std::uint64_t>::max() - request.length)
        throw std::invalid_argument("invalid file range");

    std::string out;
    put_number(out, request.file_id, 8);
    put_number(out, request.offset, 8);
    put_number(out, request.length, 4);
    return out;
}

std::optional<GetRange> decode_get_range(std::string_view payload) {
    Reader reader(payload);
    auto file_id = reader.number(8);
    auto offset = reader.number(8);
    auto length = reader.number(4);
    if (!file_id || *file_id == 0 || !offset || !length || *length == 0 ||
        *length > chunk_size || !reader.finished() ||
        *offset > std::numeric_limits<std::uint64_t>::max() - *length)
        return std::nullopt;

    return GetRange{*file_id, *offset, static_cast<std::uint32_t>(*length)};
}

// BEGIN_UPLOAD 成功响应是 upload_id，COMMIT_UPLOAD 成功响应是 file_id。
// 两种 ID 均使用独立的 8 字节大端 payload；PUT_CHUNK 成功响应为空。
// GET_RANGE 成功响应直接是原始文件字节，具体长度由请求和文件范围决定。
std::string encode_id(std::uint64_t id) {
    if (id == 0)
        throw std::invalid_argument("invalid id");
    std::string out;
    put_number(out, id, 8);
    return out;
}

std::optional<std::uint64_t> decode_id(std::string_view payload) {
    Reader reader(payload);
    auto id = reader.number(8);
    if (!id || *id == 0 || !reader.finished())
        return std::nullopt;
    return id;
}

} // namespace file::protocol

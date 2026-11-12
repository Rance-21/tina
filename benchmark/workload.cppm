export module bench.workload;

import std;
import ide.client_protocol;

export namespace bench {

enum class WorkloadType : std::uint8_t {
    ping = 0,
    kv_set = 1,
    kv_get = 2,
    kv_mixed = 3,
    file_read = 4,
    list_dir = 5,
};

[[nodiscard]] constexpr std::string_view workload_name(WorkloadType type) noexcept {
    switch (type) {
    case WorkloadType::ping:
        return "ping";
    case WorkloadType::kv_set:
        return "kv_set";
    case WorkloadType::kv_get:
        return "kv_get";
    case WorkloadType::kv_mixed:
        return "kv_mixed";
    case WorkloadType::file_read:
        return "file_read";
    case WorkloadType::list_dir:
        return "list_dir";
    }
    return "unknown";
}

[[nodiscard]] WorkloadType parse_workload_type(std::string_view text) {
    if (text == "ping")
        return WorkloadType::ping;
    if (text == "kv_set")
        return WorkloadType::kv_set;
    if (text == "kv_get")
        return WorkloadType::kv_get;
    if (text == "kv_mixed")
        return WorkloadType::kv_mixed;
    if (text == "file_read")
        return WorkloadType::file_read;
    if (text == "list_dir")
        return WorkloadType::list_dir;
    throw std::invalid_argument("unknown workload type: " + std::string(text));
}

struct WorkloadRequest {
    Method method{Method::system_ping};
    std::string payload;

    [[nodiscard]] inline std::size_t wire_bytes() const noexcept {
        return frame_header_size + payload.size();
    }
};

struct WorkloadConfig {
    WorkloadType type{WorkloadType::ping};
    std::size_t key_space{1024};
    std::size_t value_bytes{64};
    std::string file_path{"tina/CMakeLists.txt"};
    std::string dir_path{"tina/src"};
    std::uint32_t read_max_bytes{4096};
};

// Workload：生成 Ping、KV、文件操作等请求，并校验服务端响应。
class Workload {
  public:
    explicit Workload(WorkloadConfig config = {})
        : config_(std::move(config)), value_template_(config_.value_bytes, 'x') {
        if (config_.key_space == 0) {
            config_.key_space = 1;
        }
        if (config_.type == WorkloadType::file_read) {
            cached_file_read_payload_ = client::encode_workspace_read_file(
                config_.file_path, 0, config_.read_max_bytes);
        } else if (config_.type == WorkloadType::list_dir) {
            cached_list_dir_payload_ = client::encode_workspace_list_dir(config_.dir_path);
        }
    }

    [[nodiscard]] inline WorkloadType type() const noexcept {
        return config_.type;
    }

    [[nodiscard]] WorkloadRequest next_request() {
        const auto seq = sequence_++;

        switch (config_.type) {
        case WorkloadType::ping:
            return WorkloadRequest{
                .method = Method::system_ping,
                .payload = {},
            };

        case WorkloadType::kv_set:
            return make_kv_set(seq);

        case WorkloadType::kv_get:
            return make_kv_get(seq);

        case WorkloadType::kv_mixed:
            // 80% GET + 20% SET
            if (seq % 5 == 0) {
                return make_kv_set(seq);
            }
            return make_kv_get(seq);

        case WorkloadType::file_read:
            return WorkloadRequest{
                .method = Method::workspace_read_file,
                .payload = cached_file_read_payload_,
            };

        case WorkloadType::list_dir:
            return WorkloadRequest{
                .method = Method::workspace_list_dir,
                .payload = cached_list_dir_payload_,
            };
        }

        return WorkloadRequest{.method = Method::system_ping, .payload = {}};
    }

    [[nodiscard]] bool validate_response(Method request_method, const Frame &response) const noexcept {
        if (response.kind != Kind::response || response.method != request_method) {
            return false;
        }

        switch (request_method) {
        case Method::system_ping:
            return response.status == Status::ok && response.payload == "PONG";

        case Method::kv_set:
            return response.status == Status::ok && response.payload == "OK";

        case Method::kv_get:
            return response.status == Status::ok || response.status == Status::not_found;

        case Method::kv_del:
            return response.status == Status::ok || response.status == Status::not_found;

        case Method::workspace_read_file:
        case Method::workspace_list_dir:
        case Method::workspace_list_projects:
            return response.status == Status::ok;

        default:
            return response.status == Status::ok;
        }
    }

  private:
    [[nodiscard]] WorkloadRequest make_kv_set(std::uint64_t seq) const {
        const auto key_id = seq % config_.key_space;
        std::string payload;
        payload.reserve(16 + value_template_.size());
        payload.append("bench-key-");
        payload.append(std::to_string(key_id));
        payload.push_back('\n');
        payload.append(value_template_);
        return WorkloadRequest{
            .method = Method::kv_set,
            .payload = std::move(payload),
        };
    }

    [[nodiscard]] WorkloadRequest make_kv_get(std::uint64_t seq) const {
        const auto key_id = seq % config_.key_space;
        return WorkloadRequest{
            .method = Method::kv_get,
            .payload = "bench-key-" + std::to_string(key_id),
        };
    }

    WorkloadConfig config_;
    std::uint64_t sequence_{0};
    std::string value_template_;
    std::string cached_file_read_payload_;
    std::string cached_list_dir_payload_;
};

} // namespace bench

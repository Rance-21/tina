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
    fs_mixed = 6, // 仅 workspace_read_file + workspace_list_dir 的混合负载（遍历大型目录树）
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
    case WorkloadType::fs_mixed:
        return "fs_mixed";
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
    if (text == "fs_mixed")
        return WorkloadType::fs_mixed;
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

    // fs_mixed 专用：目录树根（相对 ~/projects，例如 "linux"）。
    // 压测程序需与服务端运行在同一台机器上，启动时会在本地扫描该目录树以构建路径池。
    std::string fs_root{"linux"};
    // 路径池上限：超出时按固定种子确定性下采样，避免一次性对服务端注册过多 inotify 监听。
    std::size_t fs_max_dirs{2048};
    std::size_t fs_max_files{65536};
    // list_dir 请求占比（百分比），其余为 read_file。
    std::uint32_t fs_list_dir_percent{50};
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
        } else if (config_.type == WorkloadType::fs_mixed) {
            build_fs_pool();
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

        case WorkloadType::fs_mixed:
            return make_fs_mixed(seq);
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
    // SplitMix64：对序号做无状态哈希，使多连接共享同一 Workload 时仍能得到均匀且可复现的随机选择。
    [[nodiscard]] static std::uint64_t mix64(std::uint64_t x) noexcept {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    // 确定性下采样：固定种子洗牌后截断到 limit 个。
    static void downsample(std::vector<std::string> &items, std::size_t limit) {
        if (items.size() <= limit)
            return;
        std::mt19937_64 rng(0x7131A);
        std::ranges::shuffle(items, rng);
        items.resize(limit);
    }

    // 扫描 ~/projects/<fs_root>，收集所有目录与普通文件的“相对 ~/projects 的路径”。
    // 跳过符号链接（避免越出目录树或循环）、跳过 .git，以及超出协议 u16 长度上限的路径。
    void build_fs_pool() {
        const char *home = std::getenv("HOME");
        if (home == nullptr || home[0] == '\0')
            throw std::runtime_error("fs_mixed: HOME environment variable is not set");

        const std::filesystem::path projects = std::filesystem::path(home) / "projects";
        const std::filesystem::path root = projects / config_.fs_root;
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec))
            throw std::runtime_error("fs_mixed: directory not found: " + root.string());

        constexpr std::size_t max_path_len = std::numeric_limits<std::uint16_t>::max();

        // 根目录本身也是合法的 list_dir 目标。
        fs_dirs_.push_back(config_.fs_root);

        std::filesystem::recursive_directory_iterator it(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            const auto &entry = *it;
            if (entry.path().filename() == ".git") {
                it.disable_recursion_pending();
                continue;
            }

            std::error_code type_ec;
            const auto status = entry.symlink_status(type_ec);
            if (type_ec || std::filesystem::is_symlink(status))
                continue;

            std::string rel = std::filesystem::relative(entry.path(), projects, type_ec).generic_string();
            if (type_ec || rel.empty() || rel.size() > max_path_len)
                continue;

            if (std::filesystem::is_directory(status))
                fs_dirs_.push_back(std::move(rel));
            else if (std::filesystem::is_regular_file(status))
                fs_files_.push_back(std::move(rel));
        }

        downsample(fs_dirs_, config_.fs_max_dirs);
        downsample(fs_files_, config_.fs_max_files);

        if (fs_dirs_.empty() || fs_files_.empty())
            throw std::runtime_error("fs_mixed: no directories or files found under " + root.string());

        std::cerr << "[Workload] fs_mixed pool: " << fs_dirs_.size() << " dirs, "
                  << fs_files_.size() << " files under ~/projects/" << config_.fs_root << "\n";
    }

    // 按序号哈希：低位决定操作类型，高位决定目标路径，二者互不相关。
    [[nodiscard]] WorkloadRequest make_fs_mixed(std::uint64_t seq) const {
        const auto h = mix64(seq);
        const bool do_list = (h % 100) < config_.fs_list_dir_percent;
        const auto pick = h >> 8;

        if (do_list) {
            const auto &dir = fs_dirs_[pick % fs_dirs_.size()];
            return WorkloadRequest{
                .method = Method::workspace_list_dir,
                .payload = client::encode_workspace_list_dir(dir),
            };
        }

        const auto &file = fs_files_[pick % fs_files_.size()];
        return WorkloadRequest{
            .method = Method::workspace_read_file,
            .payload = client::encode_workspace_read_file(file, 0, config_.read_max_bytes),
        };
    }

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
    std::vector<std::string> fs_dirs_;  // fs_mixed：目录路径池（相对 ~/projects）
    std::vector<std::string> fs_files_; // fs_mixed：普通文件路径池（相对 ~/projects）
};

} // namespace bench

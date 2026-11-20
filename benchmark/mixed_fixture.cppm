module;
#include <stdlib.h>

export module bench.mixed_fixture;

import std;
import bench.workload;
import service.workspace_cache;

export namespace bench::mixed {

namespace fs = std::filesystem;

struct SourceFile {
    std::string path;
    std::uint64_t size;
};

inline bool is_within(const fs::path &path, const fs::path &parent) {
    auto p = path.begin();
    for (auto part = parent.begin(); part != parent.end(); ++part, ++p)
        if (p == path.end() || *p != *part)
            return false;
    return true;
}

inline std::string shell_quote(std::string_view text) {
    std::string result = "'";
    for (char ch : text) {
        if (ch == '\'')
            result += "'\\''";
        else
            result += ch;
    }
    return result + "'";
}

// 源码仅扫描/读取；全部可变数据都在 mkdtemp 独占的另一个工作区里。
class Fixture {
  public:
    explicit Fixture(const WorkloadConfig &config, std::size_t connections) {
        const auto *home = std::getenv("HOME");
        if (!home || !*home)
            throw std::runtime_error("ide_mixed requires HOME");
        projects = fs::canonical(fs::path(home) / "projects");
        const auto relative = fs::path(config.fs_root);
        if (relative.empty() || relative.is_absolute())
            throw std::invalid_argument("fs-root must be relative to ~/projects");
        source = fs::canonical(projects / relative);
        if (!is_within(source, projects) || source == projects || !fs::is_directory(source))
            throw std::invalid_argument("fs-root must be a directory within ~/projects");

        auto scratch_arg = fs::path(config.fs_scratch_root);
        if (scratch_arg.is_absolute())
            throw std::invalid_argument("fs-scratch-root must be relative to ~/projects");
        const auto parent = fs::canonical(projects / scratch_arg);
        if (!is_within(parent, projects) || is_within(parent, source))
            throw std::invalid_argument(
                "scratch directory must be inside ~/projects and outside fs-root");

        scan(config);
        std::string pattern = (parent / ".tina-bench-XXXXXX").string();
        if (!::mkdtemp(pattern.data()))
            throw std::runtime_error("cannot create benchmark scratch directory under " +
                                     parent.string());
        scratch = pattern;
        relative_scratch = scratch.lexically_relative(projects).generic_string();
        initial_content = "tina benchmark ownership probe: " + relative_scratch + "\n";
        try {
            for (std::size_t i = 0; i < connections; ++i) {
                const auto dir = scratch / std::to_string(i);
                fs::create_directory(dir);
                std::ofstream file(dir / "write.txt", std::ios::binary);
                file << initial_content;
                file.close();
                if (!file)
                    throw std::runtime_error("failed to create benchmark write fixture");
            }
        } catch (...) {
            cleanup();
            throw;
        }
        std::cerr << "[负载] ide_mixed: " << directories.size() << " 个目录, " << hot.size()
                  << " 个热文件 (<=32K: " << hot_small_count << ", >32K: " << hot_large_count
                  << "), " << cold.size() << " 个冷文件\n"
                  << "[负载] 临时工作区: " << scratch << " (源码树全程只读)\n";
    }

    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    ~Fixture() {
        cleanup();
    }

    void cleanup() noexcept {
        if (scratch.empty())
            return;
        std::error_code ec;
        fs::remove_all(scratch, ec);
        if (ec)
            std::cerr << "[Workload] Cleanup failed: " << scratch << ": " << ec.message() << '\n';
        else
            scratch.clear();
    }

    void cleanup_checked() {
        cleanup();
        if (!scratch.empty())
            throw std::runtime_error("benchmark scratch cleanup failed");
    }

    fs::path projects, source, scratch;
    std::string relative_scratch, initial_content;
    std::vector<std::string> directories;
    std::vector<SourceFile> hot, cold;
    std::size_t hot_small_count{0}, hot_large_count{0};
    const std::size_t cache_limit = service::WorkspaceCacheConfig{}.max_per_file_bytes;

  private:
    // 【作用】扫描只读源码树构建目录池、冷文件池（> 2MiB）与分层热文件池（<=32K 与 >32K 各占一半）。
    // 【原理】按 32 KiB 阈值把热文件分为 Inline 组与 Normal 组各抽样最多 32 个并交替排布（任一组不足时
    //         由另一组补足至 64 个），确保热读负载同时充分压测 I/O Inline 与 Normal Worker 路径。
    void scan(const WorkloadConfig &config) {
        constexpr std::size_t kInlineThreshold = 32 * 1024;
        std::vector<SourceFile> hot_small, hot_large;

        directories.push_back(source.lexically_relative(projects).generic_string());
        for (fs::recursive_directory_iterator it(source), end; it != end; ++it) {
            const auto name = it->path().filename().string();
            const auto status = it->symlink_status();
            if (name == ".git" || name.starts_with(".tina-bench-") || fs::is_symlink(status)) {
                it.disable_recursion_pending();
                continue;
            }
            const auto path = it->path().lexically_relative(projects).generic_string();
            if (path.size() > std::numeric_limits<std::uint16_t>::max() ||
                path.find_first_of("\n\r") != std::string::npos) {
                it.disable_recursion_pending();
                continue;
            }
            if (fs::is_directory(status)) {
                directories.push_back(path);
            } else if (fs::is_regular_file(status)) {
                const auto size = it->file_size();
                if (size > cache_limit) {
                    cold.push_back({path, size});
                } else if (size > 0 && size <= std::min<std::size_t>(cache_limit, 256 * 1024)) {
                    if (size <= kInlineThreshold)
                        hot_small.push_back({path, size});
                    else
                        hot_large.push_back({path, size});
                }
            }
        }
        // 排序后固定种子抽样，避免 readdir 顺序影响路径池；热集按 <=32K / >32K 分层平衡。
        std::mt19937_64 rng(0x7131A);
        std::ranges::sort(directories);
        std::ranges::shuffle(directories, rng);
        if (directories.size() > config.fs_max_dirs)
            directories.resize(config.fs_max_dirs);
        for (auto *pool : {&hot_small, &hot_large, &cold}) {
            std::ranges::sort(*pool, {}, &SourceFile::path);
            std::ranges::shuffle(*pool, rng);
        }
        if (cold.size() > 64)
            cold.resize(64);

        const std::size_t want_small = std::min<std::size_t>(
            hot_small.size(), 32 + (hot_large.size() < 32 ? (32 - hot_large.size()) : 0));
        const std::size_t want_large = std::min<std::size_t>(
            hot_large.size(), 64 - want_small);
        hot_small_count = want_small;
        hot_large_count = want_large;
        hot.reserve(want_small + want_large);
        for (std::size_t i = 0; i < std::max(want_small, want_large); ++i) {
            if (i < want_small)
                hot.push_back(std::move(hot_small[i]));
            if (i < want_large)
                hot.push_back(std::move(hot_large[i]));
        }

        if (hot.empty() || (config.fs_hot_percent < 100 && cold.empty()))
            throw std::runtime_error(
                "ide_mixed requires nonempty files <=256 KiB and, for cold reads, "
                "files larger than " +
                std::to_string(cache_limit) + " bytes");
    }
};

} // namespace bench::mixed

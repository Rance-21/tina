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
        std::cerr << "[Workload] ide_mixed: " << directories.size() << " dirs, " << hot.size()
                  << " hot files, " << cold.size() << " uncached files\n"
                  << "[Workload] Scratch: " << scratch << " (source is read-only)\n";
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
    const std::size_t cache_limit = service::WorkspaceCacheConfig{}.max_per_file_bytes;

  private:
    void scan(const WorkloadConfig &config) {
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
                if (size > cache_limit)
                    cold.push_back({path, size});
                else if (size > 0 && size <= std::min<std::size_t>(cache_limit, 256 * 1024))
                    hot.push_back({path, size});
            }
        }
        // 排序后固定种子抽样，避免 readdir 顺序影响路径池；热集最多 16 MiB。
        std::mt19937_64 rng(0x7131A);
        std::ranges::sort(directories);
        std::ranges::shuffle(directories, rng);
        if (directories.size() > config.fs_max_dirs)
            directories.resize(config.fs_max_dirs);
        for (auto *pool : {&hot, &cold}) {
            std::ranges::sort(*pool, {}, &SourceFile::path);
            std::ranges::shuffle(*pool, rng);
            if (pool->size() > 64)
                pool->resize(64);
        }
        if (hot.empty() || (config.fs_hot_percent < 100 && cold.empty()))
            throw std::runtime_error(
                "ide_mixed requires nonempty files <=256 KiB and, for cold reads, "
                "files larger than " +
                std::to_string(cache_limit) + " bytes");
    }
};

} // namespace bench::mixed

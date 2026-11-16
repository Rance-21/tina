export module service.workspace_cache;

export import :lru;

import std;
import ide.protocol;

export namespace service {

// ============================================================================
// Workspace 目录与文件内容缓存配置
// ============================================================================
struct WorkspaceCacheConfig {
    std::size_t max_dir_entries{4096};               // 目录缓存最大条目数
    std::size_t max_dir_bytes{32 * 1024 * 1024};     // 目录缓存最大内存估算（32 MiB）
    std::size_t max_file_entries{8192};              // 文件缓存最大条目数
    std::size_t max_file_bytes{128 * 1024 * 1024};   // 文件缓存最大内存估算（128 MiB）
    std::size_t max_per_file_bytes{2 * 1024 * 1024}; // 单文件允许缓存的最大字节数（2 MiB）
};

// ============================================================================
// Workspace 统一缓存门面（同时管理目录列表与文件内容两类数据结构）
// ============================================================================
class WorkspaceCache {
  public:
    explicit WorkspaceCache(WorkspaceCacheConfig config = {})
        : config_(config), dirs_(config.max_dir_entries, config.max_dir_bytes),
          files_(config.max_file_entries, config.max_file_bytes) {
    }

    WorkspaceCache(const WorkspaceCache &) = delete;
    WorkspaceCache &operator=(const WorkspaceCache &) = delete;

    [[nodiscard]] std::size_t max_cacheable_file_bytes() const noexcept {
        return config_.max_per_file_bytes;
    }

    // ---- 目录列表缓存（workspace_list_dir，缓存 std::vector<DirEntry>） ----

    [[nodiscard]] inline std::shared_ptr<const std::vector<DirEntry>>
    get_dir(std::string_view rel_path) {
        return dirs_.get(rel_path);
    }

    [[nodiscard]] std::uint64_t dir_epoch(std::string_view rel_path) const {
        return dirs_.epoch(rel_path);
    }

    void put_dir(std::string_view rel_path, std::shared_ptr<const std::vector<DirEntry>> entries,
                 std::uint64_t expected_epoch) {
        if (!entries)
            return;
        const std::size_t approx_bytes = estimate_dir_bytes(rel_path, *entries);
        dirs_.put(rel_path, std::move(entries), approx_bytes, expected_epoch, false);
    }

    // ---- 文件内容缓存（workspace_read_file，缓存完整 ReadFileResponse） ----

    [[nodiscard]] std::shared_ptr<const ReadFileResponse> get_file(std::string_view rel_path) {
        return files_.get(rel_path);
    }

    [[nodiscard]] std::uint64_t file_epoch(std::string_view rel_path) const {
        return files_.epoch(rel_path);
    }

    void put_file(std::string_view rel_path, std::shared_ptr<const ReadFileResponse> response,
                  std::uint64_t expected_epoch) {
        if (!response || response->content.size() > config_.max_per_file_bytes)
            return;
        const std::size_t approx_bytes = estimate_file_bytes(rel_path, *response);
        files_.put(rel_path, std::move(response), approx_bytes, expected_epoch, false);
    }

    // workspace_write_file 成功落盘后调用：若大小在上限内则直接更新缓存，否则失效旧条目。
    void put_file_after_write(std::string_view rel_path,
                              std::shared_ptr<const ReadFileResponse> response) {
        if (!response || response->content.size() > config_.max_per_file_bytes) {
            files_.invalidate(rel_path);
            return;
        }
        const std::size_t approx_bytes = estimate_file_bytes(rel_path, *response);
        files_.put(rel_path, std::move(response), approx_bytes, std::nullopt, true);
    }

    // ---- 失效与淘汰接口（供 inotify 事件、unwatch、write_file 调用） ----

    void invalidate_dir(std::string_view rel_path) noexcept {
        dirs_.invalidate(rel_path);
    }

    void invalidate_file(std::string_view rel_path) noexcept {
        files_.invalidate(rel_path);
    }

    // unwatch 目录时调用：淘汰该目录自身缓存及其所有直接子文件的缓存。
    void invalidate_dir_and_files(std::string_view dir_rel_path) noexcept {
        dirs_.invalidate(dir_rel_path);
        files_.invalidate_by_parent(dir_rel_path);
    }

    // 目录被删除/移动或内核销毁监听时调用：级联淘汰整棵子目录树。
    void invalidate_subtree(std::string_view dir_rel_path) noexcept {
        dirs_.invalidate_subtree(dir_rel_path);
        files_.invalidate_subtree(dir_rel_path);
    }

    // 设置文件缓存容量淘汰回调（供 FileWatcher 释放对应文件的 inotify watch）。
    void set_file_evict_callback(std::function<void(std::string_view)> cb) {
        files_.set_evict_callback(std::move(cb));
    }

    void clear_dirs() noexcept {
        dirs_.clear();
    }

    void clear_files() noexcept {
        files_.clear();
    }

    void clear() noexcept {
        dirs_.clear();
        files_.clear();
    }

    [[nodiscard]] std::size_t dir_cache_size() const noexcept {
        return dirs_.size();
    }

    [[nodiscard]] std::size_t file_cache_size() const noexcept {
        return files_.size();
    }

  private:
    [[nodiscard]] static std::size_t
    estimate_dir_bytes(std::string_view rel_path, const std::vector<DirEntry> &entries) noexcept {
        std::size_t total = 64 + rel_path.size() + entries.size() * sizeof(DirEntry);
        for (const auto &entry : entries)
            total += entry.name.size();
        return total;
    }

    [[nodiscard]] static std::size_t
    estimate_file_bytes(std::string_view rel_path, const ReadFileResponse &response) noexcept {
        return 64 + rel_path.size() + sizeof(ReadFileResponse) + response.content.size();
    }

    WorkspaceCacheConfig config_;
    detail::ShardedLruCache<std::vector<DirEntry>, false> dirs_;
    detail::ShardedLruCache<ReadFileResponse, true> files_;
};

// 进程级全局 Workspace 缓存实例，由 Worker 线程与各 I/O 线程的 DirWatcher 共享。
[[nodiscard]] inline WorkspaceCache &global_workspace_cache() {
    static WorkspaceCache instance{};
    return instance;
}

} // namespace service

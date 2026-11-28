export module service.workspace_cache;

export import :lru;

import std;
import rpc.payload;
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
        : max_per_file_bytes_(config.max_per_file_bytes),
          dirs_(config.max_dir_entries, config.max_dir_bytes),
          files_(config.max_file_entries, config.max_file_bytes) {
    }

    WorkspaceCache(const WorkspaceCache &) = delete;
    WorkspaceCache &operator=(const WorkspaceCache &) = delete;

    [[nodiscard]] std::size_t max_cacheable_file_bytes() const noexcept {
        return max_per_file_bytes_;
    }

    // ---- 目录列表缓存（workspace_list_dir，直接缓存协议编码后的 Payload） ----
    using DirHandle = detail::CacheHandle<std::string>;
    using FileHandle = detail::CacheHandle<ReadFileResponse>;

    [[nodiscard]] DirHandle get_dir(std::string_view rel_path,
                                    std::uint64_t *miss_epoch = nullptr) {
        return dirs_.get(rel_path, miss_epoch);
    }

    [[nodiscard]] std::uint64_t dir_epoch(std::string_view rel_path) const {
        return dirs_.epoch(rel_path);
    }

    DirHandle put_dir(std::string_view rel_path, std::shared_ptr<const std::string> payload,
                      std::uint64_t expected_epoch) {
        if (!payload)
            return {};
        const std::size_t bytes = 64 + rel_path.size() + sizeof(std::string) + payload->size();
        return dirs_.put(rel_path, std::move(payload), bytes, expected_epoch);
    }

    // ---- 文件内容缓存（workspace_read_file，缓存完整 ReadFileResponse） ----
    [[nodiscard]] FileHandle get_file(std::string_view rel_path,
                                      std::uint64_t *miss_epoch = nullptr) {
        return files_.get(rel_path, miss_epoch);
    }

    [[nodiscard]] std::uint64_t file_epoch(std::string_view rel_path) const {
        return files_.epoch(rel_path);
    }

    FileHandle put_file(std::string_view rel_path, std::shared_ptr<const ReadFileResponse> response,
                        std::uint64_t expected_epoch) {
        if (!response || response->content.size() > max_per_file_bytes_)
            return {};
        const std::size_t bytes =
            64 + rel_path.size() + sizeof(ReadFileResponse) + response->content.size();
        return files_.put(rel_path, std::move(response), bytes, expected_epoch);
    }

    // workspace_write_file 成功落盘后调用：先推进代数失效旧条目，若大小在上限内则写入新快照。
    void put_file_after_write(std::string_view rel_path,
                              std::shared_ptr<const ReadFileResponse> response) {
        put_file(rel_path, std::move(response), files_.invalidate(rel_path));
    }

    // ---- 失效与淘汰接口（供 inotify 事件、unwatch、write_file 调用） ----

    void invalidate_dir(std::string_view rel_path) noexcept {
        dirs_.invalidate(rel_path);
    }

    std::uint64_t invalidate_file(std::string_view rel_path) noexcept {
        return files_.invalidate(rel_path);
    }

    // unwatch 目录时调用：淘汰该目录自身缓存及其所有直接子文件的缓存。
    void invalidate_dir_and_files(std::string_view dir_rel_path) noexcept {
        dirs_.invalidate(dir_rel_path);
        files_.invalidate_matching([&](auto path) {
            return detail::parent_dir_of(path) == dir_rel_path;
        });
    }

    // 目录被删除/移动或内核销毁监听时调用：级联淘汰整棵子目录树。
    void invalidate_subtree(std::string_view dir_rel_path) noexcept {
        if (dir_rel_path.empty())
            return;
        dirs_.invalidate_matching([&](auto path) {
            return detail::is_same_or_under(path, dir_rel_path);
        });
        files_.invalidate_matching([&](auto path) {
            return detail::is_same_or_under(detail::parent_dir_of(path), dir_rel_path);
        });
    }

    bool cancel_file_fill_if_absent(std::string_view path) {
        return files_.invalidate_if_absent(path);
    }

    // 设置文件缓存容量淘汰回调（供 FileWatcher 释放对应文件的 inotify watch）。
    void set_file_evict_callback(std::function<void(std::string_view)> cb) {
        files_.set_evict_callback(std::move(cb));
    }

    void clear_dirs() noexcept { dirs_.clear(); }
    void clear_files() noexcept { files_.clear(); }
    void clear() noexcept {
        dirs_.clear();
        files_.clear();
    }

    [[nodiscard]] std::size_t dir_cache_size() const noexcept { return dirs_.size(); }
    [[nodiscard]] std::size_t file_cache_size() const noexcept { return files_.size(); }

  private:
    friend class WorkspaceHotCache;
    const std::size_t max_per_file_bytes_;
    detail::ShardedLruCache<std::string> dirs_;
    detail::ShardedLruCache<ReadFileResponse> files_;
};

// 进程级全局 Workspace 缓存实例，由 Worker 线程与各 I/O 线程的 DirWatcher 共享。
[[nodiscard]] inline WorkspaceCache &global_workspace_cache() {
    static WorkspaceCache instance{};
    return instance;
}

// 每 I/O 线程独立的二路组相联热表（每桶 2 x 32B 恰好独占单条 64B 缓存行）：
// 仅接纳二次命中的真热点，命中时零原子借用 Pin 免除跨簇引用计数乒乓与直映冲突抖动。
class WorkspaceHotCache {
    template <class T, std::size_t Buckets> struct Table {
        struct Entry {
            std::size_t hash{};
            std::string_view key{};
            detail::CacheHandle<T> pin{};
        };
        struct alignas(64) Bucket {
            Entry ways[2]{};
        };
        static_assert(sizeof(Bucket) == 64, "HotCache 2-way bucket must fit in one cache line");

        std::unique_ptr<Bucket[]> buckets{std::make_unique<Bucket[]>(Buckets)};

        detail::CacheHandle<T> get(detail::ShardedLruCache<T> &cache, std::string_view path,
                                   std::vector<PayloadPin> &deferred, auto bytes_of) {
            const auto h = detail::TransparentStringHash{}(path);
            auto &b = buckets[h & (Buckets - 1)];
            for (auto &e : b.ways) {
                if (e.hash == h && e.pin) {
                    if (e.pin.active()) {
                        if (e.key == path)
                            return e.pin.borrow();
                    } else {
                        deferred.push_back(std::move(e.pin));
                        e = {};
                    }
                }
            }
            std::string_view pinned_key;
            bool was_hot = false;
            auto pin = cache.get_hashed(path, h, nullptr, &pinned_key, &was_hot);
            if (!pin || !was_hot || bytes_of(*pin) > 64 * 1024)
                return pin;
            for (auto &e : b.ways) {
                if (!e.pin || !e.pin.active()) {
                    if (e.pin)
                        deferred.push_back(std::move(e.pin));
                    e = {h, pinned_key, std::move(pin)};
                    return e.pin.borrow();
                }
            }
            return pin;
        }
    };

  public:
    WorkspaceHotCache() { deferred_.reserve(32); }

    WorkspaceCache::DirHandle get_dir(WorkspaceCache &cache, std::string_view path) {
        return dirs_.get(cache.dirs_, path, deferred_, [](const auto &s) { return s.size(); });
    }
    WorkspaceCache::FileHandle get_file(WorkspaceCache &cache, std::string_view path) {
        return files_.get(cache.files_, path, deferred_,
                          [](const auto &f) { return f.content.size(); });
    }
    void end_batch() noexcept {
        if (!deferred_.empty())
            deferred_.clear();
    }

  private:
    Table<std::string, 2048> dirs_;
    Table<ReadFileResponse, 256> files_;
    std::vector<PayloadPin> deferred_;
};

} // namespace service

export module service.workspace_cache:lru;

import std;

export namespace service {

namespace detail {

// 支持 std::string_view 异构查找的透明哈希器，查询与淘汰时避免构造临时 std::string。
struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }

    [[nodiscard]] std::size_t operator()(const std::string &s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

// 提取相对路径的父目录（如 "tina/src/main.cpp" -> "tina/src"，"a.txt" -> ""）。
[[nodiscard]] inline std::string_view parent_dir_of(std::string_view rel_path) noexcept {
    const std::size_t slash = rel_path.rfind('/');
    return (slash == std::string_view::npos) ? std::string_view{} : rel_path.substr(0, slash);
}

// 判断 path 是否等于 dir_prefix 或位于其子路径下（按 '/' 边界严格匹配，避免前缀误伤）。
[[nodiscard]] inline bool is_same_or_under(std::string_view path,
                                           std::string_view dir_prefix) noexcept {
    if (path == dir_prefix)
        return true;
    return path.size() > dir_prefix.size() && path.starts_with(dir_prefix) &&
           path[dir_prefix.size()] == '/';
}

// ============================================================================
// 分片式线程安全 LRU 缓存模板
// ============================================================================
// - 分片数量固定为 16，通过路径哈希低位分片降低多线程并发锁竞争；
// - 每个分片独立维护 LRU 双向链表（std::list）与哈希表（std::unordered_map），O(1) 命中与淘汰；
// - 每个分片维护单调递增的失效版本号 epoch：后台线程读盘前记录 epoch，写回缓存时若 epoch 已变化，
//   说明读盘期间发生过 inotify 或写入失效，直接放弃写入以杜绝旧数据覆盖新状态；
// - 当 TrackParentDir == true 时额外维护父目录到子文件集合的倒排索引，支持 unwatch 时按目录 O(k)
// 淘汰。
template <typename Value, bool TrackParentDir> class ShardedLruCache {
  public:
    using EvictCallback = std::function<void(std::string_view)>;
    static constexpr std::size_t kShardCount = 16;

    ShardedLruCache(std::size_t total_max_entries, std::size_t total_max_bytes)
        : max_entries_per_shard_(
              std::max<std::size_t>(1, (total_max_entries + kShardCount - 1) / kShardCount)),
          max_bytes_per_shard_(
              std::max<std::size_t>(1, (total_max_bytes + kShardCount - 1) / kShardCount)) {
    }

    // 启动期注册容量淘汰回调（并发读写开始前设置一次，分片互斥锁释放后只读调用）。
    void set_evict_callback(EvictCallback cb) {
        evict_cb_ = std::move(cb);
    }

    // 读取缓存：命中时提升至 LRU 链表头部并返回只读共享指针，不复制底层数据结构。
    [[nodiscard]] std::shared_ptr<const Value> get(std::string_view key) {
        Shard &shard = shard_for(key);
        std::lock_guard lock(shard.mutex);

        auto it = shard.entries.find(key);
        if (it == shard.entries.end())
            return nullptr;

        shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_it);
        return it->second.value;
    }

    // 获取当前分片的失效版本号（读盘前调用）。
    [[nodiscard]] std::uint64_t epoch(std::string_view key) const {
        const Shard &shard = shard_for(key);
        std::lock_guard lock(shard.mutex);
        return shard.epoch;
    }

    // 写入或更新缓存：若提供 expected_epoch 且与当前分片版本号不一致，则放弃写入。
    // bump_epoch 为 true 时主动推进版本号（供 write_file 覆盖更新时拒绝并发旧读回填）。
    void put(std::string_view key, std::shared_ptr<const Value> value, std::size_t approx_bytes,
             std::optional<std::uint64_t> expected_epoch = std::nullopt, bool bump_epoch = false) {
        if (!value || key.empty() || approx_bytes > max_bytes_per_shard_)
            return;

        Shard &shard = shard_for(key);
        std::unique_lock lock(shard.mutex);

        if (expected_epoch && shard.epoch != *expected_epoch)
            return;
        if (bump_epoch)
            ++shard.epoch;

        if (auto it = shard.entries.find(key); it != shard.entries.end()) {
            shard.total_bytes -= it->second.approx_bytes;
            shard.total_bytes += approx_bytes;
            it->second.value = std::move(value);
            it->second.approx_bytes = approx_bytes;
            shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_it);
        } else {
            auto [inserted_it, _] = shard.entries.try_emplace(std::string(key), Entry{});
            bool pushed_lru = false;
            try {
                shard.lru.push_front(std::string_view(inserted_it->first));
                pushed_lru = true;
                if constexpr (TrackParentDir) {
                    const std::string_view key_view(inserted_it->first);
                    const std::string_view parent = parent_dir_of(key_view);
                    auto dit = shard.dir_index.find(parent);
                    if (dit == shard.dir_index.end())
                        dit = shard.dir_index.emplace(std::string(parent), KeyViewSet{}).first;
                    dit->second.insert(key_view);
                }
            } catch (...) {
                if (pushed_lru)
                    shard.lru.pop_front();
                shard.entries.erase(inserted_it);
                throw;
            }

            inserted_it->second.value = std::move(value);
            inserted_it->second.approx_bytes = approx_bytes;
            inserted_it->second.lru_it = shard.lru.begin();
            shard.total_bytes += approx_bytes;
        }

        auto evicted_keys = evict_if_needed_locked(shard);
        lock.unlock();

        if (evict_cb_) {
            for (const std::string &evicted : evicted_keys)
                evict_cb_(evicted);
        }
    }

    // 失效单个键（推进版本号并在存在时删除）。
    void invalidate(std::string_view key) noexcept {
        if (key.empty())
            return;

        Shard &shard = shard_for(key);
        std::lock_guard lock(shard.mutex);
        ++shard.epoch;

        if (auto it = shard.entries.find(key); it != shard.entries.end())
            erase_entry_locked(shard, it);
    }

    // 失效指定父目录下的所有直接子文件（用于 unwatch 目录时淘汰关联文件缓存）。
    void invalidate_by_parent(std::string_view parent_dir) noexcept
        requires TrackParentDir
    {
        for (Shard &shard : shards_) {
            std::lock_guard lock(shard.mutex);
            ++shard.epoch;

            if (auto dit = shard.dir_index.find(parent_dir); dit != shard.dir_index.end())
                (void)erase_dir_bucket_locked(shard, dit);
        }
    }

    // 失效整棵子目录树（用于目录被删除/移动或 IN_IGNORED）。
    void invalidate_subtree(std::string_view dir_prefix) noexcept {
        if (dir_prefix.empty())
            return;

        for (Shard &shard : shards_) {
            std::lock_guard lock(shard.mutex);
            ++shard.epoch;

            if constexpr (TrackParentDir) {
                for (auto dit = shard.dir_index.begin(); dit != shard.dir_index.end();) {
                    dit = is_same_or_under(dit->first, dir_prefix)
                              ? erase_dir_bucket_locked(shard, dit)
                              : std::next(dit);
                }
            } else {
                for (auto it = shard.entries.begin(); it != shard.entries.end();) {
                    if (is_same_or_under(it->first, dir_prefix)) {
                        shard.total_bytes -= it->second.approx_bytes;
                        shard.lru.erase(it->second.lru_it);
                        it = shard.entries.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
    }

    // 清空全部分片缓存（用于 inotify IN_Q_OVERFLOW 队列溢出恢复）。
    void clear() noexcept {
        for (Shard &shard : shards_) {
            std::lock_guard lock(shard.mutex);
            ++shard.epoch;
            shard.lru.clear();
            shard.entries.clear();
            if constexpr (TrackParentDir)
                shard.dir_index.clear();
            shard.total_bytes = 0;
        }
    }

    // 统计当前缓存条目总数（主要用于单测验证）。
    [[nodiscard]] std::size_t size() const noexcept {
        std::size_t total = 0;
        for (const Shard &shard : shards_) {
            std::lock_guard lock(shard.mutex);
            total += shard.entries.size();
        }
        return total;
    }

  private:
    using LruList = std::list<std::string_view>;
    using KeyViewSet = std::unordered_set<std::string_view, TransparentStringHash, std::equal_to<>>;

    struct Entry {
        std::shared_ptr<const Value> value;
        std::size_t approx_bytes{};
        typename LruList::iterator lru_it;
    };

    using EntryMap = std::unordered_map<std::string, Entry, TransparentStringHash, std::equal_to<>>;
    using DirIndexMap =
        std::unordered_map<std::string, KeyViewSet, TransparentStringHash, std::equal_to<>>;

    struct EmptyIndex {};

    struct alignas(64) Shard {
        mutable std::mutex mutex;
        std::uint64_t epoch{0};
        std::size_t total_bytes{0};
        LruList lru;
        EntryMap entries;
        [[no_unique_address]] std::conditional_t<TrackParentDir, DirIndexMap, EmptyIndex> dir_index;
    };

    [[nodiscard]] Shard &shard_for(std::string_view key) noexcept {
        return shards_[TransparentStringHash{}(key) & (kShardCount - 1)];
    }

    [[nodiscard]] const Shard &shard_for(std::string_view key) const noexcept {
        return shards_[TransparentStringHash{}(key) & (kShardCount - 1)];
    }

    // 仅从 entries 与 lru 中删除节点（不触碰 dir_index），要求在删除 map 节点前先删除 string_view
    // 引用。
    static void erase_from_map_and_lru_locked(Shard &shard,
                                              typename EntryMap::iterator it) noexcept {
        shard.total_bytes -= it->second.approx_bytes;
        shard.lru.erase(it->second.lru_it);
        shard.entries.erase(it);
    }

    static void erase_entry_locked(Shard &shard, typename EntryMap::iterator it) noexcept {
        if constexpr (TrackParentDir) {
            const std::string_view key_view(it->first);
            const std::string_view parent = parent_dir_of(key_view);
            if (auto dit = shard.dir_index.find(parent); dit != shard.dir_index.end()) {
                dit->second.erase(key_view);
                if (dit->second.empty())
                    shard.dir_index.erase(dit);
            }
        }
        erase_from_map_and_lru_locked(shard, it);
    }

    // 删除某父目录索引桶及其关联的所有文件条目，返回 dir_index 的下一个有效迭代器。
    static typename DirIndexMap::iterator
    erase_dir_bucket_locked(Shard &shard, typename DirIndexMap::iterator dit) noexcept
        requires TrackParentDir
    {
        for (std::string_view file_key : dit->second) {
            if (auto eit = shard.entries.find(file_key); eit != shard.entries.end())
                erase_from_map_and_lru_locked(shard, eit);
        }
        return shard.dir_index.erase(dit);
    }

    [[nodiscard]] std::vector<std::string> evict_if_needed_locked(Shard &shard) {
        std::vector<std::string> evicted_keys;
        const bool collect_evicted = static_cast<bool>(evict_cb_);
        while ((shard.entries.size() > max_entries_per_shard_ ||
                shard.total_bytes > max_bytes_per_shard_) &&
               !shard.lru.empty()) {
            const std::string_view victim_key = shard.lru.back();
            auto it = shard.entries.find(victim_key);
            if (it == shard.entries.end()) {
                shard.lru.pop_back();
                continue;
            }
            if (collect_evicted)
                evicted_keys.emplace_back(it->first);
            erase_entry_locked(shard, it);
        }
        return evicted_keys;
    }

    std::size_t max_entries_per_shard_;
    std::size_t max_bytes_per_shard_;
    std::array<Shard, kShardCount> shards_;
    EvictCallback evict_cb_;
};

} // namespace detail

} // namespace service

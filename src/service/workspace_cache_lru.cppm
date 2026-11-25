export module service.workspace_cache:lru;

import std;
import rpc.payload;

export namespace service::detail {

struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view path) const noexcept {
        return std::hash<std::string_view>{}(path);
    }
};

inline std::string_view parent_dir_of(std::string_view path) noexcept {
    const auto slash = path.rfind('/');
    return slash == path.npos ? std::string_view{} : path.substr(0, slash);
}

inline bool is_same_or_under(std::string_view path, std::string_view dir) noexcept {
    return path == dir ||
           (path.size() > dir.size() && path.starts_with(dir) && path[dir.size()] == '/');
}

template <class T> class CacheHandle : public PayloadPin {
  public:
    using PayloadPin::PayloadPin;
    const T *get() const noexcept {
        return static_cast<const T *>(PayloadPin::get());
    }
    const T *operator->() const noexcept {
        return get();
    }
    const T &operator*() const noexcept {
        return *get();
    }
};

// 每个 shard 启动时一次性分配槽位和哈希桶。桶链处理完整哈希冲突，最后按完整
// 路径判等。LRU 与空闲链直接链接槽位，命中、换位和复用均不分配节点。
template <class T> class ShardedLruCache {
    static constexpr unsigned shard_bits = 4;
    static constexpr std::size_t shard_count = 1u << shard_bits;
    struct Shard;
    struct alignas(64) Slot : PayloadPin::Slot {
        Shard *shard;
        std::string key;
        std::size_t hash{}, bytes{};
        Slot *chain{}, *newer{}, *older{};
    };

    struct alignas(64) Shard {
        std::mutex mutex;
        std::size_t bytes{};
        Slot *head{}, *tail{}, *free{};
        std::vector<Slot> slots;
        // 同一个桶内的键共用失效代数。单键驱逐只会撤销本桶的在途填充，
        // 不会令整个分片其余路径刚完成的读盘作废。批量失效也覆盖尚未入表的键。
        struct Bucket {
            Slot *first{};
            std::uint64_t epoch{};
        };
        std::vector<Bucket> buckets;
        // 通常由缓存独占；仅当缓存先于外借 Handle 销毁时，由最后一个槽位释放本分片。
        std::shared_ptr<Shard> orphan;

        explicit Shard(std::size_t n) : slots(n), buckets(std::bit_ceil(n)) {
            for (auto &s : slots) {
                s.shard = this;
                s.reclaim = release_retired;
                s.chain = free;
                free = &s;
            }
        }

        Bucket &bucket(std::size_t hash) {
            return buckets[(hash >> shard_bits) & (buckets.size() - 1)];
        }
        Slot *find(std::string_view key, std::size_t hash) {
            for (auto *s = bucket(hash).first; s; s = s->chain)
                if (s->hash == hash && s->key == key)
                    return s;
            return nullptr;
        }

        void unlink_lru(Slot &s) {
            if (s.newer)
                s.newer->older = s.older;
            else
                head = s.older;
            if (s.older)
                s.older->newer = s.newer;
            else
                tail = s.newer;
        }
        void push_front(Slot &s) {
            s.newer = nullptr;
            s.older = head;
            if (head)
                head->newer = &s;
            else
                tail = &s;
            head = &s;
        }

        // 必须持锁。GET 的查表和加 Pin 在同一把锁下完成，摘掉索引后再置 retired，
        // fetch_or 的旧值精确裁决由失效方还是最后一个读者回收，同一条目只会归还一次。
        bool retire(Slot &s) {
            Slot **link = &bucket(s.hash).first;
            while (*link != &s)
                link = &(*link)->chain;
            *link = s.chain;
            unlink_lru(s);
            return s.state.fetch_or(Slot::retired, std::memory_order_acq_rel) == 0;
        }

        // 批量失效/淘汰的线程已独占这些槽位；借用 chain 串起待清理项。先在锁外
        // 释放正文和路径，再一次加锁还回空闲链。被 Pin 的旧版本在实际回收前仍占预算。
        static void
        release_list(Slot *list,
                     const std::function<void(std::string_view)> *notify = nullptr) noexcept {
            if (!list)
                return;
            Shard &shard = *list->shard;
            Slot *last = nullptr;
            std::size_t released = 0;
            for (auto *s = list; s; s = s->chain) {
                if (notify && *notify) {
                    try {
                        (*notify)(s->key);
                    } catch (...) {
                    }
                }
                s->value.reset();
                std::string{}.swap(s->key);
                released += s->bytes;
                last = s;
            }
            std::shared_ptr<Shard> keep;
            std::lock_guard lock(shard.mutex);
            shard.bytes -= released;
            last->chain = shard.free;
            shard.free = list;
            if (shard.bytes == 0)
                keep = std::move(shard.orphan);
        }

        static void release_retired(PayloadPin::Slot &base) noexcept {
            auto &s = static_cast<Slot &>(base);
            s.chain = nullptr;
            release_list(&s);
        }
    };

  public:
    using Handle = CacheHandle<T>;
    using EvictCallback = std::function<void(std::string_view)>;

    ShardedLruCache(std::size_t entries, std::size_t bytes)
        : limit_(std::max<std::size_t>(1, bytes / shard_count + (bytes % shard_count != 0))) {
        const auto n =
            std::max<std::size_t>(1, entries / shard_count + (entries % shard_count != 0));
        for (auto &s : shards_)
            s = std::make_shared<Shard>(n);
    }
    ~ShardedLruCache() {
        clear();
        for (auto &s : shards_) {
            std::lock_guard lock(s->mutex);
            if (s->bytes)
                s->orphan = s;
        }
    }
    ShardedLruCache(const ShardedLruCache &) = delete;
    ShardedLruCache &operator=(const ShardedLruCache &) = delete;

    // 在启动屏障内安装。回调执行时没有持有 shard 锁，可重新查询缓存。
    void set_evict_callback(EvictCallback cb) {
        on_evict_ = std::move(cb);
    }

    Handle get(std::string_view key, std::uint64_t *miss_epoch = nullptr) {
        const auto h = TransparentStringHash{}(key);
        auto &shard = *shards_[h & (shard_count - 1)];
        std::lock_guard lock(shard.mutex);
        auto *s = shard.find(key, h);
        if (!s) {
            if (miss_epoch)
                *miss_epoch = shard.bucket(h).epoch;
            return {};
        }
        if (shard.head != s) {
            shard.unlink_lru(*s);
            shard.push_front(*s);
        }
        s->state.fetch_add(1, std::memory_order_relaxed);
        return Handle(*s);
    }

    std::uint64_t epoch(std::string_view key) const {
        const auto h = TransparentStringHash{}(key);
        auto &s = *shards_[h & (shard_count - 1)];
        std::lock_guard lock(s.mutex);
        return s.bucket(h).epoch;
    }

    // key 与不可变正文在锁外准备。容量不足先从 LRU 尾估算可回收量；全被占用时
    // 直接放弃。第二遍只摘真正需要的 victim，不会为了插入失败而逐出无关条目。
    Handle put(std::string_view key, std::shared_ptr<const T> value, std::size_t bytes,
               std::optional<std::uint64_t> expected) {
        bytes = std::max<std::size_t>(1, bytes); // 非空槽位至少占一字节预算。
        if (!value || key.empty() || bytes > limit_) {
            if (!expected)
                invalidate(key);
            return {};
        }
        std::string name;
        try {
            name.assign(key);
        } catch (const std::bad_alloc &) {
            if (!expected)
                invalidate(key);
            return {};
        }
        const auto h = TransparentStringHash{}(key);
        auto &shard = *shards_[h & (shard_count - 1)];
        Slot *evicted = nullptr, *old = nullptr;
        Slot **evicted_tail = &evicted;
        std::shared_ptr<const void> incoming = std::move(value);
        Handle result;
        bool notify_reused = false;
        {
            std::lock_guard lock(shard.mutex);
            if (expected && *expected != shard.bucket(h).epoch)
                return {};
            if (!expected)
                ++shard.bucket(h).epoch;
            if (auto *s = shard.find(key, h); s && shard.retire(*s))
                old = s;
            // old 无读者时可以原槽换入。其旧正文由 incoming 带出锁外析构。
            std::size_t remaining = shard.bytes - (old ? old->bytes : 0);
            bool has_slot = old || shard.free;
            auto *victim = shard.tail;
            while ((!has_slot || remaining > limit_ - bytes) && victim) {
                if (victim->state.load(std::memory_order_relaxed) == 0) {
                    remaining -= victim->bytes;
                    has_slot = true;
                }
                victim = victim->newer;
            }
            if (has_slot && remaining <= limit_ - bytes) {
                auto *dest = old;
                if (!dest && shard.free) {
                    dest = shard.free;
                    shard.free = dest->chain;
                }
                // 持锁时零读者槽位无法再取得 Pin。即使另一个 Pin 此时释放，
                // 第二遍始终按当下的 LRU 顺序挑选，并在已够用时停下。
                remaining = shard.bytes - (old ? old->bytes : 0);
                for (auto *s = shard.tail; !dest || remaining > limit_ - bytes;) {
                    auto *next = s->newer;
                    if (s->state.load(std::memory_order_relaxed) == 0) {
                        shard.retire(*s);
                        remaining -= s->bytes;
                        if (!dest) {
                            dest = s;
                            notify_reused = true;
                        } else {
                            // 这些无读者的槽位已摘下，锁外析构后才会重新进入空闲链。
                            shard.bytes -= s->bytes;
                            s->bytes = 0;
                            // 沿 LRU 尾到头追加，让锁外的回调也保持淘汰顺序。
                            *evicted_tail = s;
                            evicted_tail = &s->chain;
                        }
                    }
                    s = next;
                }
                *evicted_tail = nullptr;
                if (old || notify_reused)
                    shard.bytes -= dest->bytes;
                old = nullptr;
                dest->value.swap(incoming);
                dest->key.swap(name);
                dest->hash = h;
                dest->bytes = bytes;
                shard.bytes += bytes;
                dest->chain = shard.bucket(h).first;
                shard.bucket(h).first = dest;
                shard.push_front(*dest);
                dest->state.store(1, std::memory_order_relaxed);
                result = Handle(*dest);
            } else if (old)
                old->chain = nullptr;
        }
        if (notify_reused && on_evict_) {
            try {
                on_evict_(name);
            } catch (...) {
            }
        }
        Shard::release_list(evicted, &on_evict_);
        Shard::release_list(old);
        return result;
    }

    // 监听淘汰方在 watcher mutex 内调用。缓存仍在则保留 watch；否则推进 epoch，
    // 使已经取得该监听、尚未提交的旧读盘也无法在 rm_watch 之后发布。
    bool invalidate_if_absent(std::string_view key) {
        const auto h = TransparentStringHash{}(key);
        auto &s = *shards_[h & (shard_count - 1)];
        std::lock_guard lock(s.mutex);
        if (s.find(key, h))
            return false;
        ++s.bucket(h).epoch;
        return true;
    }

    std::uint64_t invalidate(std::string_view key) noexcept {
        if (key.empty())
            return 0;
        const auto h = TransparentStringHash{}(key);
        auto &s = *shards_[h & (shard_count - 1)];
        Slot *dead = nullptr;
        std::uint64_t epoch;
        {
            std::lock_guard lock(s.mutex);
            epoch = ++s.bucket(h).epoch;
            if (auto *slot = s.find(key, h); slot && s.retire(*slot)) {
                slot->chain = nullptr;
                dead = slot;
            }
        }
        Shard::release_list(dead);
        return epoch;
    }

    void invalidate_matching(auto matches) noexcept {
        for (auto &shard : shards_) {
            Slot *dead = nullptr;
            {
                std::lock_guard lock(shard->mutex);
                for (auto &bucket : shard->buckets)
                    ++bucket.epoch;
                for (auto &s : shard->slots) {
                    if (!(s.state.load(std::memory_order_relaxed) & Slot::retired) &&
                        matches(std::string_view(s.key)) && shard->retire(s)) {
                        s.chain = dead;
                        dead = &s;
                    }
                }
            }
            Shard::release_list(dead);
        }
    }
    void clear() noexcept {
        invalidate_matching([](auto) {
            return true;
        });
    }
    std::size_t size() const noexcept {
        std::size_t total = 0;
        for (auto &s : shards_) {
            std::lock_guard lock(s->mutex);
            for (auto *slot = s->head; slot; slot = slot->older)
                ++total;
        }
        return total;
    }

  private:
    std::size_t limit_;
    std::array<std::shared_ptr<Shard>, shard_count> shards_;
    EvictCallback on_evict_;
};

} // namespace service::detail

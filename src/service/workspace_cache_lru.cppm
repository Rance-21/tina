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
    explicit CacheHandle(PayloadPin pin) noexcept : PayloadPin(std::move(pin)) {}
    CacheHandle borrow() const noexcept { return CacheHandle(PayloadPin::borrow()); }
    const T *get() const noexcept { return static_cast<const T *>(PayloadPin::get()); }
    const T *operator->() const noexcept { return get(); }
    const T &operator*() const noexcept { return *get(); }
};

// 每个 shard 启动时一次性分配槽位和哈希桶。读路径无锁遍历原子桶链，先对槽位加 Pin
// 再核对哈希与键名；槽位双缓存行物理分离可变 Pin/CLOCK 状态与只读查找元数据。
template <class T> class ShardedLruCache {
    static constexpr std::size_t max_shards = 32;
    struct Shard;

    struct alignas(64) Slot : PayloadPin::Slot {
        Slot *free_next{};

        alignas(64) std::atomic<std::size_t> hash{};
        std::atomic<Slot *> chain{};
        Shard *shard{};
        std::size_t bytes{};
        std::string key{};
    };
    static_assert(sizeof(Slot) == 2 * 64, "Cache Slot must span exactly two cache lines");

    struct alignas(64) Shard {
        // 同一个桶内的键共用失效代数。单键驱逐只会撤销本桶的在途填充，
        // 不会令整个分片其余路径刚完成的读盘作废。批量失效也覆盖尚未入表的键。
        struct Bucket {
            std::atomic<Slot *> first{};
            std::atomic<std::uint64_t> epoch{};
        };
        static constexpr std::size_t ghost_size = 64;
        std::vector<Slot> slots;
        std::vector<Bucket> buckets;

        alignas(64) std::array<std::atomic<std::uint32_t>, ghost_size> ghost{};
        alignas(64) std::mutex mutex;
        std::atomic<std::size_t> bytes{};
        std::size_t clock_hand{};
        std::atomic<Slot *> free{};
        // 通常由缓存独占；仅当缓存先于外借 Handle 销毁时，由最后一个槽位释放本分片。
        std::shared_ptr<Shard> orphan;

        explicit Shard(std::size_t n) : slots(n), buckets(std::bit_ceil(n * 2)) {
            Slot *head = nullptr;
            for (auto &s : slots) {
                s.shard = this;
                s.reclaim = release_retired;
                s.free_next = head;
                head = &s;
            }
            free.store(head, std::memory_order_relaxed);
        }

        // 分片未满时直接放行首次回填；已满进入淘汰态时仅放行近期二次命中的路径，过滤单次冷扫抖动。
        bool should_admit(std::size_t h, std::size_t limit) noexcept {
            if (free.load(std::memory_order_relaxed) != nullptr &&
                bytes.load(std::memory_order_relaxed) < limit - limit / 8) {
                return true;
            }
            const auto tag = static_cast<std::uint32_t>(h >> 32) | 1u;
            auto &g = ghost[(h >> 12) & (ghost_size - 1)];
            if (g.load(std::memory_order_relaxed) == tag) {
                g.store(0, std::memory_order_relaxed);
                return true;
            }
            g.store(tag, std::memory_order_relaxed);
            return false;
        }

        Bucket &bucket(std::size_t hash) noexcept {
            return buckets[(hash >> 6) & (buckets.size() - 1)];
        }

        Slot *find_locked(std::string_view key, std::size_t hash) noexcept {
            for (auto *s = bucket(hash).first.load(std::memory_order_relaxed); s;
                 s = s->chain.load(std::memory_order_relaxed))
                if (s->hash.load(std::memory_order_relaxed) == hash && s->key == key)
                    return s;
            return nullptr;
        }

        static CacheHandle<T> try_pin(Slot &s, std::string_view key, std::size_t h,
                                      std::string_view *pinned_key, bool *was_hot) noexcept {
            if (s.hash.load(std::memory_order_relaxed) != h)
                return {};
            auto st = s.state.load(std::memory_order_relaxed);
            while (!(st & Slot::retired)) {
                if (s.state.compare_exchange_weak(st, (st | Slot::used) + 1,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
                    CacheHandle<T> handle(s);
                    if (s.hash.load(std::memory_order_relaxed) != h || s.key != key)
                        return {};
                    if (pinned_key)
                        *pinned_key = s.key;
                    if (was_hot)
                        *was_hot = (st & Slot::used) != 0;
                    return handle;
                }
            }
            return {};
        }

        bool retire(Slot &s) noexcept {
            auto &b = bucket(s.hash.load(std::memory_order_relaxed));
            auto *link = &b.first;
            while (link->load(std::memory_order_relaxed) != &s)
                link = &link->load(std::memory_order_relaxed)->chain;
            link->store(s.chain.load(std::memory_order_relaxed), std::memory_order_relaxed);
            b.epoch.fetch_add(1, std::memory_order_release);
            if ((s.state.fetch_or(Slot::retired, std::memory_order_acq_rel) & Slot::count_mask) !=
                0)
                return false;
            bytes.store(bytes.load(std::memory_order_relaxed) - std::exchange(s.bytes, 0),
                        std::memory_order_relaxed);
            return true;
        }

        static void invoke_evict(const std::function<void(std::string_view)> *cb,
                                 std::string_view key) noexcept {
            if (cb && *cb) {
                try {
                    (*cb)(key);
                } catch (...) {
                }
            }
        }

        static void
        release_list(Slot *list,
                     const std::function<void(std::string_view)> *notify = nullptr) noexcept {
            if (!list)
                return;
            Shard &shard = *list->shard;
            Slot *last = nullptr;
            std::size_t released = 0;
            for (auto *s = list; s; s = s->free_next) {
                invoke_evict(notify, s->key);
                s->value.reset();
                if (s->key.capacity() > 256)
                    std::string{}.swap(s->key);
                else
                    s->key.clear();
                released += std::exchange(s->bytes, 0);
                last = s;
            }
            std::shared_ptr<Shard> keep;
            std::lock_guard lock(shard.mutex);
            const auto rem = shard.bytes.load(std::memory_order_relaxed) - released;
            shard.bytes.store(rem, std::memory_order_relaxed);
            last->free_next = shard.free.load(std::memory_order_relaxed);
            shard.free.store(list, std::memory_order_relaxed);
            if (rem == 0)
                keep = std::move(shard.orphan);
        }

        static void release_retired(PayloadPin::Slot &base) noexcept {
            auto &s = static_cast<Slot &>(base);
            s.free_next = nullptr;
            release_list(&s);
        }
    };

  public:
    using Handle = CacheHandle<T>;
    using EvictCallback = std::function<void(std::string_view)>;

    ShardedLruCache(std::size_t entries, std::size_t bytes) {
        const auto shard_cnt = std::bit_floor(std::clamp<std::size_t>(entries / 4, 1, max_shards));
        shard_mask_ = shard_cnt - 1;
        limit_ = std::max<std::size_t>(1, (bytes + shard_cnt - 1) / shard_cnt);
        const auto n = std::max<std::size_t>(1, (entries + shard_cnt - 1) / shard_cnt);
        shards_.reserve(shard_cnt);
        for (std::size_t i = 0; i < shard_cnt; ++i)
            shards_.push_back(std::make_shared<Shard>(n));
    }
    ~ShardedLruCache() {
        clear();
        for (auto &s : shards_) {
            std::lock_guard lock(s->mutex);
            if (s->bytes.load(std::memory_order_relaxed))
                s->orphan = s;
        }
    }
    ShardedLruCache(const ShardedLruCache &) = delete;
    ShardedLruCache &operator=(const ShardedLruCache &) = delete;

    // 在启动屏障内安装。回调执行时没有持有 shard 锁，可重新查询缓存。
    void set_evict_callback(EvictCallback cb) { on_evict_ = std::move(cb); }

    Handle get_hashed(std::string_view key, std::size_t h, std::uint64_t *miss_epoch = nullptr,
                      std::string_view *pinned_key = nullptr, bool *was_hot = nullptr,
                      bool *admit = nullptr) {
        auto &shard = *shards_[h & shard_mask_];
        auto &b = shard.bucket(h);
        const auto ep = b.epoch.load(std::memory_order_acquire);
        for (auto *s = b.first.load(std::memory_order_acquire); s;
             s = s->chain.load(std::memory_order_acquire)) {
            if (auto pin = Shard::try_pin(*s, key, h, pinned_key, was_hot))
                return pin;
            if (b.epoch.load(std::memory_order_acquire) != ep)
                break;
        }
        if (b.epoch.load(std::memory_order_acquire) != ep) {
            std::lock_guard lock(shard.mutex);
            if (auto *s = shard.find_locked(key, h))
                return Shard::try_pin(*s, key, h, pinned_key, was_hot);
            if (miss_epoch)
                *miss_epoch = b.epoch.load(std::memory_order_relaxed);
            if (admit)
                *admit = shard.should_admit(h, limit_);
            return {};
        }
        if (miss_epoch)
            *miss_epoch = ep;
        if (admit)
            *admit = shard.should_admit(h, limit_);
        return {};
    }

    Handle get(std::string_view key, std::uint64_t *miss_epoch = nullptr,
               bool *admit = nullptr) {
        return get_hashed(key, TransparentStringHash{}(key), miss_epoch, nullptr, nullptr, admit);
    }

    std::uint64_t epoch(std::string_view key) const noexcept {
        const auto h = TransparentStringHash{}(key);
        return shards_[h & shard_mask_]->bucket(h).epoch.load(std::memory_order_acquire);
    }

    // CLOCK 二次机会淘汰：键名在锁外准备，锁内全程 noexcept 复用空闲或无读者槽位。
    Handle put(std::string_view key, std::shared_ptr<const T> value, std::size_t bytes,
               std::uint64_t expected) {
        bytes = std::max<std::size_t>(1, bytes);
        if (!value || key.empty() || bytes > limit_)
            return {};
        std::string name;
        try {
            name.assign(key);
        } catch (const std::bad_alloc &) {
            return {};
        }
        const auto h = TransparentStringHash{}(key);
        auto &shard = *shards_[h & shard_mask_];
        Slot *evicted = nullptr, *old = nullptr, **evicted_tail = &evicted;
        std::shared_ptr<const void> incoming = std::move(value);
        bool notify_reused = false;
        Handle result;
        {
            std::lock_guard lock(shard.mutex);
            auto &b = shard.bucket(h);
            if (expected != b.epoch.load(std::memory_order_relaxed))
                return {};
            if (auto *s = shard.find_locked(key, h); s && shard.retire(*s))
                (old = s)->free_next = nullptr;

            std::size_t avail = limit_ - bytes;
            Slot *free_head = shard.free.load(std::memory_order_relaxed);
            bool can_fit = (old || free_head) &&
                           shard.bytes.load(std::memory_order_relaxed) <= avail;
            for (auto &s : shard.slots) {
                if (can_fit)
                    break;
                if (!(s.state.load(std::memory_order_relaxed) & ~Slot::used))
                    can_fit = (avail += s.bytes) >= shard.bytes.load(std::memory_order_relaxed);
            }

            if (can_fit) {
                const std::size_t n = shard.slots.size();
                for (std::size_t step = 0;
                     ((!old && !free_head && !evicted) ||
                      shard.bytes.load(std::memory_order_relaxed) > limit_ - bytes) &&
                     step < n * 2;
                     ++step) {
                    auto &s = shard.slots[shard.clock_hand];
                    shard.clock_hand = (shard.clock_hand + 1 == n) ? 0 : shard.clock_hand + 1;
                    auto st = s.state.load(std::memory_order_relaxed);
                    if (st & ~Slot::used)
                        continue;
                    if (step < n && (st & Slot::used)) {
                        (void)s.state.compare_exchange_strong(st, 0, std::memory_order_relaxed,
                                                              std::memory_order_relaxed);
                        continue;
                    }
                    if (shard.retire(s)) {
                        *evicted_tail = &s;
                        evicted_tail = &s.free_next;
                    }
                }
                *evicted_tail = nullptr;

                const auto cur_bytes = shard.bytes.load(std::memory_order_relaxed);
                if (cur_bytes <= limit_ - bytes && (old || free_head || evicted)) {
                    Slot *dest = std::exchange(old, nullptr);
                    if (!dest && free_head) {
                        dest = free_head;
                        shard.free.store(free_head->free_next, std::memory_order_relaxed);
                    } else if (!dest) {
                        dest = std::exchange(evicted, evicted->free_next);
                        notify_reused = true;
                    }
                    dest->value.swap(incoming);
                    dest->key.swap(name);
                    dest->bytes = bytes;
                    shard.bytes.store(cur_bytes + bytes, std::memory_order_relaxed);
                    dest->hash.store(h, std::memory_order_relaxed);
                    dest->chain.store(b.first.load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
                    dest->state.store(1, std::memory_order_release);
                    b.first.store(dest, std::memory_order_release);
                    result = Handle(*dest);
                }
            }
        }
        if (notify_reused)
            Shard::invoke_evict(&on_evict_, name);
        Shard::release_list(evicted, &on_evict_);
        Shard::release_list(old);
        return result;
    }

    // 监听淘汰方在 watcher mutex 内调用。缓存仍在则保留 watch；否则推进 epoch，
    // 使已经取得该监听、尚未提交的旧读盘也无法在 rm_watch 之后发布。
    bool invalidate_if_absent(std::string_view key) {
        const auto h = TransparentStringHash{}(key);
        auto &s = *shards_[h & shard_mask_];
        std::lock_guard lock(s.mutex);
        if (s.find_locked(key, h))
            return false;
        s.bucket(h).epoch.fetch_add(1, std::memory_order_release);
        return true;
    }

    std::uint64_t invalidate(std::string_view key) noexcept {
        if (key.empty())
            return 0;
        const auto h = TransparentStringHash{}(key);
        auto &s = *shards_[h & shard_mask_];
        Slot *dead = nullptr;
        std::uint64_t epoch;
        {
            std::lock_guard lock(s.mutex);
            auto &b = s.bucket(h);
            if (auto *slot = s.find_locked(key, h)) {
                if (s.retire(*slot))
                    (dead = slot)->free_next = nullptr;
            } else {
                b.epoch.fetch_add(1, std::memory_order_release);
            }
            epoch = b.epoch.load(std::memory_order_relaxed);
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
                    bucket.epoch.fetch_add(1, std::memory_order_release);
                for (auto &s : shard->slots) {
                    if (!(s.state.load(std::memory_order_relaxed) & Slot::retired) &&
                        matches(std::string_view(s.key)) && shard->retire(s)) {
                        s.free_next = dead;
                        dead = &s;
                    }
                }
            }
            Shard::release_list(dead);
        }
    }
    void clear() noexcept {
        invalidate_matching([](auto) { return true; });
        for (auto &shard : shards_)
            for (auto &g : shard->ghost)
                g.store(0, std::memory_order_relaxed);
    }
    std::size_t size() const noexcept {
        std::size_t total = 0;
        for (const auto &s : shards_) {
            std::lock_guard lock(s->mutex);
            for (const auto &slot : s->slots)
                if (!(slot.state.load(std::memory_order_relaxed) & Slot::retired))
                    ++total;
        }
        return total;
    }

  private:
    std::size_t limit_{}, shard_mask_{};
    std::vector<std::shared_ptr<Shard>> shards_;
    EvictCallback on_evict_;
};

} // namespace service::detail

export module broker.service;

import std;
import rpc.protocol;
import broker.protocol;

export namespace broker {

class BrokerService {
  public:
    // 这些只是内存版本的安全上限。
    // max_bytes_per_partition 只统计消息内容，不含容器开销。
    static constexpr std::size_t max_topics = 128;
    static constexpr std::size_t max_messages_per_partition = 65536;
    static constexpr std::size_t max_bytes_per_partition = 64 * 1024 * 1024;
    static constexpr std::size_t max_consumers_per_partition = 1024;

    BrokerService() = default;

    BrokerService(const BrokerService &) = delete;
    BrokerService &operator=(const BrokerService &) = delete;

    // PUBLISH：向 Topic 的 Partition 末尾追加消息。
    [[nodiscard]]
    protocol::PublishResult publish(const protocol::Publish &request) {
        validate_address(request.topic, request.partition);

        if (request.message.size() > protocol::max_message_size)
            throw std::invalid_argument("message exceeds protocol limit");

        auto partition = get_or_create_partition(request.topic);

        // 仅锁住目标 Partition，其他 Topic 可以并发操作。
        std::lock_guard lock(partition->mutex);

        if (partition->messages.size() >= max_messages_per_partition ||
            request.message.size() > max_bytes_per_partition - partition->stored_bytes ||
            partition->next_offset == std::numeric_limits<std::uint64_t>::max())
            throw std::length_error("partition log is full");

        const auto offset = partition->next_offset;

        // push_back 可能因为内存分配失败抛异常。只有追加成功，才推进 Offset 和字节计数。
        partition->messages.push_back(request.message);

        partition->stored_bytes += request.message.size();
        ++partition->next_offset;

        return {offset};
    }

    // FETCH：从指定 Offset 开始读取，返回一批消息。
    [[nodiscard]]
    protocol::FetchResult fetch(const protocol::Fetch &request) const {
        validate_address(request.topic, request.partition);

        if (request.max_bytes < 12 || request.max_bytes > rpc::max_payload_size)
            throw std::invalid_argument("invalid fetch byte budget");

        auto partition = find_partition(request.topic);

        std::lock_guard lock(partition->mutex);

        if (request.offset > partition->next_offset)
            throw std::invalid_argument("fetch offset exceeds log end");

        protocol::FetchResult result{.next_offset = request.offset, .records = {}};

        // 响应固定头：next_offset:u64 + record_count:u32
        std::size_t used = 12;

        for (auto offset = request.offset; offset < partition->next_offset; ++offset) {
            // 第一版不会删除消息，因此 Offset 就是 vector 下标。
            const auto &message = partition->messages[static_cast<std::size_t>(offset)];

            // 每条记录：offset:u64 + message_len:u32 + message:bytes
            const std::size_t bytes = 12 + message.size();

            if (bytes > request.max_bytes - used) {
                // 第一条就放不下时直接报错，
                // 否则 Consumer 可能永远卡在这个 Offset。
                if (result.records.empty())
                    throw std::length_error("fetch budget cannot fit the next record");

                break;
            }

            result.records.push_back({offset, message});
            used += bytes;
            result.next_offset = offset + 1;
        }

        // 返回值已经复制了消息。
        // 离开函数时释放 Partition 锁。
        return result;
    }

    // COMMIT_OFFSET：记录 Consumer 下一次读取的位置。
    void commit_offset(const protocol::CommitOffset &request) {
        validate_address(request.topic, request.partition);

        if (!valid_name(request.consumer))
            throw std::invalid_argument("invalid consumer name");

        auto partition = find_partition(request.topic);

        std::lock_guard lock(partition->mutex);

        // 不能提交尚不存在的消息之后的位置。
        if (request.next_offset > partition->next_offset)
            throw std::invalid_argument("cannot commit beyond log end");

        auto it = partition->committed_offsets.find(request.consumer);

        if (it != partition->committed_offsets.end()) {
            // 暂不支持主动重置消费进度。
            if (request.next_offset < it->second)
                throw std::invalid_argument("committed offset cannot move backward");

            it->second = request.next_offset;
        } else {
            if (partition->committed_offsets.size() >= max_consumers_per_partition)
                throw std::length_error("too many consumers");

            partition->committed_offsets.emplace(request.consumer, request.next_offset);
        }
    }

    // 查询某个 Consumer 已提交的进度。
    // 尚未提交过则返回 nullopt。
    // 本阶段作为内部接口和测试接口，暂不新增 RPC Method。
    [[nodiscard]]
    std::optional<std::uint64_t> committed_offset(const std::string &consumer,
                                                  const std::string &topic,
                                                  std::uint32_t partition_id = 0) const {
        validate_address(topic, partition_id);

        if (!valid_name(consumer))
            throw std::invalid_argument("invalid consumer name");

        auto partition = find_partition(topic);

        std::lock_guard lock(partition->mutex);

        auto it = partition->committed_offsets.find(consumer);

        if (it == partition->committed_offsets.end())
            return std::nullopt;

        return it->second;
    }

  private:
    struct Partition {
        // 保护消息、Offset 和 Consumer 进度。
        std::mutex mutex;

        // 当前内存版的 Append-only Log。
        std::vector<std::string> messages;

        // Consumer 名称 -> 下次读取的位置。
        std::unordered_map<std::string, std::uint64_t> committed_offsets;

        std::uint64_t next_offset{0};
        std::size_t stored_bytes{0};
    };

    static bool valid_name(std::string_view name) {
        if (name.empty() || name.size() > 128 || name == "." || name == "..")
            return false;

        for (unsigned char c : name) {
            const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');

            const bool digit = c >= '0' && c <= '9';

            if (!alpha && !digit && c != '_' && c != '-' && c != '.')
                return false;
        }

        return true;
    }

    static void validate_address(std::string_view topic, std::uint32_t partition) {
        if (!valid_name(topic) || partition != 0)
            throw std::invalid_argument("invalid topic or partition (only partition 0 supported)");
    }

    // PUBLISH 使用：Topic 不存在时自动创建。
    std::shared_ptr<Partition> get_or_create_partition(const std::string &topic) {
        std::lock_guard lock(topics_mutex_);

        if (auto it = topics_.find(topic); it != topics_.end())
            return it->second;

        if (topics_.size() >= max_topics)
            throw std::length_error("too many broker topics");

        auto partition = std::make_shared<Partition>();
        topics_.emplace(topic, partition);

        return partition;
    }

    // FETCH / COMMIT 使用：不存在的 Topic 不能自动创建。
    std::shared_ptr<Partition> find_partition(const std::string &topic) const {
        std::lock_guard lock(topics_mutex_);

        auto it = topics_.find(topic);

        if (it == topics_.end())
            throw std::out_of_range("topic not found");

        return it->second;
    }

    // 只保护 Topic 注册表，不保护每条消息。
    mutable std::mutex topics_mutex_;

    // 初版每个 Topic 只有 Partition 0。
    std::unordered_map<std::string, std::shared_ptr<Partition>> topics_;
};

} // namespace broker

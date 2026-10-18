
export module broker.service;

import std;
import rpc.protocol;
import broker.protocol;

export namespace broker {

class BrokerService {
  public:
    // 限制都是单个 Topic / Partition 的局部上限
    static constexpr std::size_t max_topics = 128;
    static constexpr std::size_t max_partitions_per_topic = 64;
    static constexpr std::size_t max_messages_per_partition = 65536;
    static constexpr std::size_t max_bytes_per_partition = 64 * 1024 * 1024;
    static constexpr std::size_t max_consumers_per_partition = 1024;

    BrokerService() = default;
    BrokerService(const BrokerService &) = delete;
    BrokerService &operator=(const BrokerService &) = delete;

    // 显式创建 Topic，Partition 编号 [0, partition_count)。
    // 当前不支持删除 Topic，也不支持调整 Partition 数量。
    void create_topic(std::string name, std::size_t partition_count) {
        if (!valid_name(name) || partition_count == 0 || partition_count > max_partitions_per_topic)
            throw std::invalid_argument("invalid topic name or partition count");

        Topic topic;
        topic.partitions.reserve(partition_count);

        for (std::size_t i = 0; i < partition_count; ++i)
            topic.partitions.push_back(std::make_shared<Partition>());

        std::lock_guard lock(topics_mutex_);

        if (topics_.contains(name))
            throw std::invalid_argument("topic already exists");

        if (topics_.size() >= max_topics)
            throw std::length_error("too many broker topics");

        topics_.emplace(std::move(name), std::move(topic));
    }

    [[nodiscard]]
    // 锁住 topics_ 并获取 partition 数量
    std::size_t partition_count(const std::string &topic) const {
        if (!valid_name(topic))
            throw std::invalid_argument("invalid topic name");

        std::lock_guard lock(topics_mutex_);

        auto it = topics_.find(topic);
        if (it == topics_.end())
            throw std::out_of_range("topic not found");

        return it->second.partitions.size();
    }

    // PUBLISH：追加消息，只锁目标 Partition。
    [[nodiscard]]
    protocol::PublishResult publish(const protocol::Publish &request) {
        if (request.message.size() > protocol::max_message_size)
            throw std::invalid_argument("message exceeds protocol limit");

        auto partition = find_partition(request.topic, request.partition);
        std::lock_guard lock(partition->mutex);

        if (partition->messages.size() >= max_messages_per_partition ||
            request.message.size() > max_bytes_per_partition - partition->stored_bytes ||
            partition->next_offset == std::numeric_limits<std::uint64_t>::max())
            throw std::length_error("partition log is full");

        const auto offset = partition->next_offset;

        // 若 push_back 抛异常，Offset 和容量计数不会改变。
        partition->messages.push_back(request.message);
        partition->stored_bytes += request.message.size();
        ++partition->next_offset;

        return {offset};
    }

    // FETCH：按指定 Partition 的 Offset 读取
    [[nodiscard]]
    protocol::FetchResult fetch(const protocol::Fetch &request) const {
        // 12 是空的 fetch res 返回的字节数
        if (request.max_bytes < 12 || request.max_bytes > rpc::max_payload_size)
            throw std::invalid_argument("invalid fetch byte budget");

        auto partition = find_partition(request.topic, request.partition);
        // 目前线程安全，如果以后删除 Topic 可能出问题
        std::lock_guard lock(partition->mutex);

        if (request.offset > partition->next_offset)
            throw std::invalid_argument("fetch offset exceeds log end");

        protocol::FetchResult result{.next_offset = request.offset, .records = {}};

        // next_offset:u64 + record_count:u32
        std::size_t used = 12;

        for (auto offset = request.offset; offset < partition->next_offset; ++offset) {

            // 尚未引入 Retention，Offset 恰好等于 vector 下标。
            const auto &message = partition->messages[static_cast<std::size_t>(offset)];

            // offset:u64 + message_len:u32 + data
            const std::size_t bytes = 12 + message.size();

            if (bytes > request.max_bytes - used) {
                if (result.records.empty())
                    throw std::length_error("fetch budget cannot fit the next record");

                break;
            }

            result.records.push_back({offset, message});
            used += bytes;
            result.next_offset = offset + 1;
        }

        return result;
    }

    // COMMIT_OFFSET：消费进度按 (Consumer, Topic, Partition) 独立保存。
    void commit_offset(const protocol::CommitOffset &request) {
        if (!valid_name(request.consumer))
            throw std::invalid_argument("invalid consumer name");

        auto partition = find_partition(request.topic, request.partition);

        std::lock_guard lock(partition->mutex);

        if (request.next_offset > partition->next_offset)
            throw std::invalid_argument("cannot commit beyond log end");

        auto it = partition->committed_offsets.find(request.consumer);

        if (it != partition->committed_offsets.end()) {
            if (request.next_offset < it->second)
                throw std::invalid_argument("committed offset cannot move backward");

            it->second = request.next_offset;
        } else {
            if (partition->committed_offsets.size() >= max_consumers_per_partition)
                throw std::length_error("too many consumers");

            partition->committed_offsets.emplace(request.consumer, request.next_offset);
        }
    }

    [[nodiscard]]
    std::optional<std::uint64_t> committed_offset(const std::string &consumer,
                                                  const std::string &topic,
                                                  std::uint32_t partition_id) const {
        if (!valid_name(consumer))
            throw std::invalid_argument("invalid consumer name");

        auto partition = find_partition(topic, partition_id);
        std::lock_guard lock(partition->mutex);

        auto it = partition->committed_offsets.find(consumer);

        if (it == partition->committed_offsets.end())
            return std::nullopt;

        return it->second;
    }

  private:
    struct Partition {
        std::mutex mutex;

        std::vector<std::string> messages;
        // 不同 consumer自己的 offset
        std::unordered_map<std::string, std::uint64_t> committed_offsets;

        std::uint64_t next_offset{0};
        std::size_t stored_bytes{0};
    };

    struct Topic {
        std::vector<std::shared_ptr<Partition>> partitions;
    };

    // static，不必传入 this
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

    [[nodiscard]]
    std::shared_ptr<Partition> find_partition(const std::string &name,
                                              std::uint32_t partition_id) const {
        if (!valid_name(name))
            throw std::invalid_argument("invalid topic name");

        // 锁只保护查找，不覆盖消息的读写。
        std::lock_guard lock(topics_mutex_);

        auto it = topics_.find(name);
        if (it == topics_.end())
            throw std::out_of_range("topic not found");

        if (partition_id >= it->second.partitions.size())
            throw std::out_of_range("partition not found");

        return it->second.partitions[partition_id];
    }

    mutable std::mutex topics_mutex_;

    std::unordered_map<std::string, Topic> topics_;
};

} // namespace broker

export module broker.service;

import std;
import rpc.protocol;
import broker.protocol;
import broker.log;

export namespace broker {

class BrokerService {
  public:
    static constexpr std::size_t max_topics = 128;
    static constexpr std::size_t max_partitions_per_topic = 64;
    static constexpr std::size_t max_consumers_per_partition = 1024;

    // The default storage location is relative to the server working directory.
    explicit BrokerService(std::filesystem::path data_dir = "data")
        : data_dir_(std::move(data_dir)) {
        if (data_dir_.empty())
            throw std::invalid_argument("empty broker data directory");
    }

    BrokerService(const BrokerService &) = delete;
    BrokerService &operator=(const BrokerService &) = delete;

    // Register all partitions and recover their on-disk logs before serving.
    // Topic metadata is currently static: no deletion or resizing.
    void create_topic(std::string name, std::size_t partition_count) {
        if (!valid_name(name) || partition_count == 0 ||
            partition_count > max_partitions_per_topic)
            throw std::invalid_argument("invalid topic name or partition count");

        // Registration is serialized, including disk recovery.
        // Topic creation currently occurs only at server startup.
        std::lock_guard lock(topics_mutex_);

        if (topics_.contains(name))
            throw std::invalid_argument("topic already exists");
        if (topics_.size() >= max_topics)
            throw std::length_error("too many broker topics");

        Topic topic;
        topic.partitions.reserve(partition_count);

        for (std::size_t id = 0; id < partition_count; ++id) {
            auto path = data_dir_ / name /
                        ("partition-" + std::to_string(id)) / "00000000.log";
            topic.partitions.push_back(std::make_shared<Partition>(path));
        }

        topics_.emplace(std::move(name), std::move(topic));
    }

    [[nodiscard]] std::size_t partition_count(const std::string &topic) const {
        if (!valid_name(topic))
            throw std::invalid_argument("invalid topic name");

        std::lock_guard lock(topics_mutex_);
        auto it = topics_.find(topic);
        if (it == topics_.end())
            throw std::out_of_range("topic not found");
        return it->second.partitions.size();
    }

    // PUBLISH: ACK only after MessageLog has finished fdatasync.
    [[nodiscard]] protocol::PublishResult publish(const protocol::Publish &request) {
        if (request.message.size() > protocol::max_message_size)
            throw std::invalid_argument("message exceeds protocol limit");

        auto partition = find_partition(request.topic, request.partition);
        std::lock_guard lock(partition->mutex);
        return {partition->log.append(request.message)};
    }

    // FETCH: use the in-memory Offset index, then pread from the log file.
    [[nodiscard]] protocol::FetchResult fetch(const protocol::Fetch &request) const {
        if (request.max_bytes < 12 || request.max_bytes > rpc::max_payload_size)
            throw std::invalid_argument("invalid fetch byte budget");

        auto partition = find_partition(request.topic, request.partition);
        std::lock_guard lock(partition->mutex);

        if (request.offset > partition->log.next_offset())
            throw std::invalid_argument("fetch offset exceeds log end");

        protocol::FetchResult result{.next_offset = request.offset, .records = {}};
        std::size_t used = 12; // next_offset:u64 + record_count:u32

        for (auto offset = request.offset;
             offset < partition->log.next_offset(); ++offset) {
            auto payload_length = partition->log.payload_size(offset);
            if (!payload_length)
                throw std::runtime_error("message log index inconsistent");

            // Record in FETCH response: offset:u64 + length:u32 + payload.
            const std::size_t bytes = 12 + *payload_length;
            if (bytes > request.max_bytes - used) {
                if (result.records.empty())
                    throw std::length_error("fetch budget cannot fit the next record");
                break;
            }

            auto message = partition->log.read(offset);
            if (!message)
                throw std::runtime_error("message disappeared from log");

            result.records.push_back({offset, std::move(*message)});
            used += bytes;
            result.next_offset = offset + 1;
        }

        return result;
    }

    // COMMIT_OFFSET remains volatile until SQLite integration in stage 5.
    // The bound is the persisted message log end, not an in-memory write cursor.
    void commit_offset(const protocol::CommitOffset &request) {
        if (!valid_name(request.consumer))
            throw std::invalid_argument("invalid consumer name");

        auto partition = find_partition(request.topic, request.partition);
        std::lock_guard lock(partition->mutex);

        if (request.next_offset > partition->log.next_offset())
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
        MessageLog log;
        std::unordered_map<std::string, std::uint64_t> committed_offsets;

        explicit Partition(const std::filesystem::path &path) : log(path) {}
    };

    struct Topic {
        std::vector<std::shared_ptr<Partition>> partitions;
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

    [[nodiscard]]
    std::shared_ptr<Partition> find_partition(const std::string &name,
                                              std::uint32_t partition_id) const {
        if (!valid_name(name))
            throw std::invalid_argument("invalid topic name");

        // Shared ownership makes the Partition live past the registry lock.
        std::lock_guard lock(topics_mutex_);
        auto it = topics_.find(name);
        if (it == topics_.end())
            throw std::out_of_range("topic not found");
        if (partition_id >= it->second.partitions.size())
            throw std::out_of_range("partition not found");
        return it->second.partitions[partition_id];
    }

    std::filesystem::path data_dir_;
    mutable std::mutex topics_mutex_;
    std::unordered_map<std::string, Topic> topics_;
};

} // namespace broker

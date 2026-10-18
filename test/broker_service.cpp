import std;
import broker.protocol;
import broker.service;

namespace bp = broker::protocol;

void require(bool condition, std::string_view name) {
    if (!condition)
        throw std::runtime_error("broker service test failed: " + std::string(name));
}

template <typename Error, typename Fn> void must_fail(Fn &&fn, std::string_view name) {
    try {
        fn();
    } catch (const Error &) {
        return;
    }

    throw std::runtime_error("expected exception: " + std::string(name));
}

int main() {
    broker::BrokerService broker;

    broker.create_topic("orders", 3);
    broker.create_topic("logs", 2);

    require(broker.partition_count("orders") == 3 && broker.partition_count("logs") == 2,
            "topic partition counts");

    // Offset 属于 Partition，而不是整个 Topic。
    require(broker.publish({"orders", 0, "A"}).offset == 0, "orders-0 first");
    require(broker.publish({"orders", 0, "B"}).offset == 1, "orders-0 second");
    require(broker.publish({"orders", 1, "C"}).offset == 0, "orders-1 first");
    require(broker.publish({"orders", 2, "D"}).offset == 0, "orders-2 first");
    require(broker.publish({"logs", 0, "E"}).offset == 0, "logs-0 first");

    auto p0 = broker.fetch({"orders", 0, 0, 1024});
    auto p1 = broker.fetch({"orders", 1, 0, 1024});
    auto empty = broker.fetch({"logs", 1, 0, 1024});

    require(p0.records.size() == 2 && p0.next_offset == 2 && p0.records[0].message == "A" &&
                p0.records[1].message == "B",
            "partition 0 isolation");

    require(p1.records.size() == 1 && p1.next_offset == 1 && p1.records[0].message == "C",
            "partition 1 isolation");

    require(empty.records.empty() && empty.next_offset == 0, "empty partition");

    // 12 字节响应头 + 12 字节记录头 + 1 字节消息。
    require(broker.fetch({"orders", 0, 0, 25}).records.size() == 1, "fetch budget");

    must_fail<std::length_error>(
        [&] {
            (void)broker.fetch({"orders", 0, 0, 24});
        },
        "first message exceeds fetch budget");

    // 同一个 Consumer 在不同 Partition 具有独立进度。
    broker.commit_offset({"worker", "orders", 0, 2});
    broker.commit_offset({"worker", "orders", 1, 1});
    broker.commit_offset({"worker", "logs", 0, 0});
    broker.commit_offset({"other", "orders", 0, 1});

    require(broker.committed_offset("worker", "orders", 0) == 2 &&
                broker.committed_offset("worker", "orders", 1) == 1 &&
                broker.committed_offset("worker", "logs", 0) == 0 &&
                broker.committed_offset("other", "orders", 0) == 1,
            "independent consumer offsets");

    must_fail<std::invalid_argument>(
        [&] {
            broker.commit_offset({"worker", "orders", 0, 1});
        },
        "reject rollback");

    must_fail<std::invalid_argument>(
        [&] {
            broker.commit_offset({"worker", "orders", 1, 2});
        },
        "reject beyond end");

    must_fail<std::out_of_range>(
        [&] {
            (void)broker.publish({"orders", 3, "no"});
        },
        "reject nonexistent partition");

    must_fail<std::out_of_range>(
        [&] {
            (void)broker.publish({"missing", 0, "no"});
        },
        "reject nonexistent topic");

    must_fail<std::invalid_argument>(
        [&] {
            broker.create_topic("orders", 2);
        },
        "reject duplicate topic");

    must_fail<std::invalid_argument>(
        [&] {
            broker.create_topic("zero", 0);
        },
        "reject zero partitions");

    // 多线程同时追加到同一个 Partition。
    broker.create_topic("parallel", 4);

    constexpr int threads = 8;
    constexpr int per_thread = 200;

    std::vector<std::uint64_t> offsets(threads * per_thread);
    std::vector<std::thread> workers;

    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&, i] {
            for (int j = 0; j < per_thread; ++j) {
                offsets[i * per_thread + j] = broker.publish({"parallel", 0, "m"}).offset;
            }
        });
    }

    for (auto &t : workers)
        t.join();

    std::sort(offsets.begin(), offsets.end());

    for (std::size_t i = 0; i < offsets.size(); ++i)
        require(offsets[i] == i, "concurrent offsets unique");

    // 多线程写入不同 Partition，Offset 各自独立。
    workers.clear();

    std::vector<std::thread> writers;

    for (std::uint32_t id = 1; id < 4; ++id) {
        writers.emplace_back([&, id] {
            for (int i = 0; i < per_thread; ++i)
                (void)broker.publish({"parallel", id, "message"});
        });
    }

    for (auto &t : writers)
        t.join();

    for (std::uint32_t id = 1; id < 4; ++id) {
        auto last = broker.fetch({"parallel", id, per_thread - 1, 1024});

        require(last.records.size() == 1 && last.records[0].offset == per_thread - 1 &&
                    last.next_offset == per_thread,
                "independent concurrent partition");
    }

    std::cout << "broker multi-partition: all tests passed\n";
}

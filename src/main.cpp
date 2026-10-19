import std;

import rpc.client;
import rpc.protocol;
import rpc.server;

import broker.client;
import broker.protocol;

namespace {

// 解析命令行无符号整数。
template <typename T> T parse_number(std::string_view text, std::string_view name) {
    T value{};

    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);

    if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size()) {
        throw std::invalid_argument("invalid " + std::string(name) + ": " + std::string(text));
    }

    return value;
}

void print_usage() {
    std::cout << "usage:\n"
              << "  ./tina server [port]\n"
              << "\n"
              << "  ./tina publish <topic> <partition> <message>\n"
              << "  ./tina fetch <topic> <partition> [offset]\n"
              << "  ./tina consume <topic> <partition> [offset]\n"
              << "  ./tina commit <consumer> <topic> <partition> <next_offset>\n"
              << "\n"
              << "  ./tina ping\n"
              << "  ./tina set <key> <value>\n"
              << "  ./tina get <key>\n"
              << "  ./tina del <key>\n"
              << "  ./tina async-test\n";
}

// 按实际长度输出消息。
// 与直接使用 C 字符串不同，消息中允许包含 NUL。
void print_records(const broker::protocol::FetchResult &result) {
    for (const auto &record : result.records) {
        std::cout << "[offset=" << record.offset << ", bytes=" << record.message.size() << "] ";

        std::cout.write(record.message.data(), static_cast<std::streamsize>(record.message.size()));

        std::cout << '\n';
    }

    std::cout.flush();

    if (!std::cout)
        throw std::runtime_error("failed to write consumed messages");
}

// 原有 KV 命令行测试。
int run_kv_command(int argc, char *argv[], const std::string &command) {
    rpc::Method method;
    std::string payload;

    if (command == "ping" && argc == 2) {
        method = rpc::Method::ping;
    } else if (command == "set" && argc == 4) {
        method = rpc::Method::set;
        payload = std::string(argv[2]) + "\n" + argv[3];
    } else if (command == "get" && argc == 3) {
        method = rpc::Method::get;
        payload = argv[2];
    } else if (command == "del" && argc == 3) {
        method = rpc::Method::del;
        payload = argv[2];
    } else {
        throw std::invalid_argument("invalid KV command arguments");
    }

    rpc::RpcClient client;
    auto response = client.async_call(method, std::move(payload)).get();

    if (response.status != rpc::Status::ok) {
        std::cerr << "RPC status=" << static_cast<unsigned>(response.status) << ": "
                  << response.payload << '\n';
        return 1;
    }

    std::cout << response.payload << '\n';
    return 0;
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    try {
        const std::string command = argv[1];

        if (command == "server") {
            if (argc > 3)
                throw std::invalid_argument("usage: ./tina server [port]");

            rpc::run_server(argc == 3 ? argv[2] : "3490");
            return 0;
        }

        // 通过 BrokerClient 发布消息。
        if (command == "publish") {
            if (argc != 5)
                throw std::invalid_argument("usage: ./tina publish <topic> <partition> <message>");

            const std::string topic = argv[2];
            const auto partition = parse_number<std::uint32_t>(argv[3], "partition");

            broker::BrokerClient client;

            auto result =
                client.publish({.topic = topic, .partition = partition, .message = argv[4]});

            std::cout << "PUBLISH OK: " << topic << "/" << partition << " offset=" << result.offset
                      << '\n';
            return 0;
        }

        // fetch：拉取一次后退出。
        // consume：持续轮询，直到用户中断。
        if (command == "fetch" || command == "consume") {
            if (argc != 4 && argc != 5)
                throw std::invalid_argument(
                    "usage: ./tina fetch|consume <topic> <partition> [offset]");

            const std::string topic = argv[2];
            const auto partition = parse_number<std::uint32_t>(argv[3], "partition");

            std::uint64_t offset = argc == 5 ? parse_number<std::uint64_t>(argv[4], "offset") : 0;

            const bool continuous = (command == "consume");

            broker::BrokerClient client;

            while (true) {
                auto result = client.fetch({.topic = topic,
                                            .partition = partition,
                                            .offset = offset,
                                            .max_bytes = rpc::max_payload_size});

                print_records(result);

                // 下次从这次成功获取的消息之后继续读取。
                offset = result.next_offset;

                if (!continuous)
                    break;

                // 没有新消息时，降低空轮询频率。
                // 这里只暂停 CLI 线程，不阻塞服务端 Worker。
                if (result.records.empty()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{200});
                }
            }

            return 0;
        }

        // 手动提交 Consumer 进度。
        if (command == "commit") {
            if (argc != 6)
                throw std::invalid_argument("usage: ./tina commit <consumer> <topic> "
                                            "<partition> <next_offset>");

            const std::string consumer = argv[2];
            const std::string topic = argv[3];

            const auto partition = parse_number<std::uint32_t>(argv[4], "partition");

            const auto next_offset = parse_number<std::uint64_t>(argv[5], "next_offset");

            broker::BrokerClient client;

            client.commit_offset({.consumer = consumer,
                                  .topic = topic,
                                  .partition = partition,
                                  .next_offset = next_offset});

            std::cout << "COMMIT OK: " << consumer << " " << topic << "/" << partition
                      << " next_offset=" << next_offset << '\n';

            return 0;
        }

        // 保留 KV。
        if (command == "ping" || command == "set" || command == "get" || command == "del") {
            return run_kv_command(argc, argv, command);
        }

        // 原有的异步 RPC 测试。
        if (command == "async-test" && argc == 2) {
            constexpr int count = 32;

            // 一个 RpcClient，一条 TCP 连接，一个 I/O 线程。
            rpc::RpcClient client;

            // 每个 Future 对应一个尚未完成的 RPC。
            std::vector<std::future<rpc::Response>> futures;
            futures.reserve(count);

            // 第一轮：连续提交 32 个 SET，不立即等待结果。
            for (int i = 0; i < count; ++i) {
                std::string payload = "async-" + std::to_string(i) + "\nvalue-" + std::to_string(i);

                futures.push_back(client.async_call(rpc::Method::set, std::move(payload)));
            }

            // 所有请求提交完毕，再依次检查响应。
            for (auto &future : futures) {
                auto response = future.get();

                if (response.status != rpc::Status::ok || response.payload != "OK")
                    throw std::runtime_error("async SET failed");
            }

            futures.clear();

            // 第二轮：同时提交 32 个 GET。
            for (int i = 0; i < count; ++i) {
                futures.push_back(
                    client.async_call(rpc::Method::get, "async-" + std::to_string(i)));
            }

            // 每个 Future 必须返回对应 key 的 value。
            for (int i = 0; i < count; ++i) {
                auto response = futures[i].get();

                std::string expected = "value-" + std::to_string(i);

                if (response.status != rpc::Status::ok || response.payload != expected)
                    throw std::runtime_error("async GET mismatch at index " + std::to_string(i));
            }

            std::cout << "async-test PASS: " << count * 2 << " RPC requests\n";

            return 0;
        }

        throw std::invalid_argument("unknown command: " + command);

    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

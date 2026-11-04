import std;

import rpc.client;
import rpc.protocol;
import rpc.server;

namespace {

void print_usage() {
    std::cout << "usage:\n"
              << "  ./tina server [port]\n"
              << "\n"
              << "  ./tina ping\n"
              << "  ./tina set <key> <value>\n"
              << "  ./tina get <key>\n"
              << "  ./tina del <key>\n"
              << "  ./tina async-test\n";
}

int run_client_command(int argc, char *argv[], const std::string &command) {
    Method method;
    std::string payload;
    if (command == "ping" && argc == 2) {
        method = Method::system_ping;
    } else if (command == "set" && argc == 4) {
        method = Method::kv_set;
        payload = std::string(argv[2]) + "\n" + argv[3];
    } else if (command == "get" && argc == 3) {
        method = Method::kv_get;
        payload = argv[2];
    } else if (command == "del" && argc == 3) {
        method = Method::kv_del;
        payload = argv[2];
    } else {
        throw std::invalid_argument("invalid client command arguments");
    }

    rpc::RpcClient client;
    auto pending = method == Method::system_ping ? client.ping()
                                                 : client.async_call(method, std::move(payload));
    auto response = pending.get();

    if (response.status != Status::ok) {
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

        if (command == "ping" || command == "set" || command == "get" || command == "del") {
            return run_client_command(argc, argv, command);
        }

        // 原有的异步 RPC 测试。
        if (command == "async-test" && argc == 2) {
            constexpr int count = 32;

            // 一个 RpcClient，一条 TCP 连接，一个 I/O 线程。
            rpc::RpcClient client;

            // 每个 Future 对应一个尚未完成的 RPC。
            std::vector<std::future<Frame>> futures;
            futures.reserve(count);

            // 第一轮：连续提交 32 个 SET，不立即等待结果。
            for (int i = 0; i < count; ++i) {
                std::string payload = "async-" + std::to_string(i) + "\nvalue-" + std::to_string(i);

                futures.push_back(client.async_call(Method::kv_set, std::move(payload)));
            }

            // 所有请求提交完毕，再依次检查响应。
            for (auto &future : futures) {
                auto response = future.get();

                if (response.status != Status::ok || response.payload != "OK")
                    throw std::runtime_error("async SET failed");
            }

            futures.clear();

            // 第二轮：同时提交 32 个 GET。
            for (int i = 0; i < count; ++i) {
                futures.push_back(
                    client.async_call(Method::kv_get, "async-" + std::to_string(i)));
            }

            // 每个 Future 必须返回对应 key 的 value。
            for (int i = 0; i < count; ++i) {
                auto response = futures[i].get();

                std::string expected = "value-" + std::to_string(i);

                if (response.status != Status::ok || response.payload != expected)
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

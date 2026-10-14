import std;

import rpc.client;
import rpc.protocol;
import rpc.server;

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cout << "usage:\n"
                     "  ./tina server\n"
                     "  ./tina ping\n"
                     "  ./tina set <key> <value>\n"
                     "  ./tina get <key>\n"
                     "  ./tina del <key>\n"
                     "  ./tina async-test\n";

        return 0;
    }

    try {
        std::string command = argv[1];

        if (command == "server") {
            if (argc > 3) {
                std::cerr << "usage: ./tina server [port]\n";
                return 1;
            }

            rpc::run_server(argc == 3 ? argv[2] : "3490");
            return 0;
        }

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

        rpc::Response response;

        std::cout << response.payload << '\n';
    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

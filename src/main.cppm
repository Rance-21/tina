import std;

import rpc.client;
import rpc.protocol;
import rpc.server;
import runtime.config;
import runtime.cpu_affinity;
import runtime.log;

namespace {

// 【作用】向标准输出打印 tina 服务端与客户端的命令行帮助说明。
void print_usage() {
    std::cout
        << "usage:\n"
        << "  ./tina server [port] [options]\n"
        << "\n"
        << "server options:\n"
        << "  --port <port>              Listen port (default: 3490)\n"
        << "  --normal-workers <k>       Number of Normal Workers, 1 <= k <= 7 (default: 3)\n"
        << "  --queue-capacity <n>       Per-lane SPSC queue capacity, power of 2 (default: 128)\n"
        << "  --inline-threshold <bytes> Max cache-hit size handled inline on IO loop (default: "
           "32768)\n"
        << "  --io-cpus <list>           Comma/range CPU list for EventLoops (default: 0,2,4,8)\n"
        << "  --worker-cpus <list>       Comma/range CPU list for Workers (default: 16-23)\n"
        << "  --no-affinity              Disable CPU topology validation and thread pinning\n"
        << "\n"
        << "client commands:\n"
        << "  ./tina ping\n"
        << "  ./tina set <key> <value>\n"
        << "  ./tina get <key>\n"
        << "  ./tina del <key>\n"
        << "  ./tina async-test\n";
}

struct ServerCliArgs {
    std::string port{"3490"};
    runtime::ServerConfig config{};
};

// 【作用】解析 `./tina server ...` 的端口和全部运行时配置参数。
// 【原理】顺序扫描 argv[2..argc-1]：兼容原有的位置端口参数（如 `./tina server 3490 --normal-workers
// 3`）
//         与命名参数（--port / --normal-workers / --io-cpus 等），末尾统一调用 config.validate()
//         校验范围。
ServerCliArgs parse_server_args(int argc, char *argv[]) {
    ServerCliArgs parsed;
    bool port_set = false;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];

        auto require_value = [&](std::string_view flag) -> std::string_view {
            if (i + 1 >= argc)
                throw std::invalid_argument("missing value for " + std::string(flag));
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else if (arg == "--port") {
            parsed.port = std::string(require_value(arg));
            port_set = true;
        } else if (arg == "--normal-workers") {
            const auto val = std::stoull(std::string(require_value(arg)));
            parsed.config.non_blk_worker_cnt = static_cast<std::size_t>(val);
        } else if (arg == "--queue-capacity") {
            const auto val = std::stoull(std::string(require_value(arg)));
            parsed.config.queue_cap = static_cast<std::size_t>(val);
        } else if (arg == "--inline-threshold") {
            const auto val = std::stoull(std::string(require_value(arg)));
            parsed.config.io_inline_threshold = static_cast<std::size_t>(val);
        } else if (arg == "--io-cpus") {
            parsed.config.io_cpus = runtime::affinity::parse_cpu_list(require_value(arg));
        } else if (arg == "--worker-cpus") {
            parsed.config.worker_cpus = runtime::affinity::parse_cpu_list(require_value(arg));
        } else if (arg == "--no-affinity") {
            parsed.config.enable_cpu_affinity = false;
        } else if (!arg.starts_with('-') && !port_set) {
            parsed.port = std::string(arg);
            port_set = true;
        } else {
            throw std::invalid_argument("unknown server argument: " + std::string(arg));
        }
    }

    parsed.config.validate();
    return parsed;
}

// 【作用】执行一次简单的命令行客户端 RPC 调用（ping / set / get / del）并打印服务端响应。
// 【原理】按子命令拼出 Method 与 payload -> 创建 RpcClient 异步发出请求 -> future.get()
// 同步等结果并检查状态码。
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

// 【作用】tina 程序总入口：根据第一个子命令分发到服务端、单次客户端命令或异步流水线自测。
// 【原理】server 模式会先解析配置、运行 rpc::run_server()，并在优雅退出后调用 log::shutdown()
//         把异步日志队列中的最后几条日志刷盘再返回；所有异常统一在最外层捕获并打印清晰错误信息。
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    try {
        const std::string command = argv[1];

        if (command == "--help" || command == "-h") {
            print_usage();
            return 0;
        }

        if (command == "server") {
            auto server_args = parse_server_args(argc, argv);
            rpc::run_server(server_args.port, std::move(server_args.config));
            runtime::log::shutdown();
            return 0;
        }

        if (command == "ping" || command == "set" || command == "get" || command == "del") {
            return run_client_command(argc, argv, command);
        }

        if (command == "async-test" && argc == 2) {
            constexpr int count = 32;
            rpc::RpcClient client;
            std::vector<std::future<Frame>> futures;
            futures.reserve(count);

            for (int i = 0; i < count; ++i) {
                std::string payload = "async-" + std::to_string(i) + "\nvalue-" + std::to_string(i);
                futures.push_back(client.async_call(Method::kv_set, std::move(payload)));
            }
            for (auto &future : futures) {
                auto response = future.get();
                if (response.status != Status::ok || response.payload != "OK")
                    throw std::runtime_error("async SET failed");
            }

            futures.clear();
            for (int i = 0; i < count; ++i) {
                futures.push_back(client.async_call(Method::kv_get, "async-" + std::to_string(i)));
            }
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

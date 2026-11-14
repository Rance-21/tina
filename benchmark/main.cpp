import std;
import bench.client;
import bench.workload;
import bench.stats;

namespace {

void print_usage() {
    std::cout << "usage:\n"
              << "  ./tina_bench [options]\n\n"
              << "options:\n"
              << "  --host <addr>         Server host (default: 127.0.0.1)\n"
              << "  --port <port>         Server port (default: 3490)\n"
              << "  --workload <name>     ping | kv_set | kv_get | kv_mixed | file_read | list_dir | fs_mixed (default: ping)\n"
              << "  --fs-root <path>      fs_mixed 扫描的目录，相对 ~/projects (default: linux)\n"
              << "  --fs-list-percent <n> fs_mixed 中 list_dir 占比 0..100，其余为 read_file (default: 50)\n"
              << "  --connections <n>     Number of TCP connections (default: 1)\n"
              << "  --inflight <n>        Max in-flight requests per connection, 1..64 (default: 64)\n"
              << "  --warmup <sec>        Warmup duration in seconds (default: 5)\n"
              << "  --duration <sec>      Total benchmark duration in seconds (default: 30)\n"
              << "  --output <path>       Output report file path (default: benchmark/result.txt)\n"
              << "  --help                Show this help message\n";
}

bench::BenchConfig parse_args(int argc, char *argv[]) {
    bench::BenchConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        auto require_value = [&](std::string_view flag) -> std::string_view {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + std::string(flag));
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else if (arg == "--host") {
            config.host = std::string(require_value(arg));
        } else if (arg == "--port") {
            config.port = std::string(require_value(arg));
        } else if (arg == "--workload") {
            config.workload.type = bench::parse_workload_type(require_value(arg));
        } else if (arg == "--fs-root") {
            config.workload.fs_root = std::string(require_value(arg));
        } else if (arg == "--fs-list-percent") {
            config.workload.fs_list_dir_percent = static_cast<std::uint32_t>(
                std::min<unsigned long long>(std::stoull(std::string(require_value(arg))), 100));
        } else if (arg == "--connections") {
            config.connections = std::stoull(std::string(require_value(arg)));
        } else if (arg == "--inflight") {
            config.max_inflight = std::stoull(std::string(require_value(arg)));
        } else if (arg == "--warmup") {
            config.warmup_duration =
                std::chrono::seconds(std::stoll(std::string(require_value(arg))));
        } else if (arg == "--duration") {
            config.total_duration =
                std::chrono::seconds(std::stoll(std::string(require_value(arg))));
        } else if (arg == "--output") {
            config.output_file = std::filesystem::path(require_value(arg));
        } else {
            throw std::invalid_argument("unknown argument: " + std::string(arg));
        }
    }

    return config;
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        auto config = parse_args(argc, argv);
        bench::BenchRunner runner(config);
        auto summary = runner.run();

        std::cout << "\n" << bench::format_report(summary);
        std::cout << "[BenchRunner] Report written to: " << config.output_file.string() << "\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "benchmark error: " << e.what() << "\n";
        return 1;
    }
}

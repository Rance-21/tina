import std;
import rpc.protocol;
import bench.client;
import bench.workload;
import bench.stats;
import bench.ide_mixed;

namespace {

void print_usage() {
    std::cout
        << "usage:\n"
        << "  ./tina_bench [options]\n\n"
        << "options:\n"
        << "  --host <addr>         Server host (default: 127.0.0.1)\n"
        << "  --port <port>         Server port (default: 3490)\n"
        << "  --workload <name>     ping | kv_set | kv_get | kv_mixed | file_read | list_dir | "
           "fs_mixed | ide_mixed\n"
        << "  --fs-root <path>      fs_mixed / ide_mixed 扫描的目录，相对 ~/projects (default: "
           "linux)\n"
        << "  --fs-list-percent <n> fs_mixed 中 list_dir 占比 0..100，其余为 read_file (default: "
           "50)\n"
        << "  --fs-hot-percent <n>  ide_mixed 文件读中热文件占比 0..100 (default: 70)\n"
        << "  --fs-scratch-root <p> ide_mixed 临时工作区的父目录，相对 ~/projects，须在源码树外 "
           "(default: .)\n"
        << "  --read-bytes <n>      单次文件读取上限 (ide_mixed default: 65536; others: 4096)\n"
        << "  --connections <n>     Number of TCP connections (default: 1)\n"
        << "  --inflight <n>        Max in-flight requests per connection, 1..64 (default: 64)\n"
        << "  --warmup <sec>        Warmup duration in seconds (default: 5)\n"
        << "  --duration <sec>      Total benchmark duration in seconds (default: 30)\n"
        << "  --output <path>       Output report file path (default: benchmark/result.txt)\n"
        << "  --help                Show this help message\n";
    std::cout
        << "\nide_mixed defaults: 10s warmup + 50s measured (--duration 60 includes warmup),\n"
        << "output benchmark/result_ide_mixed.txt; same host/HOME as server required.\n";
}

bench::BenchConfig parse_args(int argc, char *argv[]) {
    bench::BenchConfig config;
    bool warmup_set = false, duration_set = false, output_set = false, read_set = false;

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
        } else if (arg == "--fs-hot-percent") {
            const auto value = std::stoull(std::string(require_value(arg)));
            if (value > 100)
                throw std::invalid_argument("fs-hot-percent must be in 0..100");
            config.workload.fs_hot_percent = static_cast<std::uint32_t>(value);
        } else if (arg == "--fs-scratch-root") {
            config.workload.fs_scratch_root = std::string(require_value(arg));
        } else if (arg == "--read-bytes") {
            const auto value = std::stoull(std::string(require_value(arg)));
            if (value == 0 || value > max_file_read_bytes)
                throw std::invalid_argument("read-bytes exceeds protocol limit");
            config.workload.read_max_bytes = static_cast<std::uint32_t>(value);
            read_set = true;
        } else if (arg == "--connections") {
            config.connections = std::stoull(std::string(require_value(arg)));
        } else if (arg == "--inflight") {
            config.max_inflight = std::stoull(std::string(require_value(arg)));
        } else if (arg == "--warmup") {
            warmup_set = true;
            config.warmup_duration =
                std::chrono::seconds(std::stoll(std::string(require_value(arg))));
        } else if (arg == "--duration") {
            duration_set = true;
            config.total_duration =
                std::chrono::seconds(std::stoll(std::string(require_value(arg))));
        } else if (arg == "--output") {
            output_set = true;
            config.output_file = std::filesystem::path(require_value(arg));
        } else {
            throw std::invalid_argument("unknown argument: " + std::string(arg));
        }
    }

    if (config.workload.type == bench::WorkloadType::ide_mixed) {
        if (!warmup_set)
            config.warmup_duration = std::chrono::seconds(10);
        if (!duration_set)
            config.total_duration = config.warmup_duration + std::chrono::seconds(50);
        if (!output_set)
            config.output_file = "benchmark/result_ide_mixed.txt";
        if (!read_set)
            config.workload.read_max_bytes = 65536;
    }
    return config;
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        auto config = parse_args(argc, argv);
        auto summary = config.workload.type == bench::WorkloadType::ide_mixed
                           ? bench::MixedRunner(config).run()
                           : bench::BenchRunner(config).run();

        std::cout << "\n" << bench::format_report(summary);
        std::cout << "[BenchRunner] Report written to: " << config.output_file.string() << "\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "benchmark error: " << e.what() << "\n";
        return 1;
    }
}

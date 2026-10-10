module;

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
export module runtime.log;
import std;

export namespace runtime::log {
enum class Level { debug, info, warn, error };
// 启动时初始化一次，之后供所有线程使用。
void init(std::string_view filename = "logs/tina.log", Level level = Level::info) {
    static std::once_flag once;

    std::call_once(once, [=] {
        if (filename.empty()) {
            throw std::invalid_argument("log filename is empty");
        }

        std::filesystem::path path{std::string(filename)};

        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        // 8192 条日志队列，一个后台写入线程。
        spdlog::init_thread_pool(8192, 1);

        // 控制台输出。
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        // 每个文件最多 10 MiB，保留 3 个轮转文件。
        auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path.string(),
                                                                           10 * 1024 * 1024, 3);

        std::vector<spdlog::sink_ptr> sinks{console, file};

        auto logger = std::make_shared<spdlog::async_logger>("tina", sinks.begin(), sinks.end(),
                                                             spdlog::thread_pool(),
                                                             spdlog::async_overflow_policy::block);

        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [tid %t] %v");

        spdlog::level::level_enum native_level;

        switch (level) {
        case Level::debug:
            native_level = spdlog::level::debug;
            break;
        case Level::info:
            native_level = spdlog::level::info;
            break;
        case Level::warn:
            native_level = spdlog::level::warn;
            break;
        case Level::error:
            native_level = spdlog::level::err;
            break;
        }

        logger->set_level(native_level);
        logger->flush_on(spdlog::level::err);

        spdlog::set_default_logger(std::move(logger));
    });
}

void debug(std::string_view message) {
    spdlog::debug("{}", message);
}

void info(std::string_view message) {
    spdlog::info("{}", message);
}

void warn(std::string_view message) {
    spdlog::warn("{}", message);
}

void error(std::string_view message) {
    spdlog::error("{}", message);
}

// 所有业务线程退出后再调用。
void shutdown() {
    spdlog::shutdown();
}

} // namespace runtime::log

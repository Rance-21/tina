module;
#include <arpa/inet.h>
export module rpc.protocol;
import std;
import net.socket;

// 本地和网络的简单协议
export namespace rpc {

enum class Method : std::uint8_t { ping = 1, set = 2, get = 3, del = 4 };
enum class Status : std::uint8_t { ok = 0, not_found = 1, bad_request = 2 };

struct Request {
    std::uint32_t id;
    Method method;
    std::string payload;
};

struct Response {
    std::uint32_t request_id;
    Status status;
    std::string payload;
};

constexpr std::uint32_t max_payload_size = 1024 * 1024;
} // namespace rpc

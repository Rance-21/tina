module;
#include <arpa/inet.h>

export module rpc.protocol;

import std;
import net.socket;

export namespace rpc {

/*
// 一条消息就是一串二进制字节
using Message = std::vector<uint8_t>;

// 一个 Topic 对应一个不断往后追加（push_back）的消息数组
using TopicLog = std::vector<Message>;

// 整个 Broker 的核心存储：用 Topic 名字（字符串）映射到对应的消息数组
std::unordered_map<std::string, TopicLog> broker_storage;
*/

enum class Method : std::uint8_t {
    ping = 1,
    set = 2,
    get = 3,
    del = 4,

    // Message Broker，16 开始以位运算
    // 客户端往指定的 topic 发送一条或多条数据
    publish = 16,
    // 客户端（Consumer）带着参数去问服务器拿数据。请求包里通常会包含 Topic 名称 + 起始 Offset +
    // 最大拉取条数/字节数
    fetch = 17,
    // 告诉服务端某 Topic 属于该客户端的已经处理到 offset = 109
    commit_offset = 18
};

enum class Status : std::uint8_t {
    ok = 0,
    not_found = 1,
    bad_request = 2,
    conflict = 3,
    incomplete = 4,
    io_error = 5,
    resource_exhausted = 6
};

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
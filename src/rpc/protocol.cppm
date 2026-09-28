module;

#include <arpa/inet.h>

export module tina.rpc.protocol;

import std;
import tina.net.socket;

export namespace tina::rpc
{

enum class Method : std::uint8_t
{
    ping = 1,
    set = 2,
    get = 3,
    del = 4
};

enum class Status : std::uint8_t
{
    ok = 0,
    not_found = 1,
    bad_request = 2
};

struct Request
{
    std::uint32_t request_id{};
    Method method{};
    std::string payload;
};

struct Response
{
    std::uint32_t request_id{};
    Status status{};
    std::string payload;
};

constexpr std::uint32_t max_payload_size = 1024 * 1024;

// Request frame:
// [length:4][request_id:4][method:1][payload:N]
//
// length 表示它后面还有多少字节，也就是 4 + 1 + payload。
void send_request(int fd, const Request& request)
{
    if (request.payload.size() > max_payload_size)
        throw std::runtime_error("payload too large");

    std::uint32_t length =
        5 + static_cast<std::uint32_t>(request.payload.size());

    std::uint32_t network_length = ::htonl(length);
    std::uint32_t network_id = ::htonl(request.request_id);
    std::uint8_t method = static_cast<std::uint8_t>(request.method);

    net::send_all(fd, &network_length, sizeof(network_length));
    net::send_all(fd, &network_id, sizeof(network_id));
    net::send_all(fd, &method, sizeof(method));

    if (!request.payload.empty())
        net::send_all(fd, request.payload.data(), request.payload.size());
}

bool recv_request(int fd, Request& request)
{
    std::uint32_t network_length{};

    if (!net::recv_exact(fd, &network_length, sizeof(network_length)))
        return false;

    std::uint32_t length = ::ntohl(network_length);

    if (length < 5 || length > max_payload_size + 5)
        throw std::runtime_error("invalid request frame length");

    std::uint32_t network_id{};
    std::uint8_t method{};

    if (!net::recv_exact(fd, &network_id, sizeof(network_id)))
        throw std::runtime_error("incomplete request");

    if (!net::recv_exact(fd, &method, sizeof(method)))
        throw std::runtime_error("incomplete request");

    request.request_id = ::ntohl(network_id);
    request.method = static_cast<Method>(method);

    std::size_t payload_size = length - 5;
    request.payload.resize(payload_size);

    if (payload_size != 0 &&
        !net::recv_exact(fd, request.payload.data(), payload_size))
    {
        throw std::runtime_error("incomplete request");
    }

    return true;
}

// Response frame:
// [length:4][request_id:4][status:1][payload:N]
void send_response(int fd, const Response& response)
{
    if (response.payload.size() > max_payload_size)
        throw std::runtime_error("payload too large");

    std::uint32_t length =
        5 + static_cast<std::uint32_t>(response.payload.size());

    std::uint32_t network_length = ::htonl(length);
    std::uint32_t network_id = ::htonl(response.request_id);
    std::uint8_t status = static_cast<std::uint8_t>(response.status);

    net::send_all(fd, &network_length, sizeof(network_length));
    net::send_all(fd, &network_id, sizeof(network_id));
    net::send_all(fd, &status, sizeof(status));

    if (!response.payload.empty())
        net::send_all(fd, response.payload.data(), response.payload.size());
}

bool recv_response(int fd, Response& response)
{
    std::uint32_t network_length{};

    if (!net::recv_exact(fd, &network_length, sizeof(network_length)))
        return false;

    std::uint32_t length = ::ntohl(network_length);

    if (length < 5 || length > max_payload_size + 5)
        throw std::runtime_error("invalid response frame length");

    std::uint32_t network_id{};
    std::uint8_t status{};

    if (!net::recv_exact(fd, &network_id, sizeof(network_id)))
        throw std::runtime_error("incomplete response");

    if (!net::recv_exact(fd, &status, sizeof(status)))
        throw std::runtime_error("incomplete response");

    response.request_id = ::ntohl(network_id);
    response.status = static_cast<Status>(status);

    std::size_t payload_size = length - 5;
    response.payload.resize(payload_size);

    if (payload_size != 0 &&
        !net::recv_exact(fd, response.payload.data(), payload_size))
    {
        throw std::runtime_error("incomplete response");
    }

    return true;
}

}

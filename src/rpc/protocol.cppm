module;
#include <arpa/inet.h>
export module rpc.protocol;
import std;
import net.socket;
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

void send_request(net::Socket &socket, const Request &request) {
    if (request.payload.size() > max_payload_size) {
        throw std::runtime_error("payload too large");
    }

    // request_id 4 bytes + method 1 byte + payload
    std::uint32_t length = 4 + 1 + static_cast<std::uint32_t>(request.payload.size());

    // 主机字节序 -> 网络字节序
    std::uint32_t network_length = htonl(length);
    std::uint32_t network_id = htonl(request.id);
    std::uint8_t method = static_cast<std::uint8_t>(request.method);

    socket.send_all(&network_length, sizeof(network_length));
    socket.send_all(&network_id, sizeof(network_id));
    socket.send_all(&method, sizeof(method));

    if (!request.payload.empty()) {
        socket.send_all(request.payload.data(), request.payload.size());
    }
}

bool recv_request(net::Socket &socket, Request &request) {
    std::uint32_t network_length;

    // 连 length 都没收到就断开，说明这个连接正常结束。
    if (!socket.recv_exact(&network_length, sizeof(network_length))) {
        return false;
    }

    std::uint32_t length = ntohl(network_length);

    if (length < 5 || length > max_payload_size + 5) {
        throw std::runtime_error("invalid frame length");
    }

    std::uint32_t network_id;
    std::uint8_t method;

    if (!socket.recv_exact(&network_id, sizeof(network_id))) {
        throw std::runtime_error("incomplete request");
    }

    if (!socket.recv_exact(&method, sizeof(method))) {
        throw std::runtime_error("incomplete request");
    }

    request.id = ntohl(network_id);
    request.method = static_cast<Method>(method);

    std::size_t payload_size = length - 5;

    request.payload.resize(payload_size);

    if (payload_size != 0 && !socket.recv_exact(request.payload.data(), payload_size)) {
        throw std::runtime_error("incomplete request");
    }

    return true;
}

void send_response(net::Socket &socket, const Response &response) {
    if (response.payload.size() > max_payload_size) {
        throw std::runtime_error("payload too large");
    }

    std::uint32_t length = 4 + 1 + static_cast<std::uint32_t>(response.payload.size());

    std::uint32_t network_length = ::htonl(length);
    std::uint32_t network_id = ::htonl(response.request_id);
    std::uint8_t status = static_cast<std::uint8_t>(response.status);

    socket.send_all(&network_length, sizeof(network_length));
    socket.send_all(&network_id, sizeof(network_id));
    socket.send_all(&status, sizeof(status));

    if (!response.payload.empty()) {
        socket.send_all(response.payload.data(), response.payload.size());
    }
}

bool recv_response(net::Socket &socket, Response &response) {
    std::uint32_t network_length;

    if (!socket.recv_exact(&network_length, sizeof(network_length))) {
        return false;
    }

    std::uint32_t length = ::ntohl(network_length);

    if (length < 5 || length > max_payload_size + 5)
        throw std::runtime_error("invalid frame length");

    std::uint32_t network_id;
    std::uint8_t status;

    if (!socket.recv_exact(&network_id, sizeof(network_id)))
        throw std::runtime_error("incomplete response");

    if (!socket.recv_exact(&status, sizeof(status)))
        throw std::runtime_error("incomplete response");

    response.request_id = ::ntohl(network_id);
    response.status = static_cast<Status>(status);

    std::size_t payload_size = length - 5;
    response.payload.resize(payload_size);

    if (payload_size != 0 && !socket.recv_exact(response.payload.data(), payload_size)) {
        throw std::runtime_error("incomplete response");
    }

    return true;
}

} // namespace rpc

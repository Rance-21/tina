export module rpc.client;

import std;

import net.socket;
import rpc.protocol;

export namespace rpc {

Response call(Method method, std::string payload, std::string_view host = "127.0.0.1",
              std::string_view port = "3490") {
    auto socket = net::Socket::connect_tcp(host, port);

    Request request{.request_id = 1, .method = method, .payload = std::move(payload)};

    send_request(socket, request);

    Response response;

    if (!recv_response(socket, response))
        throw std::runtime_error("server closed before response");

    return response;
}

} // namespace rpc

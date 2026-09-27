export module tina.rpc.client;

import std;

import tina.net.socket;
import tina.rpc.protocol;

export namespace tina::rpc {

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

} // namespace tina::rpc
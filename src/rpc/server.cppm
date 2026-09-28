export module rpc.server;

import std;

import net.socket;
import rpc.protocol;
import service.kv;

export namespace rpc {

void run_server(std::string_view port) {
    auto listener = net::Socket::listen_tcp(port);

    service::KvService kv;

    std::cout << "tina: listening on port " << port << '\n';

    while (true) {
        try {
            // 目前这里是 blocking accept。
            auto client = listener.accept_client();

            std::cout << "tina: client connected\n";

            Request request;

            // 同一个 TCP connection 可以连续执行多个 RPC。
            while (recv_request(client, request)) {
                Response response = kv.handle(request);
                send_response(client, response);
            }

            std::cout << "tina: client disconnected\n";
        } catch (const std::exception &error) {
            std::cerr << "connection error: " << error.what() << '\n';
        }
    }
}

} // namespace rpc

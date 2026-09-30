export module rpc.server;

import std;

import net.socket;
import rpc.event_loop;

export namespace rpc {

void run_server(std::string_view port) {
    auto listener = net::Socket::listen_tcp(port);

    std::cout << "tina: listening on port " << port << std::endl;

    EventLoop loop{std::move(listener)};

    loop.run();
}

} // namespace rpc
export module rpc.server;

import std;
import net.socket;
import net.epoll;
import rpc.connection;
import service.kv;

export namespace rpc {

void run_server(std::string_view port) {
    auto listener = net::Socket::listen_tcp(port);
    listener.set_non_blocking();

    net::Epoll epoll;
    epoll.add(listener.native_handle(), net::EpollInterest::read);

    std::unordered_map<int, Connection> connections;
    service::KvService kv;
    std::cout << "tina: listening on port " << port << std::endl;

    while (true) {
        for (const auto event : epoll.wait()) {
            if (event.fd == listener.native_handle()) {
                while (auto client = listener.try_accept()) {

                    int fd = client->native_handle();
                    auto [it, inserted] = connections.emplace(fd, std::move(*client));

                    try {
                        epoll.add(fd, net::EpollInterest::read | net::EpollInterest::peer_closed);
                    } catch (...) {
                        connections.erase(it);
                        throw;
                    }
                }
                continue;
            }

            auto it = connections.find(event.fd);
            if (it == connections.end())
                continue;

            auto &connection = it->second;
            bool keep = !event.error();

            try {
                if (keep && (event.readable() || event.peer_closed() || event.hangup()))
                    keep = connection.read(kv);
                if (keep && connection.wants_write())
                    connection.write();
                if (keep && event.hangup() && !event.readable())
                    connection.mark_peer_closed();
                if (keep && connection.finished())
                    keep = false;
                if (keep) {
                    auto interest = net::EpollInterest::read | net::EpollInterest::peer_closed;
                    if (connection.wants_write())
                        interest = interest | net::EpollInterest::write;
                    epoll.modify(event.fd, interest);
                }
            } catch (const std::exception &error) {
                std::cerr << "connection error: " << error.what() << '\n';
                keep = false;
            }
            if (!keep) {
                epoll.remove(event.fd);
                connections.erase(it);
            }
        }
    }
}
} // namespace rpc

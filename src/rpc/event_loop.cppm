export module rpc.event_loop;

import std;

import net.epoll;
import net.socket;
import rpc.connection;
import service.kv;

export namespace rpc {

class EventLoop {
  public:
    explicit EventLoop(net::Socket listener) : listener_(std::move(listener)) {
        listener_.set_non_blocking();
        epoll_.add(listener_.native_handle(), net::EpollInterest::read);
    }

    void run() {
        while (true) {
            auto events = epoll_.wait();

            for (const auto &event : events) {
                if (event.fd == listener_.native_handle()) {

                    handle_accept();

                    continue;
                }

                handle_connection(event);
            }
        }
    }

  private:
    // listen socket readable：
    // accept queue 中已经存在建立完成的连接。
    void handle_accept() {
        try {
            while (auto client = listener_.try_accept()) {

                const int fd = client->native_handle();

                auto [it, inserted] = connections_.emplace(fd, Connection{std::move(*client)});

                if (!inserted) {
                    throw std::runtime_error("duplicate connection fd");
                }

                try {
                    epoll_.add(fd, net::EpollInterest::read | net::EpollInterest::peer_closed);
                } catch (...) {
                    connections_.erase(it);
                    throw;
                }
            }
        } catch (const std::exception &error) {
            std::cerr << "accept error: " << error.what() << '\n';
        }
    }

    void handle_connection(const net::EpollEvent &event) {
        auto it = connections_.find(event.fd);

        // 理论上不应该出现。
        // 但 event 可能来自已经被移除的旧状态，
        // 这里直接忽略即可。
        if (it == connections_.end()) {
            return;
        }

        Connection &connection = it->second;

        bool keep = !event.error();

        try {
            // EPOLLIN / RDHUP / HUP 都尝试把
            // 内核 receive queue 中剩余的数据读出来。
            if (keep && (event.readable() || event.peer_closed() || event.hangup())) {

                keep = connection.read(kv_);
            }

            // read() 可能刚刚生成 Response，
            // 所以不需要等下一轮 EPOLLOUT，
            // 先主动尝试发送一次。
            if (keep && connection.wants_write()) {

                connection.write();
            }

            if (keep && connection.finished()) {

                keep = false;
            }

            if (keep) {
                update_interest(connection);
            }
        } catch (const std::exception &error) {
            std::cerr << "connection " << event.fd << " error: " << error.what() << '\n';

            keep = false;
        }

        if (!keep) {
            close_connection(it);
        }
    }

    void update_interest(const Connection &connection) {
        auto interest = net::EpollInterest::read | net::EpollInterest::peer_closed;

        // 只有存在尚未发送的数据， 才监听 EPOLLOUT。
        if (connection.wants_write()) {
            interest = interest | net::EpollInterest::write;
        }

        epoll_.modify(connection.fd(), interest);
    }

    void close_connection(std::unordered_map<int, Connection>::iterator it) {
        const int fd = it->first;
        epoll_.remove(fd);
        connections_.erase(it);
    }

    net::Socket listener_;
    net::Epoll epoll_;
    std::unordered_map<int, Connection> connections_;
    // 当前还是单线程，
    // 所有 RPC 都直接在 EventLoop 线程执行。
    service::KvService kv_;
};

} // namespace rpc
module;
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
export module tina.net.socket;
import std;

export namespace tina::net {
class Socket {
  public:
    ~Socket() {
        if (fd_ >= 0)
            ::close(fd_);
    }

    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    Socket(Socket &&other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    Socket &operator=(Socket &&other) noexcept {
        if (this == &other)
            return *this;

        if (fd_ >= 0)
            ::close(fd_);

        fd_ = other.fd_;
        other.fd_ = -1;

        return *this;
    }

    // 创建 bind + listen 完成后的 TCP listening socket。
    static Socket listen_tcp(std::string_view port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        addrinfo *results = nullptr;
        std::string port_string{port};

        int rv = ::getaddrinfo(nullptr, port_string.c_str(), &hints, &results);

        if (rv != 0)
            throw std::runtime_error(::gai_strerror(rv));

        for (addrinfo *p = results; p != nullptr; p = p->ai_next) {
            int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);

            if (fd == -1)
                continue;

            int yes = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

            if (::bind(fd, p->ai_addr, p->ai_addrlen) == -1) {
                ::close(fd);
                continue;
            }

            if (::listen(fd, 128) == -1) {
                ::close(fd);
                continue;
            }

            ::freeaddrinfo(results);
            return Socket{fd};
        }

        ::freeaddrinfo(results);
        throw std::runtime_error("failed to bind/listen");
    }

    // 创建一个 TCP socket，并连接服务器。
    static Socket connect_tcp(std::string_view host, std::string_view port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *results = nullptr;

        std::string host_string{host};
        std::string port_string{port};

        int rv = ::getaddrinfo(host_string.c_str(), port_string.c_str(), &hints, &results);

        if (rv != 0)
            throw std::runtime_error(::gai_strerror(rv));

        for (addrinfo *p = results; p != nullptr; p = p->ai_next) {
            int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);

            if (fd == -1)
                continue;

            if (::connect(fd, p->ai_addr, p->ai_addrlen) == -1) {
                ::close(fd);
                continue;
            }

            ::freeaddrinfo(results);
            return Socket{fd};
        }

        ::freeaddrinfo(results);
        throw std::runtime_error("failed to connect");
    }

    Socket accept_client() const {
        while (true) {
            int client_fd = ::accept(fd_, nullptr, nullptr);

            if (client_fd >= 0)
                return Socket{client_fd};

            if (errno == EINTR)
                continue;

            throw std::runtime_error(std::string{"accept: "} + ::strerror(errno));
        }
    }

    // 接收恰好 bytes 字节。
    // 如果一个字节都没收到连接就关闭，返回 false。
    bool recv_exact(void *buffer, std::size_t bytes) const {
        auto *data = static_cast<char *>(buffer);
        std::size_t received = 0;

        while (received < bytes) {
            ssize_t n = ::recv(fd_, data + received, bytes - received, 0);

            if (n == 0) {
                if (received == 0)
                    return false;

                throw std::runtime_error("peer closed in the middle of a frame");
            }

            if (n < 0) {
                if (errno == EINTR)
                    continue;

                throw std::runtime_error(std::string{"recv: "} + ::strerror(errno));
            }

            received += static_cast<std::size_t>(n);
        }

        return true;
    }

    // send() 不保证一次全部写完，所以这里循环。
    void send_all(const void *buffer, std::size_t bytes) const {
        const auto *data = static_cast<const char *>(buffer);
        std::size_t sent = 0;

        while (sent < bytes) {
            ssize_t n = ::send(fd_, data + sent, bytes - sent, MSG_NOSIGNAL);

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error(std::string{"send: "} + ::strerror(errno));
            }
            sent += static_cast<std::size_t>(n);
        }
    }

  private:
    explicit Socket(int fd) : fd_(fd) {
    }
    int fd_;
};
} // namespace tina::net
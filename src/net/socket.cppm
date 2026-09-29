module;
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

export module net.socket;
import std;

export namespace net {

class Socket {
  public:
    ~Socket() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    Socket(Socket &&other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    Socket &operator=(Socket &&other) noexcept {
        if (this == &other) {
            return *this;
        }

        if (fd_ >= 0) {
            ::close(fd_);
        }

        fd_ = other.fd_;
        other.fd_ = -1;

        return *this;
    }

    static Socket listen_tcp(std::string_view port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        addrinfo *results = nullptr;
        std::string port_string{port};

        int rv = ::getaddrinfo(nullptr, port_string.c_str(), &hints, &results);
        if (rv != 0) {
            throw std::runtime_error(::gai_strerror(rv));
        }

        for (addrinfo *p = results; p != nullptr; p = p->ai_next) {
            int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);

            if (fd == -1) {
                continue;
            }

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

    static Socket connect_tcp(std::string_view host, std::string_view port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *results = nullptr;

        std::string host_string{host};
        std::string port_string{port};

        int rv = ::getaddrinfo(host_string.c_str(), port_string.c_str(), &hints, &results);
        if (rv != 0) {
            throw std::runtime_error(::gai_strerror(rv));
        }

        for (addrinfo *p = results; p != nullptr; p = p->ai_next) {
            int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);

            if (fd == -1) {
                continue;
            }

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

    // 把 socket 设置为 non-blocking。
    void set_non_blocking() {
        int flags = ::fcntl(fd_, F_GETFL, 0);

        if (flags == -1) {
            throw std::runtime_error(std::string{"fcntl F_GETFL: "} + ::strerror(errno));
        }

        if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
            throw std::runtime_error(std::string{"fcntl F_SETFL: "} + ::strerror(errno));
        }
    }

    // non-blocking accept。
    // 有连接：
    //     返回 Socket
    // 当前 accept queue 为空：
    //     返回 nullopt
    std::optional<Socket> try_accept() const {
        while (true) {
            int client_fd = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (client_fd >= 0) {
                return Socket{client_fd};
            }

            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return std::nullopt;
            }

            throw std::runtime_error(std::string{"accept4: "} + ::strerror(errno));
        }
    }

    // non-blocking recv。
    // > 0:
    //     实际读取字节数
    // == 0:
    //     对方关闭连接
    // nullopt:
    //     当前没有更多数据（EAGAIN）
    std::optional<std::size_t> try_recv(void *buffer, std::size_t bytes) const {
        while (true) {
            ssize_t n = ::recv(fd_, buffer, bytes, 0);

            if (n > 0) {
                return static_cast<std::size_t>(n);
            }

            if (n == 0) {
                return std::size_t{0};
            }

            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return std::nullopt;
            }

            throw std::runtime_error(std::string{"recv: "} + ::strerror(errno));
        }
    }

    // non-blocking send。
    // 返回：
    //     实际发送出去的字节数。
    // nullopt：
    //     send buffer 暂时没有空间。
    std::optional<std::size_t> try_send(const void *buffer, std::size_t bytes) const {
        while (true) {
            ssize_t n = ::send(fd_, buffer, bytes, MSG_NOSIGNAL);

            if (n >= 0) {
                return static_cast<std::size_t>(n);
            }

            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return std::nullopt;
            }

            throw std::runtime_error(std::string{"send: "} + ::strerror(errno));
        }
    }

    // 下面两个函数暂时保留给 blocking client 使用。
    bool recv_exact(void *buffer, std::size_t bytes) const {
        auto *data = static_cast<char *>(buffer);

        std::size_t received = 0;

        while (received < bytes) {
            ssize_t n = ::recv(fd_, data + received, bytes - received, 0);

            if (n == 0) {
                if (received == 0) {
                    return false;
                }

                throw std::runtime_error("peer closed in the middle of a frame");
            }

            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }

                throw std::runtime_error(std::string{"recv: "} + ::strerror(errno));
            }

            received += static_cast<std::size_t>(n);
        }

        return true;
    }

    void send_all(const void *buffer, std::size_t bytes) const {
        const auto *data = static_cast<const char *>(buffer);

        std::size_t sent = 0;

        while (sent < bytes) {
            ssize_t n = ::send(fd_, data + sent, bytes - sent, MSG_NOSIGNAL);

            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }

                throw std::runtime_error(std::string{"send: "} + ::strerror(errno));
            }

            sent += static_cast<std::size_t>(n);
        }
    }

    [[nodiscard]]
    int native_handle() const noexcept {
        return fd_;
    }

  private:
    explicit Socket(int fd) : fd_(fd) {
    }

    int fd_{-1};
};

} // namespace net
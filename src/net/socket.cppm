module;

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

export module tina.net.socket;

import std;

export namespace tina::net
{

int listen_tcp(std::string_view port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* results = nullptr;
    std::string port_string{port};

    int error = ::getaddrinfo(nullptr, port_string.c_str(), &hints, &results);
    if (error != 0)
        throw std::runtime_error(::gai_strerror(error));

    int listen_fd = -1;

    for (addrinfo* p = results; p != nullptr; p = p->ai_next)
    {
        listen_fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listen_fd == -1)
            continue;

        int yes = 1;
        ::setsockopt(
            listen_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &yes,
            sizeof(yes)
        );

        if (::bind(listen_fd, p->ai_addr, p->ai_addrlen) == -1)
        {
            ::close(listen_fd);
            listen_fd = -1;
            continue;
        }

        if (::listen(listen_fd, 128) == -1)
        {
            ::close(listen_fd);
            listen_fd = -1;
            continue;
        }

        break;
    }

    ::freeaddrinfo(results);

    if (listen_fd == -1)
        throw std::runtime_error("failed to bind/listen");

    return listen_fd;
}

int connect_tcp(std::string_view host, std::string_view port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    std::string host_string{host};
    std::string port_string{port};

    int error = ::getaddrinfo(
        host_string.c_str(),
        port_string.c_str(),
        &hints,
        &results
    );

    if (error != 0)
        throw std::runtime_error(::gai_strerror(error));

    int fd = -1;

    for (addrinfo* p = results; p != nullptr; p = p->ai_next)
    {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == -1)
            continue;

        if (::connect(fd, p->ai_addr, p->ai_addrlen) == -1)
        {
            ::close(fd);
            fd = -1;
            continue;
        }

        break;
    }

    ::freeaddrinfo(results);

    if (fd == -1)
        throw std::runtime_error("failed to connect");

    return fd;
}

int accept_client(int listen_fd)
{
    while (true)
    {
        int client_fd = ::accept(listen_fd, nullptr, nullptr);

        if (client_fd >= 0)
            return client_fd;

        if (errno != EINTR)
        {
            throw std::runtime_error(
                std::string{"accept: "} + ::strerror(errno)
            );
        }
    }
}

// TCP 是字节流。一次 recv() 不保证拿到我们需要的全部字节。
bool recv_exact(int fd, void* buffer, std::size_t byte_count)
{
    auto* data = static_cast<char*>(buffer);
    std::size_t received = 0;

    while (received < byte_count)
    {
        ssize_t n = ::recv(fd, data + received, byte_count - received, 0);

        if (n == 0)
        {
            if (received == 0)
                return false;

            throw std::runtime_error("peer closed in the middle of a frame");
        }

        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            throw std::runtime_error(
                std::string{"recv: "} + ::strerror(errno)
            );
        }

        received += static_cast<std::size_t>(n);
    }

    return true;
}

// 一次 send() 也不保证把全部字节写出去。
void send_all(int fd, const void* buffer, std::size_t byte_count)
{
    const auto* data = static_cast<const char*>(buffer);
    std::size_t sent = 0;

    while (sent < byte_count)
    {
        ssize_t n = ::send(
            fd,
            data + sent,
            byte_count - sent,
            MSG_NOSIGNAL
        );

        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            throw std::runtime_error(
                std::string{"send: "} + ::strerror(errno)
            );
        }

        if (n == 0)
            throw std::runtime_error("send returned 0");

        sent += static_cast<std::size_t>(n);
    }
}

void close_socket(int fd)
{
    if (fd >= 0)
        ::close(fd);
}

}

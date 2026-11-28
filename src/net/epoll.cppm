module;
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
export module net.epoll;
import std;

export namespace net {
// EPOLLERR / EPOLLHUP 不需要主动注册，发生时 epoll 仍会返回。
enum class EpollInterest : std::uint32_t {
    read = EPOLLIN,
    write = EPOLLOUT,
    peer_closed = EPOLLRDHUP,
};

// 允许这样写：EpollInterest::read | EpollInterest::peer_closed
constexpr EpollInterest operator|(EpollInterest lhs, EpollInterest rhs) noexcept {
    return static_cast<EpollInterest>(static_cast<std::uint32_t>(lhs) |
                                      static_cast<std::uint32_t>(rhs));
}

// epoll_wait() 返回给用户层的事件；布局与内核 epoll_event 严格一致，零二次拷贝且不暴露 <sys/epoll.h>。
struct
#ifdef __x86_64__
    [[gnu::packed]]
#endif
    EpollEvent {
    std::uint32_t events{0};
#ifndef __x86_64__
    std::uint32_t pad0_{0};
#endif
    int fd{-1};
    std::uint32_t pad_{0};

    [[nodiscard]] bool readable() const noexcept { return (events & EPOLLIN) != 0; }
    [[nodiscard]] bool writable() const noexcept { return (events & EPOLLOUT) != 0; }
    [[nodiscard]] bool peer_closed() const noexcept { return (events & EPOLLRDHUP) != 0; }
    [[nodiscard]] bool error() const noexcept { return (events & EPOLLERR) != 0; }
    [[nodiscard]] bool hangup() const noexcept { return (events & EPOLLHUP) != 0; }
};
static_assert(sizeof(EpollEvent) == sizeof(epoll_event) &&
              alignof(EpollEvent) == alignof(epoll_event) &&
              offsetof(EpollEvent, events) == offsetof(epoll_event, events) &&
              offsetof(EpollEvent, fd) == offsetof(epoll_event, data.fd));

class Epoll {
  public:
    explicit Epoll(std::size_t max_events = 256) : events_(max_events) {
        if (max_events == 0)
            throw std::invalid_argument("epoll max_events must be greater than 0");

        fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (fd_ == -1)
            throw std::runtime_error(std::string{"epoll_create1: "} + ::strerror(errno));
    }

    ~Epoll() {
        if (fd_ >= 0)
            ::close(fd_);
    }

    Epoll(const Epoll &) = delete;
    Epoll &operator=(const Epoll &) = delete;

    Epoll(Epoll &&other) noexcept
        : fd_(std::exchange(other.fd_, -1)), events_(std::move(other.events_)) {}

    Epoll &operator=(Epoll &&other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
            events_ = std::move(other.events_);
        }
        return *this;
    }

    // 开始监听一个 fd。
    void add(int fd, EpollInterest interest) { control(EPOLL_CTL_ADD, fd, interest); }

    // 修改已经注册的 fd 所关注的事件。
    void modify(int fd, EpollInterest interest) { control(EPOLL_CTL_MOD, fd, interest); }

    // 从 epoll 中删除 fd。
    void remove(int fd) {
        if (::epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr) == -1)
            throw std::runtime_error(std::string{"epoll_ctl DEL: "} + ::strerror(errno));
    }

    // timeout_ms: -1 一直等待，0 立即返回，>0 最多等待指定毫秒。
    // 返回的 span 直接指向内部缓冲区，下一次调用 wait() 后内容会被覆盖。
    [[nodiscard]] std::span<const EpollEvent> wait(int timeout_ms = -1) {
        while (true) {
            const int count = ::epoll_wait(fd_, reinterpret_cast<epoll_event *>(events_.data()),
                                           static_cast<int>(events_.size()), timeout_ms);
            if (count >= 0)
                return {events_.data(), static_cast<std::size_t>(count)};
            if (errno == EINTR)
                continue;
            throw std::runtime_error(std::string{"epoll_wait: "} + ::strerror(errno));
        }
    }

    [[nodiscard]] int native_handle() const noexcept { return fd_; }

  private:
    void control(int operation, int fd, EpollInterest interest) {
        epoll_event event{};
        event.events = static_cast<std::uint32_t>(interest);
        event.data.fd = fd;
        if (::epoll_ctl(fd_, operation, fd, &event) == -1)
            throw std::runtime_error(std::string{"epoll_ctl: "} + ::strerror(errno));
    }

    int fd_{-1};
    std::vector<EpollEvent> events_;
};

} // namespace net

module;
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
export module net.epoll;
import std;

export namespace net {
// EPOLLERR / EPOLLHUP 不需要主动注册，发生时 epoll 仍会返回。
enum class EpollInterest : std::uint32_t {
    read = EPOLLIN,
    write = EPOLLOUT,
    peer_closed = EPOLLRDHUP,
    edge_triggered = EPOLLET,
};

// 允许这样写：EpollInterest::read | EpollInterest::peer_closed
constexpr EpollInterest operator|(EpollInterest lhs, EpollInterest rhs) noexcept {
    return static_cast<EpollInterest>(static_cast<std::uint32_t>(lhs) |
                                      static_cast<std::uint32_t>(rhs));
}

// epoll_wait() 返回给用户层的事件。
// 不直接暴露 Linux 的 epoll_event，避免上层依赖 <sys/epoll.h>。
struct EpollEvent {
    int fd{-1};
    std::uint32_t events{0};

    [[nodiscard]] bool readable() const noexcept {
        return (events & EPOLLIN) != 0;
    }

    [[nodiscard]] bool writable() const noexcept {
        return (events & EPOLLOUT) != 0;
    }

    [[nodiscard]] bool peer_closed() const noexcept {
        return (events & EPOLLRDHUP) != 0;
    }

    [[nodiscard]] bool error() const noexcept {
        return (events & EPOLLERR) != 0;
    }

    [[nodiscard]] bool hangup() const noexcept {
        return (events & EPOLLHUP) != 0;
    }
};

class Epoll {
  public:
    explicit Epoll(std::size_t max_events = 256) : kernel_events_(max_events) {
        if (max_events == 0) {
            throw std::invalid_argument("epoll max_events must be greater than 0");
        }

        fd_ = epoll_create1(EPOLL_CLOEXEC);

        if (fd_ == -1) {
            throw std::runtime_error(std::string{"epoll_create1: "} + ::strerror(errno));
        }

        // wait() 会复用这块内存，不在每轮 event loop 中重新分配。
        ready_events_.reserve(max_events);
    }

    ~Epoll() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    Epoll(const Epoll &) = delete;
    Epoll &operator=(const Epoll &) = delete;

    Epoll(Epoll &&other) noexcept
        : fd_(other.fd_), kernel_events_(std::move(other.kernel_events_)),
          ready_events_(std::move(other.ready_events_)) {
        other.fd_ = -1;
    }

    Epoll &operator=(Epoll &&other) noexcept {
        if (this == &other) {
            return *this;
        }

        if (fd_ >= 0) {
            ::close(fd_);
        }

        fd_ = other.fd_;
        kernel_events_ = std::move(other.kernel_events_);
        ready_events_ = std::move(other.ready_events_);

        other.fd_ = -1;

        return *this;
    }

    // 开始监听一个 fd。
    void add(int fd, EpollInterest interest) {
        control(EPOLL_CTL_ADD, fd, interest);
    }

    // 修改已经注册的 fd 所关注的事件。
    void modify(int fd, EpollInterest interest) {
        control(EPOLL_CTL_MOD, fd, interest);
    }

    // 从 epoll 中删除 fd。
    void remove(int fd) {
        if (epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr) == -1) {
            throw std::runtime_error(std::string{"epoll_ctl DEL: "} + ::strerror(errno));
        }
    }

    // timeout_ms:
    //   -1  一直等待
    //    0  立即返回
    //   >0  最多等待指定毫秒
    // 返回的 span 指向 Epoll 内部内存，
    // 下一次调用 wait() 后内容会被覆盖。
    [[nodiscard]]
    std::span<const EpollEvent> wait(int timeout_ms = -1) {
        int count;

        while (true) {
            count = epoll_wait(fd_, kernel_events_.data(), static_cast<int>(kernel_events_.size()),
                               timeout_ms);

            if (count >= 0) {
                break;
            }

            // 被信号中断不算真正错误，重新等待。
            if (errno == EINTR) {
                continue;
            }

            throw std::runtime_error(std::string{"epoll_wait: "} + ::strerror(errno));
        }

        ready_events_.clear();

        for (int i = 0; i < count; ++i) {
            ready_events_.push_back(EpollEvent{
                .fd = kernel_events_[i].data.fd,
                .events = kernel_events_[i].events,
            });
        }

        return ready_events_;
    }

    [[nodiscard]]
    int native_handle() const noexcept {
        return fd_;
    }

  private:
    void control(int operation, int fd, EpollInterest interest) {
        epoll_event event{};

        event.events = static_cast<std::uint32_t>(interest);
        event.data.fd = fd;

        if (epoll_ctl(fd_, operation, fd, &event) == -1) {
            throw std::runtime_error(std::string{"epoll_ctl: "} + ::strerror(errno));
        }
    }

    int fd_{-1};

    // Linux epoll_wait() 直接写入这里。
    std::vector<epoll_event> kernel_events_;

    // 转换成不暴露 Linux 头文件的项目级事件。
    std::vector<EpollEvent> ready_events_;
};

} // namespace net
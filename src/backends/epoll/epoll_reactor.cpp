#include "backends/epoll/epoll_reactor.hpp"

#include <cerrno>
#include <system_error>

namespace fluxnet::detail {

EpollReactor::EpollReactor() {
    int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
    fd_ = FileDescriptor(fd);
}

void EpollReactor::add(int fd, epoll_event event) {
    if (::epoll_ctl(fd_.get(), EPOLL_CTL_ADD, fd, &event) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl ADD");
    }
}

void EpollReactor::mod(int fd, epoll_event event) {
    if (::epoll_ctl(fd_.get(), EPOLL_CTL_MOD, fd, &event) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl MOD");
    }
}

void EpollReactor::del(int fd) noexcept {
    if (fd < 0) {
        return;
    }
    ::epoll_ctl(fd_.get(), EPOLL_CTL_DEL, fd, nullptr);
}

int EpollReactor::wait(epoll_event* out, int max_events, int timeout_ms) {
    for (;;) {
        int n = ::epoll_wait(fd_.get(), out, max_events, timeout_ms);
        if (n < 0 && errno == EINTR) {
            return 0;
        }
        return n;
    }
}

} // namespace fluxnet::detail

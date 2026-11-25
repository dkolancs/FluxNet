#include "concurrency/wake.hpp"

#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace fluxnet::detail {

EventFd::EventFd(bool nonblock) : nonblock_(nonblock) {
    int flags = EFD_CLOEXEC;
    if (nonblock) {
        flags |= EFD_NONBLOCK;
    }
    int fd = ::eventfd(0, flags);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
    fd_ = FileDescriptor(fd);
}

void EventFd::notify() noexcept {
    const std::uint64_t one = 1;
    for (;;) {
        const ssize_t n = ::write(fd_.get(), &one, sizeof(one));
        if (n == static_cast<ssize_t>(sizeof(one))) {
            return;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // EAGAIN: the counter is already saturated, so the fd is readable.
        return;
    }
}

void EventFd::drain() noexcept {
    std::uint64_t value = 0;
    for (;;) {
        const ssize_t n = ::read(fd_.get(), &value, sizeof(value));
        if (n == static_cast<ssize_t>(sizeof(value))) {
            if (!nonblock_) {
                return;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

TimerFd::TimerFd(bool nonblock) {
    int flags = TFD_CLOEXEC;
    if (nonblock) {
        flags |= TFD_NONBLOCK;
    }
    int fd = ::timerfd_create(CLOCK_MONOTONIC, flags);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "timerfd_create");
    }
    fd_ = FileDescriptor(fd);
}

void TimerFd::arm_relative(std::chrono::nanoseconds delay) noexcept {
    if (delay < std::chrono::nanoseconds::zero()) {
        delay = std::chrono::nanoseconds::zero();
    }
    itimerspec spec{};
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(delay);
    spec.it_value.tv_sec = static_cast<time_t>(sec.count());
    spec.it_value.tv_nsec = static_cast<long>((delay - sec).count());
    if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
        spec.it_value.tv_nsec = 1;
    }
    ::timerfd_settime(fd_.get(), 0, &spec, nullptr);
}

void TimerFd::disarm() noexcept {
    itimerspec spec{};
    ::timerfd_settime(fd_.get(), 0, &spec, nullptr);
}

void TimerFd::drain() noexcept {
    std::uint64_t expirations = 0;
    for (;;) {
        const ssize_t n = ::read(fd_.get(), &expirations, sizeof(expirations));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

} // namespace fluxnet::detail

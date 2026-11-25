#pragma once

#include "net/file_descriptor.hpp"

#include <sys/epoll.h>

namespace fluxnet::detail {

class EpollReactor {
  public:
    EpollReactor();
    int fd() const noexcept { return fd_.get(); }

    void add(int fd, epoll_event event);
    void mod(int fd, epoll_event event);
    void del(int fd) noexcept;
    // Returns the number of events, or -1 on a non-EINTR error (errno set).
    // EINTR is reported as 0 so the caller can loop.
    int wait(epoll_event* out, int max_events, int timeout_ms);

  private:
    FileDescriptor fd_;
};

} // namespace fluxnet::detail

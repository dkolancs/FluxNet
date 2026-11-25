#pragma once

#include "net/file_descriptor.hpp"

#include <chrono>
#include <cstdint>

namespace fluxnet::detail {

class EventFd {
  public:
    explicit EventFd(bool nonblock);
    int fd() const noexcept { return fd_.get(); }
    void notify() noexcept;
    void drain() noexcept;

  private:
    FileDescriptor fd_;
    bool nonblock_ = true;
};

class TimerFd {
  public:
    explicit TimerFd(bool nonblock);
    int fd() const noexcept { return fd_.get(); }
    void arm_relative(std::chrono::nanoseconds delay) noexcept;
    void disarm() noexcept;
    void drain() noexcept;

  private:
    FileDescriptor fd_;
};

} // namespace fluxnet::detail

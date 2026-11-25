#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace fluxnet::test {

class Gate {
  public:
    void notify() {
        std::lock_guard lock(mu_);
        ++n_;
        cv_.notify_all();
    }

    bool wait_for(int target, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, timeout, [&] { return n_ >= target; });
    }

    int count() const {
        std::lock_guard lock(mu_);
        return n_;
    }

  private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    int n_ = 0;
};

} // namespace fluxnet::test

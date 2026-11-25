#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace fluxnet::detail {

// Bounded multi-producer multi-consumer queue. The library uses it as MPSC:
// any thread may try_push, and the owning worker is the only consumer.
template <typename T> class MutexQueue {
  public:
    explicit MutexQueue(std::size_t capacity) : capacity_(capacity < 1 ? 1 : capacity) {}

    MutexQueue(const MutexQueue&) = delete;
    MutexQueue& operator=(const MutexQueue&) = delete;

    std::size_t capacity() const noexcept { return capacity_; }

    bool try_push(T value) {
        std::lock_guard lock(mu_);
        if (q_.size() >= capacity_) {
            return false;
        }
        q_.push_back(std::move(value));
        return true;
    }

    bool try_pop(T& out) {
        std::lock_guard lock(mu_);
        if (q_.empty()) {
            return false;
        }
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    template <typename Fn> std::size_t drain(Fn&& fn, std::size_t limit) {
        std::deque<T> local;
        {
            std::lock_guard lock(mu_);
            const std::size_t n = std::min(limit, q_.size());
            for (std::size_t i = 0; i < n; ++i) {
                local.push_back(std::move(q_.front()));
                q_.pop_front();
            }
        }
        std::size_t ran = 0;
        for (auto& item : local) {
            fn(std::move(item));
            ++ran;
        }
        return ran;
    }

    std::size_t size() const {
        std::lock_guard lock(mu_);
        return q_.size();
    }

    bool empty() const {
        std::lock_guard lock(mu_);
        return q_.empty();
    }

  private:
    std::size_t capacity_;
    mutable std::mutex mu_;
    std::deque<T> q_;
};

} // namespace fluxnet::detail

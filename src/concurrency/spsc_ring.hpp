#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace fluxnet::detail {

// Bounded single-producer single-consumer ring.
//
// Producer may call try_push only. Consumer may call try_pop only.
// Do not use this for multiple producers (cross-thread sends use MutexQueue).
//
// Indices are monotonic and wrap with unsigned arithmetic. Empty is head == tail.
// Full is tail - head == capacity. Every slot is usable.
//
// Memory order: the producer publishes a slot with release on tail. The consumer
// acquires tail before reading the slot. The consumer publishes head with release
// so the producer observes freed space. Relaxed loads of a thread's own index are
// enough because that thread is the only writer of that index.
template <typename T> class SpscRing {
  public:
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "SpscRing requires lock-free size_t atomics; refusing a lock-inlined atomic");

    explicit SpscRing(std::size_t capacity)
        : capacity_(round_up_pow2(capacity < 2 ? 2 : capacity)), mask_(capacity_ - 1),
          slots_(std::make_unique<T[]>(capacity_)) {}

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    std::size_t capacity() const noexcept { return capacity_; }

    bool try_push(T value) {
        const std::size_t tail = tail_.value.load(std::memory_order_relaxed);
        const std::size_t head = head_.value.load(std::memory_order_acquire);
        if (tail - head == capacity_) {
            return false;
        }
        slots_[tail & mask_] = std::move(value);
        tail_.value.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const std::size_t head = head_.value.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.value.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = std::move(slots_[head & mask_]);
        head_.value.store(head + 1, std::memory_order_release);
        return true;
    }

    // Approximate. Safe for the consumer; racy if the producer calls it.
    bool empty() const noexcept {
        return head_.value.load(std::memory_order_acquire) ==
               tail_.value.load(std::memory_order_acquire);
    }

  private:
    static std::size_t round_up_pow2(std::size_t n) {
        std::size_t p = 2;
        while (p < n) {
            p <<= 1;
        }
        return p;
    }

    struct alignas(64) PaddedAtomic {
        std::atomic<std::size_t> value{0};
    };

    std::size_t capacity_;
    std::size_t mask_;
    PaddedAtomic head_{};
    PaddedAtomic tail_{};
    std::unique_ptr<T[]> slots_;
};

} // namespace fluxnet::detail

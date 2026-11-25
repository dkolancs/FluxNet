#include "concurrency/spsc_ring.hpp"
#include "concurrency/wake.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

TEST(SpscThreads, ChecksumAndWrap) {
    fluxnet::detail::SpscRing<std::uint64_t> ring(1024);
    constexpr int kN = 200000;
    std::atomic<bool> failed{false};
    std::thread producer([&] {
        for (int i = 1; i <= kN; ++i) {
            while (!ring.try_push(static_cast<std::uint64_t>(i))) {
                std::this_thread::yield();
            }
        }
    });
    std::uint64_t sum = 0;
    std::thread consumer([&] {
        int got = 0;
        while (got < kN) {
            std::uint64_t value = 0;
            if (ring.try_pop(value)) {
                sum += value;
                ++got;
            } else {
                std::this_thread::yield();
            }
        }
    });
    producer.join();
    consumer.join();
    const std::uint64_t expect =
        static_cast<std::uint64_t>(kN) * (static_cast<std::uint64_t>(kN) + 1) / 2;
    EXPECT_EQ(sum, expect);
    EXPECT_FALSE(failed.load());
}

TEST(SpscThreads, EventfdWakeDoesNotLoseItems) {
    fluxnet::detail::SpscRing<int> ring(128);
    fluxnet::detail::EventFd wake(true);
    constexpr int kN = 50000;
    std::atomic<int> got{0};
    std::thread consumer([&] {
        int seen = 0;
        while (seen < kN) {
            int value = 0;
            bool any = false;
            while (ring.try_pop(value)) {
                any = true;
                ++seen;
            }
            if (!any) {
                wake.drain();
                // Recheck after the drain so a push that landed just before the read is not lost.
                while (ring.try_pop(value)) {
                    ++seen;
                }
            }
        }
        got.store(seen);
    });
    for (int i = 0; i < kN; ++i) {
        while (!ring.try_push(i)) {
            std::this_thread::yield();
        }
        wake.notify();
    }
    consumer.join();
    EXPECT_EQ(got.load(), kN);
}

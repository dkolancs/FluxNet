#include "concurrency/mutex_queue.hpp"
#include "concurrency/spsc_ring.hpp"
#include "concurrency/wake.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

TEST(SpscRing, FullEmptyAndWrap) {
    fluxnet::detail::SpscRing<int> ring(4);
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.capacity(), 4u);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring.try_push(i));
    }
    EXPECT_FALSE(ring.try_push(9));
    int value = -1;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring.try_pop(value));
        EXPECT_EQ(value, i);
    }
    EXPECT_FALSE(ring.try_pop(value));
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(ring.try_push(i));
        ASSERT_TRUE(ring.try_pop(value));
        EXPECT_EQ(value, i);
    }
}

TEST(MutexQueue, FullAndDrain) {
    fluxnet::detail::MutexQueue<int> queue(2);
    ASSERT_TRUE(queue.try_push(1));
    ASSERT_TRUE(queue.try_push(2));
    EXPECT_FALSE(queue.try_push(3));
    int sum = 0;
    EXPECT_EQ(queue.drain([&](int v) { sum += v; }, 10), 2u);
    EXPECT_EQ(sum, 3);
    EXPECT_TRUE(queue.empty());
}

TEST(EventFd, NotifyAndDrain) {
    fluxnet::detail::EventFd fd(true);
    fd.notify();
    fd.notify();
    fd.drain();
    // Draining an empty nonblocking eventfd must not block.
    fd.drain();
}

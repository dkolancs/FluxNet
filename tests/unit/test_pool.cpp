#include "memory/buffer_pool.hpp"

#include <gtest/gtest.h>

TEST(BufferPool, ReuseAndCap) {
    fluxnet::detail::BufferPool pool(2, true);
    {
        auto a = pool.acquire(100);
        auto b = pool.acquire(100);
        ASSERT_TRUE(a);
        ASSERT_TRUE(b);
        EXPECT_GE(a.capacity(), 1024u);
    }
    auto stats = pool.stats();
    EXPECT_EQ(stats.acquires, 2u);
    EXPECT_EQ(stats.bytes_in_use, 0u);
    auto c = pool.acquire(100);
    stats = pool.stats();
    EXPECT_GE(stats.reuses, 1u);
    auto d = pool.acquire(80 * 1024);
    EXPECT_GE(d.capacity(), 80u * 1024u);
}

TEST(BufferPool, DisabledDoesNotReuse) {
    fluxnet::detail::BufferPool pool(8, false);
    {
        auto a = pool.acquire(32);
        EXPECT_TRUE(a);
    }
    auto b = pool.acquire(32);
    EXPECT_EQ(pool.stats().reuses, 0u);
    EXPECT_GE(pool.stats().allocs, 2u);
}

TEST(BufferPool, EncodeRespectsMax) {
    fluxnet::detail::BufferPool pool(4, true);
    std::vector<std::byte> big(100, std::byte{1});
    auto encoded = fluxnet::detail::encode_frame(pool, fluxnet::MessageType::Data, 0, big, 16);
    EXPECT_FALSE(encoded);
    encoded = fluxnet::detail::encode_frame(pool, fluxnet::MessageType::Data, 3, big, 1000);
    ASSERT_TRUE(encoded);
    EXPECT_EQ(encoded.size(), 108u);
}

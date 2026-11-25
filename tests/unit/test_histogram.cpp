#include "observability/log_histogram.hpp"

#include <gtest/gtest.h>

TEST(Histogram, PercentilesAreOrdered) {
    fluxnet::detail::LogHistogram hist;
    for (int i = 1; i <= 1000; ++i) {
        hist.record(static_cast<std::uint64_t>(i) * 1000);
    }
    EXPECT_EQ(hist.count(), 1000u);
    const auto p50 = hist.percentile(0.50);
    const auto p95 = hist.percentile(0.95);
    const auto p99 = hist.percentile(0.99);
    EXPECT_LE(p50, p95);
    EXPECT_LE(p95, p99);
    EXPECT_GE(p50, 400000u);
    EXPECT_LE(p99, 1100000u);
}

TEST(Histogram, EmptyIsZero) {
    fluxnet::detail::LogHistogram hist;
    EXPECT_EQ(hist.percentile(0.99), 0u);
    EXPECT_EQ(hist.count(), 0u);
}

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

TEST(Histogram, MergeIsNotMaxOfPercentiles) {
    fluxnet::detail::LogHistogram low;
    fluxnet::detail::LogHistogram high;
    for (int i = 0; i < 100; ++i) {
        low.record(1000);
        high.record(100000);
    }
    const auto max_of_p50 = std::max(low.percentile(0.50), high.percentile(0.50));
    fluxnet::detail::LogHistogram merged = low;
    merged.merge(high);
    EXPECT_EQ(merged.count(), 200u);
    EXPECT_EQ(merged.min(), 1000u);
    EXPECT_EQ(merged.max(), 100000u);
    // Half the merged samples are 1000, so p50 stays in that bucket.
    // The max of the per-thread p50s is the high distribution's p50.
    EXPECT_LT(merged.percentile(0.50), max_of_p50);
    EXPECT_GT(max_of_p50, 50000u);
}

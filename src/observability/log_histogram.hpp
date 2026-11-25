#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace fluxnet::detail {

// Log2 histogram with 64 linear sub-buckets inside each power of two.
// percentile() returns the upper edge of the bucket that crosses p, so it
// never under-reports. Maximum relative width of a bucket is 1/64 of its range.
// Unit is whatever the caller records (the load generator uses nanoseconds).
class LogHistogram {
  public:
    static constexpr int kSub = 64;
    static constexpr int kPowers = 42; // up to 2^42 ns ~ 73 minutes
    static constexpr int kBuckets = kSub * kPowers;

    void record(std::uint64_t value) noexcept {
        if (value == 0) {
            value = 1;
        }
        if (value > max_) {
            max_ = value;
        }
        if (value < min_) {
            min_ = value;
        }
        sum_ += value;
        ++count_;
        int power = 0;
        std::uint64_t v = value;
        while (v > 1) {
            v >>= 1;
            ++power;
        }
        if (power >= kPowers) {
            power = kPowers - 1;
        }
        const std::uint64_t base = 1ull << power;
        const std::uint64_t mantissa = value - base;
        int sub = static_cast<int>((mantissa * static_cast<std::uint64_t>(kSub)) >> power);
        if (sub >= kSub) {
            sub = kSub - 1;
        }
        if (sub < 0) {
            sub = 0;
        }
        ++buckets_[power * kSub + sub];
    }

    // Adds bucket counts. Percentiles of the result are the merged distribution,
    // not the max of the inputs' percentiles.
    void merge(const LogHistogram& other) noexcept {
        if (other.count_ == 0) {
            return;
        }
        for (int i = 0; i < kBuckets; ++i) {
            buckets_[static_cast<std::size_t>(i)] += other.buckets_[static_cast<std::size_t>(i)];
        }
        count_ += other.count_;
        sum_ += other.sum_;
        if (other.min_ < min_) {
            min_ = other.min_;
        }
        if (other.max_ > max_) {
            max_ = other.max_;
        }
    }

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }
    std::uint64_t max() const noexcept { return max_; }
    double mean() const noexcept {
        return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
    }

    std::uint64_t percentile(double p) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        if (p <= 0) {
            return min_;
        }
        if (p >= 1) {
            return max_;
        }
        const double target = p * static_cast<double>(count_);
        double acc = 0;
        for (int power = 0; power < kPowers; ++power) {
            const std::uint64_t base = 1ull << power;
            for (int sub = 0; sub < kSub; ++sub) {
                acc += static_cast<double>(buckets_[power * kSub + sub]);
                if (acc >= target) {
                    const std::uint64_t upper =
                        base + ((static_cast<std::uint64_t>(sub + 1) * base) >> 6);
                    return std::max(upper, static_cast<std::uint64_t>(1));
                }
            }
        }
        return max_;
    }

  private:
    std::array<std::uint64_t, kBuckets> buckets_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_ = 0;
};

} // namespace fluxnet::detail

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace nanomatch::bench {

/// Fixed-size latency histogram in TSC ticks. Recording never allocates and costs one array
/// increment.
///
/// Values below kExactLimit each have their own bucket, so percentiles in the range that matters
/// are exact. Larger values share 64 buckets per power of two, which keeps relative error under
/// about 1.6% with bounded memory. Percentiles of those larger values report the top of the bucket,
/// so rounding can only overstate latency and never understate it.
class LatencyHistogram {
  public:
    static constexpr std::uint64_t kExactLimit = 4096;

    void record(std::uint64_t ticks) noexcept {
        ++counts_[bucket_of(ticks)];
        ++count_;
        sum_ += ticks;
        min_ = std::min(min_, ticks);
        max_ = std::max(max_, ticks);
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }

    [[nodiscard]] double mean() const noexcept {
        return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
    }

    /// Nearest-rank percentile: the smallest recorded bucket value such that at least `fraction` of
    /// the samples are at or below it. `fraction` is in (0, 1]. Returns 0 if empty.
    [[nodiscard]] std::uint64_t percentile(double fraction) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        const double exact_rank = fraction * static_cast<double>(count_);
        auto rank = static_cast<std::uint64_t>(exact_rank);
        if (static_cast<double>(rank) < exact_rank) {
            ++rank;
        }
        rank = std::clamp<std::uint64_t>(rank, 1, count_);

        std::uint64_t seen = 0;
        for (std::size_t bucket = 0; bucket < counts_.size(); ++bucket) {
            seen += counts_[bucket];
            if (seen >= rank) {
                return std::min(upper_bound_of(bucket), max_);
            }
        }
        return max_;
    }

  private:
    static constexpr unsigned kExactBits = 12;
    static constexpr unsigned kSubBucketBits = 6;
    static constexpr std::size_t kSubBuckets = std::size_t{1} << kSubBucketBits;
    static constexpr unsigned kMaxExponent = 63;
    static constexpr std::size_t kBucketCount =
        kExactLimit + ((kMaxExponent - kExactBits + 1) * kSubBuckets);

    [[nodiscard]] static constexpr std::size_t bucket_of(std::uint64_t ticks) noexcept {
        if (ticks < kExactLimit) {
            return static_cast<std::size_t>(ticks);
        }
        const auto exponent = static_cast<unsigned>(std::bit_width(ticks) - 1);
        const auto sub_bucket =
            static_cast<std::size_t>((ticks >> (exponent - kSubBucketBits)) & (kSubBuckets - 1));
        return kExactLimit + ((exponent - kExactBits) * kSubBuckets) + sub_bucket;
    }

    [[nodiscard]] static constexpr std::uint64_t upper_bound_of(std::size_t bucket) noexcept {
        if (bucket < kExactLimit) {
            return bucket;
        }
        const std::size_t offset = bucket - kExactLimit;
        const unsigned exponent = kExactBits + static_cast<unsigned>(offset / kSubBuckets);
        const std::uint64_t sub_bucket = offset % kSubBuckets;
        const unsigned shift = exponent - kSubBucketBits;
        const std::uint64_t top = ((kSubBuckets + sub_bucket + 1) << shift) - 1;
        // The final bucket's top is one past what 64 bits can hold, and wraps to exactly this
        // value.
        return top == 0 ? std::numeric_limits<std::uint64_t>::max() : top;
    }

    std::array<std::uint64_t, kBucketCount> counts_{};
    std::uint64_t count_{0};
    std::uint64_t sum_{0};
    std::uint64_t min_{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t max_{0};
};

}  // namespace nanomatch::bench

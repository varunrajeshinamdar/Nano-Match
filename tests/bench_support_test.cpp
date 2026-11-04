#include <sched.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "histogram.hpp"
#include "json_writer.hpp"
#include "perf_counters.hpp"
#include "tsc.hpp"

namespace nanomatch::bench {
namespace {

// Heap-allocated because the histogram is too large to want on a test's stack.
[[nodiscard]] std::unique_ptr<LatencyHistogram> make_histogram() {
    return std::make_unique<LatencyHistogram>();
}

TEST(LatencyHistogramTest, EmptyHistogramReportsZero) {
    const auto histogram = make_histogram();
    EXPECT_EQ(histogram->count(), 0U);
    EXPECT_EQ(histogram->min(), 0U);
    EXPECT_EQ(histogram->max(), 0U);
    EXPECT_EQ(histogram->mean(), 0.0);
    EXPECT_EQ(histogram->percentile(0.5), 0U);
}

TEST(LatencyHistogramTest, SingleSample) {
    const auto histogram = make_histogram();
    histogram->record(42);
    EXPECT_EQ(histogram->count(), 1U);
    EXPECT_EQ(histogram->min(), 42U);
    EXPECT_EQ(histogram->max(), 42U);
    EXPECT_EQ(histogram->mean(), 42.0);
    EXPECT_EQ(histogram->percentile(0.5), 42U);
    EXPECT_EQ(histogram->percentile(0.99999), 42U);
}

TEST(LatencyHistogramTest, PercentilesUseNearestRank) {
    const auto histogram = make_histogram();
    for (std::uint64_t value = 1; value <= 1000; ++value) {
        histogram->record(value);
    }
    EXPECT_EQ(histogram->percentile(0.50), 500U);
    EXPECT_EQ(histogram->percentile(0.90), 900U);
    EXPECT_EQ(histogram->percentile(0.999), 999U);
    EXPECT_EQ(histogram->percentile(1.0), 1000U);
    EXPECT_EQ(histogram->percentile(0.0001), 1U);
    EXPECT_DOUBLE_EQ(histogram->mean(), 500.5);
}

TEST(LatencyHistogramTest, RareTailIsVisibleAtTheRightPercentile) {
    const auto histogram = make_histogram();
    for (int i = 0; i < 999; ++i) {
        histogram->record(10);
    }
    histogram->record(3000);
    EXPECT_EQ(histogram->percentile(0.999), 10U);
    EXPECT_EQ(histogram->percentile(0.9991), 3000U);
}

TEST(LatencyHistogramTest, ExactRangeHasSingleTickResolution) {
    const auto histogram = make_histogram();
    histogram->record(LatencyHistogram::kExactLimit - 1);
    EXPECT_EQ(histogram->percentile(1.0), LatencyHistogram::kExactLimit - 1);
}

TEST(LatencyHistogramTest, LargeValuesAreNeverUnderstated) {
    for (const std::uint64_t value :
         {std::uint64_t{4096}, std::uint64_t{4097}, std::uint64_t{5000}, std::uint64_t{123'456},
          std::uint64_t{987'654'321}, std::uint64_t{1} << 40U, (std::uint64_t{1} << 40U) + 1}) {
        const auto histogram = make_histogram();
        histogram->record(value);
        histogram->record(value * 2);
        const std::uint64_t reported = histogram->percentile(0.5);
        EXPECT_GE(reported, value);
        EXPECT_LE(static_cast<double>(reported), static_cast<double>(value) * 1.016) << value;
    }
}

TEST(LatencyHistogramTest, PercentileNeverExceedsMaximum) {
    const auto histogram = make_histogram();
    histogram->record(5000);
    EXPECT_EQ(histogram->percentile(1.0), 5000U);
}

TEST(LatencyHistogramTest, HandlesLargestRepresentableValue) {
    const auto histogram = make_histogram();
    const std::uint64_t largest = std::numeric_limits<std::uint64_t>::max();
    histogram->record(largest);
    EXPECT_EQ(histogram->max(), largest);
    EXPECT_EQ(histogram->percentile(1.0), largest);
}

TEST(TscTest, CounterAdvances) {
    const std::uint64_t first = serialized_tsc();
    const std::uint64_t second = serialized_tsc();
    EXPECT_GT(second, first);
}

TEST(TscTest, CalibrationGivesPlausibleFrequency) {
    const double hz = calibrate_tsc_hz(std::chrono::milliseconds{20});
    EXPECT_GT(hz, 1e8);
    EXPECT_LT(hz, 1e10);
}

/// Moves the calling thread to a P-core for its lifetime, if this is a hybrid CPU. The counters are
/// opened on the P-core PMU, so they don't count at all while the thread runs on an E-core.
class PinnedToPerformanceCore {
  public:
    PinnedToPerformanceCore() {
        sched_getaffinity(0, sizeof(original_), &original_);
        std::ifstream file{"/sys/devices/cpu_core/cpus"};
        int first_cpu = 0;
        if (file >> first_cpu) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(static_cast<std::size_t>(first_cpu), &set);
            sched_setaffinity(0, sizeof(set), &set);
        }
    }

    PinnedToPerformanceCore(const PinnedToPerformanceCore&) = delete;
    PinnedToPerformanceCore& operator=(const PinnedToPerformanceCore&) = delete;
    PinnedToPerformanceCore(PinnedToPerformanceCore&&) = delete;
    PinnedToPerformanceCore& operator=(PinnedToPerformanceCore&&) = delete;
    ~PinnedToPerformanceCore() { sched_setaffinity(0, sizeof(original_), &original_); }

  private:
    cpu_set_t original_{};
};

TEST(PerfGroupTest, CountsWorkWhenAvailable) {
    const PinnedToPerformanceCore pinned;
    PerfGroup group;
    if (!group.available()) {
        GTEST_SKIP() << "hardware counters unavailable: " << group.error();
    }
    group.start();
    volatile std::uint64_t sink = 0;
    for (std::uint64_t i = 0; i < 100'000; ++i) {
        sink = sink + i;
    }
    group.stop();
    const std::optional<PerfSample> sample = group.read_sample();
    if (!sample) {
        FAIL() << "counters opened but could not be read";
    } else if (sample->running_fraction == 0.0) {
        GTEST_SKIP() << "counters were never scheduled, which virtual machines can cause";
    } else {
        EXPECT_GT(sample->instructions, 100'000U);
        EXPECT_GT(sample->cycles, 0U);
        EXPECT_GT(sample->l1d_loads, 0U);
    }
}

TEST(JsonObjectTest, WritesEachKindOfValue) {
    tools::JsonObject inner;
    inner.add("x", 1);
    tools::JsonObject json;
    json.add("text", "a\"b").add("flag", true).add("number", 2.5).add("nested", inner);
    EXPECT_EQ(json.str(), R"({"text": "a\"b", "flag": true, "number": 2.5, "nested": {"x": 1}})");
}

}  // namespace
}  // namespace nanomatch::bench

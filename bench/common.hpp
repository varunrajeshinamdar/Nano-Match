#pragma once

#include <sched.h>
#include <sys/resource.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "histogram.hpp"
#include "json_writer.hpp"
#include "nanomatch/fnv1a.hpp"
#include "nanomatch/protocol.hpp"
#include "tsc.hpp"

/// Parts shared by the benchmark programs, so that they all measure and report in the same way.

namespace nanomatch::bench {

inline constexpr std::size_t kTimerSamples = 1'000'000;

inline constexpr std::array<std::pair<std::string_view, double>, 6> kPercentiles{{
    {"p50", 0.50},
    {"p90", 0.90},
    {"p99", 0.99},
    {"p99_9", 0.999},
    {"p99_99", 0.9999},
    {"p99_999", 0.99999},
}};

/// Stores events in a fixed ring, standing in for a downstream publisher.
///
/// Events have to be really written so that the compiler can't optimize their construction away,
/// and so that their cost is included. Writing to a preallocated buffer pays that cost without
/// measuring file or network I/O. The contents are hashed after the run, which also forces them to
/// be kept.
class RingSink {
  public:
    void on_event(const Event& event) noexcept {
        events_[count_ % kCapacity] = event;
        ++count_;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

    /// Precondition: at least one event has been received.
    [[nodiscard]] const Event& last() const noexcept { return events_[(count_ - 1) % kCapacity]; }

    [[nodiscard]] std::uint64_t contents_hash() const noexcept {
        Fnv1a64 hash;
        hash.update(std::as_bytes(std::span{events_}));
        return hash.value();
    }

  private:
    static constexpr std::size_t kCapacity = 4096;

    std::array<Event, kCapacity> events_{};
    std::uint64_t count_{0};
};

inline void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        throw std::runtime_error{std::format("cannot pin to CPU {}", cpu)};
    }
}

[[nodiscard]] inline rusage current_usage() noexcept {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage;
}

[[nodiscard]] inline tools::JsonObject usage_delta(const rusage& before, const rusage& after) {
    tools::JsonObject json;
    json.add("minor_faults", after.ru_minflt - before.ru_minflt)
        .add("major_faults", after.ru_majflt - before.ru_majflt)
        .add("voluntary_switches", after.ru_nvcsw - before.ru_nvcsw)
        .add("involuntary_switches", after.ru_nivcsw - before.ru_nivcsw);
    return json;
}

[[nodiscard]] inline tools::JsonObject summarize(const LatencyHistogram& histogram,
                                                 double ticks_per_ns) {
    const auto to_ns = [ticks_per_ns](double ticks) { return ticks / ticks_per_ns; };
    tools::JsonObject json;
    json.add("samples", histogram.count())
        .add("mean_ns", to_ns(histogram.mean()))
        .add("min_ns", to_ns(static_cast<double>(histogram.min())))
        .add("max_ns", to_ns(static_cast<double>(histogram.max())));
    for (const auto& [name, fraction] : kPercentiles) {
        json.add(std::format("{}_ns", name),
                 to_ns(static_cast<double>(histogram.percentile(fraction))));
    }
    return json;
}

/// The cost and spread of the timer itself, measured by timing nothing.
[[nodiscard]] inline tools::JsonObject measure_timer(double ticks_per_ns) {
    const auto histogram = std::make_unique<LatencyHistogram>();
    for (std::size_t i = 0; i < kTimerSamples; ++i) {
        const std::uint64_t start = serialized_tsc();
        const std::uint64_t end = serialized_tsc();
        histogram->record(end - start);
    }
    return summarize(*histogram, ticks_per_ns);
}

}  // namespace nanomatch::bench

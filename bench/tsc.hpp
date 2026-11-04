#pragma once

#include <x86intrin.h>

#include <chrono>
#include <cstdint>
#include <ctime>

namespace nanomatch::bench {

/// Reads the timestamp counter with a fence on each side.
///
/// `rdtsc` isn't a serializing instruction, so the CPU may execute it out of order with the code
/// being measured. The first fence waits for earlier instructions to finish, and the second stops
/// later instructions from starting before the counter has been read. The fences have a cost, which
/// is why every timed result is reported along with the measured overhead of this function.
[[nodiscard]] inline std::uint64_t serialized_tsc() noexcept {
    _mm_lfence();
    const std::uint64_t ticks = __rdtsc();
    _mm_lfence();
    return ticks;
}

[[nodiscard]] inline std::uint64_t monotonic_raw_ns() noexcept {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return (static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL) +
           static_cast<std::uint64_t>(now.tv_nsec);
}

/// Measures the TSC frequency against the kernel's raw monotonic clock. The frequency is measured
/// because the rate implied by a CPU's model name isn't guaranteed to be its actual TSC rate.
[[nodiscard]] inline double calibrate_tsc_hz(std::chrono::milliseconds window) noexcept {
    const std::uint64_t window_ns = static_cast<std::uint64_t>(window.count()) * 1'000'000ULL;
    const std::uint64_t start_ns = monotonic_raw_ns();
    const std::uint64_t start_ticks = serialized_tsc();
    std::uint64_t end_ns = start_ns;
    while (end_ns - start_ns < window_ns) {
        end_ns = monotonic_raw_ns();
    }
    const std::uint64_t end_ticks = serialized_tsc();
    return static_cast<double>(end_ticks - start_ticks) * 1e9 /
           static_cast<double>(end_ns - start_ns);
}

}  // namespace nanomatch::bench

#pragma once

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace nanomatch::bench {

/// Values read from a PerfGroup between start() and stop().
struct PerfSample {
    std::uint64_t cycles{};
    std::uint64_t instructions{};
    std::uint64_t l1d_loads{};
    std::uint64_t l1d_load_misses{};
    std::uint64_t llc_load_misses{};
    std::uint64_t branch_misses{};
    /// Fraction of the enabled time the counters were actually on the PMU. Anything below 1 means
    /// the kernel multiplexed them, so the counts are estimates and the run shouldn't be used.
    double running_fraction{};
};

/// Encodes a cache read event in the format perf_event_open expects.
[[nodiscard]] constexpr std::uint64_t cache_read_event(std::uint64_t cache,
                                                       std::uint64_t result) noexcept {
    return cache | (std::uint64_t{PERF_COUNT_HW_CACHE_OP_READ} << 8U) | (result << 16U);
}

/// Hardware counters for the calling thread, opened as one group.
///
/// Reading them inside the program, rather than from `perf stat`, lets counting start and stop
/// around the measured loop, so that setup and warmup can't contaminate the result. A group is
/// scheduled onto the PMU atomically, which keeps ratios between its counters meaningful. User-mode
/// only, since the engine is what's being measured.
class PerfGroup {
  public:
    PerfGroup() {
        const std::uint64_t pmu = core_pmu_type();
        for (std::size_t i = 0; i < kEvents.size(); ++i) {
            perf_event_attr attr{};
            attr.size = sizeof(attr);
            attr.type = kEvents[i].type;
            attr.config = kEvents[i].config | (pmu << PERF_PMU_TYPE_SHIFT);
            attr.disabled = i == 0 ? 1 : 0;
            attr.exclude_kernel = 1;
            attr.exclude_hv = 1;
            attr.read_format =
                PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
            const int leader = i == 0 ? -1 : fds_[0];
            const long fd = syscall(SYS_perf_event_open, &attr, 0, -1, leader, 0UL);
            if (fd < 0) {
                error_ = std::format("perf_event_open({}): {}", kEvents[i].name,
                                     std::system_category().message(errno));
                close_all();
                return;
            }
            fds_[i] = static_cast<int>(fd);
        }
    }

    PerfGroup(const PerfGroup&) = delete;
    PerfGroup& operator=(const PerfGroup&) = delete;
    PerfGroup(PerfGroup&&) = delete;
    PerfGroup& operator=(PerfGroup&&) = delete;
    ~PerfGroup() { close_all(); }

    [[nodiscard]] bool available() const noexcept { return fds_[0] >= 0; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    void start() noexcept {
        if (available()) {
            ioctl(fds_[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
            ioctl(fds_[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
        }
    }

    void stop() noexcept {
        if (available()) {
            ioctl(fds_[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        }
    }

    [[nodiscard]] std::optional<PerfSample> read_sample() const noexcept {
        if (!available()) {
            return std::nullopt;
        }
        struct {
            std::uint64_t count;
            std::uint64_t time_enabled;
            std::uint64_t time_running;
            std::array<std::uint64_t, kEventCount> values;
        } data{};
        if (::read(fds_[0], &data, sizeof(data)) != static_cast<ssize_t>(sizeof(data))) {
            return std::nullopt;
        }
        return PerfSample{
            .cycles = data.values[0],
            .instructions = data.values[1],
            .l1d_loads = data.values[2],
            .l1d_load_misses = data.values[3],
            .llc_load_misses = data.values[4],
            .branch_misses = data.values[5],
            .running_fraction = data.time_enabled == 0 ? 0.0
                                                       : static_cast<double>(data.time_running) /
                                                             static_cast<double>(data.time_enabled),
        };
    }

  private:
    struct EventSpec {
        const char* name;
        std::uint32_t type;
        std::uint64_t config;
    };

    static constexpr std::size_t kEventCount = 6;
    static constexpr std::array<EventSpec, kEventCount> kEvents{{
        {"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
        {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
        {"L1-dcache-loads", PERF_TYPE_HW_CACHE,
         cache_read_event(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_RESULT_ACCESS)},
        {"L1-dcache-load-misses", PERF_TYPE_HW_CACHE,
         cache_read_event(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_RESULT_MISS)},
        {"LLC-load-misses", PERF_TYPE_HW_CACHE,
         cache_read_event(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_RESULT_MISS)},
        {"branch-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
    }};

    /// On a hybrid CPU each core type has its own PMU, and generic events must name which one they
    /// mean. Benchmarks pin to P-cores, so this selects the P-core PMU. It returns 0 on other CPUs,
    /// which leaves the event configuration unchanged.
    [[nodiscard]] static std::uint64_t core_pmu_type() {
        std::ifstream file{"/sys/devices/cpu_core/type"};
        std::uint64_t type = 0;
        file >> type;
        return file ? type : 0;
    }

    void close_all() noexcept {
        for (int& fd : fds_) {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
        }
    }

    std::array<int, kEventCount> fds_{-1, -1, -1, -1, -1, -1};
    std::string error_;
};

}  // namespace nanomatch::bench

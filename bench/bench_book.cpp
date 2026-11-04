// Measures a book against a command log and prints one JSON record. scripts/run_bench.py runs this
// repeatedly and adds machine details. docs/BENCHMARKS.md describes the method.
//
// Mode "untimed" reads the clock only around the whole loop, which gives the average without the
// cost of per-command timers, and also collects hardware counters. Mode "timed" reads the clock
// around every command to give the latency distribution, and so reports higher values.

#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "baseline/baseline_book.hpp"
#include "cli.hpp"
#include "histogram.hpp"
#include "json_writer.hpp"
#include "nanomatch/book.hpp"
#include "nanomatch/fnv1a.hpp"
#include "nanomatch/log_io.hpp"
#include "perf_counters.hpp"
#include "tsc.hpp"
#include "workload/generator.hpp"

namespace {

using nanomatch::BookConfig;
using nanomatch::Command;
using nanomatch::CommandType;
using nanomatch::Event;
using nanomatch::MatchingBook;
using nanomatch::bench::LatencyHistogram;
using nanomatch::bench::PerfGroup;
using nanomatch::bench::PerfSample;
using nanomatch::bench::serialized_tsc;
using nanomatch::tools::Flags;
using nanomatch::tools::JsonObject;

constexpr std::string_view kUsage =
    "Usage: bench_book --log <commands> --profile <name> --book baseline\n"
    "                  --mode <untimed|timed> --warmup <commands> --cpu <n|none>\n";

constexpr std::size_t kTimerSamples = 1'000'000;
constexpr std::array<std::pair<std::string_view, double>, 6> kPercentiles{{
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

    [[nodiscard]] std::uint64_t contents_hash() const noexcept {
        nanomatch::Fnv1a64 hash;
        hash.update(std::as_bytes(std::span{events_}));
        return hash.value();
    }

  private:
    static constexpr std::size_t kCapacity = 4096;

    std::array<Event, kCapacity> events_{};
    std::uint64_t count_{0};
};

struct Options {
    std::filesystem::path log;
    std::string profile;
    std::string book;
    std::string mode;
    std::size_t warmup{};
    std::optional<int> cpu;
};

[[nodiscard]] Options parse_options(const Flags& flags) {
    Options options;
    options.log = flags.required("--log");
    options.profile = flags.required("--profile");
    options.book = flags.required("--book");
    options.mode = flags.required("--mode");
    options.warmup = flags.required_integer<std::size_t>("--warmup");
    if (flags.required("--cpu") != "none") {
        options.cpu = flags.required_integer<int>("--cpu");
    }
    if (options.mode != "untimed" && options.mode != "timed") {
        throw std::invalid_argument{std::format("unknown mode '{}'", options.mode)};
    }
    return options;
}

void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        throw std::runtime_error{std::format("cannot pin to CPU {}", cpu)};
    }
}

[[nodiscard]] rusage current_usage() noexcept {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage;
}

[[nodiscard]] JsonObject usage_delta(const rusage& before, const rusage& after) {
    JsonObject json;
    json.add("minor_faults", after.ru_minflt - before.ru_minflt)
        .add("major_faults", after.ru_majflt - before.ru_majflt)
        .add("voluntary_switches", after.ru_nvcsw - before.ru_nvcsw)
        .add("involuntary_switches", after.ru_nivcsw - before.ru_nivcsw);
    return json;
}

[[nodiscard]] JsonObject summarize(const LatencyHistogram& histogram, double ticks_per_ns) {
    const auto to_ns = [ticks_per_ns](double ticks) { return ticks / ticks_per_ns; };
    JsonObject json;
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
[[nodiscard]] JsonObject measure_timer(double ticks_per_ns) {
    const auto histogram = std::make_unique<LatencyHistogram>();
    for (std::size_t i = 0; i < kTimerSamples; ++i) {
        const std::uint64_t start = serialized_tsc();
        const std::uint64_t end = serialized_tsc();
        histogram->record(end - start);
    }
    return summarize(*histogram, ticks_per_ns);
}

[[nodiscard]] JsonObject describe_perf(const PerfGroup& group) {
    JsonObject json;
    const std::optional<PerfSample> sample = group.read_sample();
    json.add("available", sample.has_value());
    if (!sample) {
        json.add("error", group.available() ? "read failed" : group.error());
        return json;
    }
    json.add("cycles", sample->cycles)
        .add("instructions", sample->instructions)
        .add("l1d_loads", sample->l1d_loads)
        .add("l1d_load_misses", sample->l1d_load_misses)
        .add("llc_load_misses", sample->llc_load_misses)
        .add("branch_misses", sample->branch_misses)
        .add("running_fraction", sample->running_fraction);
    return json;
}

template <MatchingBook Book>
[[nodiscard]] JsonObject run(const Options& options, std::span<const Command> commands,
                             const BookConfig& config) {
    if (options.warmup >= commands.size()) {
        throw std::invalid_argument{"--warmup must be smaller than the log"};
    }
    const std::span<const Command> warmup = commands.first(options.warmup);
    const std::span<const Command> measured = commands.subspan(options.warmup);

    std::uint64_t new_orders = 0;
    for (const Command& command : measured) {
        const bool is_new_order =
            command.type == CommandType::kLimit || command.type == CommandType::kMarket;
        new_orders += is_new_order ? 1 : 0;
    }

    const double tsc_hz = nanomatch::bench::calibrate_tsc_hz(std::chrono::milliseconds{500});
    const double ticks_per_ns = tsc_hz / 1e9;

    const auto book = std::make_unique<Book>(config);
    const auto sink = std::make_unique<RingSink>();
    for (const Command& command : warmup) {
        book->apply(command, *sink);
    }

    // Locking what's mapped so far stops the log and book being paged out mid-run. Allocations made
    // during the run aren't covered, which is deliberate: they are part of what's being measured
    // for a book that allocates, and they show up as page faults in the record.
    const bool locked = mlockall(MCL_CURRENT) == 0;

    JsonObject json;
    json.add("book", options.book)
        .add("profile", options.profile)
        .add("mode", options.mode)
        .add("warmup_commands", warmup.size())
        .add("measured_commands", measured.size())
        .add("measured_new_orders", new_orders)
        .add("cpu", options.cpu.value_or(-1))
        .add("tsc_hz", tsc_hz)
        .add("memory_locked", locked)
        .add("timer", measure_timer(ticks_per_ns));

    rusage usage_before{};
    rusage usage_after{};

    if (options.mode == "untimed") {
        PerfGroup perf;
        usage_before = current_usage();
        perf.start();
        const std::uint64_t start = serialized_tsc();
        for (const Command& command : measured) {
            book->apply(command, *sink);
        }
        const std::uint64_t end = serialized_tsc();
        perf.stop();
        usage_after = current_usage();

        const double elapsed_ns = static_cast<double>(end - start) / ticks_per_ns;
        json.add("elapsed_ns", elapsed_ns)
            .add("mean_ns", elapsed_ns / static_cast<double>(measured.size()))
            .add("commands_per_second", static_cast<double>(measured.size()) / (elapsed_ns / 1e9))
            .add("perf", describe_perf(perf));
    } else {
        const auto histogram = std::make_unique<LatencyHistogram>();
        usage_before = current_usage();
        for (const Command& command : measured) {
            const std::uint64_t start = serialized_tsc();
            book->apply(command, *sink);
            const std::uint64_t end = serialized_tsc();
            histogram->record(end - start);
        }
        usage_after = current_usage();
        json.add("latency", summarize(*histogram, ticks_per_ns));
    }

    json.add("usage", usage_delta(usage_before, usage_after))
        .add("events", sink->count())
        .add("sink_hash", std::format("0x{:016x}", sink->contents_hash()))
        .add("resting_orders_at_end", book->resting_orders());
    return json;
}

void execute(const Options& options) {
    if (options.cpu) {
        pin_to_cpu(*options.cpu);
    }
    const nanomatch::workload::Profile* const profile =
        nanomatch::workload::find_profile(options.profile);
    if (profile == nullptr) {
        throw std::invalid_argument{std::format("unknown profile '{}'", options.profile)};
    }
    const BookConfig config = nanomatch::workload::book_config(*profile);
    const std::vector<Command> commands = nanomatch::read_command_log(options.log);

    if (options.book == "baseline") {
        std::cout << run<nanomatch::baseline::BaselineBook<>>(options, commands, config).str()
                  << '\n';
    } else {
        throw std::invalid_argument{std::format("unknown book '{}'", options.book)};
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const std::span<const char* const> arguments{argv + 1, static_cast<std::size_t>(argc - 1)};
        execute(parse_options(
            Flags{arguments, {"--log", "--profile", "--book", "--mode", "--warmup", "--cpu"}}));
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "error: " << error.what() << '\n' << kUsage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

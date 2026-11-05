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

#include "book_registry.hpp"
#include "cli.hpp"
#include "common.hpp"
#include "histogram.hpp"
#include "json_writer.hpp"
#include "nanomatch/book.hpp"
#include "nanomatch/log_io.hpp"
#include "perf_counters.hpp"
#include "tsc.hpp"
#include "workload/generator.hpp"

namespace {

using nanomatch::BookConfig;
using nanomatch::Command;
using nanomatch::CommandType;
using nanomatch::MatchingBook;
using nanomatch::bench::current_usage;
using nanomatch::bench::LatencyHistogram;
using nanomatch::bench::PerfGroup;
using nanomatch::bench::PerfSample;
using nanomatch::bench::pin_to_cpu;
using nanomatch::bench::RingSink;
using nanomatch::bench::serialized_tsc;
using nanomatch::bench::summarize;
using nanomatch::bench::usage_delta;
using nanomatch::tools::Flags;
using nanomatch::tools::JsonObject;

constexpr std::string_view kUsage =
    "Usage: bench_book --log <commands> --profile <name> --book <name>\n"
    "                  --mode <untimed|timed> --warmup <commands> --cpu <n|none>\n";

/// What a command turned out to do. Commands differ greatly in how much work they imply, since one
/// that trades against ten resting orders has ten times the work of one that trades against one, so
/// an overall percentile mixes very different things.
constexpr std::size_t kCommandKinds = 5;
constexpr std::array<std::string_view, kCommandKinds> kCommandKindNames{
    "passive", "aggressive", "cancel", "modify", "rejected"};

[[nodiscard]] std::size_t kind_of(const Command& command, const RingSink& sink,
                                  std::uint64_t events_before) noexcept {
    const std::uint64_t events = sink.count() - events_before;
    if (sink.last().type == nanomatch::EventType::kRejected) {
        return 4;
    }
    switch (command.type) {
        case CommandType::kLimit:
            // ACCEPTED then RESTED, with no trades in between.
            return events == 2 && sink.last().type == nanomatch::EventType::kRested ? 0 : 1;
        case CommandType::kMarket:
            return 1;
        case CommandType::kCancel:
            return 2;
        case CommandType::kModify:
            return 3;
    }
    return 4;
}

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
        .add("timer", nanomatch::bench::measure_timer(ticks_per_ns));

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
        const auto by_kind = std::make_unique<std::array<LatencyHistogram, kCommandKinds>>();
        usage_before = current_usage();
        for (const Command& next_command : measured) {
            // Reading the command first keeps the fetch from a very large log out of the sample. In
            // a running system, commands arrive through a ring in cache, and the pipeline benchmark
            // measures that path. Without this, samples mostly reflect paging through the log.
            const Command command = next_command;
            const std::uint64_t events_before = sink->count();
            const std::uint64_t start = serialized_tsc();
            book->apply(command, *sink);
            const std::uint64_t end = serialized_tsc();
            histogram->record(end - start);
            (*by_kind)[kind_of(command, *sink, events_before)].record(end - start);
        }
        usage_after = current_usage();
        JsonObject kinds;
        for (std::size_t kind = 0; kind < kCommandKinds; ++kind) {
            kinds.add(kCommandKindNames[kind], summarize((*by_kind)[kind], ticks_per_ns));
        }
        json.add("latency", summarize(*histogram, ticks_per_ns)).add("latency_by_kind", kinds);
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

    nanomatch::tools::with_book(options.book, [&]<MatchingBook Book>() {
        std::cout << run<Book>(options, commands, config).str() << '\n';
    });
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

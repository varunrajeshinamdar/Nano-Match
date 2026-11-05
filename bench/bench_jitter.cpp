// Measures how repeatable timing is, and how finely the timer itself can resolve it, then prints
// one JSON record.
//
// Replaying one log gives identical work every time, so any difference in timing comes from the
// machine and not the input. Three separate quantities are reported, because they answer different
// questions and differ by orders of magnitude:
//
// - timer: the distribution of two consecutive serialized timestamp reads. Nothing below its spread
// can be
//   resolved by the per-command measurements.
// - run_to_run_mean: how much a whole replay's average changes between replays. Averaging over
// millions
//   of commands cancels most noise, so this is far smaller than the variation of a single command.
// - per_command: for each command, how much its own latency changes between replays.
//
// Determinism isn't what's being measured here. Every replay must also produce identical events,
// and the program fails if it doesn't, but that's a check of the output and not of the timing.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "book_registry.hpp"
#include "cli.hpp"
#include "common.hpp"
#include "json_writer.hpp"
#include "nanomatch/book.hpp"
#include "nanomatch/log_io.hpp"
#include "tsc.hpp"
#include "workload/generator.hpp"

namespace {

using nanomatch::BookConfig;
using nanomatch::Command;
using nanomatch::MatchingBook;
using nanomatch::bench::RingSink;
using nanomatch::bench::serialized_tsc;
using nanomatch::tools::Flags;
using nanomatch::tools::JsonObject;

constexpr std::string_view kUsage =
    "Usage: bench_jitter --log <commands> --profile <name> --book <name> --warmup <n>\n"
    "                    --measured <n> --replays <n> --cpu <n|none>\n";

struct Options {
    std::filesystem::path log;
    std::string profile;
    std::string book;
    std::size_t warmup{};
    std::size_t measured{};
    std::size_t replays{};
};

struct Moments {
    double mean{};
    double stddev{};
    double min{};
    double max{};
};

[[nodiscard]] Moments moments_of(std::span<const double> values) {
    Moments result;
    result.mean =
        std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    double squares = 0.0;
    for (const double value : values) {
        squares += (value - result.mean) * (value - result.mean);
    }
    result.stddev = std::sqrt(squares / static_cast<double>(values.size()));
    const auto [low, high] = std::minmax_element(values.begin(), values.end());
    result.min = *low;
    result.max = *high;
    return result;
}

[[nodiscard]] JsonObject describe(const Moments& moments) {
    JsonObject json;
    json.add("mean_ns", moments.mean)
        .add("stddev_ns", moments.stddev)
        .add("min_ns", moments.min)
        .add("max_ns", moments.max);
    return json;
}

/// The value at a fraction of a sorted list. Sorts in place.
[[nodiscard]] double quantile(std::vector<double>& values, double fraction) {
    const auto rank = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(rank),
                     values.end());
    return values[rank];
}

/// Replays with no per-command timers, and returns each replay's mean nanoseconds per command.
template <MatchingBook Book>
[[nodiscard]] std::vector<double> replay_untimed(const Options& options,
                                                 std::span<const Command> warmup,
                                                 std::span<const Command> measured,
                                                 const BookConfig& config, double ticks_per_ns,
                                                 std::optional<std::uint64_t>& expected_hash) {
    std::vector<double> means;
    means.reserve(options.replays);
    for (std::size_t replay = 0; replay < options.replays; ++replay) {
        const auto book = std::make_unique<Book>(config);
        const auto sink = std::make_unique<RingSink>();
        for (const Command& command : warmup) {
            book->apply(command, *sink);
        }
        const std::uint64_t start = serialized_tsc();
        for (const Command& command : measured) {
            book->apply(command, *sink);
        }
        const std::uint64_t end = serialized_tsc();
        means.push_back(static_cast<double>(end - start) / ticks_per_ns /
                        static_cast<double>(measured.size()));

        const std::uint64_t hash = sink->contents_hash() ^ sink->count();
        if (expected_hash && *expected_hash != hash) {
            throw std::runtime_error{"replays produced different events"};
        }
        expected_hash = hash;
    }
    return means;
}

/// Replays with per-command timers, and summarizes how each command's latency varies across
/// replays.
template <MatchingBook Book>
[[nodiscard]] JsonObject replay_timed(const Options& options, std::span<const Command> warmup,
                                      std::span<const Command> measured, const BookConfig& config,
                                      double ticks_per_ns,
                                      std::optional<std::uint64_t>& expected_hash) {
    // One preallocated block, written in command order for one replay at a time.
    std::vector<std::uint32_t> ticks(options.replays * measured.size());

    for (std::size_t replay = 0; replay < options.replays; ++replay) {
        const auto book = std::make_unique<Book>(config);
        const auto sink = std::make_unique<RingSink>();
        for (const Command& command : warmup) {
            book->apply(command, *sink);
        }
        std::uint32_t* const row = ticks.data() + (replay * measured.size());
        for (std::size_t i = 0; i < measured.size(); ++i) {
            const std::uint64_t start = serialized_tsc();
            book->apply(measured[i], *sink);
            const std::uint64_t end = serialized_tsc();
            row[i] = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(end - start, std::uint32_t{0xFFFFFFFF}));
        }
        const std::uint64_t hash = sink->contents_hash() ^ sink->count();
        if (expected_hash && *expected_hash != hash) {
            throw std::runtime_error{"replays produced different events"};
        }
        expected_hash = hash;
    }

    std::vector<double> stddevs;
    std::vector<double> ranges;
    stddevs.reserve(measured.size());
    ranges.reserve(measured.size());
    std::vector<double> samples(options.replays);
    for (std::size_t i = 0; i < measured.size(); ++i) {
        for (std::size_t replay = 0; replay < options.replays; ++replay) {
            samples[replay] =
                static_cast<double>(ticks[(replay * measured.size()) + i]) / ticks_per_ns;
        }
        const Moments moments = moments_of(samples);
        stddevs.push_back(moments.stddev);
        ranges.push_back(moments.max - moments.min);
    }

    JsonObject json;
    json.add("stddev_p50_ns", quantile(stddevs, 0.50))
        .add("stddev_p90_ns", quantile(stddevs, 0.90))
        .add("stddev_p99_ns", quantile(stddevs, 0.99))
        .add("range_p50_ns", quantile(ranges, 0.50))
        .add("range_p99_ns", quantile(ranges, 0.99));
    return json;
}

template <MatchingBook Book>
[[nodiscard]] JsonObject run(const Options& options, std::span<const Command> commands,
                             const BookConfig& config) {
    if (options.warmup + options.measured > commands.size()) {
        throw std::invalid_argument{"--warmup plus --measured is longer than the log"};
    }
    const std::span<const Command> warmup = commands.first(options.warmup);
    const std::span<const Command> measured = commands.subspan(options.warmup, options.measured);

    const double tsc_hz = nanomatch::bench::calibrate_tsc_hz(std::chrono::milliseconds{500});
    const double ticks_per_ns = tsc_hz / 1e9;

    const JsonObject timer = nanomatch::bench::measure_timer(ticks_per_ns);
    std::optional<std::uint64_t> expected_hash;
    const std::vector<double> means =
        replay_untimed<Book>(options, warmup, measured, config, ticks_per_ns, expected_hash);
    const JsonObject per_command =
        replay_timed<Book>(options, warmup, measured, config, ticks_per_ns, expected_hash);

    JsonObject json;
    json.add("book", options.book)
        .add("profile", options.profile)
        .add("measured_commands", measured.size())
        .add("replays", options.replays)
        .add("tsc_hz", tsc_hz)
        .add("tick_ns", 1.0 / ticks_per_ns)
        .add("timer", timer)
        .add("run_to_run_mean", describe(moments_of(means)))
        .add("per_command", per_command)
        .add("events_identical_across_replays", true);
    return json;
}

void execute(const Flags& flags) {
    Options options;
    options.log = flags.required("--log");
    options.profile = flags.required("--profile");
    options.book = flags.required("--book");
    options.warmup = flags.required_integer<std::size_t>("--warmup");
    options.measured = flags.required_integer<std::size_t>("--measured");
    options.replays = flags.required_integer<std::size_t>("--replays");
    if (options.replays < 2) {
        throw std::invalid_argument{"--replays must be at least 2"};
    }
    if (flags.required("--cpu") != "none") {
        nanomatch::bench::pin_to_cpu(flags.required_integer<int>("--cpu"));
    }
    const nanomatch::workload::Profile* const profile =
        nanomatch::workload::find_profile(options.profile);
    if (profile == nullptr) {
        throw std::invalid_argument{std::format("unknown profile '{}'", options.profile)};
    }
    const std::vector<Command> commands = nanomatch::read_command_log(options.log);
    nanomatch::tools::with_book(options.book, [&]<MatchingBook Book>() {
        std::cout << run<Book>(options, commands, nanomatch::workload::book_config(*profile)).str()
                  << '\n';
    });
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const std::span<const char* const> arguments{argv + 1, static_cast<std::size_t>(argc - 1)};
        execute(Flags{
            arguments,
            {"--log", "--profile", "--book", "--warmup", "--measured", "--replays", "--cpu"}});
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "error: " << error.what() << '\n' << kUsage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

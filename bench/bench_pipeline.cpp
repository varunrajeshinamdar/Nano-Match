// Measures commands going from a gateway thread, through the SPSC ring, to a book on another core,
// and prints one JSON record.
//
// Mode "throughput" sends commands as fast as the ring accepts them. Latencies aren't reported for
// it, because a saturated queue would mostly measure time spent waiting in the queue.
//
// Mode "latency" sends one command at a time and waits until it has been applied. Each sample runs
// from just before the gateway pushes to just after the book finishes, which covers the crossing
// between cores and the book's work with no queueing. The two timestamps come from different cores,
// which is meaningful because this CPU's TSC is synchronized across cores.

#include <immintrin.h>

#include <atomic>
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
#include <thread>
#include <vector>

#include "book_registry.hpp"
#include "cli.hpp"
#include "common.hpp"
#include "histogram.hpp"
#include "json_writer.hpp"
#include "nanomatch/book.hpp"
#include "nanomatch/log_io.hpp"
#include "nanomatch/pipeline.hpp"
#include "nanomatch/spsc_ring.hpp"
#include "tsc.hpp"
#include "workload/generator.hpp"

namespace {

using nanomatch::BookConfig;
using nanomatch::Command;
using nanomatch::MatchingBook;
using nanomatch::SpscRing;
using nanomatch::bench::LatencyHistogram;
using nanomatch::bench::RingSink;
using nanomatch::bench::serialized_tsc;
using nanomatch::tools::Flags;
using nanomatch::tools::JsonObject;

constexpr std::size_t kRingCapacity = 4096;

constexpr std::string_view kUsage =
    "Usage: bench_pipeline --log <commands> --profile <name> --book <name> --mode "
    "<throughput|latency>\n"
    "                      --warmup <n> --gateway-cpu <n|none> --engine-cpu <n|none>\n";

struct Options {
    std::filesystem::path log;
    std::string profile;
    std::string book;
    std::string mode;
    std::size_t warmup{};
    std::optional<int> gateway_cpu;
    std::optional<int> engine_cpu;
};

struct TimedCommand {
    Command command;
    std::uint64_t sent_tsc{};
};

class StartGate {
  public:
    void arrive_and_wait() noexcept {
        arrived_.fetch_add(1, std::memory_order_acq_rel);
        while (arrived_.load(std::memory_order_acquire) < 2) {
            _mm_pause();
        }
    }

  private:
    std::atomic<int> arrived_{0};
};

template <typename Book>
[[nodiscard]] JsonObject run_throughput(const Options& options, std::span<const Command> measured,
                                        Book& book, RingSink& sink, double tsc_hz) {
    using Ring = SpscRing<Command, kRingCapacity>;
    const auto ring = std::make_unique<Ring>();
    StartGate gate;

    std::thread gateway{[&ring, &gate, &options, measured] {
        if (options.gateway_cpu) {
            nanomatch::bench::pin_to_cpu(*options.gateway_cpu);
        }
        gate.arrive_and_wait();
        nanomatch::publish_commands(measured, *ring);
    }};

    gate.arrive_and_wait();
    const std::uint64_t start = serialized_tsc();
    nanomatch::consume_commands(measured.size(), *ring, book, sink);
    const std::uint64_t end = serialized_tsc();
    gateway.join();

    const double seconds = static_cast<double>(end - start) / tsc_hz;
    JsonObject json;
    json.add("elapsed_seconds", seconds)
        .add("commands_per_second", static_cast<double>(measured.size()) / seconds);
    return json;
}

template <typename Book>
[[nodiscard]] JsonObject run_latency(const Options& options, std::span<const Command> measured,
                                     Book& book, RingSink& sink, double tsc_hz) {
    using Ring = SpscRing<TimedCommand, kRingCapacity>;
    const auto ring = std::make_unique<Ring>();
    const auto histogram = std::make_unique<LatencyHistogram>();
    // The acknowledgement gets its own cache line, so that it can't falsely share with anything
    // nearby.
    alignas(64) std::atomic<std::uint64_t> acknowledged{0};
    StartGate gate;

    std::thread gateway{[&ring, &gate, &options, &acknowledged, measured] {
        if (options.gateway_cpu) {
            nanomatch::bench::pin_to_cpu(*options.gateway_cpu);
        }
        gate.arrive_and_wait();
        std::uint64_t sent = 0;
        for (const Command& command : measured) {
            const TimedCommand message{command, serialized_tsc()};
            while (!ring->try_push(message)) {
                _mm_pause();
            }
            ++sent;
            // Acquire pairs with the engine's release, so this command is known to be finished.
            while (acknowledged.load(std::memory_order_acquire) != sent) {
                _mm_pause();
            }
        }
    }};

    gate.arrive_and_wait();
    TimedCommand message;
    for (std::size_t i = 0; i < measured.size(); ++i) {
        while (!ring->try_pop(message)) {
            _mm_pause();
        }
        book.apply(message.command, sink);
        const std::uint64_t finished = serialized_tsc();
        histogram->record(finished - message.sent_tsc);
        // Release pairs with the gateway's acquire. Nothing else depends on it, but the gateway
        // must not send the next command before the histogram update above is complete, because
        // that update would then disturb the next sample.
        acknowledged.store(i + 1, std::memory_order_release);
    }
    gateway.join();

    JsonObject json;
    json.add("latency", nanomatch::bench::summarize(*histogram, tsc_hz / 1e9));
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

    const double tsc_hz = nanomatch::bench::calibrate_tsc_hz(std::chrono::milliseconds{200});
    const auto book = std::make_unique<Book>(config);
    const auto sink = std::make_unique<RingSink>();
    for (const Command& command : warmup) {
        book->apply(command, *sink);
    }

    const JsonObject result = options.mode == "throughput"
                                  ? run_throughput(options, measured, *book, *sink, tsc_hz)
                                  : run_latency(options, measured, *book, *sink, tsc_hz);

    JsonObject json;
    json.add("book", options.book)
        .add("profile", options.profile)
        .add("mode", options.mode)
        .add("measured_commands", measured.size())
        .add("ring_capacity", kRingCapacity)
        .add("gateway_cpu", options.gateway_cpu.value_or(-1))
        .add("engine_cpu", options.engine_cpu.value_or(-1))
        .add("tsc_hz", tsc_hz)
        .add("timer", nanomatch::bench::measure_timer(tsc_hz / 1e9))
        .add("result", result)
        .add("events", sink->count())
        .add("sink_hash", std::format("0x{:016x}", sink->contents_hash()));
    return json;
}

void execute(const Flags& flags) {
    Options options;
    options.log = flags.required("--log");
    options.profile = flags.required("--profile");
    options.book = flags.required("--book");
    options.mode = flags.required("--mode");
    options.warmup = flags.required_integer<std::size_t>("--warmup");
    if (flags.required("--gateway-cpu") != "none") {
        options.gateway_cpu = flags.required_integer<int>("--gateway-cpu");
    }
    if (flags.required("--engine-cpu") != "none") {
        options.engine_cpu = flags.required_integer<int>("--engine-cpu");
    }
    if (options.mode != "throughput" && options.mode != "latency") {
        throw std::invalid_argument{std::format("unknown mode '{}'", options.mode)};
    }
    if (options.engine_cpu) {
        nanomatch::bench::pin_to_cpu(*options.engine_cpu);
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
        execute(Flags{arguments,
                      {"--log", "--profile", "--book", "--mode", "--warmup", "--gateway-cpu",
                       "--engine-cpu"}});
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "error: " << error.what() << '\n' << kUsage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

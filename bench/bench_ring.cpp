// Measures the SPSC ring on its own and prints one JSON record. Mode "throughput" sends items as
// fast as possible. Mode "pingpong" bounces one item between two threads through two rings, which
// measures how long an item takes to cross between cores when no queue has built up.
//
// Throughput is counted in items moved, where one item is one push and its matching pop.

#include <immintrin.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "cli.hpp"
#include "common.hpp"
#include "histogram.hpp"
#include "json_writer.hpp"
#include "nanomatch/protocol.hpp"
#include "nanomatch/spsc_ring.hpp"
#include "tsc.hpp"

namespace {

using nanomatch::Command;
using nanomatch::SpscRing;
using nanomatch::SpscRingLayout;
using nanomatch::bench::LatencyHistogram;
using nanomatch::bench::serialized_tsc;
using nanomatch::tools::Flags;
using nanomatch::tools::JsonObject;

constexpr std::size_t kRingCapacity = 4096;

constexpr std::string_view kUsage =
    "Usage: bench_ring --mode <throughput|pingpong> --payload <8|32> --alignment <8|64|128>\n"
    "                  --cached <0|1> --items <n> --producer-cpu <n|none> --consumer-cpu "
    "<n|none>\n";

struct Options {
    std::string mode;
    std::uint64_t items{};
    std::optional<int> producer_cpu;
    std::optional<int> consumer_cpu;
};

[[nodiscard]] std::optional<int> parse_cpu(const Flags& flags, std::string_view flag) {
    if (flags.required(flag) == "none") {
        return std::nullopt;
    }
    return flags.required_integer<int>(flag);
}

template <typename Payload>
[[nodiscard]] Payload make_payload(std::uint64_t value) noexcept {
    if constexpr (std::is_same_v<Payload, Command>) {
        Command command;
        command.seq = nanomatch::Sequence{value};
        return command;
    } else {
        return value;
    }
}

template <typename Payload>
[[nodiscard]] std::uint64_t read_payload(const Payload& payload) noexcept {
    if constexpr (std::is_same_v<Payload, Command>) {
        return payload.seq.value();
    } else {
        return payload;
    }
}

/// Holds both threads at a barrier until each is pinned and ready, so that timing covers only the
/// transfer and not thread startup.
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

template <typename Ring, typename Payload>
[[nodiscard]] JsonObject run_throughput(const Options& options, double tsc_hz) {
    const auto ring = std::make_unique<Ring>();
    StartGate gate;

    std::thread producer{[&ring, &gate, &options] {
        if (options.producer_cpu) {
            nanomatch::bench::pin_to_cpu(*options.producer_cpu);
        }
        gate.arrive_and_wait();
        for (std::uint64_t i = 0; i < options.items; ++i) {
            const auto payload = make_payload<Payload>(i);
            while (!ring->try_push(payload)) {
                _mm_pause();
            }
        }
    }};

    if (options.consumer_cpu) {
        nanomatch::bench::pin_to_cpu(*options.consumer_cpu);
    }
    gate.arrive_and_wait();
    const std::uint64_t start = serialized_tsc();
    Payload payload{};
    std::uint64_t mismatches = 0;
    for (std::uint64_t i = 0; i < options.items; ++i) {
        while (!ring->try_pop(payload)) {
            _mm_pause();
        }
        mismatches += read_payload(payload) == i ? 0U : 1U;
    }
    const std::uint64_t end = serialized_tsc();
    producer.join();

    if (mismatches != 0) {
        throw std::runtime_error{"ring delivered items out of order"};
    }
    const double seconds = static_cast<double>(end - start) / tsc_hz;
    JsonObject json;
    json.add("items", options.items)
        .add("elapsed_seconds", seconds)
        .add("items_per_second", static_cast<double>(options.items) / seconds);
    return json;
}

template <typename Ring, typename Payload>
[[nodiscard]] JsonObject run_pingpong(const Options& options, double tsc_hz) {
    const auto forward = std::make_unique<Ring>();
    const auto backward = std::make_unique<Ring>();
    StartGate gate;

    std::thread echo{[&forward, &backward, &gate, &options] {
        if (options.consumer_cpu) {
            nanomatch::bench::pin_to_cpu(*options.consumer_cpu);
        }
        gate.arrive_and_wait();
        Payload payload{};
        for (std::uint64_t i = 0; i < options.items; ++i) {
            while (!forward->try_pop(payload)) {
                _mm_pause();
            }
            while (!backward->try_push(payload)) {
                _mm_pause();
            }
        }
    }};

    if (options.producer_cpu) {
        nanomatch::bench::pin_to_cpu(*options.producer_cpu);
    }
    gate.arrive_and_wait();
    const auto histogram = std::make_unique<LatencyHistogram>();
    Payload payload{};
    for (std::uint64_t i = 0; i < options.items; ++i) {
        const auto outgoing = make_payload<Payload>(i);
        const std::uint64_t start = serialized_tsc();
        while (!forward->try_push(outgoing)) {
            _mm_pause();
        }
        while (!backward->try_pop(payload)) {
            _mm_pause();
        }
        const std::uint64_t end = serialized_tsc();
        histogram->record(end - start);
    }
    echo.join();

    JsonObject json;
    json.add("items", options.items)
        .add("round_trip", nanomatch::bench::summarize(*histogram, tsc_hz / 1e9));
    return json;
}

template <typename Payload, std::size_t Alignment, bool Cached>
[[nodiscard]] JsonObject dispatch_mode(const Options& options, double tsc_hz) {
    using Ring = SpscRing<Payload, kRingCapacity,
                          SpscRingLayout{.index_alignment = Alignment, .cache_indices = Cached}>;
    return options.mode == "throughput" ? run_throughput<Ring, Payload>(options, tsc_hz)
                                        : run_pingpong<Ring, Payload>(options, tsc_hz);
}

template <typename Payload, bool Cached>
[[nodiscard]] JsonObject dispatch_alignment(std::size_t alignment, const Options& options,
                                            double tsc_hz) {
    switch (alignment) {
        case 8:
            return dispatch_mode<Payload, 8, Cached>(options, tsc_hz);
        case 64:
            return dispatch_mode<Payload, 64, Cached>(options, tsc_hz);
        case 128:
            return dispatch_mode<Payload, 128, Cached>(options, tsc_hz);
        default:
            throw std::invalid_argument{std::format("unsupported alignment {}", alignment)};
    }
}

void execute(const Flags& flags) {
    Options options;
    options.mode = flags.required("--mode");
    options.items = flags.required_integer<std::uint64_t>("--items");
    options.producer_cpu = parse_cpu(flags, "--producer-cpu");
    options.consumer_cpu = parse_cpu(flags, "--consumer-cpu");
    if (options.mode != "throughput" && options.mode != "pingpong") {
        throw std::invalid_argument{std::format("unknown mode '{}'", options.mode)};
    }
    const auto payload = flags.required_integer<int>("--payload");
    const auto alignment = flags.required_integer<std::size_t>("--alignment");
    const bool cached = flags.required_integer<int>("--cached") != 0;

    const double tsc_hz = nanomatch::bench::calibrate_tsc_hz(std::chrono::milliseconds{200});
    JsonObject result;
    if (payload == 8) {
        result = cached ? dispatch_alignment<std::uint64_t, true>(alignment, options, tsc_hz)
                        : dispatch_alignment<std::uint64_t, false>(alignment, options, tsc_hz);
    } else if (payload == 32) {
        result = cached ? dispatch_alignment<Command, true>(alignment, options, tsc_hz)
                        : dispatch_alignment<Command, false>(alignment, options, tsc_hz);
    } else {
        throw std::invalid_argument{std::format("unsupported payload {}", payload)};
    }

    JsonObject json;
    json.add("mode", options.mode)
        .add("payload_bytes", payload)
        .add("index_alignment", alignment)
        .add("cached_indices", cached)
        .add("ring_capacity", kRingCapacity)
        .add("producer_cpu", options.producer_cpu.value_or(-1))
        .add("consumer_cpu", options.consumer_cpu.value_or(-1))
        .add("tsc_hz", tsc_hz)
        .add("result", result);
    std::cout << json.str() << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const std::span<const char* const> arguments{argv + 1, static_cast<std::size_t>(argc - 1)};
        execute(Flags{arguments,
                      {"--mode", "--payload", "--alignment", "--cached", "--items",
                       "--producer-cpu", "--consumer-cpu"}});
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "error: " << error.what() << '\n' << kUsage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

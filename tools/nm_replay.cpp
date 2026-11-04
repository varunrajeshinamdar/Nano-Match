// Replays a command log through a book and reports the hash of the resulting event log.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "baseline/baseline_book.hpp"
#include "nanomatch/book.hpp"
#include "nanomatch/log_io.hpp"

namespace {

using nanomatch::BookConfig;
using nanomatch::Command;
using nanomatch::EventLogSink;
using nanomatch::MatchingBook;
using nanomatch::Price;

constexpr std::string_view kUsage =
    "Usage: nm_replay --in <commands> --min-price <ticks> --max-price <ticks> --capacity <orders>\n"
    "                 [--out <events>] [--book baseline]\n";

struct Options {
    std::filesystem::path input;
    std::optional<std::filesystem::path> output;
    BookConfig config;
    std::string book{"baseline"};
};

template <typename Integer>
[[nodiscard]] Integer parse_integer(std::string_view flag, std::string_view text) {
    Integer value{};
    const char* const end = text.data() + text.size();
    const auto [parsed_to, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || parsed_to != end) {
        throw std::invalid_argument{std::format("{}: invalid value '{}'", flag, text)};
    }
    return value;
}

[[nodiscard]] Options parse_options(std::span<const char* const> arguments) {
    Options options;
    bool has_input = false;
    bool has_min_price = false;
    bool has_max_price = false;
    bool has_capacity = false;

    for (std::size_t i = 0; i < arguments.size(); i += 2) {
        const std::string_view flag = arguments[i];
        if (i + 1 >= arguments.size()) {
            throw std::invalid_argument{std::format("{}: missing value", flag)};
        }
        const std::string_view value = arguments[i + 1];

        if (flag == "--in") {
            options.input = value;
            has_input = true;
        } else if (flag == "--out") {
            options.output = value;
        } else if (flag == "--book") {
            options.book = value;
        } else if (flag == "--min-price") {
            options.config.min_price = Price{parse_integer<Price::rep>(flag, value)};
            has_min_price = true;
        } else if (flag == "--max-price") {
            options.config.max_price = Price{parse_integer<Price::rep>(flag, value)};
            has_max_price = true;
        } else if (flag == "--capacity") {
            options.config.max_resting_orders = parse_integer<std::uint32_t>(flag, value);
            has_capacity = true;
        } else {
            throw std::invalid_argument{std::format("unknown option '{}'", flag)};
        }
    }

    if (!has_input || !has_min_price || !has_max_price || !has_capacity) {
        throw std::invalid_argument{"--in, --min-price, --max-price and --capacity are required"};
    }
    return options;
}

template <MatchingBook Book>
void replay(const std::vector<Command>& commands, const BookConfig& config, EventLogSink& sink) {
    Book book{config};
    for (const Command& command : commands) {
        book.apply(command, sink);
    }
}

void run(const Options& options) {
    const std::vector<Command> commands = nanomatch::read_command_log(options.input);
    EventLogSink sink = options.output ? EventLogSink{*options.output} : EventLogSink{};

    if (options.book == "baseline") {
        replay<nanomatch::baseline::BaselineBook<>>(commands, options.config, sink);
    } else {
        throw std::invalid_argument{std::format("unknown book '{}'", options.book)};
    }
    sink.finish();

    std::cout << std::format("commands={} events={} hash=0x{:016x}\n", commands.size(),
                             sink.event_count(), sink.hash());
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const std::span<const char* const> arguments{argv + 1, static_cast<std::size_t>(argc - 1)};
        run(parse_options(arguments));
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "error: " << error.what() << '\n' << kUsage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

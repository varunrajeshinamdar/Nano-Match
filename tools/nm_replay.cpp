// Replays a command log through a book and reports the hash of the resulting event log.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "book_registry.hpp"
#include "cli.hpp"
#include "nanomatch/book.hpp"
#include "nanomatch/log_io.hpp"

namespace {

using nanomatch::BookConfig;
using nanomatch::Command;
using nanomatch::EventLogSink;
using nanomatch::MatchingBook;
using nanomatch::Price;
using nanomatch::tools::Flags;

constexpr std::string_view kUsage =
    "Usage: nm_replay --in <commands> --min-price <ticks> --max-price <ticks> --capacity <orders>\n"
    "                 [--out <events>] [--book <baseline|baseline_pool|fast|fast_4k>]\n";

template <MatchingBook Book>
void replay(const std::vector<Command>& commands, const BookConfig& config, EventLogSink& sink) {
    Book book{config};
    for (const Command& command : commands) {
        book.apply(command, sink);
    }
}

void run(const Flags& flags) {
    const BookConfig config{
        .min_price = Price{flags.required_integer<Price::rep>("--min-price")},
        .max_price = Price{flags.required_integer<Price::rep>("--max-price")},
        .max_resting_orders = flags.required_integer<std::uint32_t>("--capacity")};
    const std::vector<Command> commands =
        nanomatch::read_command_log(std::filesystem::path{flags.required("--in")});
    const std::optional<std::string_view> output = flags.optional("--out");
    EventLogSink sink = output ? EventLogSink{std::filesystem::path{*output}} : EventLogSink{};

    nanomatch::tools::with_book(flags.optional("--book").value_or("baseline"),
                                [&]<MatchingBook Book>() { replay<Book>(commands, config, sink); });
    sink.finish();

    std::cout << std::format("commands={} events={} hash=0x{:016x}\n", commands.size(),
                             sink.event_count(), sink.hash());
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const std::span<const char* const> arguments{argv + 1, static_cast<std::size_t>(argc - 1)};
        run(Flags{arguments,
                  {"--in", "--out", "--book", "--min-price", "--max-price", "--capacity"}});
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "error: " << error.what() << '\n' << kUsage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

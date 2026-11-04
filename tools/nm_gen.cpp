// Generates a command log from a named workload profile, and writes what it contained to
// <out>.json.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cli.hpp"
#include "json_writer.hpp"
#include "nanomatch/log_io.hpp"
#include "nanomatch/protocol.hpp"
#include "workload/generator.hpp"

namespace {

using nanomatch::Command;
using nanomatch::tools::Flags;
using nanomatch::tools::JsonObject;
namespace workload = nanomatch::workload;

constexpr std::string_view kUsage =
    "Usage: nm_gen --profile <name> --seed <n> --commands <n> --out <path>\n"
    "       nm_gen --list\n";

void list_profiles() {
    for (const workload::Profile& profile : workload::kProfiles) {
        const nanomatch::BookConfig config = workload::book_config(profile);
        std::cout << std::format("{} --min-price {} --max-price {} --capacity {}\n", profile.name,
                                 config.min_price.value(), config.max_price.value(),
                                 config.max_resting_orders);
    }
}

[[nodiscard]] JsonObject describe(const workload::Profile& profile, std::uint64_t seed,
                                  const workload::Generator& generator) {
    const workload::Stats& stats = generator.stats();
    const nanomatch::BookConfig config = workload::book_config(profile);
    JsonObject json;
    json.add("profile", profile.name)
        .add("seed", seed)
        .add("min_price", config.min_price.value())
        .add("max_price", config.max_price.value())
        .add("capacity", config.max_resting_orders)
        .add("limit_commands", stats.limit_commands)
        .add("market_commands", stats.market_commands)
        .add("cancel_commands", stats.cancel_commands)
        .add("modify_commands", stats.modify_commands)
        .add("accepted_events", stats.accepted_events)
        .add("rejected_events", stats.rejected_events)
        .add("trade_events", stats.trade_events)
        .add("rested_events", stats.rested_events)
        .add("canceled_events", stats.canceled_events)
        .add("modified_events", stats.modified_events)
        .add("book_full_events", stats.book_full_events)
        .add("traded_quantity", stats.traded_quantity)
        .add("max_resting_orders", stats.max_resting_orders)
        .add("final_resting_orders", generator.resting_orders())
        .add("commands_to_reach_target", stats.commands_to_reach_target)
        .add("lowest_best_bid", stats.lowest_best_bid)
        .add("highest_best_bid", stats.highest_best_bid);
    return json;
}

void generate(const Flags& flags) {
    const std::string_view name = flags.required("--profile");
    const workload::Profile* const profile = workload::find_profile(name);
    if (profile == nullptr) {
        throw std::invalid_argument{std::format("unknown profile '{}'", name)};
    }
    const auto seed = flags.required_integer<std::uint64_t>("--seed");
    const auto count = flags.required_integer<std::size_t>("--commands");
    const std::filesystem::path output{flags.required("--out")};

    workload::Generator generator{*profile, seed};
    std::vector<Command> commands;
    commands.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        commands.push_back(generator.next());
    }
    nanomatch::write_command_log(output, commands);

    std::filesystem::path description_path = output;
    description_path += ".json";
    std::ofstream description{description_path};
    description << describe(*profile, seed, generator).str() << '\n';
    if (!description) {
        throw nanomatch::LogError{"cannot write " + description_path.string()};
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const std::span<const char* const> arguments{argv + 1, static_cast<std::size_t>(argc - 1)};
        if (arguments.size() == 1 && std::string_view{arguments[0]} == "--list") {
            list_profiles();
            return 0;
        }
        generate(Flags{arguments, {"--profile", "--seed", "--commands", "--out"}});
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "error: " << error.what() << '\n' << kUsage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

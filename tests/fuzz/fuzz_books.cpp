// Coverage-guided fuzzing: the optimized and reference books must agree on any input, and neither
// may trip a sanitizer. Most of the input is decoded into plausible commands, because purely random
// bytes are nearly always rejected and would rarely reach the matching logic.

#include <fuzzer/FuzzedDataProvider.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "baseline/baseline_book.hpp"
#include "nanomatch/book.hpp"
#include "nanomatch/order_book.hpp"
#include "nanomatch/protocol.hpp"

namespace {

using nanomatch::Command;
using nanomatch::CommandType;
using nanomatch::Event;
using nanomatch::OrderId;
using nanomatch::Price;
using nanomatch::Quantity;
using nanomatch::Sequence;
using nanomatch::Side;

constexpr int kMaxCommands = 2048;
constexpr std::int64_t kMaxPrice = 40;

struct CollectingSink {
    std::vector<Event> events;
    void on_event(const Event& event) noexcept { events.push_back(event); }
};

[[nodiscard]] Side consume_side(FuzzedDataProvider& input) {
    return input.ConsumeBool() ? Side::kBuy : Side::kSell;
}

[[nodiscard]] Price consume_price(FuzzedDataProvider& input) {
    // Slightly outside the band, so that price rejects are reached too.
    return Price{input.ConsumeIntegralInRange<std::int64_t>(0, kMaxPrice + 1)};
}

[[nodiscard]] Quantity consume_quantity(FuzzedDataProvider& input) {
    return Quantity{input.ConsumeIntegralInRange<std::uint32_t>(0, 16)};
}

/// Keeps targeted IDs close to the newest one, so that many of them refer to resting orders.
[[nodiscard]] OrderId consume_recent_id(FuzzedDataProvider& input, std::uint64_t newest) {
    const auto distance = input.ConsumeIntegralInRange<std::uint64_t>(0, 32);
    return OrderId{newest > distance ? newest - distance : 0};
}

[[nodiscard]] Command consume_command(FuzzedDataProvider& input, std::uint64_t& newest_id) {
    Command command;
    switch (input.ConsumeIntegralInRange<int>(0, 9)) {
        case 0:
        case 1:
        case 2:
        case 3:
            newest_id += input.ConsumeIntegralInRange<std::uint64_t>(0, 2);
            command.order_id = OrderId{newest_id};
            command.type = CommandType::kLimit;
            command.side = consume_side(input);
            command.price = consume_price(input);
            command.quantity = consume_quantity(input);
            break;
        case 4:
            newest_id += input.ConsumeIntegralInRange<std::uint64_t>(0, 2);
            command.order_id = OrderId{newest_id};
            command.type = CommandType::kMarket;
            command.side = consume_side(input);
            command.quantity = consume_quantity(input);
            break;
        case 5:
        case 6:
            command.order_id = consume_recent_id(input, newest_id);
            command.type = CommandType::kCancel;
            break;
        case 7:
        case 8:
            command.order_id = consume_recent_id(input, newest_id);
            command.type = CommandType::kModify;
            command.price = consume_price(input);
            command.quantity = consume_quantity(input);
            break;
        default: {
            // Arbitrary bytes, for whatever validation might not expect.
            std::array<std::uint8_t, sizeof(Command)> bytes{};
            input.ConsumeData(bytes.data(), bytes.size());
            std::memcpy(&command, bytes.data(), sizeof(Command));
            break;
        }
    }
    return command;
}

[[noreturn]] void fail_on_divergence() {
    std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzedDataProvider input{data, size};
    const nanomatch::BookConfig config{
        .min_price = Price{1},
        .max_price = Price{kMaxPrice},
        .max_resting_orders = input.ConsumeIntegralInRange<std::uint32_t>(1, 32)};

    nanomatch::OrderBook<nanomatch::PagePolicy::kDefault> book{config};
    nanomatch::baseline::BaselineBook<> reference{config};
    CollectingSink book_events;
    CollectingSink reference_events;
    std::uint64_t newest_id = 1;

    for (int i = 0; i < kMaxCommands && input.remaining_bytes() > 0; ++i) {
        Command command = consume_command(input, newest_id);
        command.seq = Sequence{static_cast<std::uint64_t>(i) + 1};
        book_events.events.clear();
        reference_events.events.clear();
        book.apply(command, book_events);
        reference.apply(command, reference_events);

        if (book_events.events != reference_events.events ||
            book.best_bid() != reference.best_bid() || book.best_ask() != reference.best_ask() ||
            book.resting_orders() != reference.resting_orders()) {
            fail_on_divergence();
        }
    }
    return 0;
}

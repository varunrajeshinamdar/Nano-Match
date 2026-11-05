#pragma once

#include <cstdint>
#include <ostream>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "baseline/baseline_book.hpp"
#include "baseline/pool_allocator.hpp"
#include "nanomatch/book.hpp"
#include "nanomatch/order_book.hpp"
#include "nanomatch/protocol.hpp"

namespace nanomatch {

// GoogleTest finds this through argument-dependent lookup, so failures show fields rather than
// bytes.
inline void PrintTo(const Event& event, std::ostream* out) {
    *out << "{type=" << static_cast<int>(event.type) << " id=" << event.order_id.value()
         << " counter=" << event.counter_order_id.value() << " price=" << event.price.value()
         << " qty=" << event.quantity.value() << " leaves=" << event.leaves.value()
         << " counter_leaves=" << event.counter_leaves.value()
         << " side=" << static_cast<int>(event.side) << " reason=" << static_cast<int>(event.reason)
         << "}";
}

namespace test {

using Events = std::vector<Event>;

/// Every book implementation listed here runs the full behavioural test suite.
using BookTypes =
    ::testing::Types<baseline::BaselineBook<>, baseline::BaselineBook<baseline::PoolAllocator>,
                     OrderBook<PagePolicy::kHuge>, OrderBook<PagePolicy::kDefault>>;

struct VectorSink {
    Events events;

    void on_event(const Event& event) noexcept { events.push_back(event); }
};

inline constexpr BookConfig kDefaultConfig{
    .min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = 1024};

// Builders for commands. The harness fills in `seq`.

[[nodiscard]] constexpr Command make_limit(std::uint64_t id, Side side, std::int64_t price,
                                           std::uint32_t quantity) noexcept {
    return Command{.order_id = OrderId{id},
                   .price = Price{price},
                   .quantity = Quantity{quantity},
                   .type = CommandType::kLimit,
                   .side = side};
}

[[nodiscard]] constexpr Command make_market(std::uint64_t id, Side side,
                                            std::uint32_t quantity) noexcept {
    return Command{.order_id = OrderId{id},
                   .quantity = Quantity{quantity},
                   .type = CommandType::kMarket,
                   .side = side};
}

[[nodiscard]] constexpr Command make_cancel(std::uint64_t id) noexcept {
    return Command{.order_id = OrderId{id}, .type = CommandType::kCancel};
}

[[nodiscard]] constexpr Command make_modify(std::uint64_t id, std::int64_t price,
                                            std::uint32_t quantity) noexcept {
    return Command{.order_id = OrderId{id},
                   .price = Price{price},
                   .quantity = Quantity{quantity},
                   .type = CommandType::kModify};
}

// Builders for expected events, leaving out the sequence fields that the harness checks itself.

[[nodiscard]] constexpr Event accepted(std::uint64_t id, Side side, std::int64_t price,
                                       std::uint32_t quantity) noexcept {
    return Event{.order_id = OrderId{id},
                 .price = Price{price},
                 .quantity = Quantity{quantity},
                 .leaves = Quantity{quantity},
                 .type = EventType::kAccepted,
                 .side = side};
}

[[nodiscard]] constexpr Event rejected(std::uint64_t id, Side side, std::int64_t price,
                                       std::uint32_t quantity, Reason reason) noexcept {
    return Event{.order_id = OrderId{id},
                 .price = Price{price},
                 .quantity = Quantity{quantity},
                 .type = EventType::kRejected,
                 .side = side,
                 .reason = reason};
}

[[nodiscard]] constexpr Event trade(std::uint64_t id, Side side, std::uint64_t counter_id,
                                    std::int64_t price, std::uint32_t quantity,
                                    std::uint32_t leaves, std::uint32_t counter_leaves) noexcept {
    return Event{.order_id = OrderId{id},
                 .counter_order_id = OrderId{counter_id},
                 .price = Price{price},
                 .quantity = Quantity{quantity},
                 .leaves = Quantity{leaves},
                 .counter_leaves = Quantity{counter_leaves},
                 .type = EventType::kTrade,
                 .side = side};
}

[[nodiscard]] constexpr Event rested(std::uint64_t id, Side side, std::int64_t price,
                                     std::uint32_t quantity) noexcept {
    return Event{.order_id = OrderId{id},
                 .price = Price{price},
                 .quantity = Quantity{quantity},
                 .leaves = Quantity{quantity},
                 .type = EventType::kRested,
                 .side = side};
}

[[nodiscard]] constexpr Event canceled(std::uint64_t id, Side side, std::uint32_t quantity,
                                       Reason reason) noexcept {
    return Event{.order_id = OrderId{id},
                 .quantity = Quantity{quantity},
                 .type = EventType::kCanceled,
                 .side = side,
                 .reason = reason};
}

[[nodiscard]] constexpr Event modified(std::uint64_t id, Side side, std::int64_t price,
                                       std::uint32_t quantity) noexcept {
    return Event{.order_id = OrderId{id},
                 .price = Price{price},
                 .quantity = Quantity{quantity},
                 .leaves = Quantity{quantity},
                 .type = EventType::kModified,
                 .side = side};
}

/// Drives a book one command at a time. It assigns `seq`, checks the sequence fields of each event,
/// and returns events with those fields cleared, so tests can state only what they are testing.
template <MatchingBook Book>
class BookHarness {
  public:
    explicit BookHarness(const BookConfig& config = kDefaultConfig) : book_{config} {}

    Events apply(Command command) {
        command.seq = Sequence{++last_seq_};
        VectorSink sink;
        book_.apply(command, sink);
        for (Event& event : sink.events) {
            EXPECT_EQ(event.event_seq, Sequence{++last_event_seq_});
            EXPECT_EQ(event.command_seq, command.seq);
            event.event_seq = Sequence{};
            event.command_seq = Sequence{};
        }
        return std::move(sink.events);
    }

    Events limit(std::uint64_t id, Side side, std::int64_t price, std::uint32_t quantity) {
        return apply(make_limit(id, side, price, quantity));
    }
    Events market(std::uint64_t id, Side side, std::uint32_t quantity) {
        return apply(make_market(id, side, quantity));
    }
    Events cancel(std::uint64_t id) { return apply(make_cancel(id)); }
    Events modify(std::uint64_t id, std::int64_t price, std::uint32_t quantity) {
        return apply(make_modify(id, price, quantity));
    }

    [[nodiscard]] const Book& book() const noexcept { return book_; }

  private:
    Book book_;
    std::uint64_t last_seq_{0};
    std::uint64_t last_event_seq_{0};
};

}  // namespace test
}  // namespace nanomatch

// Randomized checks of every book implementation against an independent model of docs/SPEC.md.
//
// Example-based tests only cover situations someone thought of. Here a seeded stream of valid and
// invalid commands drives the book, and each command's events are checked against a small model
// that tracks every queue, so a wrong queue position, lost quantity or crossed book is caught when
// it happens. Failures reproduce exactly from the seed.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "test_support.hpp"

namespace nanomatch::test {
namespace {

/// Replays events into per-level queues, checking each one against what the specification requires.
class BookModel {
  public:
    explicit BookModel(const BookConfig& config) : config_{config} {}

    /// Returns a description of the first violation, or nothing if the events are correct.
    [[nodiscard]] std::optional<std::string> check(const Command& command, const Events& events) {
        const Reason expected_reject = expected_reject_for(command);
        if (expected_reject != Reason::kNone) {
            if (events.size() != 1 || events[0].type != EventType::kRejected ||
                events[0].reason != expected_reject) {
                return "expected a single reject with the specified reason";
            }
            const Side expected_side =
                expected_reject == Reason::kMalformed ? Side::kNone : command.side;
            if (events[0].order_id != command.order_id || events[0].price != command.price ||
                events[0].quantity != command.quantity || events[0].side != expected_side ||
                events[0].leaves != Quantity{0}) {
                return "reject must echo the command's fields";
            }
            return std::nullopt;
        }
        if (events.empty()) {
            return "valid command produced no events";
        }

        switch (command.type) {
            case CommandType::kLimit:
            case CommandType::kMarket:
                return check_new_order(command, events);
            case CommandType::kCancel:
                return check_cancel(command, events);
            case CommandType::kModify:
                return check_modify(command, events);
        }
        return "unknown command type passed validation";
    }

    /// Compares the book's observable state with the model.
    template <typename Book>
    [[nodiscard]] std::optional<std::string> check_state(const Book& book) const {
        if (book.resting_orders() != orders_.size()) {
            return "resting order count differs from the model";
        }
        const std::optional<Price> bid =
            bids_.empty() ? std::nullopt : std::optional<Price>{Price{bids_.rbegin()->first}};
        const std::optional<Price> ask =
            asks_.empty() ? std::nullopt : std::optional<Price>{Price{asks_.begin()->first}};
        if (book.best_bid() != bid) {
            return "best bid differs from the model";
        }
        if (book.best_ask() != ask) {
            return "best ask differs from the model";
        }
        if (!bids_.empty() && !asks_.empty() && bids_.rbegin()->first >= asks_.begin()->first) {
            return "book is crossed";
        }
        for (const Side side : {Side::kBuy, Side::kSell}) {
            for (const auto& [price, queue] : levels(side)) {
                std::uint64_t total = 0;
                for (const std::uint64_t id : queue) {
                    total += orders_.at(id).open;
                }
                if (book.open_quantity_at(side, Price{price}) != total) {
                    return "level quantity differs from the model";
                }
            }
        }
        return std::nullopt;
    }

  private:
    struct Order {
        Side side;
        std::int64_t price;
        std::uint32_t open;
    };

    using Levels = std::map<std::int64_t, std::deque<std::uint64_t>>;

    [[nodiscard]] Levels& levels(Side side) { return side == Side::kBuy ? bids_ : asks_; }
    [[nodiscard]] const Levels& levels(Side side) const {
        return side == Side::kBuy ? bids_ : asks_;
    }

    [[nodiscard]] Reason expected_reject_for(const Command& command) {
        if (!is_well_formed(command)) {
            return Reason::kMalformed;
        }
        const bool is_new_order =
            command.type == CommandType::kLimit || command.type == CommandType::kMarket;
        if (is_new_order) {
            if (command.order_id.value() <= highest_id_) {
                return Reason::kDuplicateId;
            }
            highest_id_ = command.order_id.value();
        } else if (!orders_.contains(command.order_id.value())) {
            return Reason::kUnknownId;
        }
        if (command.type != CommandType::kCancel) {
            if (command.type != CommandType::kMarket && !config_.contains(command.price)) {
                return Reason::kInvalidPrice;
            }
            if (command.quantity == Quantity{0}) {
                return Reason::kInvalidQuantity;
            }
        }
        return Reason::kNone;
    }

    void add(std::uint64_t id, Side side, std::int64_t price, std::uint32_t open) {
        orders_[id] = Order{side, price, open};
        levels(side)[price].push_back(id);
    }

    void remove(std::uint64_t id) {
        const Order order = orders_.at(id);
        Levels& side = levels(order.side);
        auto& queue = side.at(order.price);
        queue.erase(std::find(queue.begin(), queue.end(), id));
        if (queue.empty()) {
            side.erase(order.price);
        }
        orders_.erase(id);
    }

    /// Consumes TRADE events from `next`, then the single closing event if one is required.
    [[nodiscard]] std::optional<std::string> check_matching(const Command& command, Side side,
                                                            std::uint32_t quantity, bool is_market,
                                                            const Events& events,
                                                            std::size_t next) {
        std::uint32_t open = quantity;
        Levels& opposite = levels(side == Side::kBuy ? Side::kSell : Side::kBuy);
        for (; next < events.size() && events[next].type == EventType::kTrade; ++next) {
            const Event& event = events[next];
            if (opposite.empty()) {
                return "trade against an empty side";
            }
            const auto best = side == Side::kBuy ? opposite.begin() : std::prev(opposite.end());
            const std::int64_t best_price = best->first;
            const std::uint64_t front = best->second.front();
            if (event.counter_order_id.value() != front) {
                return "trade did not hit the front of the best level (price-time priority)";
            }
            if (event.price.value() != best_price) {
                return "trade price is not the resting price";
            }
            if (!is_market) {
                const std::int64_t limit = command.price.value();
                if (side == Side::kBuy ? best_price > limit : best_price < limit) {
                    return "trade through the limit price";
                }
            }
            Order& resting = orders_.at(front);
            const std::uint32_t traded = std::min(open, resting.open);
            if (event.quantity.value() != traded) {
                return "traded quantity is not the smaller of the two open quantities";
            }
            open -= traded;
            resting.open -= traded;
            if (event.leaves.value() != open || event.counter_leaves.value() != resting.open) {
                return "leaves fields are wrong";
            }
            if (event.side != side || event.order_id != command.order_id) {
                return "trade identifies the wrong aggressor";
            }
            if (resting.open == 0) {
                remove(front);
            }
        }

        if (open == 0) {
            return next == events.size() ? std::nullopt
                                         : std::optional<std::string>{"event after a full fill"};
        }

        // Matching may only stop early if nothing crosses any more.
        if (!opposite.empty() && !is_market) {
            const std::int64_t best =
                side == Side::kBuy ? opposite.begin()->first : opposite.rbegin()->first;
            const std::int64_t limit = command.price.value();
            if (side == Side::kBuy ? best <= limit : best >= limit) {
                return "matching stopped while the book still crossed";
            }
        }
        if (!opposite.empty() && is_market) {
            return "market order stopped with liquidity remaining";
        }
        if (next + 1 != events.size()) {
            return "expected exactly one closing event";
        }

        const Event& closing = events[next];
        if (closing.quantity.value() != open || closing.order_id != command.order_id ||
            closing.side != side) {
            return "closing event has the wrong order, side or quantity";
        }
        if (is_market) {
            if (closing.type != EventType::kCanceled ||
                closing.reason != Reason::kMarketRemainder) {
                return "market remainder must be cancelled";
            }
            return std::nullopt;
        }
        const bool slot_available =
            command.type == CommandType::kModify || orders_.size() < config_.max_resting_orders;
        if (slot_available) {
            if (closing.type != EventType::kRested || closing.price != command.price) {
                return "remainder should have rested";
            }
            add(command.order_id.value(), side, command.price.value(), open);
        } else if (closing.type != EventType::kCanceled || closing.reason != Reason::kBookFull) {
            return "remainder should have been cancelled because the book is full";
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> check_new_order(const Command& command,
                                                             const Events& events) {
        if (events[0].type != EventType::kAccepted || events[0].quantity != command.quantity) {
            return "first event of a new order must be ACCEPTED";
        }
        return check_matching(command, command.side, command.quantity.value(),
                              command.type == CommandType::kMarket, events, 1);
    }

    [[nodiscard]] std::optional<std::string> check_cancel(const Command& command,
                                                          const Events& events) {
        const std::uint64_t id = command.order_id.value();
        const Order order = orders_.at(id);
        if (events.size() != 1 || events[0].type != EventType::kCanceled ||
            events[0].reason != Reason::kUserCancel || events[0].quantity.value() != order.open ||
            events[0].side != order.side) {
            return "cancel must emit one CANCELED event with the open quantity";
        }
        remove(id);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> check_modify(const Command& command,
                                                          const Events& events) {
        const std::uint64_t id = command.order_id.value();
        const Order order = orders_.at(id);
        if (events[0].type != EventType::kModified || events[0].side != order.side ||
            events[0].price != command.price || events[0].quantity != command.quantity) {
            return "first event of a modify must be MODIFIED";
        }
        const bool keeps_priority =
            command.price.value() == order.price && command.quantity.value() <= order.open;
        if (keeps_priority) {
            if (events.size() != 1) {
                return "a modify that keeps priority emits only MODIFIED";
            }
            orders_.at(id).open = command.quantity.value();
            return std::nullopt;
        }
        remove(id);
        return check_matching(command, order.side, command.quantity.value(), false, events, 1);
    }

    BookConfig config_;
    std::map<std::uint64_t, Order> orders_;
    Levels bids_;
    Levels asks_;
    std::uint64_t highest_id_{0};
};

/// Seeded stream mixing normal flow with every kind of invalid command.
class CommandStream {
  public:
    explicit CommandStream(std::uint64_t seed) : rng_{seed} {}

    [[nodiscard]] Command next() {
        Command command = draw();
        command.seq = Sequence{++seq_};
        return command;
    }

  private:
    [[nodiscard]] std::uint64_t below(std::uint64_t limit) { return rng_() % limit; }

    [[nodiscard]] std::uint32_t small_quantity() {
        return static_cast<std::uint32_t>(1 + below(20));
    }

    [[nodiscard]] std::int64_t price_near_mid() {
        return 500 - 15 + static_cast<std::int64_t>(below(31));
    }

    [[nodiscard]] Side random_side() { return below(2) == 0 ? Side::kBuy : Side::kSell; }

    [[nodiscard]] std::uint64_t fresh_id() {
        next_id_ += 1 + below(3);
        return next_id_;
    }

    // Recent IDs are a mix of live and departed orders, which exercises both outcomes.
    [[nodiscard]] std::uint64_t recent_id() {
        return next_id_ == 0 ? 1 : next_id_ - std::min(next_id_ - 1, below(100));
    }

    [[nodiscard]] Command draw() {
        const std::uint64_t roll = below(100);
        if (roll < 50) {
            return make_limit(fresh_id(), random_side(), price_near_mid(), small_quantity());
        }
        if (roll < 55) {
            return make_market(fresh_id(), random_side(), small_quantity());
        }
        if (roll < 75) {
            return make_cancel(recent_id());
        }
        if (roll < 92) {
            return make_modify(recent_id(), price_near_mid(), small_quantity());
        }
        return draw_invalid();
    }

    [[nodiscard]] Command draw_invalid() {
        switch (below(6)) {
            case 0:
                return make_limit(recent_id(), random_side(), price_near_mid(), small_quantity());
            case 1:
                return make_limit(fresh_id(), random_side(), 5000, small_quantity());
            case 2:
                return make_limit(fresh_id(), random_side(), price_near_mid(), 0);
            case 3:
                return make_modify(recent_id(), price_near_mid(), 0);
            case 4: {
                Command command = make_limit(fresh_id(), Side::kNone, price_near_mid(), 1);
                command.reserved = 1;
                return command;
            }
            default: {
                Command command = make_cancel(recent_id());
                command.type = static_cast<CommandType>(77);
                return command;
            }
        }
    }

    std::mt19937_64 rng_;
    std::uint64_t seq_{0};
    std::uint64_t next_id_{0};
};

constexpr int kCommandsPerRun = 50'000;
constexpr int kStateCheckInterval = 64;

template <typename Book>
class BookInvariantsTest : public ::testing::Test {
  protected:
    static void run_against_model(std::uint64_t seed, std::uint32_t capacity) {
        const BookConfig config{
            .min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = capacity};
        Book book{config};
        BookModel model{config};
        CommandStream stream{seed};
        std::uint64_t last_event_seq = 0;

        for (int i = 0; i < kCommandsPerRun; ++i) {
            const Command command = stream.next();
            VectorSink sink;
            book.apply(command, sink);

            for (const Event& event : sink.events) {
                ASSERT_EQ(event.event_seq.value(), ++last_event_seq);
                ASSERT_EQ(event.command_seq, command.seq);
                ASSERT_EQ(event.reserved, 0);
            }
            const std::optional<std::string> violation = model.check(command, sink.events);
            ASSERT_EQ(violation, std::nullopt)
                << std::format("seed={} capacity={} command={}", seed, capacity, i);

            if (i % kStateCheckInterval == 0 || i == kCommandsPerRun - 1) {
                const std::optional<std::string> state = model.check_state(book);
                ASSERT_EQ(state, std::nullopt)
                    << std::format("seed={} capacity={} command={}", seed, capacity, i);
            }
        }
    }

    [[nodiscard]] static Events run_collecting_events(std::uint64_t seed) {
        Book book{kDefaultConfig};
        CommandStream stream{seed};
        VectorSink sink;
        for (int i = 0; i < kCommandsPerRun; ++i) {
            book.apply(stream.next(), sink);
        }
        return std::move(sink.events);
    }
};

TYPED_TEST_SUITE(BookInvariantsTest, BookTypes);

TYPED_TEST(BookInvariantsTest, AgreesWithModelOnRoomyBook) {
    for (const std::uint64_t seed : {1U, 2U, 3U}) {
        this->run_against_model(seed, 4096);
        if (this->HasFatalFailure()) {
            return;
        }
    }
}

TYPED_TEST(BookInvariantsTest, AgreesWithModelWhenCapacityIsScarce) {
    for (const std::uint64_t seed : {4U, 5U, 6U}) {
        this->run_against_model(seed, 8);
        if (this->HasFatalFailure()) {
            return;
        }
    }
}

TYPED_TEST(BookInvariantsTest, SameInputProducesIdenticalBytes) {
    const Events first = this->run_collecting_events(42);
    const Events second = this->run_collecting_events(42);
    ASSERT_EQ(first.size(), second.size());
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(std::memcmp(first.data(), second.data(), first.size() * sizeof(Event)), 0);
}

TYPED_TEST(BookInvariantsTest, DifferentInputProducesDifferentEvents) {
    // Guards against the determinism test passing because the stream ignores its seed.
    EXPECT_NE(this->run_collecting_events(42), this->run_collecting_events(43));
}

}  // namespace
}  // namespace nanomatch::test

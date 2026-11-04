#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "nanomatch/book.hpp"
#include "nanomatch/protocol.hpp"
#include "nanomatch/types.hpp"

namespace nanomatch::baseline {

/// Reference implementation of docs/SPEC.md using standard containers.
///
/// This is deliberately the straightforward design: a tree of price levels, a linked list per
/// level, a hash map from ID to position, and a heap allocation for each node. It exists to give
/// the optimized book a behavioural reference for differential testing and a cost reference for
/// benchmarks, so clarity comes first here and it is not tuned.
///
/// `Alloc` allows the same containers to run on a different allocator, which separates the effect
/// of allocation from the effect of data structure choice when comparing against the optimized
/// book.
///
/// `apply` is noexcept because a failed allocation has no defined outcome in the specification. It
/// terminates the process.
template <template <typename> class Alloc = std::allocator>
class BaselineBook {
  public:
    explicit BaselineBook(const BookConfig& config) : config_{config} { validate_config(config); }

    template <EventSink Sink>
    void apply(const Command& command, Sink& sink) noexcept {
        switch (command.type) {
            case CommandType::kLimit:
            case CommandType::kMarket:
                apply_new_order(command, sink);
                return;
            case CommandType::kCancel:
                apply_cancel(command, sink);
                return;
            case CommandType::kModify:
                apply_modify(command, sink);
                return;
        }
        reject(command, Reason::kMalformed, sink);
    }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept { return best_of(bids_); }
    [[nodiscard]] std::optional<Price> best_ask() const noexcept { return best_of(asks_); }

    [[nodiscard]] std::uint64_t open_quantity_at(Side side, Price price) const noexcept {
        return side == Side::kBuy ? open_quantity_in(bids_, price) : open_quantity_in(asks_, price);
    }

    [[nodiscard]] std::uint32_t resting_orders() const noexcept {
        return static_cast<std::uint32_t>(index_.size());
    }

  private:
    struct RestingOrder {
        OrderId id{};
        Quantity open{};
    };

    using Level = std::list<RestingOrder, Alloc<RestingOrder>>;

    template <typename Compare>
    using SideMap = std::map<Price, Level, Compare, Alloc<std::pair<const Price, Level>>>;

    // Ordering each side so that begin() is its best price lets matching share one code path.
    using Bids = SideMap<std::greater<>>;
    using Asks = SideMap<std::less<>>;

    struct Location {
        Side side{};
        Price price{};
        typename Level::iterator position{};
    };

    struct OrderIdHash {
        [[nodiscard]] std::size_t operator()(OrderId id) const noexcept {
            return std::hash<std::uint64_t>{}(id.value());
        }
    };

    // Never iterated, so its address-dependent and unspecified ordering can't reach the output.
    using Index = std::unordered_map<OrderId, Location, OrderIdHash, std::equal_to<>,
                                     Alloc<std::pair<const OrderId, Location>>>;

    /// The incoming side of a match: a new order, or a modified order that lost its priority.
    struct Aggressor {
        Sequence command_seq{};
        OrderId id{};
        Side side{};
        Price limit{};
        bool is_market{};
    };

    template <typename Map>
    [[nodiscard]] static std::optional<Price> best_of(const Map& side) noexcept {
        return side.empty() ? std::nullopt : std::optional<Price>{side.begin()->first};
    }

    template <typename Map>
    [[nodiscard]] static std::uint64_t open_quantity_in(const Map& side, Price price) noexcept {
        const auto level = side.find(price);
        if (level == side.end()) {
            return 0;
        }
        std::uint64_t total = 0;
        for (const RestingOrder& order : level->second) {
            total += order.open.value();
        }
        return total;
    }

    template <EventSink Sink>
    void emit(Event event, Sink& sink) noexcept {
        event.event_seq = Sequence{++last_event_seq_};
        sink.on_event(event);
    }

    template <EventSink Sink>
    void reject(const Command& command, Reason reason, Sink& sink) noexcept {
        emit(Event{.command_seq = command.seq,
                   .order_id = command.order_id,
                   .price = command.price,
                   .quantity = command.quantity,
                   .type = EventType::kRejected,
                   .side = reason == Reason::kMalformed ? Side::kNone : command.side,
                   .reason = reason},
             sink);
    }

    template <EventSink Sink>
    void emit_canceled(Sequence command_seq, OrderId id, Side side, Quantity quantity,
                       Reason reason, Sink& sink) noexcept {
        emit(Event{.command_seq = command_seq,
                   .order_id = id,
                   .quantity = quantity,
                   .type = EventType::kCanceled,
                   .side = side,
                   .reason = reason},
             sink);
    }

    template <EventSink Sink>
    void emit_rested(const Aggressor& order, Quantity open, Sink& sink) noexcept {
        emit(Event{.command_seq = order.command_seq,
                   .order_id = order.id,
                   .price = order.limit,
                   .quantity = open,
                   .leaves = open,
                   .type = EventType::kRested,
                   .side = order.side},
             sink);
    }

    template <EventSink Sink>
    void apply_new_order(const Command& command, Sink& sink) noexcept {
        if (!is_well_formed(command)) {
            reject(command, Reason::kMalformed, sink);
            return;
        }
        if (command.order_id <= highest_order_id_) {
            reject(command, Reason::kDuplicateId, sink);
            return;
        }
        highest_order_id_ = command.order_id;

        const bool is_market = command.type == CommandType::kMarket;
        if (!is_market && !config_.contains(command.price)) {
            reject(command, Reason::kInvalidPrice, sink);
            return;
        }
        if (command.quantity == Quantity{0}) {
            reject(command, Reason::kInvalidQuantity, sink);
            return;
        }

        emit(Event{.command_seq = command.seq,
                   .order_id = command.order_id,
                   .price = command.price,
                   .quantity = command.quantity,
                   .leaves = command.quantity,
                   .type = EventType::kAccepted,
                   .side = command.side},
             sink);

        const Aggressor order{.command_seq = command.seq,
                              .id = command.order_id,
                              .side = command.side,
                              .limit = command.price,
                              .is_market = is_market};
        const Quantity open = match(order, command.quantity, sink);
        if (open == Quantity{0}) {
            return;
        }
        if (is_market) {
            emit_canceled(command.seq, command.order_id, command.side, open,
                          Reason::kMarketRemainder, sink);
        } else if (index_.size() >= config_.max_resting_orders) {
            emit_canceled(command.seq, command.order_id, command.side, open, Reason::kBookFull,
                          sink);
        } else {
            rest(order, open);
            emit_rested(order, open, sink);
        }
    }

    template <EventSink Sink>
    void apply_cancel(const Command& command, Sink& sink) noexcept {
        if (!is_well_formed(command)) {
            reject(command, Reason::kMalformed, sink);
            return;
        }
        const auto found = index_.find(command.order_id);
        if (found == index_.end()) {
            reject(command, Reason::kUnknownId, sink);
            return;
        }
        const Location location = found->second;
        emit_canceled(command.seq, command.order_id, location.side, location.position->open,
                      Reason::kUserCancel, sink);
        remove(found);
    }

    template <EventSink Sink>
    void apply_modify(const Command& command, Sink& sink) noexcept {
        if (!is_well_formed(command)) {
            reject(command, Reason::kMalformed, sink);
            return;
        }
        const auto found = index_.find(command.order_id);
        if (found == index_.end()) {
            reject(command, Reason::kUnknownId, sink);
            return;
        }
        if (!config_.contains(command.price)) {
            reject(command, Reason::kInvalidPrice, sink);
            return;
        }
        if (command.quantity == Quantity{0}) {
            reject(command, Reason::kInvalidQuantity, sink);
            return;
        }

        const Location location = found->second;
        emit(Event{.command_seq = command.seq,
                   .order_id = command.order_id,
                   .price = command.price,
                   .quantity = command.quantity,
                   .leaves = command.quantity,
                   .type = EventType::kModified,
                   .side = location.side},
             sink);

        const bool keeps_priority =
            command.price == location.price && command.quantity <= location.position->open;
        if (keeps_priority) {
            location.position->open = command.quantity;
            return;
        }

        // Removing first means the order can't match against itself, and the slot it held is what
        // guarantees the remainder fits, so a modify never hits the capacity limit.
        remove(found);
        const Aggressor order{.command_seq = command.seq,
                              .id = command.order_id,
                              .side = location.side,
                              .limit = command.price,
                              .is_market = false};
        const Quantity open = match(order, command.quantity, sink);
        if (open != Quantity{0}) {
            rest(order, open);
            emit_rested(order, open, sink);
        }
    }

    /// Returns the quantity left over after trading against the opposite side.
    template <EventSink Sink>
    [[nodiscard]] Quantity match(const Aggressor& order, Quantity quantity, Sink& sink) noexcept {
        return order.side == Side::kBuy ? match_against(asks_, order, quantity, sink)
                                        : match_against(bids_, order, quantity, sink);
    }

    template <typename Map, EventSink Sink>
    [[nodiscard]] Quantity match_against(Map& opposite, const Aggressor& order, Quantity quantity,
                                         Sink& sink) noexcept {
        Quantity open = quantity;
        auto level = opposite.begin();
        while (open != Quantity{0} && level != opposite.end() && crosses(order, level->first)) {
            Level& queue = level->second;
            while (open != Quantity{0} && !queue.empty()) {
                RestingOrder& resting = queue.front();
                const Quantity traded = std::min(open, resting.open);
                open -= traded;
                resting.open -= traded;
                emit(Event{.command_seq = order.command_seq,
                           .order_id = order.id,
                           .counter_order_id = resting.id,
                           .price = level->first,
                           .quantity = traded,
                           .leaves = open,
                           .counter_leaves = resting.open,
                           .type = EventType::kTrade,
                           .side = order.side},
                     sink);
                if (resting.open == Quantity{0}) {
                    index_.erase(resting.id);
                    queue.pop_front();
                }
            }
            if (queue.empty()) {
                level = opposite.erase(level);
            }
        }
        return open;
    }

    [[nodiscard]] static bool crosses(const Aggressor& order, Price resting_price) noexcept {
        if (order.is_market) {
            return true;
        }
        return order.side == Side::kBuy ? resting_price <= order.limit
                                        : resting_price >= order.limit;
    }

    void rest(const Aggressor& order, Quantity open) {
        Level& queue = order.side == Side::kBuy ? bids_[order.limit] : asks_[order.limit];
        queue.push_back(RestingOrder{order.id, open});
        index_.emplace(order.id, Location{order.side, order.limit, std::prev(queue.end())});
    }

    void remove(typename Index::iterator found) noexcept {
        const Location location = found->second;
        if (location.side == Side::kBuy) {
            remove_from(bids_, location);
        } else {
            remove_from(asks_, location);
        }
        index_.erase(found);
    }

    template <typename Map>
    static void remove_from(Map& side, const Location& location) noexcept {
        const auto level = side.find(location.price);
        level->second.erase(location.position);
        if (level->second.empty()) {
            side.erase(level);
        }
    }

    BookConfig config_;
    Bids bids_;
    Asks asks_;
    Index index_;
    OrderId highest_order_id_{};
    std::uint64_t last_event_seq_{0};
};

static_assert(MatchingBook<BaselineBook<>>);

}  // namespace nanomatch::baseline

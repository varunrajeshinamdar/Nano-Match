#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "nanomatch/book.hpp"
#include "nanomatch/fixed_array.hpp"
#include "nanomatch/id_index.hpp"
#include "nanomatch/level_bitmap.hpp"
#include "nanomatch/order_pool.hpp"
#include "nanomatch/protocol.hpp"
#include "nanomatch/types.hpp"

namespace nanomatch {

/// Widest price band a book accepts. Each tick in the band costs 16 bytes of ladder whether or not
/// it's used, so an absurd band should fail clearly at construction rather than exhaust memory.
inline constexpr std::uint64_t kMaxPriceLevels = std::uint64_t{1} << 26U;

/// Implementation of docs/SPEC.md designed for low and predictable latency.
///
/// All memory is mapped and touched in the constructor. After that, `apply` makes no allocations
/// and no system calls, so its cost doesn't depend on allocator state or page faults.
///
/// - Orders live in a fixed pool and are linked by index into a FIFO queue at their level, so
///   appending and unlinking are O(1) and touch at most the two neighbouring orders.
/// - Levels live in a dense array indexed by price. One array serves both sides because the book is
///   never crossed between commands, so a price can only hold orders for one side at a time.
/// - A bitmap per side finds the next occupied level when the best empties.
/// - An open-addressing table maps IDs to pool slots for cancels and modifies.
///
/// Costs: new orders, cancels and modifies are O(1) plus O(1) per order traded against. Memory is
/// proportional to capacity plus band width, regardless of how much of either is in use.
///
/// Not thread-safe. Each book belongs to one thread.
template <PagePolicy Pages = PagePolicy::kHuge>
class OrderBook {
  public:
    /// Throws std::invalid_argument for unusable limits, and std::bad_alloc if memory can't be
    /// mapped.
    explicit OrderBook(const BookConfig& config)
        : config_{validated(config)},
          level_count_{level_count_of(config)},
          pool_{config.max_resting_orders, Pages},
          index_{config.max_resting_orders, Pages},
          levels_{level_count_, Pages},
          occupied_{LevelBitmap{level_count_, Pages}, LevelBitmap{level_count_, Pages}} {}

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

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        return price_or_none(best_bid_);
    }
    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        return price_or_none(best_ask_);
    }

    [[nodiscard]] std::uint64_t open_quantity_at(Side side, Price price) const noexcept {
        if (!config_.contains(price)) {
            return 0;
        }
        const std::uint32_t level = level_of(price);
        return occupied_[side_index(side)].test(level) ? levels_[level].total_open : 0;
    }

    [[nodiscard]] std::uint32_t resting_orders() const noexcept { return pool_.in_use(); }

  private:
    struct PriceLevel {
        std::uint32_t head{kNullIndex};
        std::uint32_t tail{kNullIndex};
        std::uint64_t total_open{0};
    };

    static_assert(sizeof(PriceLevel) == 16);

    /// The incoming side of a match: a new order, or a modified order that lost its priority.
    struct Aggressor {
        Sequence command_seq{};
        OrderId id{};
        Price limit{};
        std::uint32_t limit_level{};
    };

    static constexpr std::uint32_t kNoLevel = LevelBitmap::kNone;

    [[nodiscard]] static const BookConfig& validated(const BookConfig& config) {
        validate_config(config);
        if (level_span(config) > kMaxPriceLevels) {
            throw std::invalid_argument{"price band is wider than kMaxPriceLevels"};
        }
        if (config.max_resting_orders > kMaxOrders) {
            throw std::invalid_argument{"max_resting_orders is larger than the pool can index"};
        }
        return config;
    }

    /// Below the null index, and a size whose doubled hash table still fits.
    static constexpr std::uint32_t kMaxOrders = std::uint32_t{1} << 31U;

    [[nodiscard]] static std::uint64_t level_span(const BookConfig& config) noexcept {
        return static_cast<std::uint64_t>(config.max_price.value() - config.min_price.value()) + 1;
    }

    [[nodiscard]] static std::uint32_t level_count_of(const BookConfig& config) noexcept {
        return static_cast<std::uint32_t>(level_span(config));
    }

    [[nodiscard]] static constexpr std::size_t side_index(Side side) noexcept {
        return side == Side::kBuy ? 0 : 1;
    }

    [[nodiscard]] std::uint32_t level_of(Price price) const noexcept {
        return static_cast<std::uint32_t>(price.value() - config_.min_price.value());
    }

    [[nodiscard]] Price price_of(std::uint32_t level) const noexcept {
        return Price{config_.min_price.value() + static_cast<Price::rep>(level)};
    }

    [[nodiscard]] std::optional<Price> price_or_none(std::uint32_t level) const noexcept {
        return level == kNoLevel ? std::nullopt : std::optional<Price>{price_of(level)};
    }

    [[nodiscard]] std::uint32_t find_resting(OrderId id) const noexcept {
        return index_.find(id, [this, id](std::uint32_t slot) { return pool_[slot].id == id; });
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
    void apply_new_order(const Command& command, Sink& sink) noexcept {
        if (!is_well_formed(command)) [[unlikely]] {
            reject(command, Reason::kMalformed, sink);
            return;
        }
        if (command.order_id <= highest_order_id_) [[unlikely]] {
            reject(command, Reason::kDuplicateId, sink);
            return;
        }
        highest_order_id_ = command.order_id;

        const bool is_market = command.type == CommandType::kMarket;
        if (!is_market && !config_.contains(command.price)) [[unlikely]] {
            reject(command, Reason::kInvalidPrice, sink);
            return;
        }
        if (command.quantity == Quantity{0}) [[unlikely]] {
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

        if (command.side == Side::kBuy) {
            process_new_order<Side::kBuy>(command, is_market, sink);
        } else {
            process_new_order<Side::kSell>(command, is_market, sink);
        }
    }

    /// `S` is a template parameter so that each side gets its own code. The comparisons then
    /// compile to fixed instructions, leaving only one side branch per command rather than several
    /// in the loop.
    template <Side S, EventSink Sink>
    void process_new_order(const Command& command, bool is_market, Sink& sink) noexcept {
        // A market order crosses at every price, which is the same as a limit at the far end of the
        // band.
        const std::uint32_t limit_level =
            is_market ? (S == Side::kBuy ? level_count_ - 1 : 0) : level_of(command.price);
        const Aggressor order{.command_seq = command.seq,
                              .id = command.order_id,
                              .limit = command.price,
                              .limit_level = limit_level};
        const Quantity open = match<S>(order, command.quantity, sink);
        if (open == Quantity{0}) {
            return;
        }
        if (is_market) {
            emit_canceled(command.seq, command.order_id, S, open, Reason::kMarketRemainder, sink);
            return;
        }
        if (pool_.full()) [[unlikely]] {
            emit_canceled(command.seq, command.order_id, S, open, Reason::kBookFull, sink);
            return;
        }
        const std::uint32_t slot = pool_.acquire();
        OrderNode& node = pool_[slot];
        node.id = command.order_id;
        node.side = S;
        index_.insert(command.order_id, slot);
        rest<S>(order, slot, open, sink);
    }

    template <EventSink Sink>
    void apply_cancel(const Command& command, Sink& sink) noexcept {
        if (!is_well_formed(command)) [[unlikely]] {
            reject(command, Reason::kMalformed, sink);
            return;
        }
        const std::uint32_t slot = find_resting(command.order_id);
        if (slot == IdIndex::kNotFound) [[unlikely]] {
            reject(command, Reason::kUnknownId, sink);
            return;
        }
        const OrderNode& node = pool_[slot];
        emit_canceled(command.seq, command.order_id, node.side, node.open, Reason::kUserCancel,
                      sink);
        unlink(slot);
        index_.erase(command.order_id, slot);
        pool_.release(slot);
    }

    template <EventSink Sink>
    void apply_modify(const Command& command, Sink& sink) noexcept {
        if (!is_well_formed(command)) [[unlikely]] {
            reject(command, Reason::kMalformed, sink);
            return;
        }
        const std::uint32_t slot = find_resting(command.order_id);
        if (slot == IdIndex::kNotFound) [[unlikely]] {
            reject(command, Reason::kUnknownId, sink);
            return;
        }
        if (!config_.contains(command.price)) [[unlikely]] {
            reject(command, Reason::kInvalidPrice, sink);
            return;
        }
        if (command.quantity == Quantity{0}) [[unlikely]] {
            reject(command, Reason::kInvalidQuantity, sink);
            return;
        }

        OrderNode& node = pool_[slot];
        const Side side = node.side;
        emit(Event{.command_seq = command.seq,
                   .order_id = command.order_id,
                   .price = command.price,
                   .quantity = command.quantity,
                   .leaves = command.quantity,
                   .type = EventType::kModified,
                   .side = side},
             sink);

        const std::uint32_t new_level = level_of(command.price);
        if (new_level == node.level && command.quantity <= node.open) {
            levels_[new_level].total_open -= (node.open - command.quantity).value();
            node.open = command.quantity;
            return;
        }

        // Unlinking first means the order can't match against itself. It keeps its pool slot and
        // index entry while matching, which is what guarantees that its remainder always has room
        // to rest.
        unlink(slot);
        const Aggressor order{.command_seq = command.seq,
                              .id = command.order_id,
                              .limit = command.price,
                              .limit_level = new_level};
        if (side == Side::kBuy) {
            finish_modify<Side::kBuy>(order, slot, command.quantity, sink);
        } else {
            finish_modify<Side::kSell>(order, slot, command.quantity, sink);
        }
    }

    template <Side S, EventSink Sink>
    void finish_modify(const Aggressor& order, std::uint32_t slot, Quantity quantity,
                       Sink& sink) noexcept {
        const Quantity open = match<S>(order, quantity, sink);
        if (open == Quantity{0}) {
            index_.erase(order.id, slot);
            pool_.release(slot);
            return;
        }
        rest<S>(order, slot, open, sink);
    }

    /// Trades against the opposite side, and returns the quantity left over.
    template <Side S, EventSink Sink>
    [[nodiscard]] Quantity match(const Aggressor& order, Quantity quantity, Sink& sink) noexcept {
        constexpr Side kOpposite = S == Side::kBuy ? Side::kSell : Side::kBuy;
        std::uint32_t& best = S == Side::kBuy ? best_ask_ : best_bid_;

        Quantity open = quantity;
        while (open != Quantity{0} && crosses<S>(best, order.limit_level)) {
            PriceLevel& level = levels_[best];
            const Price price = price_of(best);
            while (open != Quantity{0} && level.head != kNullIndex) {
                const std::uint32_t slot = level.head;
                OrderNode& resting = pool_[slot];
                const Quantity traded = std::min(open, resting.open);
                open -= traded;
                resting.open -= traded;
                level.total_open -= traded.value();
                emit(Event{.command_seq = order.command_seq,
                           .order_id = order.id,
                           .counter_order_id = resting.id,
                           .price = price,
                           .quantity = traded,
                           .leaves = open,
                           .counter_leaves = resting.open,
                           .type = EventType::kTrade,
                           .side = S},
                     sink);
                if (resting.open == Quantity{0}) {
                    level.head = resting.next;
                    if (level.head == kNullIndex) {
                        level.tail = kNullIndex;
                    } else {
                        pool_[level.head].prev = kNullIndex;
                    }
                    index_.erase(resting.id, slot);
                    pool_.release(slot);
                }
            }
            if (level.head == kNullIndex) {
                best = remove_best_level<kOpposite>(best);
            }
        }
        return open;
    }

    /// Whether an aggressor on side `S`, limited to `limit_level`, crosses the opposite `best`
    /// level.
    template <Side S>
    [[nodiscard]] static bool crosses(std::uint32_t best, std::uint32_t limit_level) noexcept {
        if constexpr (S == Side::kBuy) {
            // kNoLevel is the largest possible value, so an empty ask side never crosses.
            return best <= limit_level;
        } else {
            return best != kNoLevel && best >= limit_level;
        }
    }

    /// Marks the emptied best level of side `S` unoccupied, and returns the side's new best level.
    template <Side S>
    [[nodiscard]] std::uint32_t remove_best_level(std::uint32_t level) noexcept {
        LevelBitmap& occupied = occupied_[side_index(S)];
        occupied.clear(level);
        return S == Side::kBuy ? occupied.first_below(level) : occupied.first_above(level);
    }

    /// Appends the node in `slot` to the queue at the order's limit price.
    template <Side S, EventSink Sink>
    void rest(const Aggressor& order, std::uint32_t slot, Quantity open, Sink& sink) noexcept {
        const std::uint32_t level_index = order.limit_level;
        PriceLevel& level = levels_[level_index];
        OrderNode& node = pool_[slot];
        node.open = open;
        node.level = level_index;
        node.next = kNullIndex;
        node.prev = level.tail;
        if (level.tail == kNullIndex) {
            level.head = slot;
            occupied_[side_index(S)].set(level_index);
            if constexpr (S == Side::kBuy) {
                best_bid_ = best_bid_ == kNoLevel ? level_index : std::max(best_bid_, level_index);
            } else {
                // kNoLevel is the largest possible value, so this also covers an empty ask side.
                best_ask_ = std::min(best_ask_, level_index);
            }
        } else {
            pool_[level.tail].next = slot;
        }
        level.tail = slot;
        level.total_open += open.value();

        emit(Event{.command_seq = order.command_seq,
                   .order_id = order.id,
                   .price = order.limit,
                   .quantity = open,
                   .leaves = open,
                   .type = EventType::kRested,
                   .side = S},
             sink);
    }

    /// Removes a resting order from its queue, updating the best price if its level empties.
    void unlink(std::uint32_t slot) noexcept {
        const OrderNode& node = pool_[slot];
        PriceLevel& level = levels_[node.level];
        if (node.prev == kNullIndex) {
            level.head = node.next;
        } else {
            pool_[node.prev].next = node.next;
        }
        if (node.next == kNullIndex) {
            level.tail = node.prev;
        } else {
            pool_[node.next].prev = node.prev;
        }
        level.total_open -= node.open.value();

        if (level.head == kNullIndex) {
            if (node.side == Side::kBuy) {
                occupied_[side_index(Side::kBuy)].clear(node.level);
                if (best_bid_ == node.level) {
                    best_bid_ = occupied_[side_index(Side::kBuy)].first_below(node.level);
                }
            } else {
                occupied_[side_index(Side::kSell)].clear(node.level);
                if (best_ask_ == node.level) {
                    best_ask_ = occupied_[side_index(Side::kSell)].first_above(node.level);
                }
            }
        }
    }

    BookConfig config_;
    std::uint32_t level_count_;
    std::uint32_t best_bid_{kNoLevel};
    std::uint32_t best_ask_{kNoLevel};
    OrderId highest_order_id_{};
    std::uint64_t last_event_seq_{0};
    OrderPool pool_;
    IdIndex index_;
    FixedArray<PriceLevel> levels_;
    std::array<LevelBitmap, 2> occupied_;
};

static_assert(MatchingBook<OrderBook<>>);

}  // namespace nanomatch

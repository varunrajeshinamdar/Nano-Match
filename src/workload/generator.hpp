#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "baseline/baseline_book.hpp"
#include "nanomatch/book.hpp"
#include "nanomatch/protocol.hpp"
#include "nanomatch/types.hpp"
#include "workload/rng.hpp"

/// Synthetic order flow for tests and benchmarks.
///
/// This follows the zero-intelligence style of model from the market microstructure literature:
/// orders arrive at random, are placed mostly near a reference price that wanders, and rest until
/// they trade or are cancelled at random. Actual markets are more structured, so these workloads
/// are stated assumptions rather than a calibrated model of any venue. docs/BENCHMARKS.md describes
/// them.

namespace nanomatch::workload {

inline constexpr Price kMinPrice{1};
inline constexpr Price kMaxPrice{1 << 20};
inline constexpr Price kInitialMid{1 << 19};

/// Parameters of a workload. All probabilities are integers in parts per 1000, so that generation
/// involves no floating point. The first five fields are the steady-state command mix and sum to
/// 1000.
struct Profile {
    std::string_view name;
    std::uint32_t passive_limit{};
    std::uint32_t crossing_limit{};
    std::uint32_t market{};
    std::uint32_t cancel{};
    std::uint32_t modify{};
    /// Share of cancels and modifies aimed at an order that has already left the book, as happens
    /// when a cancel races a fill.
    std::uint32_t stale_target{};
    /// The book is built up to this many resting orders, then held within 10% of it.
    std::uint32_t target_resting_orders{};
    /// Scales the size of crossing and market orders, which controls how far they walk the book.
    std::uint32_t aggressive_size_multiplier{1};
};

inline constexpr std::array<Profile, 6> kProfiles{{
    {.name = "realistic",
     .passive_limit = 480,
     .crossing_limit = 30,
     .market = 10,
     .cancel = 380,
     .modify = 100,
     .stale_target = 10,
     .target_resting_orders = 10'000},
    {.name = "add_cancel", .passive_limit = 500, .cancel = 500, .target_resting_orders = 10'000},
    {.name = "sweep",
     .passive_limit = 600,
     .crossing_limit = 150,
     .market = 50,
     .cancel = 150,
     .modify = 50,
     .stale_target = 10,
     .target_resting_orders = 10'000,
     .aggressive_size_multiplier = 3},
    {.name = "realistic_1k",
     .passive_limit = 480,
     .crossing_limit = 30,
     .market = 10,
     .cancel = 380,
     .modify = 100,
     .stale_target = 10,
     .target_resting_orders = 1'000},
    {.name = "realistic_100k",
     .passive_limit = 480,
     .crossing_limit = 30,
     .market = 10,
     .cancel = 380,
     .modify = 100,
     .stale_target = 10,
     .target_resting_orders = 100'000},
    {.name = "realistic_1m",
     .passive_limit = 480,
     .crossing_limit = 30,
     .market = 10,
     .cancel = 380,
     .modify = 100,
     .stale_target = 10,
     .target_resting_orders = 1'000'000},
}};

[[nodiscard]] constexpr const Profile* find_profile(std::string_view name) noexcept {
    for (const Profile& profile : kProfiles) {
        if (profile.name == name) {
            return &profile;
        }
    }
    return nullptr;
}

/// Book limits for a workload. Capacity is twice the target, so a generated workload never fills
/// the book, and the cost of the capacity path stays out of the measurements.
[[nodiscard]] constexpr BookConfig book_config(const Profile& profile) noexcept {
    return BookConfig{.min_price = kMinPrice,
                      .max_price = kMaxPrice,
                      .max_resting_orders = profile.target_resting_orders * 2};
}

/// What a workload actually contained, recorded next to each log, so results describe the measured
/// flow rather than the intended mix.
struct Stats {
    std::uint64_t limit_commands{};
    std::uint64_t market_commands{};
    std::uint64_t cancel_commands{};
    std::uint64_t modify_commands{};
    std::uint64_t accepted_events{};
    std::uint64_t rejected_events{};
    std::uint64_t trade_events{};
    std::uint64_t rested_events{};
    std::uint64_t canceled_events{};
    std::uint64_t modified_events{};
    std::uint64_t book_full_events{};
    std::uint64_t traded_quantity{};
    std::uint64_t max_resting_orders{};
    std::uint64_t commands_to_reach_target{};
    /// How far the best bid travelled over the log, which shows that the book really moved.
    std::int64_t lowest_best_bid{};
    std::int64_t highest_best_bid{};
};

/// Produces one command at a time.
///
/// A baseline book runs inside the generator, because the flow has to react to the actual book:
/// crossing orders must really cross and cancels must name orders that are still resting. Nothing
/// here depends on addresses or iteration order, so a seed always yields the same log.
class Generator {
  public:
    Generator(const Profile& profile, std::uint64_t seed)
        : profile_{profile}, rng_{seed}, book_{book_config(profile)} {
        live_.reserve(profile.target_resting_orders);
    }

    [[nodiscard]] Command next() {
        Command command = draw();
        command.seq = Sequence{++last_seq_};
        count_command(command);
        Tracker tracker{*this};
        book_.apply(command, tracker);
        stats_.max_resting_orders =
            std::max<std::uint64_t>(stats_.max_resting_orders, live_.size());
        record_best_bid();
        step_reference();
        return command;
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t resting_orders() const noexcept { return live_.size(); }

  private:
    struct LiveOrder {
        OrderId id{};
        Side side{};
        Price price{};
        Quantity open{};
    };

    struct Tracker {
        Generator& generator;
        void on_event(const Event& event) noexcept { generator.track(event); }
    };

    /// Stop probability for the distance from the best price. Most orders land close to the touch.
    static constexpr std::uint32_t kNearOffsetStop = 250;
    static constexpr std::uint64_t kMaxNearOffset = 256;
    /// Some orders are placed far away, which spreads the book over sparse price levels.
    static constexpr std::uint32_t kFarOrderChance = 50;
    static constexpr std::uint64_t kFarOffsetRange = 2'000;
    /// Stop probability for order size. Sizes are mostly small with an occasional larger order.
    static constexpr std::uint32_t kSizeStop = 150;
    static constexpr std::uint64_t kMaxSizeExtra = 1'000;
    /// How crossing limit prices reach past the best opposite price.
    static constexpr std::uint32_t kCrossOffsetStop = 500;
    static constexpr std::uint64_t kMaxCrossOffset = 16;
    /// How a modify changes an order, in parts per 1000. The rest reprice it.
    static constexpr std::uint32_t kModifyShrink = 450;
    static constexpr std::uint32_t kModifyGrow = 200;

    /// The reference price moves one tick, up or down, every this many commands.
    static constexpr std::uint64_t kReferenceStepInterval = 64;
    /// Share of aggressive orders that trade towards the reference price when the book has lagged
    /// it.
    static constexpr std::uint32_t kFollowReferenceChance = 750;

    /// Keeps the reference far enough inside the band that orders placed around it stay valid.
    static constexpr std::int64_t kReferenceMargin = 10'000;

    static constexpr std::size_t kDepartedRingSize = 64;

    [[nodiscard]] Command draw() {
        if (!target_reached_) {
            if (live_.size() >= profile_.target_resting_orders) {
                target_reached_ = true;
                stats_.commands_to_reach_target = last_seq_;
            } else {
                return draw_passive();
            }
        }

        std::uint64_t roll = rng_.below(1000);
        const auto in_band = [&roll](std::uint32_t share) {
            if (roll < share) {
                return true;
            }
            roll -= share;
            return false;
        };

        const std::size_t target = profile_.target_resting_orders;
        const bool too_many = live_.size() >= target + target / 10;
        const bool too_few = live_.size() <= target - target / 10;

        if (in_band(profile_.passive_limit)) {
            return too_many ? draw_cancel() : draw_passive();
        }
        if (in_band(profile_.crossing_limit)) {
            return draw_crossing();
        }
        if (in_band(profile_.market)) {
            return draw_market();
        }
        if (in_band(profile_.cancel)) {
            return too_few ? draw_passive() : draw_cancel();
        }
        return live_.empty() ? draw_passive() : draw_modify();
    }

    [[nodiscard]] Side draw_side() noexcept { return rng_.chance(500) ? Side::kBuy : Side::kSell; }

    [[nodiscard]] Quantity draw_size() noexcept {
        return Quantity{static_cast<std::uint32_t>(1 + rng_.geometric(kSizeStop, kMaxSizeExtra))};
    }

    [[nodiscard]] Quantity draw_aggressive_size() noexcept {
        return Quantity{draw_size().value() * profile_.aggressive_size_multiplier};
    }

    [[nodiscard]] OrderId draw_new_id() noexcept {
        // Small gaps, so nothing can rely on IDs being perfectly dense.
        last_id_ += 1 + rng_.below(4);
        return OrderId{last_id_};
    }

    [[nodiscard]] std::int64_t draw_offset() noexcept {
        const std::uint64_t offset = rng_.chance(kFarOrderChance)
                                         ? rng_.below(kFarOffsetRange)
                                         : rng_.geometric(kNearOffsetStop, kMaxNearOffset);
        return static_cast<std::int64_t>(offset);
    }

    /// A price that doesn't cross. It's anchored to the reference price rather than the best price.
    /// Anchoring to the best price makes new orders pile up there faster than aggressive orders
    /// remove them, which pins the price in place and keeps every access in a small window of the
    /// book.
    [[nodiscard]] Price draw_passive_price(Side side) noexcept {
        const std::optional<Price> bid = book_.best_bid();
        const std::optional<Price> ask = book_.best_ask();
        const std::int64_t offset = draw_offset();

        std::int64_t price = 0;
        std::int64_t low = kMinPrice.value();
        std::int64_t high = kMaxPrice.value();
        if (side == Side::kBuy) {
            price = reference_ - 1 - offset;
            high = ask ? ask->value() - 1 : high;
        } else {
            price = reference_ + 1 + offset;
            low = bid ? bid->value() + 1 : low;
        }
        return Price{std::max(std::min(price, high), low)};
    }

    /// Aggressive flow leans towards the reference price, which is what moves the book after it.
    [[nodiscard]] Side draw_aggressor_side() noexcept {
        const std::optional<Price> bid = book_.best_bid();
        const std::optional<Price> ask = book_.best_ask();
        if (!bid || !ask || bid->value() + ask->value() == 2 * reference_) {
            return draw_side();
        }
        const Side towards_reference =
            bid->value() + ask->value() < 2 * reference_ ? Side::kBuy : Side::kSell;
        if (rng_.chance(kFollowReferenceChance)) {
            return towards_reference;
        }
        return towards_reference == Side::kBuy ? Side::kSell : Side::kBuy;
    }

    void step_reference() noexcept {
        if (last_seq_ % kReferenceStepInterval == 0) {
            reference_ += rng_.chance(500) ? 1 : -1;
            reference_ = std::clamp(reference_, kMinPrice.value() + kReferenceMargin,
                                    kMaxPrice.value() - kReferenceMargin);
        }
    }

    void record_best_bid() noexcept {
        const std::optional<Price> bid = book_.best_bid();
        if (!bid) {
            return;
        }
        const std::int64_t value = bid->value();
        stats_.lowest_best_bid =
            stats_.lowest_best_bid == 0 ? value : std::min(stats_.lowest_best_bid, value);
        stats_.highest_best_bid = std::max(stats_.highest_best_bid, value);
    }

    [[nodiscard]] Command draw_passive() noexcept {
        const Side side = draw_side();
        return Command{.order_id = draw_new_id(),
                       .price = draw_passive_price(side),
                       .quantity = draw_size(),
                       .type = CommandType::kLimit,
                       .side = side};
    }

    [[nodiscard]] Command draw_crossing() noexcept {
        const Side side = draw_aggressor_side();
        const std::optional<Price> opposite =
            side == Side::kBuy ? book_.best_ask() : book_.best_bid();
        if (!opposite) {
            return draw_passive();
        }
        const auto reach =
            static_cast<std::int64_t>(rng_.geometric(kCrossOffsetStop, kMaxCrossOffset));
        const std::int64_t price =
            side == Side::kBuy ? opposite->value() + reach : opposite->value() - reach;
        return Command{.order_id = draw_new_id(),
                       .price = Price{std::clamp(price, kMinPrice.value(), kMaxPrice.value())},
                       .quantity = draw_aggressive_size(),
                       .type = CommandType::kLimit,
                       .side = side};
    }

    [[nodiscard]] Command draw_market() noexcept {
        const Side side = draw_aggressor_side();
        return Command{.order_id = draw_new_id(),
                       .quantity = draw_aggressive_size(),
                       .type = CommandType::kMarket,
                       .side = side};
    }

    /// True when this cancel or modify should name an order that has already left the book.
    [[nodiscard]] bool draw_stale() noexcept {
        return departed_count_ > 0 && rng_.chance(profile_.stale_target);
    }

    [[nodiscard]] OrderId draw_departed_id() noexcept {
        const std::size_t available = std::min(departed_count_, kDepartedRingSize);
        return departed_[rng_.below(available)];
    }

    [[nodiscard]] Command draw_cancel() noexcept {
        if (live_.empty()) {
            return draw_passive();
        }
        const OrderId id = draw_stale() ? draw_departed_id() : live_[rng_.below(live_.size())].id;
        return Command{.order_id = id, .type = CommandType::kCancel};
    }

    [[nodiscard]] Command draw_modify() noexcept {
        if (draw_stale()) {
            return Command{.order_id = draw_departed_id(),
                           .price = kInitialMid,
                           .quantity = Quantity{1},
                           .type = CommandType::kModify};
        }
        const LiveOrder order = live_[rng_.below(live_.size())];
        const std::uint64_t roll = rng_.below(1000);

        Price price = order.price;
        Quantity quantity = order.open;
        if (roll < kModifyShrink && order.open > Quantity{1}) {
            quantity = Quantity{static_cast<std::uint32_t>(1 + rng_.below(order.open.value() - 1))};
        } else if (roll < kModifyShrink + kModifyGrow) {
            quantity += draw_size();
        } else {
            price = draw_passive_price(order.side);
        }
        return Command{.order_id = order.id,
                       .price = price,
                       .quantity = quantity,
                       .type = CommandType::kModify};
    }

    void count_command(const Command& command) noexcept {
        switch (command.type) {
            case CommandType::kLimit:
                ++stats_.limit_commands;
                return;
            case CommandType::kMarket:
                ++stats_.market_commands;
                return;
            case CommandType::kCancel:
                ++stats_.cancel_commands;
                return;
            case CommandType::kModify:
                ++stats_.modify_commands;
                return;
        }
    }

    /// Keeps `live_` in step with the book by following its events.
    void track(const Event& event) noexcept {
        switch (event.type) {
            case EventType::kAccepted:
                ++stats_.accepted_events;
                return;
            case EventType::kRejected:
                ++stats_.rejected_events;
                return;
            case EventType::kRested:
                ++stats_.rested_events;
                add_live(event);
                return;
            case EventType::kTrade:
                ++stats_.trade_events;
                stats_.traded_quantity += event.quantity.value();
                if (event.counter_leaves == Quantity{0}) {
                    remove_live(event.counter_order_id);
                } else {
                    live_[index_.at(event.counter_order_id.value())].open = event.counter_leaves;
                }
                return;
            case EventType::kCanceled:
                ++stats_.canceled_events;
                if (event.reason == Reason::kUserCancel) {
                    remove_live(event.order_id);
                } else if (event.reason == Reason::kBookFull) {
                    ++stats_.book_full_events;
                }
                return;
            case EventType::kModified: {
                ++stats_.modified_events;
                LiveOrder& order = live_[index_.at(event.order_id.value())];
                if (event.price == order.price && event.quantity <= order.open) {
                    order.open = event.quantity;
                } else {
                    // The book removed it, and any remainder comes back through a RESTED event.
                    remove_live(event.order_id);
                }
                return;
            }
        }
    }

    void add_live(const Event& event) {
        index_[event.order_id.value()] = live_.size();
        live_.push_back(LiveOrder{event.order_id, event.side, event.price, event.quantity});
    }

    // Swapping with the last element keeps removal constant-time and the vector dense, which is
    // what makes drawing a uniformly random live order cheap.
    void remove_live(OrderId id) {
        const auto found = index_.find(id.value());
        const std::size_t position = found->second;
        index_.erase(found);
        const std::size_t last = live_.size() - 1;
        if (position != last) {
            live_[position] = live_[last];
            index_[live_[position].id.value()] = position;
        }
        live_.pop_back();
        departed_[departed_count_ % kDepartedRingSize] = id;
        ++departed_count_;
    }

    Profile profile_;
    Rng rng_;
    baseline::BaselineBook<> book_;
    std::vector<LiveOrder> live_;
    // Only ever looked up by key, so its iteration order can't influence the generated log.
    std::unordered_map<std::uint64_t, std::size_t> index_;
    std::array<OrderId, kDepartedRingSize> departed_{};
    std::size_t departed_count_{0};
    std::uint64_t last_seq_{0};
    std::uint64_t last_id_{0};
    std::int64_t reference_{kInitialMid.value()};
    bool target_reached_{false};
    Stats stats_{};
};

}  // namespace nanomatch::workload

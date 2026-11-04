#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "nanomatch/protocol.hpp"
#include "nanomatch/types.hpp"

namespace nanomatch {

/// Limits fixed when a book is constructed. See sections 2 and 6 of docs/SPEC.md.
struct BookConfig {
    Price min_price{1};
    Price max_price{1};
    std::uint32_t max_resting_orders{1};

    [[nodiscard]] constexpr bool contains(Price price) const noexcept {
        return min_price <= price && price <= max_price;
    }
};

/// Throws std::invalid_argument if the limits are unusable. Books call this during construction,
/// so that the matching path can rely on the limits without checking them again.
inline void validate_config(const BookConfig& config) {
    if (config.min_price < Price{1}) {
        throw std::invalid_argument{"min_price must be at least 1"};
    }
    if (config.max_price < config.min_price) {
        throw std::invalid_argument{"max_price must not be below min_price"};
    }
    if (config.max_resting_orders == 0) {
        throw std::invalid_argument{"max_resting_orders must be at least 1"};
    }
}

/// Receives the events produced by a book. Sinks are template parameters rather than virtual
/// interfaces, so the call can be inlined into the matching path.
template <typename S>
concept EventSink = requires(S& sink, const Event& event) {
    { sink.on_event(event) } noexcept;
};

/// Discards every event.
struct NullSink {
    void on_event(const Event& /*event*/) noexcept {}
};

/// The interface shared by all book implementations. Tests, tools and benchmarks are written
/// against this concept, so every implementation runs through identical code.
template <typename B>
concept MatchingBook = std::constructible_from<B, const BookConfig&> &&
                       requires(B& book, const B& const_book, const Command& command,
                                NullSink& sink, Side side, Price price) {
                           { book.apply(command, sink) } noexcept;
                           { const_book.best_bid() } noexcept -> std::same_as<std::optional<Price>>;
                           { const_book.best_ask() } noexcept -> std::same_as<std::optional<Price>>;
                           {
                               const_book.open_quantity_at(side, price)
                           } noexcept -> std::same_as<std::uint64_t>;
                           { const_book.resting_orders() } noexcept -> std::same_as<std::uint32_t>;
                       };

/// Check 1 of the validation order in section 6 of the specification: the record's shape is valid.
/// An enum holding a value outside its enumerators falls through the switch and is malformed.
[[nodiscard]] constexpr bool is_well_formed(const Command& command) noexcept {
    if (command.reserved != 0) {
        return false;
    }
    const bool has_order_side = command.side == Side::kBuy || command.side == Side::kSell;
    switch (command.type) {
        case CommandType::kLimit:
            return has_order_side;
        case CommandType::kMarket:
            return has_order_side && command.price == Price{0};
        case CommandType::kCancel:
            return command.side == Side::kNone && command.price == Price{0} &&
                   command.quantity == Quantity{0};
        case CommandType::kModify:
            return command.side == Side::kNone;
    }
    return false;
}

}  // namespace nanomatch

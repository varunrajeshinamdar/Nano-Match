#pragma once

#include <cstdint>
#include <limits>

#include "nanomatch/fixed_array.hpp"
#include "nanomatch/protocol.hpp"
#include "nanomatch/types.hpp"

namespace nanomatch {

/// Marks the end of a list. It can't collide with a real index, because capacity is limited to
/// less.
inline constexpr std::uint32_t kNullIndex = std::numeric_limits<std::uint32_t>::max();

/// A resting order, linked into the queue at its price level.
///
/// Links are 32-bit indices rather than pointers, which keeps the node small enough that two fit in
/// a cache line. Being aligned to its own size means a node never straddles two lines. The price
/// isn't stored, because it can be derived from `level`.
struct alignas(32) OrderNode {
    OrderId id{};
    Quantity open{};
    std::uint32_t next{kNullIndex};
    std::uint32_t prev{kNullIndex};
    std::uint32_t level{};
    Side side{};
};

static_assert(sizeof(OrderNode) == 32);

/// Fixed-capacity store of order nodes. Taking and returning a node are both O(1) and never
/// allocate.
///
/// Free nodes form a stack threaded through `next`. It's a stack rather than a queue because the
/// node freed most recently is the most likely to still be in cache, so reusing it first avoids a
/// miss.
class OrderPool {
  public:
    OrderPool(std::uint32_t capacity, PagePolicy pages) : nodes_{capacity, pages} {
        for (std::uint32_t i = 0; i < capacity; ++i) {
            nodes_[i].next = i + 1 < capacity ? i + 1 : kNullIndex;
        }
        free_head_ = 0;
    }

    /// Precondition: !full().
    [[nodiscard]] std::uint32_t acquire() noexcept {
        const std::uint32_t index = free_head_;
        free_head_ = nodes_[index].next;
        ++in_use_;
        return index;
    }

    /// Precondition: `index` came from acquire() and hasn't been released since.
    void release(std::uint32_t index) noexcept {
        nodes_[index].next = free_head_;
        free_head_ = index;
        --in_use_;
    }

    [[nodiscard]] OrderNode& operator[](std::uint32_t index) noexcept { return nodes_[index]; }
    [[nodiscard]] const OrderNode& operator[](std::uint32_t index) const noexcept {
        return nodes_[index];
    }

    [[nodiscard]] std::uint32_t in_use() const noexcept { return in_use_; }
    [[nodiscard]] bool full() const noexcept { return in_use_ == nodes_.size(); }

  private:
    FixedArray<OrderNode> nodes_;
    std::uint32_t free_head_{kNullIndex};
    std::uint32_t in_use_{0};
};

}  // namespace nanomatch

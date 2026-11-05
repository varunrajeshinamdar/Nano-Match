#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "nanomatch/fixed_array.hpp"
#include "nanomatch/types.hpp"

namespace nanomatch {

/// Maps the IDs of resting orders to their pool slots.
///
/// This is an open-addressing table with linear probing. Neighbouring slots share cache lines, so
/// probing is usually cheap, whereas a node-based hash map pays a likely miss for every link.
///
/// Each slot is 8 bytes: the low 32 bits of the ID and a pool slot number. Keeping the table small
/// matters more than almost anything else here, because it's accessed at random, so what doesn't
/// fit in cache misses. Two IDs can share their low bits, so `find` confirms a candidate through a
/// caller-supplied check. That check reads the pool node, which every caller reads next anyway, so
/// it adds no extra memory access in the common case.
///
/// The table is sized to keep load below 0.5 even with the book at capacity. A fuller table was
/// measured and found to be slower on average, because longer probe runs mean more mispredicted
/// loop exits, even though the smaller table misses the cache less.
///
/// Erasing shifts later entries back rather than leaving a tombstone. Tombstones would accumulate
/// and slow lookups over time, so a long-running book would drift towards worse latency.
///
/// The table is never iterated, so its layout can't affect the book's output. All operations take
/// expected O(1) time and never allocate.
class IdIndex {
  public:
    static constexpr std::uint32_t kNotFound = std::numeric_limits<std::uint32_t>::max();

    /// Preconditions: 0 < max_entries <= 2^31.
    IdIndex(std::uint32_t max_entries, PagePolicy pages)
        : slots_{table_size(max_entries), pages},
          mask_{slots_.size() - 1},
          shift_{32U - static_cast<unsigned>(std::bit_width(slots_.size() - 1))} {}

    /// Preconditions: `value` isn't kNotFound, `id` is absent, and fewer than `max_entries` are
    /// held.
    void insert(OrderId id, std::uint32_t value) noexcept {
        const auto low_bits = static_cast<std::uint32_t>(id.value());
        std::size_t position = home_of(low_bits);
        while (slots_[position].value != kNotFound) {
            position = (position + 1) & mask_;
        }
        slots_[position] = Slot{low_bits, value};
    }

    /// Returns the value stored for `id`, or kNotFound. `is_match(value)` must say whether `value`
    /// really belongs to `id`, which resolves IDs that have the same low 32 bits.
    template <typename IsMatch>
    [[nodiscard]] std::uint32_t find(OrderId id, IsMatch is_match) const noexcept {
        const auto low_bits = static_cast<std::uint32_t>(id.value());
        std::size_t position = home_of(low_bits);
        while (true) {
            const Slot& slot = slots_[position];
            if (slot.value == kNotFound) {
                return kNotFound;
            }
            if (slot.low_bits == low_bits && is_match(slot.value)) {
                return slot.value;
            }
            position = (position + 1) & mask_;
        }
    }

    /// Precondition: `value` is stored for `id`. Knowing the value means no check is needed, since
    /// each value appears only once.
    void erase(OrderId id, std::uint32_t value) noexcept {
        std::size_t hole = home_of(static_cast<std::uint32_t>(id.value()));
        while (slots_[hole].value != value) {
            hole = (hole + 1) & mask_;
        }

        // Every entry in the probe run after the hole must stay reachable from its home slot. An
        // entry moves back into the hole only if the hole is no further from the entry than its
        // home is, because otherwise a probe starting at its home would never pass through the
        // hole.
        std::size_t scan = (hole + 1) & mask_;
        while (slots_[scan].value != kNotFound) {
            const std::size_t home = home_of(slots_[scan].low_bits);
            const std::size_t distance_from_home = (scan - home) & mask_;
            const std::size_t distance_from_hole = (scan - hole) & mask_;
            if (distance_from_home >= distance_from_hole) {
                slots_[hole] = slots_[scan];
                hole = scan;
            }
            scan = (scan + 1) & mask_;
        }
        slots_[hole].value = kNotFound;
    }

  private:
    struct Slot {
        std::uint32_t low_bits{0};
        std::uint32_t value{kNotFound};
    };

    static_assert(sizeof(Slot) == 8);

    /// 2^32 divided by the golden ratio. Multiplying by it spreads sequential IDs evenly across the
    /// table, whereas masking low bits directly would cluster IDs that are close together.
    static constexpr std::uint32_t kFibonacciMultiplier = 0x9E3779B9U;

    [[nodiscard]] static constexpr std::size_t table_size(std::uint32_t max_entries) noexcept {
        // At least one slot must stay empty, because that's what ends every probe sequence.
        return std::bit_ceil((std::size_t{max_entries} * 2) + 1);
    }

    [[nodiscard]] std::size_t home_of(std::uint32_t low_bits) const noexcept {
        return (low_bits * kFibonacciMultiplier) >> shift_;
    }

    FixedArray<Slot> slots_;
    std::size_t mask_;
    unsigned shift_;
};

}  // namespace nanomatch

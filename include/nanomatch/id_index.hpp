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
/// This is an open-addressing table with linear probing, sized at construction to at least twice
/// the maximum number of entries, so its load never exceeds 0.5 and probe sequences stay short.
/// Neighbouring slots share cache lines, so probing is usually cheap. Linked-node hash maps pay a
/// miss for every link.
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
          shift_{
              static_cast<unsigned>(std::countl_zero(static_cast<std::uint64_t>(slots_.size()))) +
              1U} {}

    /// Preconditions: `id` is absent and non-zero, and the table holds fewer than `max_entries`
    /// entries.
    void insert(OrderId id, std::uint32_t value) noexcept {
        std::size_t position = home_of(id.value());
        while (slots_[position].key != kEmpty) {
            position = (position + 1) & mask_;
        }
        slots_[position] = Slot{id.value(), value};
    }

    [[nodiscard]] std::uint32_t find(OrderId id) const noexcept {
        std::size_t position = home_of(id.value());
        while (true) {
            const Slot& slot = slots_[position];
            // Empty has to be checked first. ID 0 can arrive from a command, and it equals the key
            // that marks an empty slot, so comparing keys first would report it as found.
            if (slot.key == kEmpty) {
                return kNotFound;
            }
            if (slot.key == id.value()) {
                return slot.value;
            }
            position = (position + 1) & mask_;
        }
    }

    /// Precondition: `id` is present.
    void erase(OrderId id) noexcept {
        std::size_t hole = home_of(id.value());
        while (slots_[hole].key != id.value()) {
            hole = (hole + 1) & mask_;
        }

        // Every entry in the probe run after the hole must stay reachable from its home slot. An
        // entry moves back into the hole only if the hole is no further from the entry than its
        // home is, because otherwise a probe starting at its home would never pass through the
        // hole.
        std::size_t scan = (hole + 1) & mask_;
        while (slots_[scan].key != kEmpty) {
            const std::size_t home = home_of(slots_[scan].key);
            const std::size_t distance_from_home = (scan - home) & mask_;
            const std::size_t distance_from_hole = (scan - hole) & mask_;
            if (distance_from_home >= distance_from_hole) {
                slots_[hole] = slots_[scan];
                hole = scan;
            }
            scan = (scan + 1) & mask_;
        }
        slots_[hole].key = kEmpty;
    }

  private:
    struct Slot {
        std::uint64_t key{};
        std::uint32_t value{};
    };

    /// ID 0 is reserved by the specification, so it can mark an empty slot.
    static constexpr std::uint64_t kEmpty = 0;

    /// 2^64 divided by the golden ratio. Multiplying by it spreads sequential IDs evenly across the
    /// table, whereas masking low bits directly would cluster IDs that are close together.
    static constexpr std::uint64_t kFibonacciMultiplier = 0x9E3779B97F4A7C15ULL;

    [[nodiscard]] static constexpr std::size_t table_size(std::uint32_t max_entries) noexcept {
        return std::bit_ceil(std::size_t{max_entries} * 2);
    }

    [[nodiscard]] std::size_t home_of(std::uint64_t key) const noexcept {
        return static_cast<std::size_t>((key * kFibonacciMultiplier) >> shift_);
    }

    FixedArray<Slot> slots_;
    std::size_t mask_;
    unsigned shift_;
};

}  // namespace nanomatch

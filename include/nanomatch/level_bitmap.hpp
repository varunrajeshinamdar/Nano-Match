#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "nanomatch/fixed_array.hpp"

namespace nanomatch {

/// Set of price level indices that can find the next member above or below any index.
///
/// When the best level empties, the book needs the next occupied one, which may be far away in a
/// sparse ladder. A plain bitmap would scan word by word across the gap. Here each layer has one
/// bit per word of the layer below, set when that word is non-zero, so a search climbs until it
/// finds a set bit and then descends. The number of layers grows with log64 of the size, so all
/// operations are effectively constant time, at most six word accesses per direction for any band
/// this type accepts.
class LevelBitmap {
  public:
    static constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::uint32_t kMaxBits = std::uint32_t{1} << 30U;

    /// Preconditions: 0 < bit_count <= kMaxBits.
    LevelBitmap(std::uint32_t bit_count, PagePolicy pages)
        : bit_count_{bit_count}, words_{total_words(bit_count), pages} {
        std::size_t offset = 0;
        std::size_t bits_in_layer = bit_count;
        while (true) {
            const std::size_t words = (bits_in_layer + kWordBits - 1) / kWordBits;
            layer_offset_[layer_count_] = offset;
            layer_words_[layer_count_] = words;
            ++layer_count_;
            offset += words;
            if (words == 1) {
                break;
            }
            bits_in_layer = words;
        }
    }

    /// Precondition: index < bit_count.
    [[nodiscard]] bool test(std::uint32_t index) const noexcept {
        return (word(0, index / kWordBits) & bit_mask(index)) != 0;
    }

    /// Precondition: index < bit_count.
    void set(std::uint32_t index) noexcept {
        std::size_t position = index;
        for (std::size_t layer = 0; layer < layer_count_; ++layer) {
            std::uint64_t& target = word(layer, position / kWordBits);
            const bool was_empty = target == 0;
            target |= bit_mask(position);
            if (!was_empty) {
                return;
            }
            position /= kWordBits;
        }
    }

    /// Precondition: index < bit_count.
    void clear(std::uint32_t index) noexcept {
        std::size_t position = index;
        for (std::size_t layer = 0; layer < layer_count_; ++layer) {
            std::uint64_t& target = word(layer, position / kWordBits);
            target &= ~bit_mask(position);
            if (target != 0) {
                return;
            }
            position /= kWordBits;
        }
    }

    /// The smallest member greater than `index`, or kNone. Precondition: index < bit_count.
    [[nodiscard]] std::uint32_t first_above(std::uint32_t index) const noexcept {
        std::size_t position = std::size_t{index} + 1;
        if (position >= bit_count_) {
            return kNone;
        }
        for (std::size_t layer = 0; layer < layer_count_; ++layer) {
            const std::size_t word_index = position / kWordBits;
            const std::uint64_t candidates =
                word(layer, word_index) & (~std::uint64_t{0} << (position % kWordBits));
            if (candidates != 0) {
                return descend_lowest(layer,
                                      (word_index * kWordBits) +
                                          static_cast<std::size_t>(std::countr_zero(candidates)));
            }
            position = word_index + 1;
            if (position >= layer_words_[layer]) {
                return kNone;
            }
        }
        return kNone;
    }

    /// The largest member smaller than `index`, or kNone. Precondition: index < bit_count.
    [[nodiscard]] std::uint32_t first_below(std::uint32_t index) const noexcept {
        if (index == 0) {
            return kNone;
        }
        std::size_t position = std::size_t{index} - 1;
        for (std::size_t layer = 0; layer < layer_count_; ++layer) {
            const std::size_t word_index = position / kWordBits;
            const std::uint64_t candidates =
                word(layer, word_index) &
                (~std::uint64_t{0} >> (kWordBits - 1 - (position % kWordBits)));
            if (candidates != 0) {
                const auto highest =
                    kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(candidates));
                return descend_highest(layer, (word_index * kWordBits) + highest);
            }
            if (word_index == 0) {
                return kNone;
            }
            position = word_index - 1;
        }
        return kNone;
    }

  private:
    static constexpr std::size_t kWordBits = 64;
    static constexpr std::size_t kMaxLayers = 6;

    [[nodiscard]] static constexpr std::size_t total_words(std::size_t bit_count) noexcept {
        std::size_t total = 0;
        std::size_t bits_in_layer = bit_count;
        while (true) {
            const std::size_t words = (bits_in_layer + kWordBits - 1) / kWordBits;
            total += words;
            if (words == 1) {
                return total;
            }
            bits_in_layer = words;
        }
    }

    [[nodiscard]] static constexpr std::uint64_t bit_mask(std::size_t position) noexcept {
        return std::uint64_t{1} << (position % kWordBits);
    }

    [[nodiscard]] std::uint64_t& word(std::size_t layer, std::size_t index) noexcept {
        return words_[layer_offset_[layer] + index];
    }
    [[nodiscard]] const std::uint64_t& word(std::size_t layer, std::size_t index) const noexcept {
        return words_[layer_offset_[layer] + index];
    }

    /// Given a set bit at `position` in `layer`, follows the lowest set bits down to the bottom
    /// layer.
    [[nodiscard]] std::uint32_t descend_lowest(std::size_t layer,
                                               std::size_t position) const noexcept {
        while (layer > 0) {
            --layer;
            position = (position * kWordBits) +
                       static_cast<std::size_t>(std::countr_zero(word(layer, position)));
        }
        return static_cast<std::uint32_t>(position);
    }

    /// Given a set bit at `position` in `layer`, follows the highest set bits down to the bottom
    /// layer.
    [[nodiscard]] std::uint32_t descend_highest(std::size_t layer,
                                                std::size_t position) const noexcept {
        while (layer > 0) {
            --layer;
            position = (position * kWordBits) + kWordBits - 1 -
                       static_cast<std::size_t>(std::countl_zero(word(layer, position)));
        }
        return static_cast<std::uint32_t>(position);
    }

    std::size_t bit_count_;
    std::array<std::size_t, kMaxLayers> layer_offset_{};
    std::array<std::size_t, kMaxLayers> layer_words_{};
    std::size_t layer_count_{0};
    FixedArray<std::uint64_t> words_;
};

}  // namespace nanomatch

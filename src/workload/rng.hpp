#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace nanomatch::workload {

/// SplitMix64, used only to expand a single seed into xoshiro's larger state, as its authors
/// recommend.
class SplitMix64 {
  public:
    constexpr explicit SplitMix64(std::uint64_t seed) noexcept : state_{seed} {}

    constexpr std::uint64_t next() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t mixed = state_;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
        return mixed ^ (mixed >> 31U);
    }

  private:
    std::uint64_t state_;
};

/// xoshiro256** with integer-only helpers.
///
/// The generator and its helpers are written out here because generated workloads must be identical
/// on every toolchain. `std::` engines are portable but `std::` distributions are not, and anything
/// built on floating point could differ between math libraries.
class Rng {
  public:
    constexpr explicit Rng(std::uint64_t seed) noexcept {
        SplitMix64 expander{seed};
        for (std::uint64_t& word : state_) {
            word = expander.next();
        }
    }

    /// Sets the state directly, which lets tests compare against the reference implementation.
    constexpr explicit Rng(const std::array<std::uint64_t, 4>& state) noexcept : state_{state} {}

    constexpr std::uint64_t next() noexcept {
        const std::uint64_t result = std::rotl(state_[1] * 5U, 7) * 9U;
        const std::uint64_t shifted = state_[1] << 17U;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= shifted;
        state_[3] = std::rotl(state_[3], 45);
        return result;
    }

    /// Value in [0, bound). The modulo bias is about bound / 2^64, which is negligible for the
    /// small bounds used here. Precondition: bound > 0.
    constexpr std::uint64_t below(std::uint64_t bound) noexcept { return next() % bound; }

    /// True with probability per_mille / 1000.
    constexpr bool chance(std::uint32_t per_mille) noexcept { return below(1000) < per_mille; }

    /// Number of failed trials before a success with probability per_mille / 1000, capped at `cap`.
    constexpr std::uint64_t geometric(std::uint32_t per_mille, std::uint64_t cap) noexcept {
        std::uint64_t failures = 0;
        while (failures < cap && !chance(per_mille)) {
            ++failures;
        }
        return failures;
    }

  private:
    std::array<std::uint64_t, 4> state_{};
};

}  // namespace nanomatch::workload

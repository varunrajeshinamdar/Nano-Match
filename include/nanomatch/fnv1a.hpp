#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace nanomatch {

/// Incremental 64-bit FNV-1a hash, used to compare event logs. It detects accidental divergence
/// between runs and offers no protection against deliberate tampering.
class Fnv1a64 {
  public:
    constexpr void update(std::span<const std::byte> bytes) noexcept {
        for (const std::byte byte : bytes) {
            hash_ ^= static_cast<std::uint64_t>(byte);
            hash_ *= kPrime;
        }
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return hash_; }

  private:
    static constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    static constexpr std::uint64_t kPrime = 1099511628211ULL;

    std::uint64_t hash_{kOffsetBasis};
};

}  // namespace nanomatch

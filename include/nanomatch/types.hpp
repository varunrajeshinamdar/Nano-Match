#pragma once

#include <compare>
#include <cstdint>
#include <type_traits>

namespace nanomatch {

/// Integer wrapper that turns mixing up domain values into a compile error.
///
/// With bare integers, a quantity could be passed where a price is expected and the compiler would
/// accept it. The wrapper has the same size and layout as `Rep`, so records containing it can still
/// be copied byte for byte to and from log files, and it costs nothing at runtime.
///
/// Arithmetic exists only where `Tag::kAdditive` is true. Adding two prices or two order IDs has no
/// meaning, so those expressions don't compile.
template <typename Rep, typename Tag>
class StrongInt {
  public:
    using rep = Rep;

    constexpr StrongInt() noexcept = default;
    constexpr explicit StrongInt(Rep value) noexcept : value_{value} {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

    friend constexpr auto operator<=>(StrongInt, StrongInt) noexcept = default;

    /// Precondition: the result fits in `Rep`.
    constexpr StrongInt& operator+=(StrongInt other) noexcept
        requires(Tag::kAdditive)
    {
        value_ += other.value_;
        return *this;
    }

    /// Precondition: the result fits in `Rep`.
    constexpr StrongInt& operator-=(StrongInt other) noexcept
        requires(Tag::kAdditive)
    {
        value_ -= other.value_;
        return *this;
    }

    friend constexpr StrongInt operator+(StrongInt lhs, StrongInt rhs) noexcept
        requires(Tag::kAdditive)
    {
        lhs += rhs;
        return lhs;
    }

    friend constexpr StrongInt operator-(StrongInt lhs, StrongInt rhs) noexcept
        requires(Tag::kAdditive)
    {
        lhs -= rhs;
        return lhs;
    }

  private:
    Rep value_{};
};

struct PriceTag {
    static constexpr bool kAdditive = false;
};
struct QuantityTag {
    static constexpr bool kAdditive = true;
};
struct OrderIdTag {
    static constexpr bool kAdditive = false;
};
struct SequenceTag {
    static constexpr bool kAdditive = false;
};

/// Price in integer ticks. Integers compare exactly and behave identically on every build, which
/// floating point doesn't guarantee. The mapping from ticks to currency belongs to the caller.
using Price = StrongInt<std::int64_t, PriceTag>;

/// Order size in integer lots.
using Quantity = StrongInt<std::uint32_t, QuantityTag>;

/// Sender-assigned order identifier. See section 3 of docs/SPEC.md.
using OrderId = StrongInt<std::uint64_t, OrderIdTag>;

/// Position of a command or event within its log.
using Sequence = StrongInt<std::uint64_t, SequenceTag>;

static_assert(sizeof(Price) == sizeof(Price::rep) && std::is_trivially_copyable_v<Price>);
static_assert(sizeof(Quantity) == sizeof(Quantity::rep) && std::is_trivially_copyable_v<Quantity>);
static_assert(sizeof(OrderId) == sizeof(OrderId::rep) && std::is_trivially_copyable_v<OrderId>);
static_assert(sizeof(Sequence) == sizeof(Sequence::rep) && std::is_trivially_copyable_v<Sequence>);

}  // namespace nanomatch

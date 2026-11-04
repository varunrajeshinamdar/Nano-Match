#include "nanomatch/types.hpp"

#include <concepts>

#include <gtest/gtest.h>

namespace nanomatch {
namespace {

template <typename T>
concept Addable = requires(T a, T b) { a + b; };

template <typename T>
concept InPlaceSubtractable = requires(T a, T b) { a -= b; };

// These are the guarantees the wrapper exists for, so they are checked at compile time.
static_assert(!std::convertible_to<std::int64_t, Price>, "construction must be explicit");
static_assert(!std::convertible_to<Price, std::int64_t>, "unwrapping must be explicit");
static_assert(!std::constructible_from<Price, Quantity>, "domain types must not mix");
static_assert(!std::equality_comparable_with<Price, Quantity>, "domain types must not compare");
static_assert(!Addable<Price> && !Addable<OrderId> && !Addable<Sequence>);
static_assert(Addable<Quantity> && InPlaceSubtractable<Quantity>);

TEST(StrongIntTest, DefaultConstructsToZero) {
    EXPECT_EQ(Price{}.value(), 0);
    EXPECT_EQ(Quantity{}.value(), 0U);
    EXPECT_EQ(OrderId{}.value(), 0U);
    EXPECT_EQ(Sequence{}.value(), 0U);
}

TEST(StrongIntTest, ComparesByValue) {
    EXPECT_LT(Price{99}, Price{100});
    EXPECT_GT(Price{100}, Price{-1});
    EXPECT_EQ(OrderId{7}, OrderId{7});
    EXPECT_NE(OrderId{7}, OrderId{8});
}

TEST(StrongIntTest, QuantityArithmetic) {
    EXPECT_EQ(Quantity{5} + Quantity{3}, Quantity{8});
    EXPECT_EQ(Quantity{5} - Quantity{3}, Quantity{2});

    Quantity quantity{10};
    quantity -= Quantity{4};
    EXPECT_EQ(quantity, Quantity{6});
    quantity += Quantity{1};
    EXPECT_EQ(quantity, Quantity{7});
}

TEST(StrongIntTest, UsableInConstantExpressions) {
    static_assert(Quantity{2} + Quantity{2} == Quantity{4});
    static_assert(Price{5}.value() == 5);
}

}  // namespace
}  // namespace nanomatch

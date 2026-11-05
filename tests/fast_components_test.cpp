// Components of the optimized book, checked against simple reference structures.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "nanomatch/fixed_array.hpp"
#include "nanomatch/id_index.hpp"
#include "nanomatch/level_bitmap.hpp"
#include "nanomatch/order_book.hpp"
#include "nanomatch/order_pool.hpp"

namespace nanomatch {
namespace {

TEST(FixedArrayTest, ElementsStartValueInitialized) {
    const FixedArray<std::uint64_t> array{1000, PagePolicy::kDefault};
    ASSERT_EQ(array.size(), 1000U);
    for (std::size_t i = 0; i < array.size(); ++i) {
        EXPECT_EQ(array[i], 0U);
    }
}

TEST(FixedArrayTest, RunsDefaultMemberInitializers) {
    const FixedArray<OrderNode> array{4, PagePolicy::kHuge};
    EXPECT_EQ(array[3].next, kNullIndex);
    EXPECT_EQ(array[3].prev, kNullIndex);
}

TEST(FixedArrayTest, IsAlignedToHugePageBoundary) {
    const FixedArray<std::uint64_t> array{1, PagePolicy::kHuge};
    const auto address = reinterpret_cast<std::uintptr_t>(&array[0]);
    EXPECT_EQ(address % (std::uintptr_t{2} * 1024 * 1024), 0U);
}

TEST(FixedArrayTest, ElementsAreWritable) {
    FixedArray<std::uint32_t> array{std::size_t{3} * 1024 * 1024, PagePolicy::kHuge};
    array[array.size() - 1] = 7;
    EXPECT_EQ(array[array.size() - 1], 7U);
}

TEST(OrderPoolTest, HandsOutEverySlotExactlyOnce) {
    OrderPool pool{100, PagePolicy::kDefault};
    std::set<std::uint32_t> seen;
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(pool.full());
        seen.insert(pool.acquire());
    }
    EXPECT_TRUE(pool.full());
    EXPECT_EQ(seen.size(), 100U);
    EXPECT_EQ(pool.in_use(), 100U);
    EXPECT_EQ(*seen.rbegin(), 99U);
}

TEST(OrderPoolTest, ReusesTheMostRecentlyReleasedSlot) {
    OrderPool pool{10, PagePolicy::kDefault};
    const std::uint32_t first = pool.acquire();
    const std::uint32_t second = pool.acquire();
    pool.release(first);
    pool.release(second);
    EXPECT_EQ(pool.acquire(), second);
    EXPECT_EQ(pool.acquire(), first);
}

TEST(OrderPoolTest, SingleSlotCycles) {
    OrderPool pool{1, PagePolicy::kDefault};
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t slot = pool.acquire();
        EXPECT_EQ(slot, 0U);
        EXPECT_TRUE(pool.full());
        pool.release(slot);
        EXPECT_FALSE(pool.full());
    }
}

// The bitmap is compared with std::set, whose answers are easy to trust, at sizes around each layer
// boundary, because that's where off-by-one errors would be.
class LevelBitmapTest : public ::testing::TestWithParam<std::uint32_t> {};

TEST_P(LevelBitmapTest, AgreesWithReferenceSet) {
    const std::uint32_t size = GetParam();
    LevelBitmap bitmap{size, PagePolicy::kDefault};
    std::set<std::uint32_t> reference;
    std::mt19937_64 rng{size};

    const auto expected_above = [&reference](std::uint32_t index) {
        const auto found = reference.upper_bound(index);
        return found == reference.end() ? LevelBitmap::kNone : *found;
    };
    const auto expected_below = [&reference](std::uint32_t index) {
        const auto found = reference.lower_bound(index);
        return found == reference.begin() ? LevelBitmap::kNone : *std::prev(found);
    };

    for (int step = 0; step < 20'000; ++step) {
        const auto index = static_cast<std::uint32_t>(rng() % size);
        switch (rng() % 3) {
            case 0:
                bitmap.set(index);
                reference.insert(index);
                break;
            case 1:
                bitmap.clear(index);
                reference.erase(index);
                break;
            default:
                break;
        }
        const auto probe = static_cast<std::uint32_t>(rng() % size);
        ASSERT_EQ(bitmap.test(probe), reference.contains(probe)) << "probe " << probe;
        ASSERT_EQ(bitmap.first_above(probe), expected_above(probe)) << "probe " << probe;
        ASSERT_EQ(bitmap.first_below(probe), expected_below(probe)) << "probe " << probe;
    }
}

TEST_P(LevelBitmapTest, FindsAcrossLongGaps) {
    const std::uint32_t size = GetParam();
    LevelBitmap bitmap{size, PagePolicy::kDefault};
    EXPECT_EQ(bitmap.first_above(0), LevelBitmap::kNone);
    EXPECT_EQ(bitmap.first_below(size - 1), LevelBitmap::kNone);

    bitmap.set(0);
    bitmap.set(size - 1);
    if (size > 1) {
        EXPECT_EQ(bitmap.first_above(0), size - 1);
        EXPECT_EQ(bitmap.first_below(size - 1), 0U);
    }
    EXPECT_EQ(bitmap.first_above(size - 1), LevelBitmap::kNone);
    EXPECT_EQ(bitmap.first_below(0), LevelBitmap::kNone);

    bitmap.clear(size - 1);
    bitmap.clear(0);
    EXPECT_FALSE(bitmap.test(0));
    if (size > 1) {
        EXPECT_EQ(bitmap.first_above(0), LevelBitmap::kNone);
        EXPECT_EQ(bitmap.first_below(size - 1), LevelBitmap::kNone);
    }
}

INSTANTIATE_TEST_SUITE_P(Sizes, LevelBitmapTest,
                         ::testing::Values(1U, 2U, 63U, 64U, 65U, 127U, 128U, 4095U, 4096U, 4097U,
                                           262'144U, 262'145U, 1'048'576U));

TEST(LevelBitmapSizeTest, HandlesLargestAcceptedBand) {
    LevelBitmap bitmap{static_cast<std::uint32_t>(kMaxPriceLevels), PagePolicy::kDefault};
    const auto last = static_cast<std::uint32_t>(kMaxPriceLevels - 1);
    bitmap.set(0);
    bitmap.set(last);
    EXPECT_EQ(bitmap.first_above(0), last);
    EXPECT_EQ(bitmap.first_below(last), 0U);
}

/// Stands in for the pool: answers whether a value really belongs to an ID.
class IdIndexHarness {
  public:
    explicit IdIndexHarness(std::uint32_t max_entries)
        : index_{max_entries, PagePolicy::kDefault} {}

    void insert(std::uint64_t id, std::uint32_t value) {
        index_.insert(OrderId{id}, value);
        owners_[value] = id;
    }

    void erase(std::uint64_t id, std::uint32_t value) {
        index_.erase(OrderId{id}, value);
        owners_.erase(value);
    }

    [[nodiscard]] std::uint32_t find(std::uint64_t id) const {
        return index_.find(OrderId{id}, [this, id](std::uint32_t value) {
            const auto owner = owners_.find(value);
            return owner != owners_.end() && owner->second == id;
        });
    }

  private:
    IdIndex index_;
    std::unordered_map<std::uint32_t, std::uint64_t> owners_;
};

TEST(IdIndexTest, FindsWhatWasInserted) {
    IdIndexHarness index{16};
    index.insert(5, 50);
    index.insert(6, 60);
    EXPECT_EQ(index.find(5), 50U);
    EXPECT_EQ(index.find(6), 60U);
    EXPECT_EQ(index.find(7), IdIndex::kNotFound);
}

TEST(IdIndexTest, IdZeroIsNeverFound) {
    IdIndexHarness index{16};
    EXPECT_EQ(index.find(0), IdIndex::kNotFound);
    index.insert(5, 50);
    EXPECT_EQ(index.find(0), IdIndex::kNotFound);
}

TEST(IdIndexTest, ValueZeroIsAnOrdinaryValue) {
    IdIndexHarness index{16};
    index.insert(5, 0);
    EXPECT_EQ(index.find(5), 0U);
    index.erase(5, 0);
    EXPECT_EQ(index.find(5), IdIndex::kNotFound);
}

TEST(IdIndexTest, EraseRemovesOnlyThatEntry) {
    IdIndexHarness index{16};
    index.insert(5, 50);
    index.insert(6, 60);
    index.erase(5, 50);
    EXPECT_EQ(index.find(5), IdIndex::kNotFound);
    EXPECT_EQ(index.find(6), 60U);
}

TEST(IdIndexTest, IdsSharingLowBitsStayDistinct) {
    // These all hash to the same slot and have the same stored bits, so only the check tells them
    // apart.
    constexpr std::uint64_t kStep = std::uint64_t{1} << 32U;
    IdIndexHarness index{8};
    index.insert(7, 1);
    index.insert(7 + kStep, 2);
    index.insert(7 + (2 * kStep), 3);
    EXPECT_EQ(index.find(7), 1U);
    EXPECT_EQ(index.find(7 + kStep), 2U);
    EXPECT_EQ(index.find(7 + (2 * kStep)), 3U);
    EXPECT_EQ(index.find(7 + (3 * kStep)), IdIndex::kNotFound);

    index.erase(7 + kStep, 2);
    EXPECT_EQ(index.find(7), 1U);
    EXPECT_EQ(index.find(7 + kStep), IdIndex::kNotFound);
    EXPECT_EQ(index.find(7 + (2 * kStep)), 3U);
}

TEST(IdIndexTest, HandlesTheMaximumEntryCount) {
    constexpr std::uint32_t kEntries = 4096;
    IdIndexHarness index{kEntries};
    for (std::uint32_t i = 1; i <= kEntries; ++i) {
        index.insert(i, i * 2);
    }
    for (std::uint32_t i = 1; i <= kEntries; ++i) {
        EXPECT_EQ(index.find(i), i * 2);
    }
}

// Churn across small tables forces long probe runs that wrap around the end of the table, which is
// what the backward shift has to get right. Values are unique, as pool slots are in real use.
class IdIndexChurnTest : public ::testing::TestWithParam<std::uint32_t> {};

TEST_P(IdIndexChurnTest, AgreesWithReferenceMap) {
    const std::uint32_t max_entries = GetParam();
    IdIndexHarness index{max_entries};
    std::unordered_map<std::uint64_t, std::uint32_t> reference;
    std::vector<std::uint64_t> live;
    std::mt19937_64 rng{max_entries};
    std::uint64_t next_id = 1;
    std::uint32_t next_value = 0;

    for (int step = 0; step < 200'000; ++step) {
        const bool can_insert = live.size() < max_entries;
        if (can_insert && (live.empty() || rng() % 2 == 0)) {
            const std::uint64_t id = next_id;
            // Occasional jumps past 2^32 produce IDs that share low bits with others.
            next_id += rng() % 64 == 0 ? (std::uint64_t{1} << 32U) : 1 + (rng() % 5);
            const std::uint32_t value = next_value++;
            index.insert(id, value);
            reference[id] = value;
            live.push_back(id);
        } else {
            const std::size_t position = rng() % live.size();
            const std::uint64_t id = live[position];
            index.erase(id, reference.at(id));
            reference.erase(id);
            live[position] = live.back();
            live.pop_back();
        }
        if (!live.empty()) {
            const std::uint64_t present = live[rng() % live.size()];
            ASSERT_EQ(index.find(present), reference.at(present)) << "id " << present;
        }
        const std::uint64_t maybe = 1 + (rng() % next_id);
        const auto found = reference.find(maybe);
        ASSERT_EQ(index.find(maybe), found == reference.end() ? IdIndex::kNotFound : found->second)
            << "id " << maybe;
    }
}

INSTANTIATE_TEST_SUITE_P(Sizes, IdIndexChurnTest, ::testing::Values(1U, 2U, 3U, 7U, 64U, 1000U));

TEST(OrderBookConfigTest, RefusesOversizedBand) {
    const BookConfig config{.min_price = Price{1},
                            .max_price = Price{static_cast<Price::rep>(kMaxPriceLevels) + 1},
                            .max_resting_orders = 1};
    EXPECT_THROW(const OrderBook<> book{config}, std::invalid_argument);
}

TEST(OrderBookConfigTest, AcceptsBandAtTheLimit) {
    const BookConfig config{.min_price = Price{1},
                            .max_price = Price{static_cast<Price::rep>(kMaxPriceLevels)},
                            .max_resting_orders = 1};
    EXPECT_NO_THROW(const OrderBook<PagePolicy::kDefault> book{config});
}

TEST(OrderBookConfigTest, PricesAtBandEdgesMapCorrectly) {
    const BookConfig config{
        .min_price = Price{500}, .max_price = Price{600}, .max_resting_orders = 8};
    OrderBook<> book{config};
    NullSink sink;
    book.apply(Command{.seq = Sequence{1},
                       .order_id = OrderId{1},
                       .price = Price{500},
                       .quantity = Quantity{1},
                       .type = CommandType::kLimit,
                       .side = Side::kBuy},
               sink);
    book.apply(Command{.seq = Sequence{2},
                       .order_id = OrderId{2},
                       .price = Price{600},
                       .quantity = Quantity{2},
                       .type = CommandType::kLimit,
                       .side = Side::kSell},
               sink);
    EXPECT_EQ(book.best_bid(), Price{500});
    EXPECT_EQ(book.best_ask(), Price{600});
    EXPECT_EQ(book.open_quantity_at(Side::kBuy, Price{500}), 1U);
    EXPECT_EQ(book.open_quantity_at(Side::kSell, Price{600}), 2U);
    EXPECT_EQ(book.open_quantity_at(Side::kBuy, Price{499}), 0U);
    EXPECT_EQ(book.open_quantity_at(Side::kSell, Price{601}), 0U);
}

}  // namespace
}  // namespace nanomatch

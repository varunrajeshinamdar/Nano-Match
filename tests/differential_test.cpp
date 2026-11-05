// The optimized book against the reference book: identical input must give byte-identical output.
//
// The two share no matching code and use unrelated data structures, so a bug would have to appear
// in both in the same way to go unnoticed. This is the main correctness guarantee for the optimized
// book.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "command_stream.hpp"
#include "nanomatch/book.hpp"
#include "nanomatch/order_book.hpp"
#include "test_support.hpp"
#include "workload/generator.hpp"

namespace nanomatch::test {
namespace {

using Reference = baseline::BaselineBook<>;

template <typename Book>
class DifferentialTest : public ::testing::Test {
  protected:
    /// Feeds commands to both books until `next` has produced `count`, comparing every event.
    template <typename NextCommand>
    static void compare(const BookConfig& config, std::size_t count, NextCommand next) {
        Book book{config};
        Reference reference{config};
        VectorSink book_events;
        VectorSink reference_events;

        for (std::size_t i = 0; i < count; ++i) {
            const Command command = next();
            book_events.events.clear();
            reference_events.events.clear();
            book.apply(command, book_events);
            reference.apply(command, reference_events);

            ASSERT_EQ(book_events.events, reference_events.events) << "command index " << i;
            if (!reference_events.events.empty()) {
                ASSERT_EQ(std::memcmp(book_events.events.data(), reference_events.events.data(),
                                      reference_events.events.size() * sizeof(Event)),
                          0)
                    << "bytes differ at command index " << i;
            }
            ASSERT_EQ(book.best_bid(), reference.best_bid()) << "command index " << i;
            ASSERT_EQ(book.best_ask(), reference.best_ask()) << "command index " << i;
            ASSERT_EQ(book.resting_orders(), reference.resting_orders()) << "command index " << i;

            // Depth at the touch is checked here too, because the book's running total and the
            // reference's summed queue could otherwise disagree without any event showing it.
            if (const std::optional<Price> bid = reference.best_bid()) {
                ASSERT_EQ(book.open_quantity_at(Side::kBuy, *bid),
                          reference.open_quantity_at(Side::kBuy, *bid))
                    << "command index " << i;
            }
            if (const std::optional<Price> ask = reference.best_ask()) {
                ASSERT_EQ(book.open_quantity_at(Side::kSell, *ask),
                          reference.open_quantity_at(Side::kSell, *ask))
                    << "command index " << i;
            }
        }
    }
};

using OptimizedBooks =
    ::testing::Types<OrderBook<PagePolicy::kHuge>, OrderBook<PagePolicy::kDefault>,
                     baseline::BaselineBook<baseline::PoolAllocator>>;
TYPED_TEST_SUITE(DifferentialTest, OptimizedBooks);

// Valid and invalid commands mixed, including every kind of reject, across several seeds and
// capacities.

TYPED_TEST(DifferentialTest, RandomStreamsOnRoomyBook) {
    for (const std::uint64_t seed : {11U, 12U, 13U}) {
        CommandStream stream{seed};
        this->compare(
            BookConfig{.min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = 4096},
            100'000, [&stream] { return stream.next(); });
        if (this->HasFatalFailure()) {
            return;
        }
    }
}

TYPED_TEST(DifferentialTest, RandomStreamsWhenCapacityIsScarce) {
    for (const std::uint32_t capacity : {1U, 2U, 8U, 64U}) {
        CommandStream stream{capacity};
        this->compare(
            BookConfig{
                .min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = capacity},
            50'000, [&stream] { return stream.next(); });
        if (this->HasFatalFailure()) {
            return;
        }
    }
}

// The same flow the benchmarks use, so that what's measured is known to behave identically.

TYPED_TEST(DifferentialTest, EveryBenchmarkWorkload) {
    for (const std::string_view name : {"realistic", "add_cancel", "sweep", "realistic_1k"}) {
        const workload::Profile* const profile = workload::find_profile(name);
        ASSERT_NE(profile, nullptr);
        workload::Generator generator{*profile, 7};
        this->compare(workload::book_config(*profile), 300'000,
                      [&generator] { return generator.next(); });
        if (this->HasFatalFailure()) {
            return;
        }
    }
}

// A very narrow band forces constant level creation and removal at the edges of the ladder.

TYPED_TEST(DifferentialTest, NarrowBandStressesLevelBoundaries) {
    CommandStream stream{99};
    this->compare(
        BookConfig{.min_price = Price{486}, .max_price = Price{514}, .max_resting_orders = 256},
        100'000, [&stream] { return stream.next(); });
}

}  // namespace
}  // namespace nanomatch::test

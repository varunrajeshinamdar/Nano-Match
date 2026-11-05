#include "nanomatch/spsc_ring.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "command_stream.hpp"
#include "nanomatch/log_io.hpp"
#include "nanomatch/order_book.hpp"
#include "nanomatch/pipeline.hpp"
#include "test_support.hpp"
#include "workload/generator.hpp"

namespace nanomatch {
namespace {

/// Larger than a word on purpose, so that a torn read or write would show up as a mismatch.
struct Payload {
    std::array<std::uint64_t, 5> values{};

    friend bool operator==(const Payload&, const Payload&) noexcept = default;
};

[[nodiscard]] Payload payload_for(std::uint64_t sequence) noexcept {
    Payload payload;
    for (std::size_t i = 0; i < payload.values.size(); ++i) {
        payload.values[i] = (sequence * 31) + i;
    }
    return payload;
}

using PaddedCached = SpscRing<Payload, 16>;
using PaddedUncached = SpscRing<Payload, 16, SpscRingLayout{.cache_indices = false}>;
using UnpaddedCached = SpscRing<Payload, 16, SpscRingLayout{.index_alignment = 8}>;
using UnpaddedUncached =
    SpscRing<Payload, 16, SpscRingLayout{.index_alignment = 8, .cache_indices = false}>;
using WidePadding = SpscRing<Payload, 16, SpscRingLayout{.index_alignment = 128}>;

static_assert(alignof(PaddedCached) == 64);
static_assert(alignof(WidePadding) == 128);
static_assert(alignof(UnpaddedCached) == 8);
static_assert(sizeof(PaddedCached) >= (std::size_t{4} * 64) + sizeof(std::array<Payload, 16>));

template <typename Ring>
class SpscRingTest : public ::testing::Test {};

using RingLayouts =
    ::testing::Types<PaddedCached, PaddedUncached, UnpaddedCached, UnpaddedUncached, WidePadding>;
TYPED_TEST_SUITE(SpscRingTest, RingLayouts);

TYPED_TEST(SpscRingTest, StartsEmpty) {
    const auto ring = std::make_unique<TypeParam>();
    Payload item;
    EXPECT_FALSE(ring->try_pop(item));
}

TYPED_TEST(SpscRingTest, EmptyPopLeavesTheOutputUntouched) {
    const auto ring = std::make_unique<TypeParam>();
    Payload item = payload_for(7);
    EXPECT_FALSE(ring->try_pop(item));
    EXPECT_EQ(item, payload_for(7));
}

TYPED_TEST(SpscRingTest, HoldsExactlyItsCapacity) {
    const auto ring = std::make_unique<TypeParam>();
    for (std::uint64_t i = 0; i < TypeParam::capacity(); ++i) {
        EXPECT_TRUE(ring->try_push(payload_for(i)));
    }
    EXPECT_FALSE(ring->try_push(payload_for(99)));

    Payload item;
    EXPECT_TRUE(ring->try_pop(item));
    EXPECT_EQ(item, payload_for(0));
    EXPECT_TRUE(ring->try_push(payload_for(99)));
    EXPECT_FALSE(ring->try_push(payload_for(100)));
}

TYPED_TEST(SpscRingTest, KeepsOrderAcrossManyWraps) {
    const auto ring = std::make_unique<TypeParam>();
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
    Payload item;
    for (int round = 0; round < 1000; ++round) {
        const auto batch = static_cast<std::size_t>((round % 16) + 1);
        for (std::size_t i = 0; i < batch; ++i) {
            ASSERT_TRUE(ring->try_push(payload_for(pushed++)));
        }
        for (std::size_t i = 0; i < batch; ++i) {
            ASSERT_TRUE(ring->try_pop(item));
            ASSERT_EQ(item, payload_for(popped++));
        }
        ASSERT_FALSE(ring->try_pop(item));
    }
}

// The real test of the memory orders. Run under ThreadSanitizer, it fails on any missing ordering,
// and checking every payload catches items that are lost, repeated, reordered or torn. A small ring
// keeps it full and empty often, which exercises both slow paths.
TYPED_TEST(SpscRingTest, TwoThreadsKeepEveryItemInOrder) {
    constexpr std::uint64_t kItems = 1'000'000;
    const auto ring = std::make_unique<TypeParam>();

    std::thread producer{[&ring] {
        for (std::uint64_t i = 0; i < kItems; ++i) {
            const Payload item = payload_for(i);
            while (!ring->try_push(item)) {
                std::this_thread::yield();
            }
        }
    }};

    std::uint64_t mismatches = 0;
    Payload item;
    for (std::uint64_t i = 0; i < kItems; ++i) {
        while (!ring->try_pop(item)) {
            std::this_thread::yield();
        }
        mismatches += item == payload_for(i) ? 0U : 1U;
    }
    producer.join();

    EXPECT_EQ(mismatches, 0U);
    EXPECT_FALSE(ring->try_pop(item));
}

// The pipeline must give the same output as applying commands directly on one thread.

using PipelineRing = SpscRing<Command, 1024>;

[[nodiscard]] std::uint64_t hash_directly(const std::vector<Command>& commands,
                                          const BookConfig& config) {
    OrderBook<> book{config};
    EventLogSink sink;
    for (const Command& command : commands) {
        book.apply(command, sink);
    }
    return sink.hash();
}

[[nodiscard]] std::uint64_t hash_through_pipeline(const std::vector<Command>& commands,
                                                  const BookConfig& config) {
    OrderBook<> book{config};
    EventLogSink sink;
    const auto ring = std::make_unique<PipelineRing>();
    run_pipeline(std::span<const Command>{commands}, *ring, book, sink);
    return sink.hash();
}

TEST(PipelineTest, MatchesSingleThreadedOutputOnAWorkload) {
    const workload::Profile* const profile = workload::find_profile("realistic");
    ASSERT_NE(profile, nullptr);
    workload::Generator generator{*profile, 21};
    std::vector<Command> commands;
    commands.reserve(200'000);
    for (int i = 0; i < 200'000; ++i) {
        commands.push_back(generator.next());
    }
    const BookConfig config = workload::book_config(*profile);
    EXPECT_EQ(hash_through_pipeline(commands, config), hash_directly(commands, config));
}

TEST(PipelineTest, MatchesSingleThreadedOutputWithInvalidCommands) {
    test::CommandStream stream{22};
    std::vector<Command> commands;
    commands.reserve(100'000);
    for (int i = 0; i < 100'000; ++i) {
        commands.push_back(stream.next());
    }
    const BookConfig config{
        .min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = 64};
    EXPECT_EQ(hash_through_pipeline(commands, config), hash_directly(commands, config));
}

TEST(PipelineTest, IsRepeatable) {
    test::CommandStream stream{23};
    std::vector<Command> commands;
    commands.reserve(50'000);
    for (int i = 0; i < 50'000; ++i) {
        commands.push_back(stream.next());
    }
    const BookConfig config{
        .min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = 1024};
    const std::uint64_t first = hash_through_pipeline(commands, config);
    for (int attempt = 0; attempt < 3; ++attempt) {
        EXPECT_EQ(hash_through_pipeline(commands, config), first);
    }
}

}  // namespace
}  // namespace nanomatch

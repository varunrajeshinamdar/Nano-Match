#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "nanomatch/book.hpp"
#include "test_support.hpp"
#include "workload/generator.hpp"
#include "workload/rng.hpp"

namespace nanomatch::workload {
namespace {

// Reference outputs for the published algorithms. These pin the generators, so that a change can't
// silently alter every workload.

TEST(RngTest, SplitMix64MatchesReferenceOutput) {
    SplitMix64 generator{1234567};
    EXPECT_EQ(generator.next(), 6457827717110365317ULL);
    EXPECT_EQ(generator.next(), 3203168211198807973ULL);
    EXPECT_EQ(generator.next(), 9817491932198370423ULL);
    EXPECT_EQ(generator.next(), 4593380528125082431ULL);
    EXPECT_EQ(generator.next(), 16408922859458223821ULL);
}

TEST(RngTest, Xoshiro256StarStarMatchesReferenceOutput) {
    Rng generator{std::array<std::uint64_t, 4>{1, 2, 3, 4}};
    EXPECT_EQ(generator.next(), 11520ULL);
    EXPECT_EQ(generator.next(), 0ULL);
    EXPECT_EQ(generator.next(), 1509978240ULL);
    EXPECT_EQ(generator.next(), 1215971899390074240ULL);
    EXPECT_EQ(generator.next(), 1216172134540287360ULL);
}

TEST(RngTest, BelowStaysInRange) {
    Rng generator{1};
    for (int i = 0; i < 10'000; ++i) {
        EXPECT_LT(generator.below(7), 7U);
    }
}

TEST(RngTest, ChanceRespectsExtremes) {
    Rng generator{1};
    for (int i = 0; i < 1'000; ++i) {
        EXPECT_FALSE(generator.chance(0));
        EXPECT_TRUE(generator.chance(1000));
    }
}

TEST(RngTest, GeometricRespectsCap) {
    Rng generator{1};
    for (int i = 0; i < 1'000; ++i) {
        EXPECT_LE(generator.geometric(1, 5), 5U);
    }
    EXPECT_EQ(generator.geometric(1000, 5), 0U);
}

constexpr std::size_t kCommands = 100'000;

[[nodiscard]] const Profile& profile_named(std::string_view name) {
    const Profile* const profile = find_profile(name);
    EXPECT_NE(profile, nullptr) << name;
    return *profile;
}

[[nodiscard]] std::vector<Command> generate(const Profile& profile, std::uint64_t seed,
                                            std::size_t count) {
    Generator generator{profile, seed};
    std::vector<Command> commands;
    commands.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        commands.push_back(generator.next());
    }
    return commands;
}

TEST(WorkloadTest, ProfileMixesSumToOneThousand) {
    for (const Profile& profile : kProfiles) {
        EXPECT_EQ(profile.passive_limit + profile.crossing_limit + profile.market + profile.cancel +
                      profile.modify,
                  1000U)
            << profile.name;
        EXPECT_NO_THROW(validate_config(book_config(profile))) << profile.name;
    }
}

TEST(WorkloadTest, UnknownProfileIsNotFound) {
    EXPECT_EQ(find_profile("nonsense"), nullptr);
}

TEST(WorkloadTest, SameSeedGivesSameCommands) {
    const Profile& profile = profile_named("realistic");
    EXPECT_EQ(generate(profile, 1, kCommands), generate(profile, 1, kCommands));
}

TEST(WorkloadTest, DifferentSeedsGiveDifferentCommands) {
    const Profile& profile = profile_named("realistic");
    EXPECT_NE(generate(profile, 1, kCommands), generate(profile, 2, kCommands));
}

TEST(WorkloadTest, EveryProfileGeneratesOnlyWellFormedCommands) {
    for (const Profile& profile : kProfiles) {
        std::uint64_t highest_id = 0;
        std::uint64_t last_seq = 0;
        for (const Command& command : generate(profile, 1, 20'000)) {
            ASSERT_TRUE(is_well_formed(command)) << profile.name;
            ASSERT_GT(command.seq.value(), last_seq) << profile.name;
            last_seq = command.seq.value();
            if (command.type == CommandType::kLimit || command.type == CommandType::kMarket) {
                ASSERT_GT(command.order_id.value(), highest_id) << profile.name;
                highest_id = command.order_id.value();
            }
        }
    }
}

TEST(WorkloadTest, ReachesAndHoldsTargetDepth) {
    for (const std::string_view name : {"realistic", "add_cancel"}) {
        const Profile& profile = profile_named(name);
        Generator generator{profile, 1};
        const std::size_t target = profile.target_resting_orders;
        for (std::size_t i = 0; i < kCommands; ++i) {
            (void)generator.next();
            if (generator.stats().commands_to_reach_target > 0) {
                ASSERT_GE(generator.resting_orders(), target - (target / 10) - 100) << name;
                ASSERT_LE(generator.resting_orders(), target + (target / 10) + 100) << name;
            }
        }
        EXPECT_GT(generator.stats().commands_to_reach_target, 0U) << name;
    }
}

TEST(WorkloadTest, GeneratedFlowNeverFillsTheBook) {
    for (const std::string_view name : {"realistic", "add_cancel", "sweep"}) {
        Generator generator{profile_named(name), 1};
        for (std::size_t i = 0; i < kCommands; ++i) {
            (void)generator.next();
        }
        EXPECT_EQ(generator.stats().book_full_events, 0U) << name;
    }
}

TEST(WorkloadTest, AddCancelNeverTrades) {
    Generator generator{profile_named("add_cancel"), 1};
    for (std::size_t i = 0; i < kCommands; ++i) {
        (void)generator.next();
    }
    EXPECT_EQ(generator.stats().trade_events, 0U);
    EXPECT_EQ(generator.stats().rejected_events, 0U);
    EXPECT_EQ(generator.stats().modify_commands, 0U);
    EXPECT_EQ(generator.stats().market_commands, 0U);
}

TEST(WorkloadTest, RealisticFlowExercisesEveryOutcome) {
    Generator generator{profile_named("realistic"), 1};
    for (std::size_t i = 0; i < kCommands; ++i) {
        (void)generator.next();
    }
    const Stats& stats = generator.stats();
    EXPECT_GT(stats.limit_commands, 0U);
    EXPECT_GT(stats.market_commands, 0U);
    EXPECT_GT(stats.cancel_commands, 0U);
    EXPECT_GT(stats.modify_commands, 0U);
    EXPECT_GT(stats.trade_events, 0U);
    EXPECT_GT(stats.rejected_events, 0U);
}

TEST(WorkloadTest, SweepTradesFarMoreThanRealistic) {
    Generator realistic{profile_named("realistic"), 1};
    Generator sweep{profile_named("sweep"), 1};
    for (std::size_t i = 0; i < kCommands; ++i) {
        (void)realistic.next();
        (void)sweep.next();
    }
    EXPECT_GT(sweep.stats().trade_events, realistic.stats().trade_events * 3);
}

TEST(WorkloadTest, PriceWanders) {
    // A book stuck at one price would keep all accesses in a small window and flatter cache
    // behaviour.
    // add_cancel is left out because it has no aggressive flow, so its price moves far more slowly.
    for (const std::string_view name : {"realistic", "sweep"}) {
        Generator generator{profile_named(name), 1};
        for (std::size_t i = 0; i < 1'000'000; ++i) {
            (void)generator.next();
        }
        const Stats& stats = generator.stats();
        EXPECT_GE(stats.highest_best_bid - stats.lowest_best_bid, 50) << name;
    }
}

TEST(WorkloadTest, SweepHoldsDepthWhileTradingHeavily) {
    Generator generator{profile_named("sweep"), 1};
    for (std::size_t i = 0; i < 1'000'000; ++i) {
        (void)generator.next();
    }
    const std::size_t target = profile_named("sweep").target_resting_orders;
    EXPECT_GE(generator.resting_orders(), target / 2);
    EXPECT_GT(generator.stats().trade_events, 500'000U);
}

TEST(WorkloadTest, RejectsAreOnlyStaleTargets) {
    // Generated flow is valid apart from deliberately stale cancels and modifies.
    const Profile& profile = profile_named("realistic");
    test::VectorSink sink;
    baseline::BaselineBook<> book{book_config(profile)};
    for (const Command& command : generate(profile, 1, kCommands)) {
        book.apply(command, sink);
    }
    for (const Event& event : sink.events) {
        if (event.type == EventType::kRejected) {
            EXPECT_EQ(event.reason, Reason::kUnknownId);
        }
    }
}

}  // namespace
}  // namespace nanomatch::workload

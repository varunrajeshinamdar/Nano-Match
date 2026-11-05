#pragma once

#include <algorithm>
#include <cstdint>
#include <random>

#include "nanomatch/protocol.hpp"
#include "test_support.hpp"

namespace nanomatch::test {

/// Seeded stream mixing normal flow with every kind of invalid command.
class CommandStream {
  public:
    explicit CommandStream(std::uint64_t seed) : rng_{seed} {}

    [[nodiscard]] Command next() {
        Command command = draw();
        command.seq = Sequence{++seq_};
        return command;
    }

  private:
    [[nodiscard]] std::uint64_t below(std::uint64_t limit) { return rng_() % limit; }

    [[nodiscard]] std::uint32_t small_quantity() {
        return static_cast<std::uint32_t>(1 + below(20));
    }

    [[nodiscard]] std::int64_t price_near_mid() {
        return 500 - 15 + static_cast<std::int64_t>(below(31));
    }

    [[nodiscard]] Side random_side() { return below(2) == 0 ? Side::kBuy : Side::kSell; }

    [[nodiscard]] std::uint64_t fresh_id() {
        next_id_ += 1 + below(3);
        return next_id_;
    }

    // Recent IDs are a mix of live and departed orders, which exercises both outcomes.
    [[nodiscard]] std::uint64_t recent_id() {
        return next_id_ == 0 ? 1 : next_id_ - std::min(next_id_ - 1, below(100));
    }

    [[nodiscard]] Command draw() {
        const std::uint64_t roll = below(100);
        if (roll < 50) {
            return make_limit(fresh_id(), random_side(), price_near_mid(), small_quantity());
        }
        if (roll < 55) {
            return make_market(fresh_id(), random_side(), small_quantity());
        }
        if (roll < 75) {
            return make_cancel(recent_id());
        }
        if (roll < 92) {
            return make_modify(recent_id(), price_near_mid(), small_quantity());
        }
        return draw_invalid();
    }

    [[nodiscard]] Command draw_invalid() {
        switch (below(8)) {
            case 0:
                return make_limit(recent_id(), random_side(), price_near_mid(), small_quantity());
            case 1:
                return make_limit(fresh_id(), random_side(), 5000, small_quantity());
            case 2:
                return make_limit(fresh_id(), random_side(), price_near_mid(), 0);
            case 3:
                return make_modify(recent_id(), price_near_mid(), 0);
            case 6:
                return make_cancel(0);
            case 7:
                return make_modify(0, price_near_mid(), small_quantity());
            case 4: {
                Command command = make_limit(fresh_id(), Side::kNone, price_near_mid(), 1);
                command.reserved = 1;
                return command;
            }
            default: {
                Command command = make_cancel(recent_id());
                command.type = static_cast<CommandType>(77);
                return command;
            }
        }
    }

    std::mt19937_64 rng_;
    std::uint64_t seq_{0};
    std::uint64_t next_id_{0};
};

}  // namespace nanomatch::test

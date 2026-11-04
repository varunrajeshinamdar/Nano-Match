// Behaviour of every book implementation against docs/SPEC.md.

#include <optional>
#include <stdexcept>

#include <gtest/gtest.h>

#include "test_support.hpp"

namespace nanomatch::test {
namespace {

constexpr Side kBuy = Side::kBuy;
constexpr Side kSell = Side::kSell;
constexpr Side kNoSide = Side::kNone;

template <typename Book>
class BookTest : public ::testing::Test {
  protected:
    BookHarness<Book> h;
};

TYPED_TEST_SUITE(BookTest, BookTypes);

// Section 7.4: the worked example, transcribed literally.

TYPED_TEST(BookTest, SpecificationExample) {
    EXPECT_EQ(this->h.limit(10, kSell, 100, 5),
              (Events{accepted(10, kSell, 100, 5), rested(10, kSell, 100, 5)}));
    EXPECT_EQ(this->h.limit(11, kBuy, 101, 8),
              (Events{accepted(11, kBuy, 101, 8), trade(11, kBuy, 10, 100, 5, 3, 0),
                      rested(11, kBuy, 101, 3)}));
}

// Section 5: matching rules.

TYPED_TEST(BookTest, EmptyBookHasNoBestPrices) {
    EXPECT_EQ(this->h.book().best_bid(), std::nullopt);
    EXPECT_EQ(this->h.book().best_ask(), std::nullopt);
    EXPECT_EQ(this->h.book().resting_orders(), 0U);
}

TYPED_TEST(BookTest, NonCrossingOrdersRestOnTheirOwnSides) {
    this->h.limit(1, kBuy, 99, 5);
    this->h.limit(2, kSell, 101, 7);
    EXPECT_EQ(this->h.book().best_bid(), Price{99});
    EXPECT_EQ(this->h.book().best_ask(), Price{101});
    EXPECT_EQ(this->h.book().resting_orders(), 2U);
    EXPECT_EQ(this->h.book().open_quantity_at(kBuy, Price{99}), 5U);
    EXPECT_EQ(this->h.book().open_quantity_at(kSell, Price{101}), 7U);
}

TYPED_TEST(BookTest, BestPriceTracksTheBetterLevel) {
    this->h.limit(1, kBuy, 90, 1);
    this->h.limit(2, kBuy, 95, 1);
    this->h.limit(3, kBuy, 92, 1);
    EXPECT_EQ(this->h.book().best_bid(), Price{95});

    this->h.limit(4, kSell, 110, 1);
    this->h.limit(5, kSell, 105, 1);
    this->h.limit(6, kSell, 108, 1);
    EXPECT_EQ(this->h.book().best_ask(), Price{105});
}

TYPED_TEST(BookTest, EqualPricesCross) {
    this->h.limit(1, kSell, 100, 5);
    EXPECT_EQ(this->h.limit(2, kBuy, 100, 5),
              (Events{accepted(2, kBuy, 100, 5), trade(2, kBuy, 1, 100, 5, 0, 0)}));
    EXPECT_EQ(this->h.book().resting_orders(), 0U);
}

TYPED_TEST(BookTest, BuyBelowBestAskDoesNotCross) {
    this->h.limit(1, kSell, 100, 5);
    EXPECT_EQ(this->h.limit(2, kBuy, 99, 5),
              (Events{accepted(2, kBuy, 99, 5), rested(2, kBuy, 99, 5)}));
}

TYPED_TEST(BookTest, SellAboveBestBidDoesNotCross) {
    this->h.limit(1, kBuy, 100, 5);
    EXPECT_EQ(this->h.limit(2, kSell, 101, 5),
              (Events{accepted(2, kSell, 101, 5), rested(2, kSell, 101, 5)}));
}

TYPED_TEST(BookTest, TradesAtTheRestingPriceForBothSides) {
    this->h.limit(1, kSell, 100, 5);
    EXPECT_EQ(this->h.limit(2, kBuy, 105, 5),
              (Events{accepted(2, kBuy, 105, 5), trade(2, kBuy, 1, 100, 5, 0, 0)}));

    this->h.limit(3, kBuy, 100, 5);
    EXPECT_EQ(this->h.limit(4, kSell, 95, 5),
              (Events{accepted(4, kSell, 95, 5), trade(4, kSell, 3, 100, 5, 0, 0)}));
}

TYPED_TEST(BookTest, BetterPricesMatchFirst) {
    this->h.limit(1, kSell, 102, 1);
    this->h.limit(2, kSell, 100, 1);
    this->h.limit(3, kSell, 101, 1);
    EXPECT_EQ(this->h.limit(4, kBuy, 102, 3),
              (Events{accepted(4, kBuy, 102, 3), trade(4, kBuy, 2, 100, 1, 2, 0),
                      trade(4, kBuy, 3, 101, 1, 1, 0), trade(4, kBuy, 1, 102, 1, 0, 0)}));
}

TYPED_TEST(BookTest, BidsMatchHighestFirst) {
    this->h.limit(1, kBuy, 98, 1);
    this->h.limit(2, kBuy, 100, 1);
    this->h.limit(3, kBuy, 99, 1);
    EXPECT_EQ(this->h.limit(4, kSell, 98, 3),
              (Events{accepted(4, kSell, 98, 3), trade(4, kSell, 2, 100, 1, 2, 0),
                      trade(4, kSell, 3, 99, 1, 1, 0), trade(4, kSell, 1, 98, 1, 0, 0)}));
}

TYPED_TEST(BookTest, EqualPricesMatchInArrivalOrder) {
    this->h.limit(1, kSell, 100, 2);
    this->h.limit(2, kSell, 100, 2);
    this->h.limit(3, kSell, 100, 2);
    EXPECT_EQ(this->h.limit(4, kBuy, 100, 6),
              (Events{accepted(4, kBuy, 100, 6), trade(4, kBuy, 1, 100, 2, 4, 0),
                      trade(4, kBuy, 2, 100, 2, 2, 0), trade(4, kBuy, 3, 100, 2, 0, 0)}));
}

TYPED_TEST(BookTest, MatchingStopsAtTheLimitPrice) {
    this->h.limit(1, kSell, 100, 1);
    this->h.limit(2, kSell, 101, 1);
    this->h.limit(3, kSell, 102, 1);
    EXPECT_EQ(this->h.limit(4, kBuy, 101, 5),
              (Events{accepted(4, kBuy, 101, 5), trade(4, kBuy, 1, 100, 1, 4, 0),
                      trade(4, kBuy, 2, 101, 1, 3, 0), rested(4, kBuy, 101, 3)}));
    EXPECT_EQ(this->h.book().best_bid(), Price{101});
    EXPECT_EQ(this->h.book().best_ask(), Price{102});
}

TYPED_TEST(BookTest, PartiallyFilledRestingOrderKeepsItsPosition) {
    this->h.limit(1, kSell, 100, 10);
    this->h.limit(2, kSell, 100, 10);
    EXPECT_EQ(this->h.limit(3, kBuy, 100, 4),
              (Events{accepted(3, kBuy, 100, 4), trade(3, kBuy, 1, 100, 4, 0, 6)}));
    EXPECT_EQ(this->h.limit(4, kBuy, 100, 8),
              (Events{accepted(4, kBuy, 100, 8), trade(4, kBuy, 1, 100, 6, 2, 0),
                      trade(4, kBuy, 2, 100, 2, 0, 8)}));
    EXPECT_EQ(this->h.book().open_quantity_at(kSell, Price{100}), 8U);
}

// Section 4.2: market orders.

TYPED_TEST(BookTest, MarketOrderSweepsEveryPrice) {
    this->h.limit(1, kSell, 100, 1);
    this->h.limit(2, kSell, 900, 1);
    EXPECT_EQ(this->h.market(3, kBuy, 2),
              (Events{accepted(3, kBuy, 0, 2), trade(3, kBuy, 1, 100, 1, 1, 0),
                      trade(3, kBuy, 2, 900, 1, 0, 0)}));
    EXPECT_EQ(this->h.book().best_ask(), std::nullopt);
}

TYPED_TEST(BookTest, MarketSellMatchesBids) {
    this->h.limit(1, kBuy, 100, 3);
    EXPECT_EQ(this->h.market(2, kSell, 2),
              (Events{accepted(2, kSell, 0, 2), trade(2, kSell, 1, 100, 2, 0, 1)}));
    EXPECT_EQ(this->h.book().open_quantity_at(kBuy, Price{100}), 1U);
}

TYPED_TEST(BookTest, MarketOrderOnEmptyBookIsCancelledInFull) {
    EXPECT_EQ(this->h.market(1, kBuy, 5),
              (Events{accepted(1, kBuy, 0, 5), canceled(1, kBuy, 5, Reason::kMarketRemainder)}));
    EXPECT_EQ(this->h.book().resting_orders(), 0U);
}

TYPED_TEST(BookTest, MarketRemainderIsCancelledAndNeverRests) {
    this->h.limit(1, kSell, 100, 2);
    EXPECT_EQ(this->h.market(2, kBuy, 5),
              (Events{accepted(2, kBuy, 0, 5), trade(2, kBuy, 1, 100, 2, 3, 0),
                      canceled(2, kBuy, 3, Reason::kMarketRemainder)}));
    EXPECT_EQ(this->h.book().resting_orders(), 0U);
    EXPECT_EQ(this->h.book().best_bid(), std::nullopt);
}

// Section 4.3: cancels.

TYPED_TEST(BookTest, CancelRemovesOrderAndEmptyLevel) {
    this->h.limit(1, kBuy, 100, 5);
    EXPECT_EQ(this->h.cancel(1), (Events{canceled(1, kBuy, 5, Reason::kUserCancel)}));
    EXPECT_EQ(this->h.book().best_bid(), std::nullopt);
    EXPECT_EQ(this->h.book().resting_orders(), 0U);
}

TYPED_TEST(BookTest, CancelReportsOpenQuantityAfterPartialFill) {
    this->h.limit(1, kSell, 100, 5);
    this->h.limit(2, kBuy, 100, 2);
    EXPECT_EQ(this->h.cancel(1), (Events{canceled(1, kSell, 3, Reason::kUserCancel)}));
}

TYPED_TEST(BookTest, CancelFromMiddleOfQueuePreservesOrderOfOthers) {
    this->h.limit(1, kSell, 100, 1);
    this->h.limit(2, kSell, 100, 1);
    this->h.limit(3, kSell, 100, 1);
    this->h.cancel(2);
    EXPECT_EQ(this->h.limit(4, kBuy, 100, 2),
              (Events{accepted(4, kBuy, 100, 2), trade(4, kBuy, 1, 100, 1, 1, 0),
                      trade(4, kBuy, 3, 100, 1, 0, 0)}));
}

TYPED_TEST(BookTest, CancellingBestLevelExposesNextLevel) {
    this->h.limit(1, kBuy, 100, 1);
    this->h.limit(2, kBuy, 90, 1);
    this->h.cancel(1);
    EXPECT_EQ(this->h.book().best_bid(), Price{90});
}

// Section 3: order IDs.

TYPED_TEST(BookTest, IdMustBeStrictlyIncreasing) {
    this->h.limit(5, kBuy, 100, 1);
    EXPECT_EQ(this->h.limit(5, kBuy, 100, 1),
              (Events{rejected(5, kBuy, 100, 1, Reason::kDuplicateId)}));
    EXPECT_EQ(this->h.limit(4, kBuy, 100, 1),
              (Events{rejected(4, kBuy, 100, 1, Reason::kDuplicateId)}));
    EXPECT_EQ(this->h.book().resting_orders(), 1U);
}

TYPED_TEST(BookTest, IdsMayHaveGaps) {
    EXPECT_EQ(this->h.limit(1000000, kBuy, 100, 1),
              (Events{accepted(1000000, kBuy, 100, 1), rested(1000000, kBuy, 100, 1)}));
}

TYPED_TEST(BookTest, IdZeroIsRejected) {
    EXPECT_EQ(this->h.limit(0, kBuy, 100, 1),
              (Events{rejected(0, kBuy, 100, 1, Reason::kDuplicateId)}));
}

TYPED_TEST(BookTest, IdOfDepartedOrderCannotBeReused) {
    this->h.limit(1, kBuy, 100, 1);
    this->h.cancel(1);
    EXPECT_EQ(this->h.limit(1, kBuy, 100, 1),
              (Events{rejected(1, kBuy, 100, 1, Reason::kDuplicateId)}));
}

TYPED_TEST(BookTest, RejectedOrderStillAdvancesHighWaterMark) {
    EXPECT_EQ(this->h.limit(10, kBuy, 5000, 1),
              (Events{rejected(10, kBuy, 5000, 1, Reason::kInvalidPrice)}));
    EXPECT_EQ(this->h.limit(10, kBuy, 100, 1),
              (Events{rejected(10, kBuy, 100, 1, Reason::kDuplicateId)}));
}

TYPED_TEST(BookTest, MalformedOrderDoesNotAdvanceHighWaterMark) {
    Command command = make_limit(10, kBuy, 100, 1);
    command.reserved = 1;
    EXPECT_EQ(this->h.apply(command), (Events{rejected(10, kNoSide, 100, 1, Reason::kMalformed)}));
    EXPECT_EQ(this->h.limit(10, kBuy, 100, 1),
              (Events{accepted(10, kBuy, 100, 1), rested(10, kBuy, 100, 1)}));
}

TYPED_TEST(BookTest, MarketOrdersShareTheIdSequence) {
    this->h.market(5, kBuy, 1);
    EXPECT_EQ(this->h.limit(5, kBuy, 100, 1),
              (Events{rejected(5, kBuy, 100, 1, Reason::kDuplicateId)}));
}

// Section 6: validation.

TYPED_TEST(BookTest, UnknownCommandTypeIsMalformed) {
    Command command = make_limit(1, kBuy, 100, 1);
    command.type = static_cast<CommandType>(0);
    EXPECT_EQ(this->h.apply(command), (Events{rejected(1, kNoSide, 100, 1, Reason::kMalformed)}));
    command.type = static_cast<CommandType>(200);
    EXPECT_EQ(this->h.apply(command), (Events{rejected(1, kNoSide, 100, 1, Reason::kMalformed)}));
}

TYPED_TEST(BookTest, InvalidSideIsMalformed) {
    EXPECT_EQ(this->h.limit(1, kNoSide, 100, 1),
              (Events{rejected(1, kNoSide, 100, 1, Reason::kMalformed)}));
    EXPECT_EQ(this->h.limit(1, static_cast<Side>(9), 100, 1),
              (Events{rejected(1, kNoSide, 100, 1, Reason::kMalformed)}));
    EXPECT_EQ(this->h.market(1, kNoSide, 1),
              (Events{rejected(1, kNoSide, 0, 1, Reason::kMalformed)}));
}

TYPED_TEST(BookTest, MarketWithPriceIsMalformed) {
    Command command = make_market(1, kBuy, 1);
    command.price = Price{100};
    EXPECT_EQ(this->h.apply(command), (Events{rejected(1, kNoSide, 100, 1, Reason::kMalformed)}));
}

TYPED_TEST(BookTest, CancelWithExtraFieldsIsMalformed) {
    this->h.limit(1, kBuy, 100, 1);

    Command with_side = make_cancel(1);
    with_side.side = kBuy;
    EXPECT_EQ(this->h.apply(with_side), (Events{rejected(1, kNoSide, 0, 0, Reason::kMalformed)}));

    Command with_price = make_cancel(1);
    with_price.price = Price{100};
    EXPECT_EQ(this->h.apply(with_price),
              (Events{rejected(1, kNoSide, 100, 0, Reason::kMalformed)}));

    Command with_quantity = make_cancel(1);
    with_quantity.quantity = Quantity{1};
    EXPECT_EQ(this->h.apply(with_quantity),
              (Events{rejected(1, kNoSide, 0, 1, Reason::kMalformed)}));

    EXPECT_EQ(this->h.book().resting_orders(), 1U);
}

TYPED_TEST(BookTest, ModifyWithSideIsMalformed) {
    this->h.limit(1, kBuy, 100, 1);
    Command command = make_modify(1, 100, 1);
    command.side = kBuy;
    EXPECT_EQ(this->h.apply(command), (Events{rejected(1, kNoSide, 100, 1, Reason::kMalformed)}));
}

TYPED_TEST(BookTest, ReservedBytesMustBeZeroOnEveryCommand) {
    this->h.limit(1, kBuy, 100, 1);
    for (Command command : {make_market(2, kBuy, 1), make_cancel(1), make_modify(1, 100, 1)}) {
        command.reserved = 7;
        const Events events = this->h.apply(command);
        ASSERT_EQ(events.size(), 1U);
        EXPECT_EQ(events[0].type, EventType::kRejected);
        EXPECT_EQ(events[0].reason, Reason::kMalformed);
    }
}

TYPED_TEST(BookTest, PriceOutsideBandIsRejected) {
    EXPECT_EQ(this->h.limit(1, kBuy, 0, 1),
              (Events{rejected(1, kBuy, 0, 1, Reason::kInvalidPrice)}));
    EXPECT_EQ(this->h.limit(2, kBuy, -5, 1),
              (Events{rejected(2, kBuy, -5, 1, Reason::kInvalidPrice)}));
    EXPECT_EQ(this->h.limit(3, kBuy, 1001, 1),
              (Events{rejected(3, kBuy, 1001, 1, Reason::kInvalidPrice)}));
}

TYPED_TEST(BookTest, PricesAtBandEdgesAreValid) {
    EXPECT_EQ(this->h.limit(1, kBuy, 1, 1),
              (Events{accepted(1, kBuy, 1, 1), rested(1, kBuy, 1, 1)}));
    EXPECT_EQ(this->h.limit(2, kSell, 1000, 1),
              (Events{accepted(2, kSell, 1000, 1), rested(2, kSell, 1000, 1)}));
}

TYPED_TEST(BookTest, ZeroQuantityIsRejected) {
    EXPECT_EQ(this->h.limit(1, kBuy, 100, 0),
              (Events{rejected(1, kBuy, 100, 0, Reason::kInvalidQuantity)}));
    EXPECT_EQ(this->h.market(2, kBuy, 0),
              (Events{rejected(2, kBuy, 0, 0, Reason::kInvalidQuantity)}));
}

TYPED_TEST(BookTest, ValidationRunsInSpecifiedOrder) {
    this->h.limit(5, kBuy, 100, 1);

    // Malformed beats a duplicate ID, a bad price and a bad quantity.
    const Command malformed = make_limit(5, kNoSide, 5000, 0);
    EXPECT_EQ(this->h.apply(malformed),
              (Events{rejected(5, kNoSide, 5000, 0, Reason::kMalformed)}));

    // A duplicate ID beats a bad price and a bad quantity.
    EXPECT_EQ(this->h.limit(5, kBuy, 5000, 0),
              (Events{rejected(5, kBuy, 5000, 0, Reason::kDuplicateId)}));

    // A bad price beats a bad quantity.
    EXPECT_EQ(this->h.limit(6, kBuy, 5000, 0),
              (Events{rejected(6, kBuy, 5000, 0, Reason::kInvalidPrice)}));

    // For a modify, an unknown ID beats a bad price and a bad quantity.
    EXPECT_EQ(this->h.modify(99, 5000, 0),
              (Events{rejected(99, kNoSide, 5000, 0, Reason::kUnknownId)}));

    // For a modify, a bad price beats a bad quantity.
    EXPECT_EQ(this->h.modify(5, 5000, 0),
              (Events{rejected(5, kNoSide, 5000, 0, Reason::kInvalidPrice)}));
}

TYPED_TEST(BookTest, CancelOrModifyOfUnknownIdIsRejected) {
    EXPECT_EQ(this->h.cancel(1), (Events{rejected(1, kNoSide, 0, 0, Reason::kUnknownId)}));
    EXPECT_EQ(this->h.modify(1, 100, 1),
              (Events{rejected(1, kNoSide, 100, 1, Reason::kUnknownId)}));
}

TYPED_TEST(BookTest, CancelOrModifyOfDepartedOrderIsRejected) {
    this->h.limit(1, kBuy, 100, 1);
    this->h.cancel(1);
    EXPECT_EQ(this->h.cancel(1), (Events{rejected(1, kNoSide, 0, 0, Reason::kUnknownId)}));

    this->h.limit(2, kBuy, 100, 1);
    this->h.limit(3, kSell, 100, 1);
    EXPECT_EQ(this->h.cancel(2), (Events{rejected(2, kNoSide, 0, 0, Reason::kUnknownId)}));
    EXPECT_EQ(this->h.modify(2, 100, 1),
              (Events{rejected(2, kNoSide, 100, 1, Reason::kUnknownId)}));
}

TYPED_TEST(BookTest, RejectedModifyLeavesOrderUntouched) {
    this->h.limit(1, kBuy, 100, 5);
    this->h.limit(2, kBuy, 100, 5);
    this->h.modify(1, 5000, 9);
    this->h.modify(1, 100, 0);
    EXPECT_EQ(this->h.book().open_quantity_at(kBuy, Price{100}), 10U);
    EXPECT_EQ(this->h.limit(3, kSell, 100, 1),
              (Events{accepted(3, kSell, 100, 1), trade(3, kSell, 1, 100, 1, 0, 4)}));
}

// Section 4.4: modify priority table.

TYPED_TEST(BookTest, ModifyToSamePriceAndQuantityKeepsPriority) {
    this->h.limit(1, kSell, 100, 5);
    this->h.limit(2, kSell, 100, 5);
    EXPECT_EQ(this->h.modify(1, 100, 5), (Events{modified(1, kSell, 100, 5)}));
    EXPECT_EQ(this->h.limit(3, kBuy, 100, 1),
              (Events{accepted(3, kBuy, 100, 1), trade(3, kBuy, 1, 100, 1, 0, 4)}));
}

TYPED_TEST(BookTest, ModifyDownKeepsPriority) {
    this->h.limit(1, kSell, 100, 5);
    this->h.limit(2, kSell, 100, 5);
    EXPECT_EQ(this->h.modify(1, 100, 3), (Events{modified(1, kSell, 100, 3)}));
    EXPECT_EQ(this->h.limit(3, kBuy, 100, 1),
              (Events{accepted(3, kBuy, 100, 1), trade(3, kBuy, 1, 100, 1, 0, 2)}));
    EXPECT_EQ(this->h.book().open_quantity_at(kSell, Price{100}), 7U);
}

TYPED_TEST(BookTest, ModifyUpLosesPriority) {
    this->h.limit(1, kSell, 100, 5);
    this->h.limit(2, kSell, 100, 5);
    EXPECT_EQ(this->h.modify(1, 100, 6),
              (Events{modified(1, kSell, 100, 6), rested(1, kSell, 100, 6)}));
    EXPECT_EQ(this->h.limit(3, kBuy, 100, 1),
              (Events{accepted(3, kBuy, 100, 1), trade(3, kBuy, 2, 100, 1, 0, 4)}));
}

TYPED_TEST(BookTest, ModifyToNewPriceLosesPriority) {
    this->h.limit(1, kSell, 100, 5);
    this->h.limit(2, kSell, 101, 5);
    EXPECT_EQ(this->h.modify(1, 101, 5),
              (Events{modified(1, kSell, 101, 5), rested(1, kSell, 101, 5)}));
    EXPECT_EQ(this->h.book().best_ask(), Price{101});
    EXPECT_EQ(this->h.limit(3, kBuy, 101, 1),
              (Events{accepted(3, kBuy, 101, 1), trade(3, kBuy, 2, 101, 1, 0, 4)}));
}

TYPED_TEST(BookTest, ModifyAwayAndBackLosesPriority) {
    this->h.limit(1, kSell, 100, 5);
    this->h.limit(2, kSell, 100, 5);
    this->h.modify(1, 101, 5);
    this->h.modify(1, 100, 5);
    EXPECT_EQ(this->h.limit(3, kBuy, 100, 1),
              (Events{accepted(3, kBuy, 100, 1), trade(3, kBuy, 2, 100, 1, 0, 4)}));
}

TYPED_TEST(BookTest, ModifyQuantityComparesAgainstOpenQuantity) {
    // After a partial fill, 3 of 5 lots remain, so a modify to 4 is an increase that loses
    // priority.
    this->h.limit(1, kSell, 100, 5);
    this->h.limit(2, kSell, 100, 5);
    this->h.limit(3, kBuy, 100, 2);
    EXPECT_EQ(this->h.modify(1, 100, 4),
              (Events{modified(1, kSell, 100, 4), rested(1, kSell, 100, 4)}));
    EXPECT_EQ(this->h.limit(4, kBuy, 100, 1),
              (Events{accepted(4, kBuy, 100, 1), trade(4, kBuy, 2, 100, 1, 0, 4)}));
}

TYPED_TEST(BookTest, CrossingModifyMatchesThenRests) {
    this->h.limit(1, kSell, 100, 2);
    this->h.limit(2, kBuy, 90, 5);
    EXPECT_EQ(this->h.modify(2, 100, 5),
              (Events{modified(2, kBuy, 100, 5), trade(2, kBuy, 1, 100, 2, 3, 0),
                      rested(2, kBuy, 100, 3)}));
    EXPECT_EQ(this->h.book().best_bid(), Price{100});
    EXPECT_EQ(this->h.book().best_ask(), std::nullopt);
}

TYPED_TEST(BookTest, CrossingModifyCanFillCompletely) {
    this->h.limit(1, kSell, 100, 5);
    this->h.limit(2, kBuy, 90, 2);
    EXPECT_EQ(this->h.modify(2, 100, 2),
              (Events{modified(2, kBuy, 100, 2), trade(2, kBuy, 1, 100, 2, 0, 3)}));
    EXPECT_EQ(this->h.book().resting_orders(), 1U);
    EXPECT_EQ(this->h.cancel(2), (Events{rejected(2, kNoSide, 0, 0, Reason::kUnknownId)}));
}

TYPED_TEST(BookTest, ModifyEmptyingLevelExposesNextLevel) {
    this->h.limit(1, kBuy, 100, 1);
    this->h.limit(2, kBuy, 90, 1);
    this->h.modify(1, 80, 1);
    EXPECT_EQ(this->h.book().best_bid(), Price{90});
}

// Section 6: capacity.

TYPED_TEST(BookTest, FullBookCancelsRemainderOfNewOrder) {
    BookHarness<TypeParam> small{
        BookConfig{.min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = 2}};
    small.limit(1, kBuy, 90, 1);
    small.limit(2, kBuy, 91, 1);
    EXPECT_EQ(small.limit(3, kBuy, 92, 7),
              (Events{accepted(3, kBuy, 92, 7), canceled(3, kBuy, 7, Reason::kBookFull)}));
    EXPECT_EQ(small.book().resting_orders(), 2U);
    EXPECT_EQ(small.book().best_bid(), Price{91});
}

TYPED_TEST(BookTest, FullBookStillAllowsFullFills) {
    BookHarness<TypeParam> small{
        BookConfig{.min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = 1}};
    small.limit(1, kSell, 100, 5);
    EXPECT_EQ(small.limit(2, kBuy, 100, 2),
              (Events{accepted(2, kBuy, 100, 2), trade(2, kBuy, 1, 100, 2, 0, 3)}));
}

TYPED_TEST(BookTest, FullBookKeepsFillsAndCancelsOnlyTheRemainder) {
    BookHarness<TypeParam> small{
        BookConfig{.min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = 2}};
    small.limit(1, kSell, 100, 5);
    small.limit(2, kSell, 100, 5);
    EXPECT_EQ(small.limit(3, kBuy, 100, 7),
              (Events{accepted(3, kBuy, 100, 7), trade(3, kBuy, 1, 100, 5, 2, 0),
                      trade(3, kBuy, 2, 100, 2, 0, 3)}));

    small.limit(4, kSell, 101, 1);
    EXPECT_EQ(small.book().resting_orders(), 2U);
    EXPECT_EQ(small.limit(5, kBuy, 90, 1),
              (Events{accepted(5, kBuy, 90, 1), canceled(5, kBuy, 1, Reason::kBookFull)}));
}

TYPED_TEST(BookTest, FillThatFreesSlotLetsRemainderRest) {
    // The slot freed by filling order 1 is available to the aggressor's remainder.
    BookHarness<TypeParam> small{
        BookConfig{.min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = 1}};
    small.limit(1, kSell, 100, 1);
    EXPECT_EQ(small.limit(2, kBuy, 100, 3),
              (Events{accepted(2, kBuy, 100, 3), trade(2, kBuy, 1, 100, 1, 2, 0),
                      rested(2, kBuy, 100, 2)}));
}

TYPED_TEST(BookTest, CancelFreesSlot) {
    BookHarness<TypeParam> small{
        BookConfig{.min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = 1}};
    small.limit(1, kBuy, 90, 1);
    small.cancel(1);
    EXPECT_EQ(small.limit(2, kBuy, 91, 1),
              (Events{accepted(2, kBuy, 91, 1), rested(2, kBuy, 91, 1)}));
}

TYPED_TEST(BookTest, ModifyNeverHitsCapacity) {
    BookHarness<TypeParam> small{
        BookConfig{.min_price = Price{1}, .max_price = Price{1000}, .max_resting_orders = 1}};
    small.limit(1, kBuy, 90, 1);
    EXPECT_EQ(small.modify(1, 95, 9), (Events{modified(1, kBuy, 95, 9), rested(1, kBuy, 95, 9)}));
    EXPECT_EQ(small.book().resting_orders(), 1U);
}

TYPED_TEST(BookTest, BadConfigurationIsRefused) {
    EXPECT_THROW(TypeParam(BookConfig{.min_price = Price{0}, .max_price = Price{10}}),
                 std::invalid_argument);
    EXPECT_THROW(TypeParam(BookConfig{.min_price = Price{10}, .max_price = Price{9}}),
                 std::invalid_argument);
    EXPECT_THROW(TypeParam(BookConfig{
                     .min_price = Price{1}, .max_price = Price{10}, .max_resting_orders = 0}),
                 std::invalid_argument);
}

// Open quantity by level, used by callers inspecting depth.

TYPED_TEST(BookTest, OpenQuantityAtSumsTheLevel) {
    this->h.limit(1, kBuy, 100, 3);
    this->h.limit(2, kBuy, 100, 4);
    EXPECT_EQ(this->h.book().open_quantity_at(kBuy, Price{100}), 7U);
    EXPECT_EQ(this->h.book().open_quantity_at(kBuy, Price{99}), 0U);
    EXPECT_EQ(this->h.book().open_quantity_at(kSell, Price{100}), 0U);
}

}  // namespace
}  // namespace nanomatch::test

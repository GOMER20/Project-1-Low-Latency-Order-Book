// Edge cases, grouped by the behaviour they protect: partial fills, queue
// jumping, full cancellations and crossing the spread.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "support.hpp"

namespace {

using lob::kInvalidOrderId;
using lob::OrderId;
using lob::Price;
using lob::Quantity;
using lob::Side;
using lob::SubmitResult;
using lob_test::kNoPrice;

class PartialFills : public lob_test::BookFixture {};
class QueueJumping : public lob_test::BookFixture {};
class FullCancellations : public lob_test::BookFixture {};
class CrossingTheSpread : public lob_test::BookFixture {};

// --- Partial fills -----------------------------------------------------------

TEST_F(PartialFills, OrderLargerThanTheWholeSideSweepsItAndRestsTheRemainder) {
  (void)submit(Side::Sell, 150, 10);
  (void)submit(Side::Sell, 151, 20);
  (void)submit(Side::Sell, 152, 30);

  const SubmitResult taker = submit(Side::Buy, 199, 100);

  EXPECT_EQ(trades.size(), 3u);
  EXPECT_EQ(taker.filled, 60u);
  EXPECT_EQ(taker.resting, 40u);
  EXPECT_EQ(best_ask(), kNoPrice);
  EXPECT_EQ(best_bid(), 199);
  EXPECT_EQ(book.quantity_at(Side::Buy, 199), 40u);
  EXPECT_EQ(book.size(), 1u);
}

TEST_F(PartialFills, ManySmallOrdersDrainOneLargeRestingOrder) {
  const SubmitResult maker = submit(Side::Sell, 150, 100);

  for (int i = 0; i < 9; ++i) {
    (void)submit(Side::Buy, 150, 10);
  }
  ASSERT_TRUE(book.find(maker.id) != nullptr);
  EXPECT_EQ(book.find(maker.id)->quantity, 10u);

  (void)submit(Side::Buy, 150, 10);

  ASSERT_EQ(trades.size(), 10u);
  for (const lob::Trade& trade : trades) {
    EXPECT_EQ(trade.maker_id, maker.id);
    EXPECT_EQ(trade.quantity, 10u);
  }
  EXPECT_TRUE(book.empty());
}

TEST_F(PartialFills, PartlyFilledOrderCanBeCancelledForItsRemainder) {
  const SubmitResult maker = submit(Side::Sell, 150, 100);
  (void)submit(Side::Buy, 150, 30);

  EXPECT_TRUE(book.cancel(maker.id));

  EXPECT_EQ(book.quantity_at(Side::Sell, 150), 0u);
  EXPECT_TRUE(book.empty());

  const SubmitResult later = submit(Side::Buy, 150, 10);
  EXPECT_EQ(later.filled, 0u);
  EXPECT_EQ(trades.size(), 1u);
}

TEST_F(PartialFills, PartialFillAtTheBestLevelLeavesDeeperLevelsUntouched) {
  (void)submit(Side::Sell, 150, 50);
  (void)submit(Side::Sell, 151, 50);

  (void)submit(Side::Buy, 151, 20);

  EXPECT_EQ(book.quantity_at(Side::Sell, 150), 30u);
  EXPECT_EQ(book.quantity_at(Side::Sell, 151), 50u);
  EXPECT_EQ(best_ask(), 150);
}

TEST_F(PartialFills, FilledPlusRestingAlwaysEqualsTheSubmittedQuantity) {
  (void)submit(Side::Sell, 150, 7);
  (void)submit(Side::Sell, 151, 5);

  const SubmitResult taker = submit(Side::Buy, 151, 20);

  std::uint64_t traded = 0;
  for (const lob::Trade& trade : trades) {
    traded += trade.quantity;
  }
  EXPECT_EQ(traded, 12u);
  EXPECT_EQ(taker.filled, 12u);
  EXPECT_EQ(taker.filled + taker.resting, 20u);
}

// Two maximum-size orders at one price exceed 32 bits in total; the level's
// running total must not wrap.
TEST_F(PartialFills, LevelTotalDoesNotOverflowWithMaximumSizeOrders) {
  constexpr Quantity kMax = std::numeric_limits<Quantity>::max();
  const SubmitResult first = submit(Side::Sell, 150, kMax);
  const SubmitResult second = submit(Side::Sell, 150, kMax);
  EXPECT_EQ(book.quantity_at(Side::Sell, 150), 2 * std::uint64_t{kMax});

  const SubmitResult taker = submit(Side::Buy, 150, kMax);

  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].maker_id, first.id);
  EXPECT_EQ(trades[0].quantity, kMax);
  EXPECT_EQ(taker.resting, 0u);
  EXPECT_EQ(book.quantity_at(Side::Sell, 150), std::uint64_t{kMax});
  EXPECT_EQ(queue_at(Side::Sell, 150), (std::vector<OrderId>{second.id}));
}

TEST_F(PartialFills, OneLotOrdersFillOneAtATime) {
  const SubmitResult first = submit(Side::Buy, 150, 1);
  const SubmitResult second = submit(Side::Buy, 150, 1);

  (void)submit(Side::Sell, 150, 1);

  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].maker_id, first.id);
  EXPECT_EQ(queue_at(Side::Buy, 150), (std::vector<OrderId>{second.id}));
}

// --- Queue jumping -----------------------------------------------------------

TEST_F(QueueJumping, LaterOrderAtTheSamePriceIsNotTouchedWhileAnEarlierOneRemains) {
  const SubmitResult earlier = submit(Side::Sell, 150, 10);
  const SubmitResult later = submit(Side::Sell, 150, 10);

  (void)submit(Side::Buy, 150, 10);

  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].maker_id, earlier.id);
  ASSERT_TRUE(book.find(later.id) != nullptr);
  EXPECT_EQ(book.find(later.id)->quantity, 10u);
}

TEST_F(QueueJumping, CancelAndResubmitGoesToTheBackOfTheQueue) {
  const SubmitResult first = submit(Side::Sell, 150, 10);
  const SubmitResult second = submit(Side::Sell, 150, 10);

  EXPECT_TRUE(book.cancel(first.id));
  const SubmitResult resubmitted = submit(Side::Sell, 150, 10);

  EXPECT_EQ(queue_at(Side::Sell, 150), (std::vector<OrderId>{second.id, resubmitted.id}));

  (void)submit(Side::Buy, 150, 10);
  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].maker_id, second.id);
}

// The new order physically reuses the cancelled order's memory. It must still
// queue last.
TEST_F(QueueJumping, RecycledSlotDoesNotInheritTheOldQueuePosition) {
  const SubmitResult first = submit(Side::Sell, 150, 10);
  const SubmitResult second = submit(Side::Sell, 150, 10);
  const SubmitResult third = submit(Side::Sell, 150, 10);

  EXPECT_TRUE(book.cancel(first.id));
  const SubmitResult recycled = submit(Side::Sell, 150, 10);

  ASSERT_EQ(static_cast<lob::OrderIndex>(recycled.id), static_cast<lob::OrderIndex>(first.id));
  EXPECT_EQ(queue_at(Side::Sell, 150),
            (std::vector<OrderId>{second.id, third.id, recycled.id}));
}

TEST_F(QueueJumping, PartialFillDoesNotCostAnOrderItsPlace) {
  const SubmitResult first = submit(Side::Sell, 150, 100);
  const SubmitResult second = submit(Side::Sell, 150, 50);

  (void)submit(Side::Buy, 150, 30);
  const SubmitResult third = submit(Side::Sell, 150, 20);

  EXPECT_EQ(queue_at(Side::Sell, 150),
            (std::vector<OrderId>{first.id, second.id, third.id}));
}

TEST_F(QueueJumping, OrderSizeGivesNoPriority) {
  const SubmitResult small_first = submit(Side::Sell, 150, 1);
  (void)submit(Side::Sell, 150, 1'000);
  const SubmitResult large_first = submit(Side::Buy, 140, 1'000);
  (void)submit(Side::Buy, 140, 1);

  (void)submit(Side::Buy, 150, 1);
  (void)submit(Side::Sell, 140, 1);

  ASSERT_EQ(trades.size(), 2u);
  EXPECT_EQ(trades[0].maker_id, small_first.id);
  EXPECT_EQ(trades[1].maker_id, large_first.id);
}

// The one legitimate way to get ahead: offer a better price.
TEST_F(QueueJumping, BetterPriceTradesFirstEvenThoughItArrivedLater) {
  (void)submit(Side::Sell, 151, 10);
  const SubmitResult better = submit(Side::Sell, 150, 10);

  (void)submit(Side::Buy, 151, 10);

  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].maker_id, better.id);
  EXPECT_EQ(trades[0].price, 150);
}

// --- Full cancellations ------------------------------------------------------

TEST_F(FullCancellations, CancellingEveryOrderLeavesAnEmptyBook) {
  std::vector<OrderId> ids;
  for (Price offset = 0; offset < 10; ++offset) {
    ids.push_back(submit(Side::Buy, 140 - offset, 10).id);
    ids.push_back(submit(Side::Buy, 140 - offset, 20).id);
    ids.push_back(submit(Side::Sell, 150 + offset, 10).id);
    ids.push_back(submit(Side::Sell, 150 + offset, 20).id);
  }
  ASSERT_EQ(book.size(), 40u);

  // Cancel from both ends towards the middle, so neither side empties in order.
  for (std::size_t i = 0; i < ids.size() / 2; ++i) {
    EXPECT_TRUE(book.cancel(ids[i]));
    EXPECT_TRUE(book.cancel(ids[ids.size() - 1 - i]));
  }

  EXPECT_TRUE(book.empty());
  EXPECT_EQ(best_bid(), kNoPrice);
  EXPECT_EQ(best_ask(), kNoPrice);
  for (Price price = 100; price < 200; ++price) {
    EXPECT_EQ(book.quantity_at(Side::Buy, price), 0u);
    EXPECT_EQ(book.quantity_at(Side::Sell, price), 0u);
  }
  EXPECT_TRUE(trades.empty());
}

TEST_F(FullCancellations, EmptiedBookAcceptsAFullLoadOfNewOrders) {
  std::vector<OrderId> ids;
  for (int i = 0; i < 64; ++i) {
    ids.push_back(submit(Side::Buy, 140, 1).id);
    ASSERT_NE(ids.back(), kInvalidOrderId);
  }
  EXPECT_EQ(submit(Side::Buy, 140, 1).id, kInvalidOrderId);

  for (const OrderId id : ids) {
    EXPECT_TRUE(book.cancel(id));
  }

  for (int i = 0; i < 64; ++i) {
    EXPECT_NE(submit(Side::Sell, 160, 1).id, kInvalidOrderId);
  }
  EXPECT_EQ(submit(Side::Sell, 160, 1).id, kInvalidOrderId);
  EXPECT_EQ(book.quantity_at(Side::Sell, 160), 64u);
}

TEST_F(FullCancellations, CancellingAWholeLevelMakesIncomingOrdersSkipIt) {
  const SubmitResult first = submit(Side::Sell, 150, 10);
  const SubmitResult second = submit(Side::Sell, 150, 10);
  const SubmitResult deeper = submit(Side::Sell, 151, 10);

  EXPECT_TRUE(book.cancel(first.id));
  EXPECT_TRUE(book.cancel(second.id));
  EXPECT_EQ(best_ask(), 151);

  const SubmitResult too_low = submit(Side::Buy, 150, 10);
  EXPECT_EQ(too_low.filled, 0u);
  EXPECT_TRUE(trades.empty());
  EXPECT_TRUE(book.cancel(too_low.id));

  (void)submit(Side::Buy, 151, 10);
  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].maker_id, deeper.id);
  EXPECT_EQ(trades[0].price, 151);
}

TEST_F(FullCancellations, CancellingHeadMiddleAndTailLeavesTheRestInOrder) {
  std::vector<OrderId> ids;
  for (int i = 0; i < 5; ++i) {
    ids.push_back(submit(Side::Buy, 150, 10).id);
  }

  EXPECT_TRUE(book.cancel(ids[0]));
  EXPECT_TRUE(book.cancel(ids[2]));
  EXPECT_TRUE(book.cancel(ids[4]));

  EXPECT_EQ(queue_at(Side::Buy, 150), (std::vector<OrderId>{ids[1], ids[3]}));
  EXPECT_EQ(book.quantity_at(Side::Buy, 150), 20u);

  (void)submit(Side::Sell, 150, 20);
  ASSERT_EQ(trades.size(), 2u);
  EXPECT_EQ(trades[0].maker_id, ids[1]);
  EXPECT_EQ(trades[1].maker_id, ids[3]);
}

TEST_F(FullCancellations, OrderCancelledBeforeAnIncomingOrderArrivesNeverTrades) {
  const SubmitResult ask = submit(Side::Sell, 150, 10);
  EXPECT_TRUE(book.cancel(ask.id));

  const SubmitResult buy = submit(Side::Buy, 150, 10);

  EXPECT_TRUE(trades.empty());
  EXPECT_EQ(buy.resting, 10u);
  EXPECT_EQ(best_bid(), 150);
}

// --- Crossing the spread -----------------------------------------------------

TEST_F(CrossingTheSpread, BuyOneTickBelowTheBestAskRests) {
  (void)submit(Side::Sell, 150, 10);

  const SubmitResult buy = submit(Side::Buy, 149, 10);

  EXPECT_TRUE(trades.empty());
  EXPECT_EQ(buy.resting, 10u);
  EXPECT_EQ(best_bid(), 149);
  EXPECT_EQ(best_ask(), 150);
}

TEST_F(CrossingTheSpread, BuyExactlyAtTheBestAskTrades) {
  (void)submit(Side::Sell, 150, 10);

  const SubmitResult buy = submit(Side::Buy, 150, 10);

  EXPECT_EQ(trades.size(), 1u);
  EXPECT_EQ(buy.filled, 10u);
}

TEST_F(CrossingTheSpread, SellOneTickAboveTheBestBidRests) {
  (void)submit(Side::Buy, 150, 10);

  const SubmitResult sell = submit(Side::Sell, 151, 10);

  EXPECT_TRUE(trades.empty());
  EXPECT_EQ(sell.resting, 10u);
  EXPECT_EQ(best_bid(), 150);
  EXPECT_EQ(best_ask(), 151);
}

TEST_F(CrossingTheSpread, SellExactlyAtTheBestBidTrades) {
  (void)submit(Side::Buy, 150, 10);

  const SubmitResult sell = submit(Side::Sell, 150, 10);

  EXPECT_EQ(trades.size(), 1u);
  EXPECT_EQ(sell.filled, 10u);
}

TEST_F(CrossingTheSpread, SweepEndsWithAPartialFillOfTheLastLevel) {
  (void)submit(Side::Sell, 150, 10);
  (void)submit(Side::Sell, 151, 10);
  const SubmitResult last = submit(Side::Sell, 152, 10);

  const SubmitResult taker = submit(Side::Buy, 152, 25);

  ASSERT_EQ(trades.size(), 3u);
  EXPECT_EQ(trades[2].maker_id, last.id);
  EXPECT_EQ(trades[2].quantity, 5u);
  EXPECT_EQ(taker.resting, 0u);
  EXPECT_EQ(book.quantity_at(Side::Sell, 152), 5u);
  EXPECT_EQ(best_ask(), 152);
  EXPECT_EQ(best_bid(), kNoPrice);
}

TEST_F(CrossingTheSpread, WorksAtBothEdgesOfThePriceRange) {
  const SubmitResult lowest_bid = submit(Side::Buy, 100, 10);
  const SubmitResult highest_ask = submit(Side::Sell, 199, 10);

  (void)submit(Side::Sell, 100, 10);
  (void)submit(Side::Buy, 199, 10);

  ASSERT_EQ(trades.size(), 2u);
  EXPECT_EQ(trades[0].maker_id, lowest_bid.id);
  EXPECT_EQ(trades[0].price, 100);
  EXPECT_EQ(trades[1].maker_id, highest_ask.id);
  EXPECT_EQ(trades[1].price, 199);
  EXPECT_TRUE(book.empty());
}

TEST_F(CrossingTheSpread, AggressivelyPricedOrderIntoAnEmptySideSimplyRests) {
  const SubmitResult buy = submit(Side::Buy, 199, 10);

  EXPECT_TRUE(trades.empty());
  EXPECT_EQ(buy.resting, 10u);
  EXPECT_EQ(best_bid(), 199);
}

// A remainder that rests becomes liquidity for the next incoming order.
TEST_F(CrossingTheSpread, RestingRemainderIsHitByTheNextOrderFromTheOtherSide) {
  (void)submit(Side::Sell, 150, 10);
  const SubmitResult buy = submit(Side::Buy, 152, 30);  // takes 10, rests 20 at 152

  const SubmitResult sell = submit(Side::Sell, 151, 50);  // takes 20 at 152, rests 30 at 151

  ASSERT_EQ(trades.size(), 2u);
  EXPECT_EQ(trades[1].maker_id, buy.id);
  EXPECT_EQ(trades[1].taker_id, sell.id);
  EXPECT_EQ(trades[1].price, 152);
  EXPECT_EQ(trades[1].quantity, 20u);
  EXPECT_EQ(sell.resting, 30u);
  EXPECT_EQ(best_bid(), kNoPrice);
  EXPECT_EQ(best_ask(), 151);
}

}  // namespace

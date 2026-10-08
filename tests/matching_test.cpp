#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "support.hpp"

namespace {

using lob::kInvalidOrderId;
using lob::OrderBook;
using lob::Side;
using lob::SubmitResult;
using lob::Trade;
using lob_test::kNoPrice;

class Matching : public lob_test::BookFixture {};

TEST_F(Matching, OrdersThatDoNotCrossRestWithoutTrading) {
  const SubmitResult bid = submit(Side::Buy, 140, 10);
  const SubmitResult ask = submit(Side::Sell, 150, 20);

  EXPECT_TRUE(trades.empty());
  EXPECT_NE(bid.id, kInvalidOrderId);
  EXPECT_EQ(bid.filled, 0u);
  EXPECT_EQ(bid.resting, 10u);
  EXPECT_EQ(ask.resting, 20u);
  EXPECT_EQ(best_bid(), 140);
  EXPECT_EQ(best_ask(), 150);
}

TEST_F(Matching, CrossingOrderProducesOneFullyDescribedTrade) {
  const SubmitResult maker = submit(Side::Sell, 150, 10);
  const SubmitResult taker = submit(Side::Buy, 150, 10);

  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].maker_id, maker.id);
  EXPECT_EQ(trades[0].taker_id, taker.id);
  EXPECT_EQ(trades[0].price, 150);
  EXPECT_EQ(trades[0].quantity, 10u);
  EXPECT_EQ(trades[0].taker_side, Side::Buy);
  EXPECT_EQ(std::string_view(trades[0].symbol, 4), std::string_view("TEST"));

  EXPECT_EQ(taker.filled, 10u);
  EXPECT_EQ(taker.resting, 0u);
  EXPECT_TRUE(book.empty());
}

TEST_F(Matching, BuyTradesAtTheRestingAskPriceNotItsOwnLimit) {
  (void)submit(Side::Sell, 150, 10);
  (void)submit(Side::Buy, 160, 10);

  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].price, 150);
}

TEST_F(Matching, SellTradesAtTheRestingBidPriceNotItsOwnLimit) {
  (void)submit(Side::Buy, 150, 10);
  (void)submit(Side::Sell, 140, 10);

  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].price, 150);
  EXPECT_EQ(trades[0].taker_side, Side::Sell);
}

// Price priority: asks were entered worst price first, but must trade best first.
TEST_F(Matching, BuySweepsAsksFromTheLowestPriceUp) {
  const SubmitResult at_152 = submit(Side::Sell, 152, 10);
  const SubmitResult at_150 = submit(Side::Sell, 150, 10);
  const SubmitResult at_151 = submit(Side::Sell, 151, 10);

  const SubmitResult taker = submit(Side::Buy, 152, 30);

  ASSERT_EQ(trades.size(), 3u);
  EXPECT_EQ(trades[0].maker_id, at_150.id);
  EXPECT_EQ(trades[0].price, 150);
  EXPECT_EQ(trades[1].maker_id, at_151.id);
  EXPECT_EQ(trades[1].price, 151);
  EXPECT_EQ(trades[2].maker_id, at_152.id);
  EXPECT_EQ(trades[2].price, 152);
  EXPECT_EQ(taker.filled, 30u);
  EXPECT_TRUE(book.empty());
}

TEST_F(Matching, SellSweepsBidsFromTheHighestPriceDown) {
  const SubmitResult at_148 = submit(Side::Buy, 148, 10);
  const SubmitResult at_150 = submit(Side::Buy, 150, 10);
  const SubmitResult at_149 = submit(Side::Buy, 149, 10);

  (void)submit(Side::Sell, 148, 30);

  ASSERT_EQ(trades.size(), 3u);
  EXPECT_EQ(trades[0].maker_id, at_150.id);
  EXPECT_EQ(trades[1].maker_id, at_149.id);
  EXPECT_EQ(trades[2].maker_id, at_148.id);
  EXPECT_TRUE(book.empty());
}

// Time priority: at one price, the order that arrived first trades first.
TEST_F(Matching, OldestOrderAtAPriceTradesFirst) {
  const SubmitResult first = submit(Side::Sell, 150, 10);
  const SubmitResult second = submit(Side::Sell, 150, 10);
  const SubmitResult third = submit(Side::Sell, 150, 10);

  (void)submit(Side::Buy, 150, 25);

  ASSERT_EQ(trades.size(), 3u);
  EXPECT_EQ(trades[0].maker_id, first.id);
  EXPECT_EQ(trades[0].quantity, 10u);
  EXPECT_EQ(trades[1].maker_id, second.id);
  EXPECT_EQ(trades[1].quantity, 10u);
  EXPECT_EQ(trades[2].maker_id, third.id);
  EXPECT_EQ(trades[2].quantity, 5u);

  ASSERT_TRUE(book.find(third.id) != nullptr);
  EXPECT_EQ(book.find(third.id)->quantity, 5u);
  EXPECT_EQ(book.quantity_at(Side::Sell, 150), 5u);
}

TEST_F(Matching, PartlyFilledRestingOrderKeepsItsPlaceInTheQueue) {
  const SubmitResult first = submit(Side::Sell, 150, 100);
  const SubmitResult second = submit(Side::Sell, 150, 50);

  (void)submit(Side::Buy, 150, 30);
  (void)submit(Side::Buy, 150, 80);

  ASSERT_EQ(trades.size(), 3u);
  EXPECT_EQ(trades[0].maker_id, first.id);
  EXPECT_EQ(trades[0].quantity, 30u);
  EXPECT_EQ(trades[1].maker_id, first.id);
  EXPECT_EQ(trades[1].quantity, 70u);
  EXPECT_EQ(trades[2].maker_id, second.id);
  EXPECT_EQ(trades[2].quantity, 10u);
  EXPECT_EQ(book.quantity_at(Side::Sell, 150), 40u);
}

TEST_F(Matching, UnfilledRemainderRestsAtItsLimitPrice) {
  (void)submit(Side::Sell, 150, 10);

  const SubmitResult taker = submit(Side::Buy, 155, 25);

  EXPECT_EQ(taker.filled, 10u);
  EXPECT_EQ(taker.resting, 15u);
  EXPECT_EQ(best_bid(), 155);
  EXPECT_EQ(best_ask(), kNoPrice);
  EXPECT_EQ(book.quantity_at(Side::Buy, 155), 15u);
}

// The ID reported in the trade must be the ID the remainder can be cancelled by.
TEST_F(Matching, RestingRemainderKeepsTheIdReportedInItsTrades) {
  (void)submit(Side::Sell, 150, 10);
  const SubmitResult taker = submit(Side::Buy, 155, 25);

  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].taker_id, taker.id);
  ASSERT_TRUE(book.find(taker.id) != nullptr);
  EXPECT_EQ(book.find(taker.id)->quantity, 15u);

  EXPECT_TRUE(book.cancel(taker.id));
  EXPECT_TRUE(book.empty());
}

TEST_F(Matching, NeverTradesThroughTheLimitPrice) {
  (void)submit(Side::Sell, 150, 10);
  (void)submit(Side::Sell, 151, 10);
  const SubmitResult beyond_limit = submit(Side::Sell, 152, 10);

  const SubmitResult taker = submit(Side::Buy, 151, 100);

  ASSERT_EQ(trades.size(), 2u);
  EXPECT_EQ(trades[0].price, 150);
  EXPECT_EQ(trades[1].price, 151);
  EXPECT_EQ(taker.filled, 20u);
  EXPECT_EQ(taker.resting, 80u);
  EXPECT_EQ(best_bid(), 151);
  EXPECT_EQ(best_ask(), 152);
  ASSERT_TRUE(book.find(beyond_limit.id) != nullptr);
  EXPECT_EQ(book.find(beyond_limit.id)->quantity, 10u);
}

TEST_F(Matching, FullyFilledIncomingOrderLeavesNothingBehind) {
  (void)submit(Side::Sell, 150, 10);
  const SubmitResult taker = submit(Side::Buy, 150, 10);

  EXPECT_TRUE(book.empty());
  EXPECT_TRUE(book.find(taker.id) == nullptr);
  EXPECT_FALSE(book.cancel(taker.id));
  EXPECT_EQ(best_bid(), kNoPrice);
  EXPECT_EQ(best_ask(), kNoPrice);
}

TEST_F(Matching, FilledRestingOrderCanNoLongerBeCancelled) {
  const SubmitResult maker = submit(Side::Sell, 150, 10);
  (void)submit(Side::Buy, 150, 10);

  EXPECT_FALSE(book.cancel(maker.id));
}

TEST_F(Matching, CancelledOrderDoesNotTrade) {
  const SubmitResult cancelled = submit(Side::Sell, 150, 10);
  const SubmitResult kept = submit(Side::Sell, 150, 10);
  EXPECT_TRUE(book.cancel(cancelled.id));

  (void)submit(Side::Buy, 150, 10);

  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].maker_id, kept.id);
}

TEST_F(Matching, RejectedOrdersTradeNothing) {
  (void)submit(Side::Sell, 150, 10);

  const SubmitResult out_of_range = submit(Side::Buy, 200, 10);
  const SubmitResult zero_quantity = submit(Side::Buy, 150, 0);

  EXPECT_EQ(out_of_range.id, kInvalidOrderId);
  EXPECT_EQ(out_of_range.filled, 0u);
  EXPECT_EQ(zero_quantity.id, kInvalidOrderId);
  EXPECT_TRUE(trades.empty());
  EXPECT_EQ(book.quantity_at(Side::Sell, 150), 10u);
}

// Documented limitation: a slot is reserved before matching, so a book with
// no free slot rejects every new order, even one that would have traded.
TEST(MatchingCapacity, FullBookRejectsEvenOrdersThatWouldTrade) {
  OrderBook book({.symbol = "TEST", .min_price = 100, .num_levels = 100, .max_orders = 1});
  std::vector<Trade> trades;
  const auto record = [&](const Trade& trade) { trades.push_back(trade); };

  const SubmitResult maker = book.submit(Side::Sell, 150, 10, record);
  const SubmitResult taker = book.submit(Side::Buy, 150, 10, record);

  EXPECT_NE(maker.id, kInvalidOrderId);
  EXPECT_EQ(taker.id, kInvalidOrderId);
  EXPECT_TRUE(trades.empty());
  EXPECT_EQ(book.quantity_at(Side::Sell, 150), 10u);
}

TEST_F(Matching, BookIsNeverLeftCrossed) {
  (void)submit(Side::Buy, 148, 10);
  (void)submit(Side::Sell, 152, 10);
  (void)submit(Side::Buy, 153, 4);    // takes 4 of the 152 ask
  (void)submit(Side::Sell, 147, 25);  // takes the 148 bid, rests 15 at 147
  (void)submit(Side::Buy, 150, 30);   // takes the 147 ask, rests 15 at 150

  EXPECT_EQ(best_bid(), 150);
  EXPECT_EQ(best_ask(), 152);
  EXPECT_EQ(book.quantity_at(Side::Buy, 150), 15u);
  EXPECT_EQ(book.quantity_at(Side::Sell, 152), 6u);
  EXPECT_EQ(book.size(), 2u);
}

}  // namespace

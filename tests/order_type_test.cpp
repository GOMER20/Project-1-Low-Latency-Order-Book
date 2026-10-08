// Market, IOC and FOK orders. Plain limit orders are covered by matching_test.
#include <vector>

#include <gtest/gtest.h>

#include "support.hpp"

namespace {

using lob::kInvalidOrderId;
using lob::OrderBook;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Quantity;
using lob::Side;
using lob::SubmitResult;
using lob::Trade;
using lob_test::kNoPrice;

class OrderTypes : public lob_test::BookFixture {
 protected:
  SubmitResult submit_as(OrderType type, Side side, Price price, Quantity quantity) {
    return book.submit(side, price, quantity, type,
                       [this](const Trade& trade) { trades.push_back(trade); });
  }
};

class MarketOrders : public OrderTypes {};
class IocOrders : public OrderTypes {};
class FokOrders : public OrderTypes {};

// --- Market ------------------------------------------------------------------

TEST_F(MarketOrders, BuyTakesTheBestAsksWhateverTheirPrice) {
  const SubmitResult cheap = submit(Side::Sell, 150, 10);
  const SubmitResult dear = submit(Side::Sell, 199, 10);

  const SubmitResult market = submit_as(OrderType::Market, Side::Buy, 0, 15);

  ASSERT_EQ(trades.size(), 2u);
  EXPECT_EQ(trades[0].maker_id, cheap.id);
  EXPECT_EQ(trades[0].price, 150);
  EXPECT_EQ(trades[0].quantity, 10u);
  EXPECT_EQ(trades[1].maker_id, dear.id);
  EXPECT_EQ(trades[1].price, 199);
  EXPECT_EQ(trades[1].quantity, 5u);
  EXPECT_EQ(market.filled, 15u);
  EXPECT_EQ(market.resting, 0u);
}

TEST_F(MarketOrders, SellTakesTheBestBidsWhateverTheirPrice) {
  const SubmitResult high = submit(Side::Buy, 150, 10);
  const SubmitResult low = submit(Side::Buy, 100, 10);

  const SubmitResult market = submit_as(OrderType::Market, Side::Sell, 0, 20);

  ASSERT_EQ(trades.size(), 2u);
  EXPECT_EQ(trades[0].maker_id, high.id);
  EXPECT_EQ(trades[1].maker_id, low.id);
  EXPECT_EQ(market.filled, 20u);
  EXPECT_TRUE(book.empty());
}

// The price argument is ignored, even one the book would reject for a limit order.
TEST_F(MarketOrders, PriceIsIgnored) {
  (void)submit(Side::Sell, 150, 10);

  const SubmitResult market = submit_as(OrderType::Market, Side::Buy, -12'345, 10);

  EXPECT_NE(market.id, kInvalidOrderId);
  EXPECT_EQ(market.filled, 10u);
}

TEST_F(MarketOrders, UnfilledRemainderIsDiscardedNotRested) {
  (void)submit(Side::Sell, 150, 10);

  const SubmitResult market = submit_as(OrderType::Market, Side::Buy, 0, 25);

  EXPECT_EQ(market.filled, 10u);
  EXPECT_EQ(market.resting, 0u);
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(best_bid(), kNoPrice);
}

TEST_F(MarketOrders, IntoAnEmptySideFillsNothingAndLeavesNoTrace) {
  const SubmitResult market = submit_as(OrderType::Market, Side::Buy, 0, 10);

  EXPECT_NE(market.id, kInvalidOrderId);
  EXPECT_EQ(market.filled, 0u);
  EXPECT_TRUE(trades.empty());
  EXPECT_TRUE(book.empty());
}

TEST_F(MarketOrders, ZeroQuantityIsRejected) {
  (void)submit(Side::Sell, 150, 10);
  EXPECT_EQ(submit_as(OrderType::Market, Side::Buy, 0, 0).id, kInvalidOrderId);
  EXPECT_TRUE(trades.empty());
}

// --- IOC ---------------------------------------------------------------------

TEST_F(IocOrders, FillsWhatItCanWithinItsLimitAndDiscardsTheRest) {
  (void)submit(Side::Sell, 150, 10);
  const SubmitResult beyond = submit(Side::Sell, 151, 10);

  const SubmitResult ioc = submit_as(OrderType::IOC, Side::Buy, 150, 25);

  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].price, 150);
  EXPECT_EQ(ioc.filled, 10u);
  EXPECT_EQ(ioc.resting, 0u);
  EXPECT_EQ(best_bid(), kNoPrice);  // the unfilled 15 did not rest
  EXPECT_EQ(book.quantity_at(Side::Buy, 150), 0u);
  ASSERT_TRUE(book.find(beyond.id) != nullptr);
  EXPECT_EQ(book.find(beyond.id)->quantity, 10u);
}

TEST_F(IocOrders, FillsCompletelyWhenThereIsEnough) {
  (void)submit(Side::Buy, 150, 30);

  const SubmitResult ioc = submit_as(OrderType::IOC, Side::Sell, 150, 20);

  EXPECT_EQ(ioc.filled, 20u);
  EXPECT_EQ(book.quantity_at(Side::Buy, 150), 10u);
}

TEST_F(IocOrders, ThatCannotTradeLeavesNoTrace) {
  (void)submit(Side::Sell, 150, 10);

  const SubmitResult ioc = submit_as(OrderType::IOC, Side::Buy, 149, 10);

  EXPECT_NE(ioc.id, kInvalidOrderId);
  EXPECT_EQ(ioc.filled, 0u);
  EXPECT_TRUE(trades.empty());
  EXPECT_EQ(book.size(), 1u);
  EXPECT_EQ(best_bid(), kNoPrice);
}

TEST_F(IocOrders, PriceOutsideTheBookIsRejected) {
  (void)submit(Side::Sell, 150, 10);
  EXPECT_EQ(submit_as(OrderType::IOC, Side::Buy, 200, 10).id, kInvalidOrderId);
  EXPECT_EQ(submit_as(OrderType::IOC, Side::Buy, 99, 10).id, kInvalidOrderId);
  EXPECT_TRUE(trades.empty());
}

// --- FOK ---------------------------------------------------------------------

TEST_F(FokOrders, FillsInFullAcrossSeveralOrdersAndLevels) {
  (void)submit(Side::Sell, 150, 10);
  (void)submit(Side::Sell, 150, 5);
  (void)submit(Side::Sell, 152, 20);

  const SubmitResult fok = submit_as(OrderType::FOK, Side::Buy, 152, 30);

  ASSERT_EQ(trades.size(), 3u);
  EXPECT_EQ(fok.filled, 30u);
  EXPECT_EQ(fok.resting, 0u);
  EXPECT_EQ(book.quantity_at(Side::Sell, 152), 5u);
}

TEST_F(FokOrders, DoesNothingAtAllWhenItCannotBeFilledInFull) {
  const SubmitResult first = submit(Side::Sell, 150, 10);
  const SubmitResult second = submit(Side::Sell, 151, 10);

  const SubmitResult fok = submit_as(OrderType::FOK, Side::Buy, 151, 25);

  EXPECT_NE(fok.id, kInvalidOrderId);  // killed, not rejected
  EXPECT_EQ(fok.filled, 0u);
  EXPECT_EQ(fok.resting, 0u);
  EXPECT_TRUE(trades.empty());
  EXPECT_EQ(book.size(), 2u);
  EXPECT_EQ(book.find(first.id)->quantity, 10u);
  EXPECT_EQ(book.find(second.id)->quantity, 10u);
  EXPECT_EQ(best_bid(), kNoPrice);
}

// There is enough in the book, but not at prices the order will accept.
TEST_F(FokOrders, CountsOnlyQuantityWithinItsLimit) {
  (void)submit(Side::Sell, 150, 10);
  (void)submit(Side::Sell, 153, 100);

  EXPECT_EQ(submit_as(OrderType::FOK, Side::Buy, 152, 20).filled, 0u);
  EXPECT_TRUE(trades.empty());

  EXPECT_EQ(submit_as(OrderType::FOK, Side::Buy, 153, 20).filled, 20u);
}

TEST_F(FokOrders, ExactlyEnoughFillsAndOneShortDoesNot) {
  (void)submit(Side::Sell, 150, 10);
  (void)submit(Side::Sell, 151, 10);

  EXPECT_EQ(submit_as(OrderType::FOK, Side::Buy, 151, 21).filled, 0u);
  EXPECT_TRUE(trades.empty());

  EXPECT_EQ(submit_as(OrderType::FOK, Side::Buy, 151, 20).filled, 20u);
  EXPECT_TRUE(book.empty());
}

TEST_F(FokOrders, SellSideWalksBidsDownwards) {
  (void)submit(Side::Buy, 150, 10);
  (void)submit(Side::Buy, 140, 10);
  (void)submit(Side::Buy, 110, 10);

  EXPECT_EQ(submit_as(OrderType::FOK, Side::Sell, 140, 25).filled, 0u);
  EXPECT_TRUE(trades.empty());

  const SubmitResult fok = submit_as(OrderType::FOK, Side::Sell, 110, 25);
  ASSERT_EQ(trades.size(), 3u);
  EXPECT_EQ(trades[0].price, 150);
  EXPECT_EQ(trades[1].price, 140);
  EXPECT_EQ(trades[2].price, 110);
  EXPECT_EQ(fok.filled, 25u);
  EXPECT_EQ(book.quantity_at(Side::Buy, 110), 5u);
}

TEST_F(FokOrders, AgainstAnEmptySideIsKilled) {
  const SubmitResult fok = submit_as(OrderType::FOK, Side::Buy, 150, 1);
  EXPECT_NE(fok.id, kInvalidOrderId);
  EXPECT_EQ(fok.filled, 0u);
  EXPECT_TRUE(book.empty());
}

// --- Common to the orders that never rest ------------------------------------

TEST_F(OrderTypes, ExplicitLimitTypeBehavesLikeThePlainSubmit) {
  (void)submit(Side::Sell, 150, 10);

  const SubmitResult limit = submit_as(OrderType::Limit, Side::Buy, 150, 25);

  EXPECT_EQ(limit.filled, 10u);
  EXPECT_EQ(limit.resting, 15u);
  EXPECT_EQ(best_bid(), 150);
}

TEST_F(OrderTypes, NonRestingOrdersGetDistinctIdsThatAreNeverLive) {
  (void)submit(Side::Sell, 150, 30);

  const SubmitResult market = submit_as(OrderType::Market, Side::Buy, 0, 10);
  const SubmitResult ioc = submit_as(OrderType::IOC, Side::Buy, 150, 10);
  const SubmitResult fok = submit_as(OrderType::FOK, Side::Buy, 150, 10);

  ASSERT_EQ(trades.size(), 3u);
  EXPECT_EQ(trades[0].taker_id, market.id);
  EXPECT_EQ(trades[1].taker_id, ioc.id);
  EXPECT_EQ(trades[2].taker_id, fok.id);
  EXPECT_NE(market.id, ioc.id);
  EXPECT_NE(ioc.id, fok.id);

  for (const OrderId id : {market.id, ioc.id, fok.id}) {
    EXPECT_TRUE(book.find(id) == nullptr);
    EXPECT_FALSE(book.cancel(id));
    EXPECT_EQ(book.execute(id, 1), 0u);
  }
  EXPECT_TRUE(book.empty());
}

// A limit order needs a free slot before it can even try to trade. Orders
// that never rest do not, so they still work when the book is full.
TEST(OrderTypesCapacity, NonRestingOrdersTradeEvenWhenTheBookIsFull) {
  OrderBook book({.symbol = "TEST", .min_price = 100, .num_levels = 100, .max_orders = 3});
  std::vector<Trade> trades;
  const auto record = [&](const Trade& trade) { trades.push_back(trade); };
  for (int i = 0; i < 3; ++i) {
    ASSERT_NE(book.submit(Side::Sell, 150, 10, record).id, kInvalidOrderId);
  }
  ASSERT_EQ(book.submit(Side::Buy, 150, 10, record).id, kInvalidOrderId);  // limit: no room

  EXPECT_EQ(book.submit(Side::Buy, 0, 10, OrderType::Market, record).filled, 10u);
  EXPECT_EQ(book.submit(Side::Buy, 150, 10, OrderType::IOC, record).filled, 10u);
  EXPECT_EQ(book.submit(Side::Buy, 150, 10, OrderType::FOK, record).filled, 10u);

  EXPECT_EQ(trades.size(), 3u);
  EXPECT_TRUE(book.empty());
}

}  // namespace

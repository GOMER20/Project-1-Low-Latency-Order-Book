#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "lob/order_book.hpp"

namespace {

using lob::kInvalidOrderId;
using lob::OrderBook;
using lob::OrderId;
using lob::Price;
using lob::Side;

// Tradable prices are 100..199.
constexpr lob::BookConfig kConfig{
    .symbol = "TEST", .min_price = 100, .num_levels = 100, .max_orders = 64};

constexpr Price kNoPrice = -1;

Price best_bid(const OrderBook& book) { return book.best_bid().value_or(kNoPrice); }
Price best_ask(const OrderBook& book) { return book.best_ask().value_or(kNoPrice); }

std::vector<OrderId> queue_at(const OrderBook& book, Side side, Price price) {
  std::vector<OrderId> ids;
  book.for_each_order(side, price, [&](const lob::Order& order) { ids.push_back(order.id); });
  return ids;
}

// --- Add -------------------------------------------------------------------

TEST(OrderBook, StartsEmpty) {
  OrderBook book(kConfig);
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.size(), 0u);
  EXPECT_EQ(best_bid(book), kNoPrice);
  EXPECT_EQ(best_ask(book), kNoPrice);
  EXPECT_EQ(book.symbol(), std::string_view("TEST"));
}

TEST(OrderBook, AddRestsOrderWithItsDetails) {
  OrderBook book(kConfig);
  const OrderId id = book.add(Side::Buy, 150, 10);

  ASSERT_NE(id, kInvalidOrderId);
  const lob::Order* order = book.find(id);
  ASSERT_TRUE(order != nullptr);
  EXPECT_EQ(order->id, id);
  EXPECT_EQ(order->price, 150);
  EXPECT_EQ(order->quantity, 10u);
  EXPECT_EQ(order->side, Side::Buy);
  EXPECT_EQ(std::string_view(order->symbol, 4), std::string_view("TEST"));
  EXPECT_EQ(book.size(), 1u);
}

TEST(OrderBook, BestBidIsHighestBidAndBestAskIsLowestAsk) {
  OrderBook book(kConfig);
  (void)book.add(Side::Buy, 140, 10);
  (void)book.add(Side::Buy, 145, 10);
  (void)book.add(Side::Buy, 120, 10);
  (void)book.add(Side::Sell, 160, 10);
  (void)book.add(Side::Sell, 155, 10);
  (void)book.add(Side::Sell, 199, 10);

  EXPECT_EQ(best_bid(book), 145);
  EXPECT_EQ(best_ask(book), 155);
}

TEST(OrderBook, OrdersAtOnePriceQueueInArrivalOrder) {
  OrderBook book(kConfig);
  const OrderId first = book.add(Side::Sell, 150, 10);
  const OrderId second = book.add(Side::Sell, 150, 20);
  const OrderId third = book.add(Side::Sell, 150, 30);

  EXPECT_EQ(queue_at(book, Side::Sell, 150), (std::vector<OrderId>{first, second, third}));
  EXPECT_EQ(book.quantity_at(Side::Sell, 150), 60u);
}

TEST(OrderBook, BidsAndAsksAtTheSamePriceAreSeparateQueues) {
  OrderBook book(kConfig);
  const OrderId bid = book.add(Side::Buy, 150, 10);
  const OrderId ask = book.add(Side::Sell, 150, 25);

  EXPECT_EQ(queue_at(book, Side::Buy, 150), (std::vector<OrderId>{bid}));
  EXPECT_EQ(queue_at(book, Side::Sell, 150), (std::vector<OrderId>{ask}));
  EXPECT_EQ(book.quantity_at(Side::Buy, 150), 10u);
  EXPECT_EQ(book.quantity_at(Side::Sell, 150), 25u);
}

TEST(OrderBook, AcceptsBothEndsOfThePriceRange) {
  OrderBook book(kConfig);
  EXPECT_NE(book.add(Side::Buy, 100, 1), kInvalidOrderId);
  EXPECT_NE(book.add(Side::Sell, 199, 1), kInvalidOrderId);
}

TEST(OrderBook, RejectsPricesOutsideTheRange) {
  OrderBook book(kConfig);
  EXPECT_EQ(book.add(Side::Buy, 99, 1), kInvalidOrderId);
  EXPECT_EQ(book.add(Side::Sell, 200, 1), kInvalidOrderId);
  EXPECT_EQ(book.add(Side::Buy, -5, 1), kInvalidOrderId);
  EXPECT_TRUE(book.empty());
}

TEST(OrderBook, RejectsZeroQuantity) {
  OrderBook book(kConfig);
  EXPECT_EQ(book.add(Side::Buy, 150, 0), kInvalidOrderId);
  EXPECT_TRUE(book.empty());
}

TEST(OrderBook, RejectsWhenFullThenAcceptsAfterACancel) {
  OrderBook book({.symbol = "TEST", .min_price = 100, .num_levels = 100, .max_orders = 2});
  const OrderId first = book.add(Side::Buy, 150, 1);
  EXPECT_NE(book.add(Side::Buy, 150, 1), kInvalidOrderId);
  EXPECT_EQ(book.add(Side::Buy, 150, 1), kInvalidOrderId);

  EXPECT_TRUE(book.cancel(first));
  EXPECT_NE(book.add(Side::Buy, 150, 1), kInvalidOrderId);
}

// --- Cancel ----------------------------------------------------------------

TEST(OrderBook, CancelRemovesTheOrder) {
  OrderBook book(kConfig);
  const OrderId id = book.add(Side::Buy, 150, 10);

  EXPECT_TRUE(book.cancel(id));

  EXPECT_TRUE(book.find(id) == nullptr);
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(best_bid(book), kNoPrice);
  EXPECT_EQ(book.quantity_at(Side::Buy, 150), 0u);
}

TEST(OrderBook, CancelFromMiddleOfQueueKeepsTheOthersInOrder) {
  OrderBook book(kConfig);
  const OrderId first = book.add(Side::Buy, 150, 10);
  const OrderId second = book.add(Side::Buy, 150, 20);
  const OrderId third = book.add(Side::Buy, 150, 30);

  EXPECT_TRUE(book.cancel(second));

  EXPECT_EQ(queue_at(book, Side::Buy, 150), (std::vector<OrderId>{first, third}));
  EXPECT_EQ(book.quantity_at(Side::Buy, 150), 40u);
  EXPECT_EQ(best_bid(book), 150);
}

TEST(OrderBook, CancellingTheBestLevelRevealsTheNextBest) {
  OrderBook book(kConfig);
  (void)book.add(Side::Buy, 110, 10);
  const OrderId top_bid = book.add(Side::Buy, 190, 10);
  (void)book.add(Side::Sell, 195, 10);
  const OrderId top_ask = book.add(Side::Sell, 192, 10);

  EXPECT_TRUE(book.cancel(top_bid));
  EXPECT_TRUE(book.cancel(top_ask));

  EXPECT_EQ(best_bid(book), 110);
  EXPECT_EQ(best_ask(book), 195);
}

TEST(OrderBook, CancellingTwiceFailsTheSecondTime) {
  OrderBook book(kConfig);
  const OrderId id = book.add(Side::Buy, 150, 10);
  const OrderId other = book.add(Side::Buy, 150, 10);

  EXPECT_TRUE(book.cancel(id));
  EXPECT_FALSE(book.cancel(id));

  EXPECT_EQ(book.size(), 1u);
  EXPECT_EQ(queue_at(book, Side::Buy, 150), (std::vector<OrderId>{other}));
}

TEST(OrderBook, CancelRejectsIdsThatWereNeverIssued) {
  OrderBook book(kConfig);
  (void)book.add(Side::Buy, 150, 10);

  EXPECT_FALSE(book.cancel(kInvalidOrderId));
  EXPECT_FALSE(book.cancel(7));               // slot 7 has never held an order
  EXPECT_FALSE(book.cancel(0xFFFF'FFFFULL));  // slot beyond the pool
  EXPECT_EQ(book.size(), 1u);
}

// The cancelled order's slot is recycled for the next order. The old ID must
// not be able to cancel the new occupant.
TEST(OrderBook, StaleIdCannotTouchTheOrderThatReusedItsSlot) {
  OrderBook book(kConfig);
  const OrderId old_id = book.add(Side::Buy, 150, 10);
  EXPECT_TRUE(book.cancel(old_id));
  const OrderId new_id = book.add(Side::Sell, 160, 20);

  ASSERT_EQ(static_cast<lob::OrderIndex>(old_id), static_cast<lob::OrderIndex>(new_id));
  EXPECT_NE(old_id, new_id);
  EXPECT_FALSE(book.cancel(old_id));
  EXPECT_EQ(book.execute(old_id, 5), 0u);

  ASSERT_TRUE(book.find(new_id) != nullptr);
  EXPECT_EQ(book.find(new_id)->quantity, 20u);
}

// --- Execute ---------------------------------------------------------------

TEST(OrderBook, PartialExecuteShrinksOrderAndKeepsItsPlace) {
  OrderBook book(kConfig);
  const OrderId first = book.add(Side::Sell, 150, 100);
  const OrderId second = book.add(Side::Sell, 150, 50);

  EXPECT_EQ(book.execute(first, 30), 30u);

  ASSERT_TRUE(book.find(first) != nullptr);
  EXPECT_EQ(book.find(first)->quantity, 70u);
  EXPECT_EQ(book.quantity_at(Side::Sell, 150), 120u);
  EXPECT_EQ(queue_at(book, Side::Sell, 150), (std::vector<OrderId>{first, second}));
}

TEST(OrderBook, FullExecuteRemovesTheOrder) {
  OrderBook book(kConfig);
  const OrderId first = book.add(Side::Sell, 150, 100);
  const OrderId second = book.add(Side::Sell, 150, 50);

  EXPECT_EQ(book.execute(first, 100), 100u);

  EXPECT_TRUE(book.find(first) == nullptr);
  EXPECT_EQ(book.quantity_at(Side::Sell, 150), 50u);
  EXPECT_EQ(queue_at(book, Side::Sell, 150), (std::vector<OrderId>{second}));
  EXPECT_FALSE(book.cancel(first));
}

TEST(OrderBook, ExecuteNeverFillsMoreThanRemains) {
  OrderBook book(kConfig);
  const OrderId id = book.add(Side::Buy, 150, 40);

  EXPECT_EQ(book.execute(id, 1'000), 40u);

  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.quantity_at(Side::Buy, 150), 0u);
}

TEST(OrderBook, RepeatedPartialExecutesAddUpToAFullFill) {
  OrderBook book(kConfig);
  const OrderId id = book.add(Side::Buy, 150, 30);

  EXPECT_EQ(book.execute(id, 10), 10u);
  EXPECT_EQ(book.execute(id, 10), 10u);
  EXPECT_EQ(book.execute(id, 10), 10u);

  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.execute(id, 10), 0u);
}

TEST(OrderBook, ExecutingTheLastOrderAtTheBestPriceMovesTheBest) {
  OrderBook book(kConfig);
  const OrderId best = book.add(Side::Sell, 150, 10);
  (void)book.add(Side::Sell, 175, 10);

  EXPECT_EQ(book.execute(best, 10), 10u);

  EXPECT_EQ(best_ask(book), 175);
}

TEST(OrderBook, ExecuteOnUnknownIdFillsNothing) {
  OrderBook book(kConfig);
  EXPECT_EQ(book.execute(12'345, 10), 0u);
}

}  // namespace

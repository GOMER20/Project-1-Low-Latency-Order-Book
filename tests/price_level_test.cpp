#include <vector>

#include <gtest/gtest.h>

#include "lob/price_level.hpp"

namespace {

using lob::kNullIndex;
using lob::OrderId;
using lob::OrderIndex;
using lob::OrderPool;
using lob::PriceLevel;
using lob::Quantity;

OrderIndex add(OrderPool& pool, PriceLevel& level, OrderId id, Quantity quantity) {
  const OrderIndex index = pool.allocate();
  pool[index].id = id;
  pool[index].quantity = quantity;
  level.push_back(pool, index);
  return index;
}

std::vector<OrderId> front_to_back(const OrderPool& pool, const PriceLevel& level) {
  std::vector<OrderId> ids;
  for (OrderIndex i = level.head; i != kNullIndex; i = pool[i].next) {
    ids.push_back(pool[i].id);
  }
  return ids;
}

std::vector<OrderId> back_to_front(const OrderPool& pool, const PriceLevel& level) {
  std::vector<OrderId> ids;
  for (OrderIndex i = level.tail; i != kNullIndex; i = pool[i].prev) {
    ids.push_back(pool[i].id);
  }
  return ids;
}

TEST(PriceLevel, StartsEmpty) {
  PriceLevel level;
  EXPECT_TRUE(level.empty());
  EXPECT_EQ(level.head, kNullIndex);
  EXPECT_EQ(level.tail, kNullIndex);
  EXPECT_EQ(level.total_quantity, 0u);
}

TEST(PriceLevel, PushBackKeepsArrivalOrder) {
  OrderPool pool(8);
  PriceLevel level;
  add(pool, level, 1, 10);
  add(pool, level, 2, 20);
  add(pool, level, 3, 30);

  EXPECT_EQ(front_to_back(pool, level), (std::vector<OrderId>{1, 2, 3}));
  EXPECT_EQ(back_to_front(pool, level), (std::vector<OrderId>{3, 2, 1}));
  EXPECT_EQ(level.total_quantity, 60u);
}

TEST(PriceLevel, EraseHeadPromotesNextOrder) {
  OrderPool pool(8);
  PriceLevel level;
  const OrderIndex first = add(pool, level, 1, 10);
  add(pool, level, 2, 20);
  add(pool, level, 3, 30);

  level.erase(pool, first);

  EXPECT_EQ(front_to_back(pool, level), (std::vector<OrderId>{2, 3}));
  EXPECT_EQ(back_to_front(pool, level), (std::vector<OrderId>{3, 2}));
  EXPECT_EQ(level.total_quantity, 50u);
}

TEST(PriceLevel, EraseTailLeavesEarlierOrdersUntouched) {
  OrderPool pool(8);
  PriceLevel level;
  add(pool, level, 1, 10);
  add(pool, level, 2, 20);
  const OrderIndex last = add(pool, level, 3, 30);

  level.erase(pool, last);

  EXPECT_EQ(front_to_back(pool, level), (std::vector<OrderId>{1, 2}));
  EXPECT_EQ(back_to_front(pool, level), (std::vector<OrderId>{2, 1}));
  EXPECT_EQ(level.total_quantity, 30u);
}

TEST(PriceLevel, EraseMiddleJoinsNeighbours) {
  OrderPool pool(8);
  PriceLevel level;
  add(pool, level, 1, 10);
  const OrderIndex middle = add(pool, level, 2, 20);
  add(pool, level, 3, 30);

  level.erase(pool, middle);

  EXPECT_EQ(front_to_back(pool, level), (std::vector<OrderId>{1, 3}));
  EXPECT_EQ(back_to_front(pool, level), (std::vector<OrderId>{3, 1}));
  EXPECT_EQ(level.total_quantity, 40u);
}

TEST(PriceLevel, ErasingOnlyOrderEmptiesLevel) {
  OrderPool pool(8);
  PriceLevel level;
  const OrderIndex only = add(pool, level, 1, 10);

  level.erase(pool, only);

  EXPECT_TRUE(level.empty());
  EXPECT_EQ(level.head, kNullIndex);
  EXPECT_EQ(level.tail, kNullIndex);
  EXPECT_EQ(level.total_quantity, 0u);
}

TEST(PriceLevel, ReduceShrinksOrderButKeepsQueuePosition) {
  OrderPool pool(8);
  PriceLevel level;
  const OrderIndex first = add(pool, level, 1, 100);
  add(pool, level, 2, 50);

  level.reduce(pool[first], 30);

  EXPECT_EQ(pool[first].quantity, 70u);
  EXPECT_EQ(level.total_quantity, 120u);
  EXPECT_EQ(front_to_back(pool, level), (std::vector<OrderId>{1, 2}));
}

// A cancelled order's slot is recycled by the pool. The new order must join
// the back of the queue, not inherit the old order's position.
TEST(PriceLevel, RecycledSlotJoinsBackOfQueue) {
  OrderPool pool(8);
  PriceLevel level;
  const OrderIndex first = add(pool, level, 1, 10);
  add(pool, level, 2, 20);
  add(pool, level, 3, 30);

  level.erase(pool, first);
  pool.deallocate(first);
  const OrderIndex recycled = add(pool, level, 4, 40);

  EXPECT_EQ(recycled, first);
  EXPECT_EQ(front_to_back(pool, level), (std::vector<OrderId>{2, 3, 4}));
  EXPECT_EQ(back_to_front(pool, level), (std::vector<OrderId>{4, 3, 2}));
  EXPECT_EQ(level.total_quantity, 90u);
}

TEST(PriceLevel, LevelsSharingOnePoolStayIndependent) {
  OrderPool pool(8);
  PriceLevel bid;
  PriceLevel ask;
  add(pool, bid, 1, 10);
  const OrderIndex ask_order = add(pool, ask, 2, 20);
  add(pool, bid, 3, 30);

  ask.erase(pool, ask_order);

  EXPECT_TRUE(ask.empty());
  EXPECT_EQ(front_to_back(pool, bid), (std::vector<OrderId>{1, 3}));
  EXPECT_EQ(bid.total_quantity, 40u);
}

}  // namespace

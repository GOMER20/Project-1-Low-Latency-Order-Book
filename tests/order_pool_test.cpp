#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "lob/order_pool.hpp"

namespace {

using lob::kNullIndex;
using lob::Order;
using lob::OrderIndex;
using lob::OrderPool;

TEST(Order, OccupiesExactlyOneCacheLine) {
  EXPECT_EQ(sizeof(Order), 64u);
  EXPECT_EQ(alignof(Order), 64u);
}

TEST(OrderPool, StartsEmpty) {
  OrderPool pool(8);
  EXPECT_EQ(pool.capacity(), 8u);
  EXPECT_EQ(pool.size(), 0u);
  EXPECT_TRUE(pool.empty());
  EXPECT_FALSE(pool.full());
}

TEST(OrderPool, SlotsStartZeroed) {
  OrderPool pool(4);
  const OrderIndex index = pool.allocate();
  EXPECT_EQ(pool[index].id, 0u);
  EXPECT_EQ(pool[index].quantity, 0u);
}

TEST(OrderPool, HandsOutEverySlotExactlyOnce) {
  constexpr OrderIndex kCapacity = 64;
  OrderPool pool(kCapacity);
  std::vector<bool> seen(kCapacity, false);

  for (OrderIndex i = 0; i < kCapacity; ++i) {
    const OrderIndex index = pool.allocate();
    ASSERT_NE(index, kNullIndex);
    ASSERT_LT(index, kCapacity);
    EXPECT_FALSE(seen[index]);
    seen[index] = true;
  }

  EXPECT_TRUE(pool.full());
  EXPECT_EQ(pool.size(), kCapacity);
}

TEST(OrderPool, ReturnsNullIndexWhenExhausted) {
  OrderPool pool(2);
  EXPECT_NE(pool.allocate(), kNullIndex);
  EXPECT_NE(pool.allocate(), kNullIndex);
  EXPECT_EQ(pool.allocate(), kNullIndex);
  EXPECT_EQ(pool.size(), 2u);
}

TEST(OrderPool, ZeroCapacityPoolIsAlwaysExhausted) {
  OrderPool pool(0);
  EXPECT_EQ(pool.allocate(), kNullIndex);
  EXPECT_TRUE(pool.full());
}

TEST(OrderPool, ReusesMostRecentlyFreedSlotFirst) {
  OrderPool pool(4);
  const OrderIndex a = pool.allocate();
  const OrderIndex b = pool.allocate();

  pool.deallocate(a);
  pool.deallocate(b);

  EXPECT_EQ(pool.allocate(), b);
  EXPECT_EQ(pool.allocate(), a);
}

TEST(OrderPool, SlotsAreCacheLineAligned) {
  OrderPool pool(16);
  for (OrderIndex i = 0; i < pool.capacity(); ++i) {
    const OrderIndex index = pool.allocate();
    const auto address = reinterpret_cast<std::uintptr_t>(&pool[index]);
    EXPECT_EQ(address % 64, 0u);
  }
}

TEST(OrderPool, DataSurvivesAllocationsOfOtherSlots) {
  OrderPool pool(4);
  const OrderIndex first = pool.allocate();
  pool[first].id = 42;
  pool[first].price = 10'050;
  pool[first].quantity = 300;
  pool[first].side = lob::Side::Sell;

  const OrderIndex second = pool.allocate();
  pool[second].id = 43;
  pool[second].quantity = 1;
  pool.deallocate(second);

  EXPECT_EQ(pool[first].id, 42u);
  EXPECT_EQ(pool[first].price, 10'050);
  EXPECT_EQ(pool[first].quantity, 300u);
  EXPECT_EQ(pool[first].side, lob::Side::Sell);
}

TEST(OrderPool, RecoversFullCapacityAfterDrain) {
  constexpr OrderIndex kCapacity = 32;
  OrderPool pool(kCapacity);
  std::vector<OrderIndex> held;

  for (int round = 0; round < 3; ++round) {
    for (OrderIndex i = 0; i < kCapacity; ++i) {
      held.push_back(pool.allocate());
      ASSERT_NE(held.back(), kNullIndex);
    }
    EXPECT_EQ(pool.allocate(), kNullIndex);

    for (const OrderIndex index : held) {
      pool.deallocate(index);
    }
    held.clear();
    EXPECT_TRUE(pool.empty());
  }
}

}  // namespace

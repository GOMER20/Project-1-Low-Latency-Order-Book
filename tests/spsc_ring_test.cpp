#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lob/spsc_ring.hpp"
#include "lob/trade.hpp"

namespace {

using lob::SpscRing;

// --- One thread: the queue's behaviour ---------------------------------------

TEST(SpscRing, CapacityRoundsUpToAPowerOfTwo) {
  EXPECT_EQ(SpscRing<int>(0).capacity(), 1u);
  EXPECT_EQ(SpscRing<int>(1).capacity(), 1u);
  EXPECT_EQ(SpscRing<int>(5).capacity(), 8u);
  EXPECT_EQ(SpscRing<int>(8).capacity(), 8u);
  EXPECT_EQ(SpscRing<int>(1'000).capacity(), 1'024u);
}

// The producer's and consumer's counters must not share a cache line.
TEST(SpscRing, OccupiesThreeWholeCacheLines) {
  EXPECT_EQ(alignof(SpscRing<int>), 64u);
  EXPECT_EQ(sizeof(SpscRing<int>), 3 * 64u);
}

TEST(SpscRing, StartsEmpty) {
  SpscRing<int> ring(8);
  int value = -1;
  EXPECT_TRUE(ring.empty());
  EXPECT_EQ(ring.size(), 0u);
  EXPECT_FALSE(ring.try_pop(value));
  EXPECT_EQ(value, -1);
}

TEST(SpscRing, ItemsComeOutInTheOrderTheyWentIn) {
  SpscRing<int> ring(8);
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(ring.try_push(i * 10));
  }
  EXPECT_EQ(ring.size(), 5u);

  for (int i = 0; i < 5; ++i) {
    int value = -1;
    ASSERT_TRUE(ring.try_pop(value));
    EXPECT_EQ(value, i * 10);
  }
  EXPECT_TRUE(ring.empty());
}

TEST(SpscRing, EverySlotIsUsableAndAFullRingRejectsPushes) {
  SpscRing<int> ring(4);
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(ring.try_push(i));
  }
  EXPECT_EQ(ring.size(), 4u);
  EXPECT_FALSE(ring.try_push(99));

  int value = -1;
  ASSERT_TRUE(ring.try_pop(value));
  EXPECT_EQ(value, 0);
  EXPECT_TRUE(ring.try_push(4));
  EXPECT_FALSE(ring.try_push(99));

  for (int expected = 1; expected <= 4; ++expected) {
    ASSERT_TRUE(ring.try_pop(value));
    EXPECT_EQ(value, expected);
  }
  EXPECT_FALSE(ring.try_pop(value));
}

TEST(SpscRing, SingleSlotRingAlternatesBetweenFullAndEmpty) {
  SpscRing<int> ring(1);
  int value = -1;
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(ring.try_push(i));
    EXPECT_FALSE(ring.try_push(i));
    ASSERT_TRUE(ring.try_pop(value));
    EXPECT_EQ(value, i);
    EXPECT_FALSE(ring.try_pop(value));
  }
}

// Bursts of varying size push the counters around the ring many times.
TEST(SpscRing, KeepsOrderAcrossManyWrapArounds) {
  SpscRing<std::uint64_t> ring(8);
  std::uint64_t next_in = 0;
  std::uint64_t next_out = 0;

  for (int round = 0; round < 1'000; ++round) {
    const int burst = 1 + round % 8;
    for (int i = 0; i < burst; ++i) {
      ASSERT_TRUE(ring.try_push(next_in++));
    }
    for (int i = 0; i < burst; ++i) {
      std::uint64_t value = 0;
      ASSERT_TRUE(ring.try_pop(value));
      ASSERT_EQ(value, next_out++);
    }
  }
  EXPECT_TRUE(ring.empty());
}

TEST(SpscRing, DrainVisitsEverythingInOrderAndEmptiesTheRing) {
  SpscRing<int> ring(8);
  int ignored = 0;
  // Move the counters off zero first, so the drain crosses the wrap point.
  for (int i = 0; i < 6; ++i) {
    ASSERT_TRUE(ring.try_push(-1));
    ASSERT_TRUE(ring.try_pop(ignored));
  }
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(ring.try_push(i));
  }

  std::vector<int> seen;
  EXPECT_EQ(ring.drain([&](const int& value) { seen.push_back(value); }), 5u);

  EXPECT_EQ(seen, (std::vector<int>{0, 1, 2, 3, 4}));
  EXPECT_TRUE(ring.empty());
  EXPECT_EQ(ring.drain([&](const int&) { seen.push_back(-1); }), 0u);
  EXPECT_EQ(seen.size(), 5u);

  // The ring keeps working after a drain.
  EXPECT_TRUE(ring.try_push(7));
  ASSERT_TRUE(ring.try_pop(ignored));
  EXPECT_EQ(ignored, 7);
}

TEST(SpscRing, CarriesTradeEvents) {
  SpscRing<lob::Trade> ring(4);
  lob::Trade sent{};
  sent.maker_id = 11;
  sent.taker_id = 22;
  sent.price = 15'000;
  sent.quantity = 40;
  sent.taker_side = lob::Side::Sell;

  ASSERT_TRUE(ring.try_push(sent));
  lob::Trade received{};
  ASSERT_TRUE(ring.try_pop(received));

  EXPECT_EQ(received.maker_id, 11u);
  EXPECT_EQ(received.taker_id, 22u);
  EXPECT_EQ(received.price, 15'000);
  EXPECT_EQ(received.quantity, 40u);
  EXPECT_EQ(received.taker_side, lob::Side::Sell);
}

// --- Two threads: nothing lost, duplicated, reordered or torn -----------------
// These are most valuable under ThreadSanitizer (-DLOB_ENABLE_TSAN=ON).

// A small ring forces the producer to hit "full" and the consumer to hit
// "empty" constantly, which is where the two threads interact.
TEST(SpscRingThreads, EveryItemArrivesOnceAndInOrder) {
  constexpr std::uint64_t kItems = 1'000'000;
  SpscRing<std::uint64_t> ring(64);

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      while (!ring.try_push(i)) {
        std::this_thread::yield();
      }
    }
  });

  std::uint64_t expected = 0;
  std::uint64_t out_of_order = 0;
  while (expected < kItems) {
    std::uint64_t value = 0;
    if (ring.try_pop(value)) {
      out_of_order += value != expected ? 1 : 0;
      ++expected;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();

  EXPECT_EQ(out_of_order, 0u);
  EXPECT_TRUE(ring.empty());
}

// Each message fills most of a cache line with values derived from its
// sequence number, so a half-written message would be detected.
TEST(SpscRingThreads, MessagesAreNeverTornWhenDraining) {
  struct Message {
    std::uint64_t sequence;
    std::uint64_t inverse;
    std::uint64_t copies[5];
  };
  constexpr std::uint64_t kItems = 500'000;
  SpscRing<Message> ring(256);

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      const Message message{i, ~i, {i, i, i, i, i}};
      while (!ring.try_push(message)) {
        std::this_thread::yield();
      }
    }
  });

  std::uint64_t expected = 0;
  std::uint64_t corrupt = 0;
  while (expected < kItems) {
    const std::size_t drained = ring.drain([&](const Message& message) {
      bool intact = message.sequence == expected && message.inverse == ~expected;
      for (const std::uint64_t copy : message.copies) {
        intact = intact && copy == expected;
      }
      corrupt += intact ? 0 : 1;
      ++expected;
    });
    if (drained == 0) {
      std::this_thread::yield();
    }
  }
  producer.join();

  EXPECT_EQ(corrupt, 0u);
  EXPECT_EQ(expected, kItems);
  EXPECT_TRUE(ring.empty());
}

}  // namespace

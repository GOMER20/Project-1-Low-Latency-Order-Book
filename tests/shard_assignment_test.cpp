#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "lob/shard_assignment.hpp"

namespace {

using lob::assign_shards;

using Loads = std::vector<std::uint64_t>;
using Shards = std::vector<std::uint16_t>;

// The load each shard ends up carrying.
Loads totals(const Loads& loads, const Shards& shard_of, std::size_t shards) {
  Loads carried(shards, 0);
  for (std::size_t symbol = 0; symbol < loads.size(); ++symbol) {
    carried[shard_of[symbol]] += loads[symbol];
  }
  return carried;
}

std::uint64_t busiest(const Loads& carried) {
  return *std::max_element(carried.begin(), carried.end());
}

// The best possible result, found by trying every assignment. Only for tiny inputs.
std::uint64_t best_possible_busiest(const Loads& loads, std::size_t shards) {
  std::uint64_t best = ~std::uint64_t{0};
  Shards shard_of(loads.size(), 0);
  while (true) {
    best = std::min(best, busiest(totals(loads, shard_of, shards)));
    // Count upwards in base `shards`.
    std::size_t digit = 0;
    while (digit < shard_of.size() && ++shard_of[digit] == shards) {
      shard_of[digit++] = 0;
    }
    if (digit == shard_of.size()) {
      return best;
    }
  }
}

TEST(ShardAssignment, EqualLoadsAreDealtInTurn) {
  EXPECT_EQ(assign_shards({7, 7, 7, 7, 7}, 2), (Shards{0, 1, 0, 1, 0}));
  EXPECT_EQ(assign_shards({1, 1, 1, 1, 1, 1, 1}, 3), (Shards{0, 1, 2, 0, 1, 2, 0}));
}

// In turn, the two busy symbols (IDs 0 and 2) would land on the same shard.
TEST(ShardAssignment, BusySymbolsAreKeptApart) {
  const Loads loads{100, 1, 90, 1, 1, 1};

  const Shards shard_of = assign_shards(loads, 2);

  EXPECT_NE(shard_of[0], shard_of[2]);
  EXPECT_EQ(totals(loads, shard_of, 2), (Loads{100, 94}));
}

TEST(ShardAssignment, OneDominantSymbolGetsAShardToItself) {
  const Loads loads{1, 1, 1'000, 1, 1, 1};

  const Shards shard_of = assign_shards(loads, 2);

  for (std::size_t symbol = 0; symbol < loads.size(); ++symbol) {
    if (symbol != 2) {
      EXPECT_NE(shard_of[symbol], shard_of[2]) << "symbol " << symbol;
    }
  }
}

TEST(ShardAssignment, QuietSymbolsFillInBehindTheBusyOnes) {
  const Loads loads{50, 40, 30, 20, 10, 10};

  const Shards shard_of = assign_shards(loads, 3);

  // 50 | 40 + 10 | 30 + 20, then the last 10 joins the lightest shard.
  const Loads carried = totals(loads, shard_of, 3);
  EXPECT_EQ(busiest(carried), 60u);
  EXPECT_EQ(*std::min_element(carried.begin(), carried.end()), 50u);
}

TEST(ShardAssignment, NoShardIsLeftEmptyEvenIfEveryLoadIsZero) {
  for (const Loads& loads : {Loads{0, 0, 0, 0}, Loads{5, 0, 0, 0}, Loads{0, 0, 0, 9}}) {
    const Shards shard_of = assign_shards(loads, 4);
    Shards sorted = shard_of;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(sorted, (Shards{0, 1, 2, 3}));
  }
}

TEST(ShardAssignment, OneShardTakesEverything) {
  EXPECT_EQ(assign_shards({3, 1, 4, 1, 5}, 1), (Shards{0, 0, 0, 0, 0}));
}

TEST(ShardAssignment, NoSymbolsGivesAnEmptyAnswer) {
  EXPECT_TRUE(assign_shards({}, 3).empty());
}

TEST(ShardAssignment, SameInputAlwaysGivesTheSameAnswer) {
  const Loads loads{9, 9, 9, 3, 3, 3, 1, 1, 1, 0, 0};
  EXPECT_EQ(assign_shards(loads, 4), assign_shards(loads, 4));
}

// The largest-first rule promises that the busiest shard carries at most 4/3
// of what it would under the best possible split. Checked against every
// possible split for 400 small random cases.
TEST(ShardAssignment, IsNeverWorseThanFourThirdsOfTheBestPossibleSplit) {
  std::mt19937_64 rng(11);
  for (int trial = 0; trial < 400; ++trial) {
    const std::size_t shards = 2 + rng() % 3;   // 2..4
    const std::size_t symbols = shards + rng() % (9 - shards);  // up to 8
    Loads loads(symbols);
    for (std::uint64_t& load : loads) {
      // A mix of quiet and very busy symbols.
      load = rng() % 4 == 0 ? 50 + rng() % 950 : rng() % 50;
    }

    const std::uint64_t ours = busiest(totals(loads, assign_shards(loads, shards), shards));
    const std::uint64_t best = best_possible_busiest(loads, shards);

    ASSERT_GE(ours, best) << "trial " << trial;
    ASSERT_LE(3 * ours, 4 * best) << "trial " << trial;
  }
}

// On traffic concentrated in a few symbols, it should do clearly better than
// dealing the symbols in turn.
TEST(ShardAssignment, BeatsDealingInTurnWhenTrafficIsConcentrated) {
  Loads loads(64, 10);
  for (const std::size_t busy : {0u, 4u, 8u, 12u}) {  // all on shard 0 if dealt in turn
    loads[busy] = 5'000;
  }
  Shards in_turn(loads.size());
  for (std::size_t symbol = 0; symbol < loads.size(); ++symbol) {
    in_turn[symbol] = static_cast<std::uint16_t>(symbol % 4);
  }

  const std::uint64_t dealt = busiest(totals(loads, in_turn, 4));
  const std::uint64_t balanced = busiest(totals(loads, assign_shards(loads, 4), 4));

  EXPECT_EQ(dealt, 4 * 5'000u + 12 * 10u);
  EXPECT_EQ(balanced, 5'000u + 15 * 10u);
}

}  // namespace

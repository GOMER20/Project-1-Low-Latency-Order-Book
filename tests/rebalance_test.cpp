// Evening out the shards automatically, from the traffic each symbol really gets.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "lob/shard_assignment.hpp"
#include "lob/sharded_engine.hpp"

namespace {

using lob::choose_rebalancing_move;
using lob::Event;
using lob::EventType;
using lob::RebalancingMove;
using lob::SendStatus;
using lob::ShardedEngine;
using lob::Side;
using lob::SymbolId;

using Loads = std::vector<std::uint64_t>;
using Shards = std::vector<std::uint16_t>;

// --- The decision on its own ------------------------------------------------------

// Asks for a move with the given tolerance and reports what each shard carries.
std::optional<RebalancingMove> decide(const Loads& loads, const Shards& shard_of,
                                      std::size_t shards, std::uint32_t tolerance_percent,
                                      Loads* carried_out = nullptr) {
  Loads carried(shards, 0);
  const auto move = choose_rebalancing_move(loads, shard_of, carried, tolerance_percent);
  if (carried_out != nullptr) {
    *carried_out = carried;
  }
  return move;
}

TEST(RebalanceDecision, EvenShardsAreLeftAlone) {
  EXPECT_FALSE(decide({10, 10, 10, 10}, {0, 1, 0, 1}, 2, 0).has_value());
}

TEST(RebalanceDecision, ReportsWhatEachShardIsCarrying) {
  Loads carried;
  (void)decide({5, 7, 11, 13, 17}, {0, 1, 2, 0, 1}, 3, 25, &carried);
  EXPECT_EQ(carried, (Loads{18, 24, 11}));
}

// Fair share is 100. With a 25% tolerance, 125 is still fine and 126 is not.
TEST(RebalanceDecision, NothingMovesUntilTheBusiestShardIsPastTheTolerance) {
  EXPECT_FALSE(decide({100, 25, 75}, {0, 0, 1}, 2, 25).has_value());
  EXPECT_TRUE(decide({100, 26, 74}, {0, 0, 1}, 2, 25).has_value());
}

TEST(RebalanceDecision, MovesFromTheBusiestShardToTheQuietest) {
  //               shard 0: 100 + 80       shard 1: 60      shard 2: 20
  const auto move = decide({100, 60, 20, 80}, {0, 1, 2, 0}, 3, 10);

  ASSERT_TRUE(move.has_value());
  EXPECT_EQ(move->from, 0u);
  EXPECT_EQ(move->to, 2u);
}

// Shard 0 carries 100 and shard 1 carries 20: a gap of 80. Moving a symbol of
// load w leaves a gap of |80 - 2w|, so the symbol nearest 40 is the one to move.
TEST(RebalanceDecision, PicksTheSymbolThatNarrowsTheGapMost) {
  const auto move = decide({10, 55, 35, 20}, {0, 0, 0, 1}, 2, 10);

  ASSERT_TRUE(move.has_value());
  EXPECT_EQ(move->symbol, 2u);  // 35 leaves a gap of 10; 55 would leave 30; 10 would leave 60
}

// Moving the only busy symbol would just move the problem to the other shard.
TEST(RebalanceDecision, LeavesASymbolTooBigToHelpWhereItIs) {
  EXPECT_FALSE(decide({1'000, 5, 5}, {0, 1, 1}, 2, 10).has_value());
  EXPECT_FALSE(decide({1'000}, {0}, 2, 10).has_value());
}

// Shard 0 carries 310 and shard 1 carries 100: a gap of 210. The busy symbol is
// too big to move, and moving a quiet one would barely change anything, so it
// is not worth the pause a move costs.
TEST(RebalanceDecision, LeavesSymbolsTooQuietToMatterWhereTheyAre) {
  EXPECT_FALSE(decide({300, 5, 5, 100}, {0, 0, 0, 1}, 2, 10).has_value());
}

// A move has to at least halve the gap. The gap here is 80.
TEST(RebalanceDecision, AMoveMustAtLeastHalveTheGap) {
  EXPECT_FALSE(decide({81, 19, 20}, {0, 0, 1}, 2, 10).has_value());  // 19 leaves 42
  EXPECT_TRUE(decide({80, 20, 20}, {0, 0, 1}, 2, 10).has_value());   // 20 leaves 40
}

TEST(RebalanceDecision, IdleSymbolsAreNeverMoved) {
  const auto move = decide({0, 0, 500, 300, 10}, {0, 0, 0, 0, 1}, 2, 10);

  ASSERT_TRUE(move.has_value());
  EXPECT_TRUE(move->symbol == 2 || move->symbol == 3);
}

TEST(RebalanceDecision, OneShardHasNothingToBalance) {
  EXPECT_FALSE(decide({9, 1, 5}, {0, 0, 0}, 1, 0).has_value());
}

TEST(RebalanceDecision, NoTrafficMeansNoMove) {
  EXPECT_FALSE(decide({0, 0, 0, 0}, {0, 0, 1, 1}, 2, 0).has_value());
}

// Applied one after another, the moves must never make the busiest shard
// busier, must come to a stop, and must stop only when the shards are within
// tolerance or no single move can narrow the gap.
TEST(RebalanceDecision, RepeatedMovesOnlyEverImproveAndAlwaysStop) {
  std::mt19937_64 rng(5);
  for (int trial = 0; trial < 300; ++trial) {
    const std::size_t shards = 2 + rng() % 4;      // 2..5
    const std::size_t symbols = 1 + rng() % 40;    // 1..40
    const auto tolerance = static_cast<std::uint32_t>(rng() % 30);
    Loads loads(symbols);
    Shards shard_of(symbols);
    for (std::size_t symbol = 0; symbol < symbols; ++symbol) {
      loads[symbol] = rng() % 5 == 0 ? rng() % 5'000 : rng() % 100;
      shard_of[symbol] = static_cast<std::uint16_t>(rng() % shards);
    }

    Loads carried;
    (void)decide(loads, shard_of, shards, tolerance, &carried);
    std::uint64_t busiest = *std::max_element(carried.begin(), carried.end());

    int moves = 0;
    while (const auto move = decide(loads, shard_of, shards, tolerance)) {
      ASSERT_EQ(shard_of[move->symbol], move->from) << "trial " << trial;
      shard_of[move->symbol] = static_cast<std::uint16_t>(move->to);
      (void)decide(loads, shard_of, shards, tolerance, &carried);
      const std::uint64_t busiest_now = *std::max_element(carried.begin(), carried.end());
      ASSERT_LE(busiest_now, busiest) << "trial " << trial << ": a move made things worse";
      busiest = busiest_now;
      ASSERT_LT(++moves, 1'000) << "trial " << trial << ": the moves never stopped";
    }
  }
}

// --- The engine balancing itself ---------------------------------------------------

// Six symbols on two shards. Dealt in turn, symbols 0, 2 and 4 are on shard 0
// and symbols 1, 3 and 5 are on shard 1. Tradable prices are 100..199.
std::vector<lob::BookConfig> six_books() {
  std::vector<lob::BookConfig> books;
  for (const char* name : {"AAA", "BBB", "CCC", "DDD", "EEE", "FFF"}) {
    books.push_back({.symbol = name, .min_price = 100, .num_levels = 100, .max_orders = 64});
  }
  return books;
}

// Runs the shards by hand until nothing is left to do, discarding the events.
void settle(ShardedEngine& engine) {
  int idle_rounds = 0;
  while (idle_rounds < 2) {
    const std::size_t handled = engine.process_pending();
    const std::size_t polled = engine.poll([](const Event&) {});
    idle_rounds = handled == 0 && polled == 0 ? idle_rounds + 1 : 0;
  }
}

// Sends `commands` commands for one symbol: sells and buys that trade with
// each other, so the book ends up empty however many are sent.
void send(ShardedEngine& engine, SymbolId symbol, int commands) {
  for (int i = 0; i < commands; ++i) {
    ASSERT_EQ(engine.submit(0, symbol, i % 2 == 0 ? Side::Sell : Side::Buy, 150, 10),
              SendStatus::Sent);
    if (i % 32 == 31) {
      settle(engine);  // keep the rings from filling
    }
  }
  settle(engine);
}

// The traffic every test here uses unless it says otherwise: symbols 0 and 2,
// which share shard 0, are busy; the rest are quiet.
void send_lopsided_traffic(ShardedEngine& engine) {
  send(engine, 0, 400);
  send(engine, 2, 400);
  for (const SymbolId quiet : {SymbolId{1}, SymbolId{3}, SymbolId{4}, SymbolId{5}}) {
    send(engine, quiet, 10);
  }
}

TEST(Rebalance, MovesABusySymbolOffTheOverloadedShard) {
  ShardedEngine engine({.books = six_books(), .shards = 2, .rebalance = {.min_sample = 100}});
  ASSERT_EQ(engine.shard_of(0), engine.shard_of(2));
  send_lopsided_traffic(engine);  // shard 0 has handled 810 commands, shard 1 has handled 30

  const std::optional<SymbolId> moved = engine.rebalance();
  settle(engine);

  ASSERT_TRUE(moved.has_value());
  EXPECT_TRUE(*moved == 0 || *moved == 2);
  EXPECT_NE(engine.shard_of(0), engine.shard_of(2));  // the busy pair has been split up
  EXPECT_EQ(engine.rebalancing_moves(), 1u);
  EXPECT_FALSE(engine.move_in_progress(*moved));
}

TEST(Rebalance, LeavesThingsAloneOnceTheyAreEven) {
  ShardedEngine engine({.books = six_books(), .shards = 2, .rebalance = {.min_sample = 100}});
  send_lopsided_traffic(engine);
  ASSERT_TRUE(engine.rebalance().has_value());
  settle(engine);

  // The same traffic, again and again: 410 against 430 commands a round now.
  for (int round = 0; round < 10; ++round) {
    send_lopsided_traffic(engine);
    EXPECT_FALSE(engine.rebalance().has_value()) << "round " << round;
    settle(engine);
  }
  EXPECT_EQ(engine.rebalancing_moves(), 1u);
}

TEST(Rebalance, WaitsUntilItHasSeenEnoughTraffic) {
  ShardedEngine engine({.books = six_books(), .shards = 2, .rebalance = {.min_sample = 500}});
  send(engine, 0, 200);
  send(engine, 2, 200);
  EXPECT_FALSE(engine.rebalance().has_value());  // 400 handled: not enough to judge on

  send(engine, 0, 100);  // 500 in all since the last decision
  EXPECT_TRUE(engine.rebalance().has_value());
}

// Sends commands for one symbol and has the shards handle them, but reads no
// events, so a move that is under way stays under way.
void send_without_reading(ShardedEngine& engine, SymbolId symbol, int commands) {
  for (int i = 0; i < commands; ++i) {
    ASSERT_EQ(engine.submit(0, symbol, i % 2 == 0 ? Side::Sell : Side::Buy, 150, 10),
              SendStatus::Sent);
  }
  engine.process_pending();
}

TEST(Rebalance, StartsOnlyOneMoveAtATime) {
  ShardedEngine engine({.books = six_books(), .shards = 2, .rebalance = {.min_sample = 100}});
  send_lopsided_traffic(engine);
  const std::optional<SymbolId> first = engine.rebalance();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(engine.move_in_progress(*first));  // nothing has been run or read yet

  // More traffic arrives for two symbols on the shard the move left behind,
  // which makes that shard look badly overloaded again. A second move would
  // be justified on the numbers, but the first has not finished.
  const SymbolId stayed = *first == 0 ? SymbolId{2} : SymbolId{0};
  send_without_reading(engine, stayed, 600);
  send_without_reading(engine, 4, 600);
  ASSERT_TRUE(engine.move_in_progress(*first));

  EXPECT_FALSE(engine.rebalance().has_value());
  EXPECT_EQ(engine.rebalancing_moves(), 1u);

  // Once the first move is done, the second goes ahead.
  settle(engine);
  ASSERT_FALSE(engine.move_in_progress(*first));
  EXPECT_TRUE(engine.rebalance().has_value());
  EXPECT_EQ(engine.rebalancing_moves(), 2u);
}

// Heavy traffic that was evenly spread must not go on outweighing a later
// shift for ever. Symbols 0 and 1 are busy first, one on each shard. Then
// they go silent, and symbols 2 and 4, which share shard 0, carry a steady
// 100 commands each per look. Because older traffic fades by a quarter at
// each look, the third look after the shift is the first to find shard 0 more
// than 25% over its share.
TEST(Rebalance, OldTrafficFadesSoALastingShiftIsActedOn) {
  ShardedEngine engine({.books = six_books(), .shards = 2, .rebalance = {.min_sample = 100}});
  send(engine, 0, 1'000);
  send(engine, 1, 1'000);
  ASSERT_FALSE(engine.rebalance().has_value());  // 1,000 on each shard: even

  for (int look = 1; look <= 2; ++look) {
    send(engine, 2, 100);
    send(engine, 4, 100);
    EXPECT_FALSE(engine.rebalance().has_value()) << "look " << look << " after the shift";
  }

  send(engine, 2, 100);
  send(engine, 4, 100);
  const std::optional<SymbolId> moved = engine.rebalance();
  settle(engine);

  ASSERT_TRUE(moved.has_value());
  EXPECT_TRUE(*moved == 2 || *moved == 4);
  EXPECT_NE(engine.shard_of(2), engine.shard_of(4));
}

TEST(Rebalance, LeavesASingleDominantSymbolWhereItIs) {
  ShardedEngine engine({.books = six_books(), .shards = 2, .rebalance = {.min_sample = 100}});
  send(engine, 0, 1'000);
  send(engine, 1, 10);

  EXPECT_FALSE(engine.rebalance().has_value());
  EXPECT_EQ(engine.rebalancing_moves(), 0u);
}

TEST(Rebalance, FollowsTheTrafficWhenItShifts) {
  ShardedEngine engine({.books = six_books(), .shards = 2, .rebalance = {.min_sample = 100}});
  send_lopsided_traffic(engine);
  ASSERT_TRUE(engine.rebalance().has_value());
  settle(engine);
  ASSERT_NE(engine.shard_of(0), engine.shard_of(2));

  // Now symbols 4 and whichever busy symbol stayed behind go quiet, and two
  // symbols that share a shard become the busy ones.
  const std::size_t crowded = engine.shard_of(1);
  SymbolId first_busy = 1;
  SymbolId second_busy = 0;
  for (SymbolId symbol = 2; symbol < 6; ++symbol) {
    if (engine.shard_of(symbol) == crowded) {
      second_busy = symbol;
      break;
    }
  }
  ASSERT_NE(second_busy, 0);
  send(engine, first_busy, 400);
  send(engine, second_busy, 400);

  const std::optional<SymbolId> moved = engine.rebalance();
  settle(engine);

  ASSERT_TRUE(moved.has_value());
  EXPECT_NE(engine.shard_of(first_busy), engine.shard_of(second_busy));
  EXPECT_EQ(engine.rebalancing_moves(), 2u);
}

TEST(Rebalance, OneShardNeverRebalances) {
  ShardedEngine engine({.books = six_books(), .shards = 1, .rebalance = {.min_sample = 1}});
  send(engine, 0, 500);
  EXPECT_FALSE(engine.rebalance().has_value());
}

// With `every` set, nobody calls rebalance(): sending commands is enough.
TEST(Rebalance, HappensByItselfWhenAskedTo) {
  ShardedEngine engine({.books = six_books(),
                        .shards = 2,
                        .rebalance = {.every = 250, .min_sample = 100}});
  ASSERT_EQ(engine.shard_of(0), engine.shard_of(2));

  for (int round = 0; round < 4; ++round) {
    send_lopsided_traffic(engine);
  }

  EXPECT_GE(engine.rebalancing_moves(), 1u);
  EXPECT_LE(engine.rebalancing_moves(), 2u);  // it settled, rather than shuffling for ever
  EXPECT_NE(engine.shard_of(0), engine.shard_of(2));
}

TEST(Rebalance, NeverHappensByItselfUnlessAskedTo) {
  ShardedEngine engine({.books = six_books(), .shards = 2, .rebalance = {.min_sample = 100}});
  for (int round = 0; round < 4; ++round) {
    send_lopsided_traffic(engine);
  }
  EXPECT_EQ(engine.rebalancing_moves(), 0u);
  EXPECT_EQ(engine.shard_of(0), engine.shard_of(2));
}

// Rebalancing must not disturb the books it moves.
TEST(Rebalance, OrdersRestingInAMovedSymbolAreStillThere) {
  ShardedEngine engine({.books = six_books(), .shards = 2, .rebalance = {.min_sample = 100}});
  std::vector<Event> events;
  for (const SymbolId symbol : {SymbolId{0}, SymbolId{2}}) {
    ASSERT_EQ(engine.submit(7, symbol, Side::Buy, 120, 33), SendStatus::Sent);
  }
  engine.process_pending();
  engine.poll([&](const Event& event) { events.push_back(event); });
  ASSERT_EQ(events.size(), 2u);

  send_lopsided_traffic(engine);
  const std::optional<SymbolId> moved = engine.rebalance();
  settle(engine);
  ASSERT_TRUE(moved.has_value());

  // The order rested before the move can still be cancelled by its ID.
  const lob::OrderId resting = events[*moved == 0 ? 0 : 1].order_id;
  EXPECT_EQ(engine.book(*moved).quantity_at(Side::Buy, 120), 33u);
  ASSERT_EQ(engine.cancel(8, *moved, resting), SendStatus::Sent);
  engine.process_pending();
  std::vector<Event> answers;
  engine.poll([&](const Event& event) { answers.push_back(event); });
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].type, EventType::Cancelled);
}

}  // namespace

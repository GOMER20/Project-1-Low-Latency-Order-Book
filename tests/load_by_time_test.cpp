// Measuring a symbol's load by the time spent on it rather than by counting
// its commands.
//
// Anything that reads a clock can be thrown off by the machine being busy, so
// the tests here that compare times do it over several independent trials and
// judge by the middle one.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "lob/sharded_engine.hpp"

namespace {

using lob::cap_timed_sample;
using lob::Event;
using lob::LoadMeasure;
using lob::MatchingEngine;
using lob::OrderType;
using lob::Price;
using lob::SendStatus;
using lob::ShardedEngine;
using lob::Side;
using lob::SymbolId;

// --- Taming the stopwatch ----------------------------------------------------------

TEST(TimedSample, TheFirstSampleIsTakenAsItIs) {
  std::uint64_t typical = 0;
  EXPECT_EQ(cap_timed_sample(500, typical), 500u);
  EXPECT_EQ(typical, 500u * 256);
}

TEST(TimedSample, OrdinarySamplesAreCountedInFull) {
  std::uint64_t typical = 0;
  for (const std::uint64_t sample : {100u, 140u, 60u, 300u, 90u, 1'000u, 20u}) {
    EXPECT_EQ(cap_timed_sample(sample, typical), sample);
  }
}

// The thread was interrupted mid-command and the stopwatch read a million.
TEST(TimedSample, AnIsolatedSpikeIsCapped) {
  std::uint64_t typical = 0;
  for (int i = 0; i < 50; ++i) {
    (void)cap_timed_sample(100, typical);
  }
  EXPECT_EQ(cap_timed_sample(1'000'000, typical), 6'400u);  // 64 times the usual 100

  // And it does not wreck the average: ordinary samples are still ordinary.
  EXPECT_EQ(cap_timed_sample(100, typical), 100u);
  EXPECT_LT(cap_timed_sample(1'000'000, typical), 40'000u);
}

// If the work really has become a thousand times heavier, the cap must not
// hold it down for long.
TEST(TimedSample, WorkThatReallyIsExpensiveIsSoonCountedInFull) {
  std::uint64_t typical = 0;
  for (int i = 0; i < 50; ++i) {
    (void)cap_timed_sample(100, typical);
  }

  int samples_until_counted_in_full = 0;
  while (cap_timed_sample(100'000, typical) != 100'000u) {
    ASSERT_LT(++samples_until_counted_in_full, 20);
  }
  EXPECT_LE(samples_until_counted_in_full, 5);
}

// Some clocks tick so slowly that most commands measure as nothing at all,
// and only the occasional one as a single tick.
TEST(TimedSample, StillWorksWhenMostSamplesAreZeroOrOneTick) {
  std::uint64_t typical = 0;
  std::uint64_t counted = 0;
  for (int i = 0; i < 400; ++i) {
    counted += cap_timed_sample(i % 4 == 0 ? 1 : 0, typical);
  }
  EXPECT_EQ(counted, 100u);                          // nothing lost
  EXPECT_LE(cap_timed_sample(50'000, typical), 64u);  // and a spike is still caught
}

// --- One engine --------------------------------------------------------------------

// Tradable prices are 100..199.
constexpr lob::BookConfig kBook{
    .symbol = "TEST", .min_price = 100, .num_levels = 100, .max_orders = 256};

// Sends commands in small batches and has the engine handle them, discarding
// the events.
template <typename Engine, typename Send>
void run(Engine& engine, int commands, Send&& send) {
  for (int i = 0; i < commands; ++i) {
    send(i);
    if (i % 32 == 31) {
      engine.process_pending();
      engine.poll([](const Event&) {});
    }
  }
  engine.process_pending();
  engine.poll([](const Event&) {});
}

TEST(TimeSpent, IsZeroWhenTimingIsOff) {
  MatchingEngine engine({.books = {kBook}, .time_one_in = 0});
  run(engine, 500, [&](int) { ASSERT_TRUE(engine.submit(0, 0, Side::Buy, 150, 1)); });

  EXPECT_EQ(engine.commands_handled(0), 500u);
  EXPECT_EQ(engine.time_spent(0), 0u);
}

TEST(TimeSpent, GrowsWithTheWorkDoneAndOnlyForTheSymbolThatDidIt) {
  MatchingEngine engine({.books = {kBook, kBook}, .time_one_in = 1});
  EXPECT_EQ(engine.time_spent(0), 0u);

  run(engine, 2'000, [&](int i) {
    ASSERT_TRUE(engine.submit(0, 0, i % 2 == 0 ? Side::Sell : Side::Buy, 150, 10));
  });
  const std::uint64_t after_first_batch = engine.time_spent(0);
  run(engine, 2'000, [&](int i) {
    ASSERT_TRUE(engine.submit(0, 0, i % 2 == 0 ? Side::Sell : Side::Buy, 150, 10));
  });

  EXPECT_GT(after_first_batch, 0u);
  EXPECT_GT(engine.time_spent(0), after_first_batch);
  EXPECT_EQ(engine.time_spent(1), 0u);  // symbol 1 was sent nothing
}

// Fills a book with 90 resting sell orders at 90 different prices. A
// fill-or-kill buy for more than they add up to must then walk every one of
// those price levels before it can be turned away: far more work than the same
// order into an empty book, though it counts as one command either way.
template <typename Engine>
void make_fill_or_kill_expensive(Engine& engine, SymbolId symbol) {
  run(engine, 90, [&](int i) {
    (void)engine.submit(0, symbol, Side::Sell, static_cast<Price>(105 + i), 1);
  });
}

template <typename Engine>
void send_fill_or_kill_buys(Engine& engine, SymbolId symbol, int commands) {
  run(engine, commands, [&](int) {
    (void)engine.submit(0, symbol, Side::Buy, 199, 1'000'000, OrderType::FOK);
  });
}

std::uint64_t middle_of(std::vector<std::uint64_t> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

// Two symbols are sent exactly the same number of commands. Counting commands
// cannot tell them apart; timing them can.
TEST(TimeSpent, TellsExpensiveCommandsFromCheapOnes) {
  std::vector<std::uint64_t> ratios_x100;
  for (int trial = 0; trial < 5; ++trial) {
    MatchingEngine engine({.books = {kBook, kBook}, .time_one_in = 1});
    make_fill_or_kill_expensive(engine, 0);
    send_fill_or_kill_buys(engine, 0, 1'910);  // 2,000 commands in all for symbol 0
    send_fill_or_kill_buys(engine, 1, 2'000);  // 2,000 for symbol 1, into an empty book

    ASSERT_EQ(engine.commands_handled(0), engine.commands_handled(1));
    ASSERT_GT(engine.time_spent(1), 0u);
    ratios_x100.push_back(engine.time_spent(0) * 100 / engine.time_spent(1));
  }
  EXPECT_GT(middle_of(ratios_x100), 300u) << "the expensive symbol should take over 3x as long";
}

// Timing one command in sixteen and scaling up should land in the same region
// as timing every command.
TEST(TimeSpent, ASampleGivesMuchTheSameAnswerAsTimingEverything) {
  std::vector<std::uint64_t> ratios_x100;
  for (int trial = 0; trial < 5; ++trial) {
    std::uint64_t estimates[2] = {0, 0};
    const std::uint32_t one_in[2] = {1, 16};
    for (int which = 0; which < 2; ++which) {
      MatchingEngine engine({.books = {kBook}, .time_one_in = one_in[which]});
      make_fill_or_kill_expensive(engine, 0);
      send_fill_or_kill_buys(engine, 0, 8'000);
      estimates[which] = engine.time_spent(0);
    }
    ASSERT_GT(estimates[0], 0u);
    ratios_x100.push_back(estimates[1] * 100 / estimates[0]);
  }
  const std::uint64_t ratio = middle_of(ratios_x100);
  EXPECT_GT(ratio, 33u);   // within a factor of three, either way
  EXPECT_LT(ratio, 300u);
}

// Commands for two symbols arrive strictly turn and turn about, and one
// command in two is timed. If every second command were timed, it would always
// be the same symbol's, and the other would look as if it cost nothing. The
// gap between timed commands is random for exactly this reason.
TEST(TimeSpent, ARegularPatternInTheOrdersDoesNotFoolTheSampling) {
  std::vector<std::uint64_t> ratios_x100;
  for (int trial = 0; trial < 5; ++trial) {
    MatchingEngine engine({.books = {kBook, kBook}, .time_one_in = 2});
    make_fill_or_kill_expensive(engine, 0);
    make_fill_or_kill_expensive(engine, 1);
    run(engine, 8'000, [&](int i) {
      (void)engine.submit(0, static_cast<SymbolId>(i % 2), Side::Buy, 199, 1'000'000,
                          OrderType::FOK);
    });

    ASSERT_GT(engine.time_spent(0), 0u);
    ASSERT_GT(engine.time_spent(1), 0u) << "one symbol was never timed";
    ratios_x100.push_back(engine.time_spent(0) * 100 / engine.time_spent(1));
  }
  const std::uint64_t ratio = middle_of(ratios_x100);  // the same work, so near 100
  EXPECT_GT(ratio, 50u);
  EXPECT_LT(ratio, 200u);
}

// --- Shards balanced by time ---------------------------------------------------------

// Six symbols on two shards. Dealt in turn, symbols 0, 2 and 4 are on shard 0
// and symbols 1, 3 and 5 are on shard 1.
std::vector<lob::BookConfig> six_books() { return std::vector<lob::BookConfig>(6, kBook); }

void settle(ShardedEngine& engine) {
  int idle_rounds = 0;
  while (idle_rounds < 2) {
    const std::size_t handled = engine.process_pending();
    const std::size_t polled = engine.poll([](const Event&) {});
    idle_rounds = handled == 0 && polled == 0 ? idle_rounds + 1 : 0;
  }
}

// Every symbol is sent 1,000 commands. For symbols 0 and 2, which share shard
// 0, they are expensive; for the other four they are cheap.
void send_equal_counts_of_unequal_work(ShardedEngine& engine) {
  for (const SymbolId expensive : {SymbolId{0}, SymbolId{2}}) {
    make_fill_or_kill_expensive(engine, expensive);
    send_fill_or_kill_buys(engine, expensive, 910);
  }
  for (const SymbolId cheap : {SymbolId{1}, SymbolId{3}, SymbolId{4}, SymbolId{5}}) {
    send_fill_or_kill_buys(engine, cheap, 1'000);
  }
  settle(engine);
}

TEST(RebalanceByTime, TimeSpentAddsUpAcrossTheShardsASymbolHasRunOn) {
  ShardedEngine engine({.books = six_books(), .shards = 2, .time_one_in = 1});
  send_fill_or_kill_buys(engine, 0, 500);
  const std::uint64_t on_first_shard = engine.time_spent(0);
  ASSERT_GT(on_first_shard, 0u);

  ASSERT_EQ(engine.move_symbol(0, 1), SendStatus::Sent);
  settle(engine);
  send_fill_or_kill_buys(engine, 0, 500);

  EXPECT_GT(engine.time_spent(0), on_first_shard);
  EXPECT_EQ(engine.measured_loads(LoadMeasure::Time)[0], engine.time_spent(0));
  EXPECT_EQ(engine.measured_loads(LoadMeasure::Commands)[0], 1'000u);
}

// With the same number of commands for every symbol, the two shards look
// identical by count, and nothing is moved.
TEST(RebalanceByTime, CountingCommandsSeesNothingWrong) {
  ShardedEngine engine({.books = six_books(),
                        .shards = 2,
                        .rebalance = {.min_sample = 100, .measure = LoadMeasure::Commands},
                        .time_one_in = 1});
  send_equal_counts_of_unequal_work(engine);

  EXPECT_FALSE(engine.rebalance().has_value());
}

// Measured by time, shard 0 is doing most of the work, and one of the two
// expensive symbols is moved off it. The decision is checked over five
// independent runs: it must come out right in at least four.
TEST(RebalanceByTime, MeasuringTimeMovesAnExpensiveSymbol) {
  int right = 0;
  for (int trial = 0; trial < 5; ++trial) {
    ShardedEngine engine({.books = six_books(),
                          .shards = 2,
                          .rebalance = {.min_sample = 100, .measure = LoadMeasure::Time},
                          .time_one_in = 1});
    ASSERT_EQ(engine.shard_of(0), engine.shard_of(2));
    send_equal_counts_of_unequal_work(engine);

    const std::optional<SymbolId> moved = engine.rebalance();
    settle(engine);

    if (moved.has_value() && (*moved == 0 || *moved == 2) &&
        engine.shard_of(0) != engine.shard_of(2)) {
      ++right;
    }
  }
  EXPECT_GE(right, 4);
}

// With timing switched off there is nothing to balance by time, and no move.
TEST(RebalanceByTime, DoesNothingIfTimingIsOff) {
  ShardedEngine engine({.books = six_books(),
                        .shards = 2,
                        .rebalance = {.min_sample = 100, .measure = LoadMeasure::Time},
                        .time_one_in = 0});
  send_equal_counts_of_unequal_work(engine);

  EXPECT_FALSE(engine.rebalance().has_value());
  EXPECT_EQ(engine.time_spent(0), 0u);
}

}  // namespace

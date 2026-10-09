// Several engine threads, each with its own share of the symbols.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lob/sharded_engine.hpp"

#if defined(__linux__)
#include <sched.h>
#endif

namespace {

using lob::Command;
using lob::CommandType;
using lob::Event;
using lob::EventType;
using lob::IdleStrategy;
using lob::kInvalidOrderId;
using lob::MatchingEngine;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Quantity;
using lob::SendStatus;
using lob::ShardedEngine;
using lob::Side;
using lob::SymbolId;

// Five symbols. With two shards, symbols 0, 2 and 4 share shard 0, and
// symbols 1 and 3 share shard 1. Tradable prices are 100..199.
std::vector<lob::BookConfig> five_books() {
  std::vector<lob::BookConfig> books;
  for (const char* name : {"AAA", "BBB", "CCC", "DDD", "EEE"}) {
    books.push_back({.symbol = name, .min_price = 100, .num_levels = 100, .max_orders = 64});
  }
  return books;
}

bool answers_a_command(const Event& event) { return event.type != EventType::Trade; }

// Polls every shard until `target` commands have been answered. Returns false
// instead of hanging if the engine stops responding.
bool poll_until_answered(ShardedEngine& engine, std::vector<Event>& events, std::size_t target) {
  std::size_t answered = 0;
  for (const Event& event : events) {
    answered += answers_a_command(event) ? 1u : 0u;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (answered < target) {
    const std::size_t polled = engine.poll([&](const Event& event) {
      events.push_back(event);
      if (answers_a_command(event)) {
        ++answered;
      }
    });
    if (polled == 0) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::yield();
    }
  }
  return true;
}

// --- Layout -------------------------------------------------------------------

TEST(ShardedLayout, SymbolsAreDealtToTheShardsInTurn) {
  ShardedEngine engine({.books = five_books(), .shards = 2});

  EXPECT_EQ(engine.shard_count(), 2u);
  EXPECT_EQ(engine.symbol_count(), 5u);
  EXPECT_EQ(engine.shard_of(0), 0u);
  EXPECT_EQ(engine.shard_of(1), 1u);
  EXPECT_EQ(engine.shard_of(2), 0u);
  EXPECT_EQ(engine.shard_of(3), 1u);
  EXPECT_EQ(engine.shard_of(4), 0u);
}

TEST(ShardedLayout, SymbolIdsAreTheSameWhateverTheNumberOfShards) {
  for (const std::size_t shards : {std::size_t{1}, std::size_t{2}, std::size_t{5}}) {
    ShardedEngine engine({.books = five_books(), .shards = shards});
    EXPECT_EQ(engine.symbol_id("AAA"), SymbolId{0});
    EXPECT_EQ(engine.symbol_id("DDD"), SymbolId{3});
    EXPECT_EQ(engine.symbol_id("EEE"), SymbolId{4});
    EXPECT_FALSE(engine.symbol_id("ZZZ").has_value());
    EXPECT_EQ(engine.book(3).symbol(), "DDD");
  }
}

TEST(ShardedLayout, NeverMoreShardsThanSymbolsAndNeverFewerThanOne) {
  EXPECT_EQ(ShardedEngine({.books = five_books(), .shards = 64}).shard_count(), 5u);
  EXPECT_EQ(ShardedEngine({.books = five_books(), .shards = 0}).shard_count(), 1u);
}

// --- Run by hand: routing -------------------------------------------------------

class ShardedByHand : public ::testing::Test {
 protected:
  // Runs every shard on this thread and returns all the events that produced.
  std::vector<Event> run() {
    engine.process_pending();
    std::vector<Event> events;
    engine.poll([&](const Event& event) { events.push_back(event); });
    return events;
  }

  ShardedEngine engine{{.books = five_books(), .shards = 2}};
};

TEST_F(ShardedByHand, AnOrderReachesItsOwnBookOnWhicheverShardThatIs) {
  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    ASSERT_EQ(engine.submit(symbol, symbol, Side::Buy, 150, Quantity{10} + symbol),
              SendStatus::Sent);
  }

  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 5u);
  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    EXPECT_EQ(engine.book(symbol).size(), 1u);
    EXPECT_EQ(engine.book(symbol).quantity_at(Side::Buy, 150), 10u + symbol);
  }
}

// Inside a shard, symbols are renumbered from zero. Callers must never see that.
TEST_F(ShardedByHand, EventsCarryTheGroupsSymbolIds) {
  ASSERT_EQ(engine.submit(1, 3, Side::Sell, 150, 10), SendStatus::Sent);
  ASSERT_EQ(engine.submit(2, 3, Side::Buy, 150, 4), SendStatus::Sent);
  ASSERT_EQ(engine.submit(3, 4, Side::Buy, 150, 4), SendStatus::Sent);

  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 4u);
  std::size_t for_symbol_3 = 0;
  std::size_t for_symbol_4 = 0;
  for (const Event& event : events) {
    for_symbol_3 += event.symbol == 3 ? 1u : 0u;
    for_symbol_4 += event.symbol == 4 ? 1u : 0u;
  }
  EXPECT_EQ(for_symbol_3, 3u);  // Accepted, Trade, Accepted
  EXPECT_EQ(for_symbol_4, 1u);
}

TEST_F(ShardedByHand, PollingOneShardReturnsOnlyThatShardsEvents) {
  ASSERT_EQ(engine.submit(1, 0, Side::Buy, 150, 10), SendStatus::Sent);  // shard 0
  ASSERT_EQ(engine.submit(2, 1, Side::Buy, 150, 10), SendStatus::Sent);  // shard 1
  ASSERT_EQ(engine.submit(3, 2, Side::Buy, 150, 10), SendStatus::Sent);  // shard 0
  engine.process_pending();

  std::vector<SymbolId> from_shard_1;
  EXPECT_EQ(engine.poll(1, [&](const Event& event) { from_shard_1.push_back(event.symbol); }), 1u);
  EXPECT_EQ(from_shard_1, (std::vector<SymbolId>{1}));

  std::vector<SymbolId> from_shard_0;
  EXPECT_EQ(engine.poll(0, [&](const Event& event) { from_shard_0.push_back(event.symbol); }), 2u);
  EXPECT_EQ(from_shard_0, (std::vector<SymbolId>{0, 2}));
}

// Symbols 0 and 2 share a shard; 0 and 1 do not. Neither pair may interact.
TEST_F(ShardedByHand, SymbolsDoNotTradeWithEachOtherOnOneShardOrAcrossShards) {
  ASSERT_EQ(engine.submit(1, 0, Side::Sell, 150, 10), SendStatus::Sent);
  ASSERT_EQ(engine.submit(2, 2, Side::Buy, 150, 10), SendStatus::Sent);  // same shard as 0
  ASSERT_EQ(engine.submit(3, 1, Side::Buy, 150, 10), SendStatus::Sent);  // other shard

  for (const Event& event : run()) {
    EXPECT_EQ(event.type, EventType::Accepted);
  }
  EXPECT_EQ(engine.book(0).size(), 1u);
  EXPECT_EQ(engine.book(1).size(), 1u);
  EXPECT_EQ(engine.book(2).size(), 1u);
}

TEST_F(ShardedByHand, CancelIsRoutedBySymbol) {
  ASSERT_EQ(engine.submit(1, 3, Side::Buy, 150, 10), SendStatus::Sent);
  const OrderId id = run().at(0).order_id;

  ASSERT_EQ(engine.cancel(2, 0, id), SendStatus::Sent);  // right ID, wrong symbol
  ASSERT_EQ(engine.cancel(3, 3, id), SendStatus::Sent);
  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 2u);
  for (const Event& event : events) {
    if (event.symbol == 0) {
      EXPECT_EQ(event.type, EventType::CancelRejected);
    } else {
      EXPECT_EQ(event.symbol, 3);
      EXPECT_EQ(event.type, EventType::Cancelled);
    }
  }
  EXPECT_TRUE(engine.book(3).empty());
}

TEST_F(ShardedByHand, UnknownSymbolIsRefusedAtTheDoor) {
  EXPECT_EQ(engine.submit(1, 5, Side::Buy, 150, 10), SendStatus::UnknownSymbol);
  EXPECT_EQ(engine.submit(2, 65'535, Side::Buy, 150, 10), SendStatus::UnknownSymbol);
  EXPECT_EQ(engine.cancel(3, 5, 1), SendStatus::UnknownSymbol);

  EXPECT_EQ(engine.process_pending(), 0u);
  EXPECT_TRUE(run().empty());
}

TEST(ShardedRings, AFullShardDoesNotBlockTheOthers) {
  ShardedEngine engine({.books = five_books(), .shards = 2, .command_capacity = 2});
  EXPECT_EQ(engine.submit(1, 0, Side::Buy, 150, 1), SendStatus::Sent);
  EXPECT_EQ(engine.submit(2, 2, Side::Buy, 150, 1), SendStatus::Sent);
  EXPECT_EQ(engine.submit(3, 4, Side::Buy, 150, 1), SendStatus::RingFull);  // shard 0 is full
  EXPECT_EQ(engine.cancel(4, 0, 1), SendStatus::RingFull);

  EXPECT_EQ(engine.submit(5, 1, Side::Buy, 150, 1), SendStatus::Sent);  // shard 1 has room

  EXPECT_EQ(engine.process_pending(), 3u);
  EXPECT_EQ(engine.commands_processed(), 3u);
  EXPECT_EQ(engine.submit(6, 4, Side::Buy, 150, 1), SendStatus::Sent);
}

// --- Shards on their own threads --------------------------------------------------

TEST(ShardedThreads, EveryShardAnswersOnItsOwnThread) {
  ShardedEngine engine({.books = five_books(), .shards = 3, .idle = IdleStrategy::Yield});
  engine.start();

  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    ASSERT_EQ(engine.submit(symbol, symbol, Side::Sell, 150, 10), SendStatus::Sent);
    ASSERT_EQ(engine.submit(100u + symbol, symbol, Side::Buy, 150, 10), SendStatus::Sent);
  }
  std::vector<Event> events;
  ASSERT_TRUE(poll_until_answered(engine, events, 10));
  engine.stop();

  std::size_t trades = 0;
  for (const Event& event : events) {
    trades += event.type == EventType::Trade ? 1u : 0u;
  }
  EXPECT_EQ(trades, 5u);
  EXPECT_EQ(engine.commands_processed(), 10u);
  EXPECT_EQ(engine.events_dropped(), 0u);
  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    EXPECT_TRUE(engine.book(symbol).empty());
  }
}

TEST(ShardedThreads, StopHandlesEverythingQueuedOnEveryShard) {
  ShardedEngine engine({.books = five_books(), .shards = 5, .idle = IdleStrategy::Yield});
  for (std::uint64_t tag = 0; tag < 100; ++tag) {
    ASSERT_EQ(engine.submit(tag, static_cast<SymbolId>(tag % 5), Side::Buy, 150, 1),
              SendStatus::Sent);
  }

  engine.start();
  engine.stop();

  std::size_t answered = 0;
  engine.poll([&](const Event& event) { answered += answers_a_command(event) ? 1u : 0u; });
  EXPECT_EQ(answered, 100u);
  EXPECT_EQ(engine.commands_processed(), 100u);
  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    EXPECT_EQ(engine.book(symbol).size(), 20u);
  }
}

TEST(ShardedThreads, PinsEachShardWhereThePlatformAllowsIt) {
#if defined(__linux__)
  const int cpu = sched_getcpu();  // certainly usable: this thread is on it now
#else
  const int cpu = 0;
#endif
  ShardedEngine engine({.books = five_books(),
                        .shards = 2,
                        .idle = IdleStrategy::Yield,
                        .pin_to_cpus = {cpu, cpu}});
  engine.start();
  EXPECT_EQ(engine.pinned(0), lob::kCanPinThreads);
  EXPECT_EQ(engine.pinned(1), lob::kCanPinThreads);
  engine.stop();

  ShardedEngine unpinned({.books = five_books(), .shards = 2, .idle = IdleStrategy::Yield});
  unpinned.start();
  EXPECT_FALSE(unpinned.pinned(0));
  EXPECT_FALSE(unpinned.pinned(1));
}

// Each shard may have a feeder thread of its own. Three threads submit at the
// same time, one per shard, while this thread reads every shard's events.
TEST(ShardedThreads, EachShardCanBeFedByItsOwnThread) {
  constexpr std::size_t kShards = 3;
  constexpr std::uint64_t kPairsPerFeeder = 20'000;
  std::vector<lob::BookConfig> books;
  for (const char* name : {"AAA", "BBB", "CCC", "DDD", "EEE", "FFF"}) {
    books.push_back({.symbol = name, .min_price = 100, .num_levels = 100, .max_orders = 64});
  }
  ShardedEngine engine({.books = books,
                        .shards = kShards,
                        .command_capacity = 64,
                        .event_capacity = 64,
                        .idle = IdleStrategy::Yield});
  engine.start();

  // Feeder for shard s uses symbols s and s + 3, which both live on shard s.
  std::vector<std::thread> feeders;
  for (std::size_t shard = 0; shard < kShards; ++shard) {
    feeders.emplace_back([&engine, shard] {
      for (std::uint64_t pair = 0; pair < kPairsPerFeeder; ++pair) {
        const auto symbol = static_cast<SymbolId>(shard + (pair % 2 == 0 ? 0 : kShards));
        for (const Side side : {Side::Sell, Side::Buy}) {
          while (engine.submit(pair, symbol, side, 150, 10) != SendStatus::Sent) {
            std::this_thread::yield();
          }
        }
      }
    });
  }

  const std::size_t expected_answers = kShards * kPairsPerFeeder * 2;
  std::vector<std::uint64_t> trades_for(books.size(), 0);
  std::vector<std::uint64_t> answers_for(books.size(), 0);
  std::size_t answered = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  while (answered < expected_answers && std::chrono::steady_clock::now() < deadline) {
    const std::size_t polled = engine.poll([&](const Event& event) {
      if (event.type == EventType::Trade) {
        ++trades_for[event.symbol];
      } else {
        ++answers_for[event.symbol];
        ++answered;
      }
    });
    if (polled == 0) {
      std::this_thread::yield();
    }
  }
  for (std::thread& feeder : feeders) {
    feeder.join();
  }
  engine.stop();

  ASSERT_EQ(answered, expected_answers);
  for (std::size_t symbol = 0; symbol < books.size(); ++symbol) {
    EXPECT_EQ(trades_for[symbol], kPairsPerFeeder / 2) << "symbol " << symbol;
    EXPECT_EQ(answers_for[symbol], kPairsPerFeeder) << "symbol " << symbol;
    EXPECT_TRUE(engine.book(static_cast<SymbolId>(symbol)).empty()) << "symbol " << symbol;
  }
  EXPECT_EQ(engine.commands_processed(), expected_answers);
  EXPECT_EQ(engine.events_dropped(), 0u);
}

// The decisive check. Each symbol's commands are first run alone through a
// private single-book engine, which gives the events that symbol must produce.
// Then all the commands are interleaved through three shard threads with
// 4-slot rings, so that every shard is constantly blocked on a full ring.
// For each symbol, the events that come back must match exactly, however the
// symbols were assigned to shards.
void expect_every_symbol_to_behave_as_if_alone(const std::vector<std::uint64_t>& loads) {
  const std::vector<lob::BookConfig> books{
      {.symbol = "AAA", .min_price = 100, .num_levels = 60, .max_orders = 128},
      {.symbol = "BBB", .min_price = 100, .num_levels = 60, .max_orders = 16},
      {.symbol = "CCC", .min_price = 130, .num_levels = 60, .max_orders = 128},
      {.symbol = "DDD", .min_price = 100, .num_levels = 30, .max_orders = 128},
      {.symbol = "EEE", .min_price = 110, .num_levels = 60, .max_orders = 64},
      {.symbol = "FFF", .min_price = 100, .num_levels = 60, .max_orders = 128},
      {.symbol = "GGG", .min_price = 100, .num_levels = 90, .max_orders = 32},
  };
  constexpr int kCommands = 12'000;

  // 1. The reference: one private engine per symbol, run by hand.
  std::vector<Command> script;
  std::vector<std::vector<Event>> expected(books.size());
  {
    std::vector<std::unique_ptr<MatchingEngine>> alone;
    for (const lob::BookConfig& book : books) {
      alone.push_back(std::make_unique<MatchingEngine>(lob::EngineConfig{.books = {book}}));
    }
    std::vector<std::vector<OrderId>> accepted(books.size());
    std::mt19937_64 rng(2026);

    for (int i = 0; i < kCommands; ++i) {
      const auto symbol = static_cast<SymbolId>(rng() % books.size());
      Command command{};
      command.client_tag = static_cast<std::uint64_t>(i);
      command.symbol = symbol;
      if (rng() % 100 < 65 || accepted[symbol].empty()) {
        command.type = CommandType::Submit;
        command.side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
        command.price = 99 + static_cast<Price>(rng() % 95);
        command.quantity = static_cast<Quantity>(rng() % 31);
        command.order_type = static_cast<OrderType>(rng() % 4);
        ASSERT_TRUE(alone[symbol]->submit(command.client_tag, 0, command.side, command.price,
                                          command.quantity, command.order_type));
      } else {
        command.type = CommandType::Cancel;
        command.order_id =
            accepted[symbol][static_cast<std::size_t>(rng() % accepted[symbol].size())];
        ASSERT_TRUE(alone[symbol]->cancel(command.client_tag, 0, command.order_id));
      }
      script.push_back(command);

      alone[symbol]->process_pending();
      alone[symbol]->poll([&](const Event& event) {
        expected[symbol].push_back(event);
        if (event.type == EventType::Accepted) {
          accepted[symbol].push_back(event.order_id);
        }
      });
    }
  }

  // 2. The same commands, interleaved, through three shard threads.
  ShardedEngine engine({.books = books,
                        .shards = 3,
                        .loads = loads,
                        .command_capacity = 4,
                        .event_capacity = 4,
                        .idle = IdleStrategy::Yield});
  engine.start();
  std::vector<std::vector<Event>> actual(books.size());
  std::size_t answered = 0;
  const auto poll_once = [&] {
    engine.poll([&](const Event& event) {
      actual[event.symbol].push_back(event);
      if (answers_a_command(event)) {
        ++answered;
      }
    });
  };

  for (const Command& command : script) {
    const auto send = [&] {
      return command.type == CommandType::Submit
                 ? engine.submit(command.client_tag, command.symbol, command.side,
                                 command.price, command.quantity, command.order_type)
                 : engine.cancel(command.client_tag, command.symbol, command.order_id);
    };
    while (send() != SendStatus::Sent) {
      poll_once();  // keep reading, or a shard stalls on a full event ring
      std::this_thread::yield();
    }
    poll_once();
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  while (answered < script.size() && std::chrono::steady_clock::now() < deadline) {
    poll_once();
    std::this_thread::yield();
  }
  engine.stop();
  ASSERT_EQ(answered, script.size());

  // 3. Symbol by symbol, the two must agree.
  for (std::size_t symbol = 0; symbol < books.size(); ++symbol) {
    ASSERT_GT(expected[symbol].size(), 1'000u) << "symbol " << symbol << " was barely exercised";
    ASSERT_EQ(actual[symbol].size(), expected[symbol].size()) << "symbol " << symbol;
    for (std::size_t i = 0; i < expected[symbol].size(); ++i) {
      const Event& got = actual[symbol][i];
      const Event& want = expected[symbol][i];
      const bool same = got.client_tag == want.client_tag && got.order_id == want.order_id &&
                        got.maker_id == want.maker_id && got.price == want.price &&
                        got.quantity == want.quantity && got.resting == want.resting &&
                        got.side == want.side && got.type == want.type;
      ASSERT_TRUE(same) << "symbol " << symbol << ", event " << i << " differs";
      ASSERT_EQ(got.symbol, symbol);
    }
  }
  EXPECT_EQ(engine.events_dropped(), 0u);
  EXPECT_EQ(engine.commands_processed(), script.size());
}

TEST(ShardedThreads, EverySymbolBehavesAsIfItHadAnEngineToItself) {
  expect_every_symbol_to_behave_as_if_alone({});
}

// The same check with the symbols assigned by load, which puts them on
// different shards from the run above.
TEST(ShardedThreads, EverySymbolBehavesAsIfAloneWhenAssignedByLoad) {
  expect_every_symbol_to_behave_as_if_alone({900, 10, 10, 800, 10, 700, 10});
}

// --- Assigning symbols to shards by load -------------------------------------------

// Symbols 0 and 2 are the busy ones. Dealt in turn they would share shard 0.
TEST(ShardedByLoad, BusySymbolsArePutOnDifferentShards) {
  ShardedEngine in_turn({.books = five_books(), .shards = 2});
  EXPECT_EQ(in_turn.shard_of(0), in_turn.shard_of(2));
  EXPECT_EQ(in_turn.shard_load(0), 3u);  // with no loads given, a shard's load is its symbol count
  EXPECT_EQ(in_turn.shard_load(1), 2u);

  ShardedEngine by_load({.books = five_books(), .shards = 2, .loads = {500, 10, 400, 10, 10}});
  EXPECT_NE(by_load.shard_of(0), by_load.shard_of(2));
  EXPECT_EQ(by_load.shard_load(by_load.shard_of(0)), 500u);
  EXPECT_EQ(by_load.shard_load(by_load.shard_of(2)), 430u);
}

// Assignment changes where a book lives, never what a symbol ID means.
TEST(ShardedByLoad, SymbolIdsAndRoutingAreUnaffected) {
  ShardedEngine engine({.books = five_books(), .shards = 3, .loads = {1, 900, 1, 800, 700}});
  EXPECT_EQ(engine.symbol_id("AAA"), SymbolId{0});
  EXPECT_EQ(engine.symbol_id("EEE"), SymbolId{4});

  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    ASSERT_EQ(engine.submit(symbol, symbol, Side::Sell, 150, Quantity{10} + symbol),
              SendStatus::Sent);
  }
  engine.process_pending();
  std::vector<SymbolId> answered;
  engine.poll([&](const Event& event) {
    EXPECT_EQ(event.type, EventType::Accepted);
    EXPECT_EQ(event.client_tag, event.symbol);
    answered.push_back(event.symbol);
  });

  EXPECT_EQ(answered.size(), 5u);
  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    EXPECT_EQ(engine.book(symbol).symbol(), five_books()[symbol].symbol);
    EXPECT_EQ(engine.book(symbol).quantity_at(Side::Sell, 150), 10u + symbol);
  }
}

TEST(ShardedByLoad, CountsTheCommandsEachSymbolReceives) {
  ShardedEngine engine({.books = five_books(), .shards = 2});
  for (int i = 0; i < 7; ++i) {
    ASSERT_EQ(engine.submit(0, 3, Side::Buy, 150, 1), SendStatus::Sent);
  }
  ASSERT_EQ(engine.submit(0, 1, Side::Buy, 150, 1), SendStatus::Sent);
  ASSERT_EQ(engine.submit(0, 1, Side::Buy, 99, 1), SendStatus::Sent);   // rejected, still work
  ASSERT_EQ(engine.cancel(0, 1, 12'345), SendStatus::Sent);            // refused, still work
  EXPECT_EQ(engine.submit(0, 9, Side::Buy, 150, 1), SendStatus::UnknownSymbol);  // never queued
  engine.process_pending();

  EXPECT_EQ(engine.commands_handled(3), 7u);
  EXPECT_EQ(engine.commands_handled(1), 3u);
  EXPECT_EQ(engine.commands_handled(0), 0u);
  EXPECT_EQ(engine.measured_loads(), (std::vector<std::uint64_t>{0, 3, 0, 7, 0}));
}

// The intended use: run, measure what each symbol really received, and use
// that to lay out the next engine.
TEST(ShardedByLoad, MeasuredLoadsFromOneRunBalanceTheNext) {
  std::vector<lob::BookConfig> books;
  for (const char* name : {"A", "B", "C", "D", "E", "F", "G", "H"}) {
    books.push_back({.symbol = name, .min_price = 100, .num_levels = 100, .max_orders = 512});
  }
  // Symbols 0, 2, 4 and 6 are busy. Dealt in turn over two shards, all four
  // land on shard 0.
  const std::vector<int> orders_for{400, 5, 300, 5, 200, 5, 100, 5};

  ShardedEngine first({.books = books, .shards = 2});
  for (SymbolId symbol = 0; symbol < books.size(); ++symbol) {
    for (int i = 0; i < orders_for[symbol]; ++i) {
      ASSERT_EQ(first.submit(0, symbol, Side::Buy, 150, 1), SendStatus::Sent);
    }
  }
  first.process_pending();
  first.poll([](const Event&) {});
  const std::vector<std::uint64_t> measured = first.measured_loads();
  ASSERT_EQ(measured, (std::vector<std::uint64_t>{400, 5, 300, 5, 200, 5, 100, 5}));

  // What each shard of the first engine actually handled.
  std::uint64_t first_shard_0 = 0;
  std::uint64_t first_shard_1 = 0;
  for (SymbolId symbol = 0; symbol < books.size(); ++symbol) {
    (first.shard_of(symbol) == 0 ? first_shard_0 : first_shard_1) += measured[symbol];
  }
  EXPECT_EQ(first_shard_0, 1'000u);
  EXPECT_EQ(first_shard_1, 20u);

  ShardedEngine second({.books = books, .shards = 2, .loads = measured});
  EXPECT_EQ(second.shard_load(0) + second.shard_load(1), 1'020u);
  EXPECT_EQ(std::max(second.shard_load(0), second.shard_load(1)), 510u);
  EXPECT_EQ(std::min(second.shard_load(0), second.shard_load(1)), 510u);
}

}  // namespace

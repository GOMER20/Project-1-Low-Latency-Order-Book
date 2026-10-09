// Moving a symbol from one shard to another while orders keep flowing.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lob/sharded_engine.hpp"

namespace {

using lob::Command;
using lob::CommandType;
using lob::Event;
using lob::EventType;
using lob::IdleStrategy;
using lob::MatchingEngine;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Quantity;
using lob::SendStatus;
using lob::ShardedEngine;
using lob::Side;
using lob::SymbolId;

// Five symbols on two shards: 0, 2 and 4 start on shard 0; 1 and 3 on shard 1.
// Tradable prices are 100..199.
std::vector<lob::BookConfig> five_books() {
  std::vector<lob::BookConfig> books;
  for (const char* name : {"AAA", "BBB", "CCC", "DDD", "EEE"}) {
    books.push_back({.symbol = name, .min_price = 100, .num_levels = 100, .max_orders = 64});
  }
  return books;
}

bool answers_a_command(const Event& event) { return event.type != EventType::Trade; }

bool is_a_public_event(const Event& event) {
  return event.type == EventType::Accepted || event.type == EventType::Rejected ||
         event.type == EventType::Trade || event.type == EventType::Cancelled ||
         event.type == EventType::CancelRejected;
}

// Runs the shards by hand until there is nothing left to do, collecting every
// event. A move needs the engines and the reader to take turns, so this
// alternates between them.
void settle(ShardedEngine& engine, std::vector<Event>& events) {
  int idle_rounds = 0;
  while (idle_rounds < 2) {
    const std::size_t handled = engine.process_pending();
    const std::size_t polled = engine.poll([&](const Event& event) { events.push_back(event); });
    idle_rounds = handled == 0 && polled == 0 ? idle_rounds + 1 : 0;
  }
}

class SymbolMove : public ::testing::Test {
 protected:
  std::vector<Event> settle() {
    std::vector<Event> events;
    ::settle(engine, events);
    return events;
  }

  // Rests one order and returns its ID.
  OrderId rest(SymbolId symbol, Side side, Price price, Quantity quantity) {
    EXPECT_EQ(engine.submit(0, symbol, side, price, quantity), SendStatus::Sent);
    const std::vector<Event> events = settle();
    EXPECT_EQ(events.size(), 1u);
    return events.empty() ? lob::kInvalidOrderId : events[0].order_id;
  }

  ShardedEngine engine{{.books = five_books(), .shards = 2}};
};

// --- What a move does ------------------------------------------------------------

TEST_F(SymbolMove, CommandsGoToTheNewShardStraightAway) {
  ASSERT_EQ(engine.shard_of(0), 0u);
  EXPECT_FALSE(engine.move_in_progress(0));

  ASSERT_EQ(engine.move_symbol(0, 1), SendStatus::Sent);

  EXPECT_EQ(engine.shard_of(0), 1u);
  EXPECT_TRUE(engine.move_in_progress(0));

  (void)settle();
  EXPECT_FALSE(engine.move_in_progress(0));
  EXPECT_EQ(engine.shard_of(0), 1u);
}

// Only the thread running the book changes. Its contents do not.
TEST_F(SymbolMove, RestingOrdersAndTheirIdsSurviveTheMove) {
  const OrderId first = rest(0, Side::Sell, 150, 10);
  const OrderId second = rest(0, Side::Sell, 150, 20);

  ASSERT_EQ(engine.move_symbol(0, 1), SendStatus::Sent);
  ASSERT_EQ(engine.cancel(1, 0, first), SendStatus::Sent);
  ASSERT_EQ(engine.submit(2, 0, Side::Buy, 150, 5), SendStatus::Sent);
  const std::vector<Event> events = settle();

  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0].type, EventType::Cancelled);
  EXPECT_EQ(events[0].order_id, first);
  EXPECT_EQ(events[1].type, EventType::Trade);
  EXPECT_EQ(events[1].maker_id, second);
  EXPECT_EQ(events[1].quantity, 5u);
  EXPECT_EQ(events[2].type, EventType::Accepted);
  EXPECT_EQ(engine.book(0).quantity_at(Side::Sell, 150), 15u);
}

TEST_F(SymbolMove, AfterTheMoveTheNewShardAnswers) {
  ASSERT_EQ(engine.move_symbol(0, 1), SendStatus::Sent);
  (void)settle();

  ASSERT_EQ(engine.submit(7, 0, Side::Buy, 150, 10), SendStatus::Sent);
  engine.process_pending();

  std::vector<Event> from_shard_0;
  std::vector<Event> from_shard_1;
  engine.poll(0, [&](const Event& event) { from_shard_0.push_back(event); });
  engine.poll(1, [&](const Event& event) { from_shard_1.push_back(event); });
  EXPECT_TRUE(from_shard_0.empty());
  ASSERT_EQ(from_shard_1.size(), 1u);
  EXPECT_EQ(from_shard_1[0].client_tag, 7u);
  EXPECT_EQ(from_shard_1[0].symbol, 0);
}

// Symbol 1 moves from shard 1 to shard 0, and poll() reads shard 0 first. If
// the new shard started on the symbol straight away, its answer to the second
// order would be read before the old shard's answer to the first.
TEST_F(SymbolMove, TheSymbolsEventsStayInOrderAcrossTheMove) {
  ASSERT_EQ(engine.submit(1, 1, Side::Buy, 150, 10), SendStatus::Sent);
  ASSERT_EQ(engine.move_symbol(1, 0), SendStatus::Sent);
  ASSERT_EQ(engine.submit(2, 1, Side::Buy, 150, 10), SendStatus::Sent);
  ASSERT_EQ(engine.submit(3, 1, Side::Sell, 150, 15), SendStatus::Sent);

  const std::vector<Event> events = settle();

  std::vector<std::uint64_t> tags;
  for (const Event& event : events) {
    EXPECT_EQ(event.symbol, 1);
    tags.push_back(event.client_tag);
  }
  // Accepted(1), Accepted(2), then the sell: two trades and its Accepted.
  EXPECT_EQ(tags, (std::vector<std::uint64_t>{1, 2, 3, 3, 3}));
  EXPECT_EQ(engine.book(1).quantity_at(Side::Buy, 150), 5u);
}

TEST_F(SymbolMove, TheInternalMarkerIsNeverShownToTheCaller) {
  ASSERT_EQ(engine.submit(1, 0, Side::Buy, 150, 10), SendStatus::Sent);
  ASSERT_EQ(engine.move_symbol(0, 1), SendStatus::Sent);
  ASSERT_EQ(engine.submit(2, 0, Side::Buy, 150, 10), SendStatus::Sent);
  ASSERT_EQ(engine.move_symbol(0, 0), SendStatus::Sent);
  ASSERT_EQ(engine.submit(3, 0, Side::Buy, 150, 10), SendStatus::Sent);

  const std::vector<Event> events = settle();

  ASSERT_EQ(events.size(), 3u);
  for (const Event& event : events) {
    EXPECT_TRUE(is_a_public_event(event));
  }
}

TEST_F(SymbolMove, OtherSymbolsOnBothShardsCarryOn) {
  ASSERT_EQ(engine.move_symbol(0, 1), SendStatus::Sent);
  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    ASSERT_EQ(engine.submit(symbol, symbol, Side::Buy, 150, 10), SendStatus::Sent);
  }

  const std::vector<Event> events = settle();

  EXPECT_EQ(events.size(), 5u);
  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    EXPECT_EQ(engine.book(symbol).size(), 1u) << "symbol " << symbol;
  }
}

TEST_F(SymbolMove, ASymbolCanMoveBackAndForthAnyNumberOfTimes) {
  std::uint64_t tag = 0;
  for (int round = 0; round < 25; ++round) {
    ASSERT_EQ(engine.submit(tag++, 0, Side::Sell, 150, 10), SendStatus::Sent);
    ASSERT_EQ(engine.move_symbol(0, round % 2 == 0 ? 1 : 0), SendStatus::Sent);
    ASSERT_EQ(engine.submit(tag++, 0, Side::Buy, 150, 10), SendStatus::Sent);
  }

  const std::vector<Event> events = settle();

  std::vector<std::uint64_t> answered;
  std::size_t trades = 0;
  for (const Event& event : events) {
    if (answers_a_command(event)) {
      answered.push_back(event.client_tag);
    } else {
      ++trades;
    }
  }
  std::vector<std::uint64_t> in_order(50);
  for (std::size_t i = 0; i < in_order.size(); ++i) {
    in_order[i] = i;
  }
  EXPECT_EQ(answered, in_order);
  EXPECT_EQ(trades, 25u);
  EXPECT_TRUE(engine.book(0).empty());
  EXPECT_FALSE(engine.move_in_progress(0));
}

// A second move may be asked for before the first has finished.
TEST(SymbolMoveChain, MovesOfOneSymbolCanBeQueuedUp) {
  ShardedEngine engine({.books = five_books(), .shards = 3});
  ASSERT_EQ(engine.shard_of(0), 0u);

  ASSERT_EQ(engine.submit(1, 0, Side::Buy, 150, 10), SendStatus::Sent);
  ASSERT_EQ(engine.move_symbol(0, 1), SendStatus::Sent);
  ASSERT_EQ(engine.submit(2, 0, Side::Buy, 150, 10), SendStatus::Sent);
  ASSERT_EQ(engine.move_symbol(0, 2), SendStatus::Sent);
  ASSERT_EQ(engine.submit(3, 0, Side::Buy, 150, 10), SendStatus::Sent);
  ASSERT_EQ(engine.move_symbol(0, 0), SendStatus::Sent);
  ASSERT_EQ(engine.submit(4, 0, Side::Buy, 150, 10), SendStatus::Sent);

  std::vector<Event> events;
  settle(engine, events);

  std::vector<std::uint64_t> tags;
  for (const Event& event : events) {
    tags.push_back(event.client_tag);
  }
  EXPECT_EQ(tags, (std::vector<std::uint64_t>{1, 2, 3, 4}));
  EXPECT_EQ(engine.shard_of(0), 0u);
  EXPECT_FALSE(engine.move_in_progress(0));
  EXPECT_EQ(engine.book(0).quantity_at(Side::Buy, 150), 40u);
}

// --- What a move costs, and what it refuses --------------------------------------

// The new shard waits at the "take up" command until the old shard's events
// have been read. Everything queued behind it on the new shard waits too.
TEST_F(SymbolMove, TheNewShardPausesUntilTheOldShardsEventsAreRead) {
  ASSERT_EQ(engine.move_symbol(0, 1), SendStatus::Sent);
  ASSERT_EQ(engine.submit(1, 3, Side::Buy, 150, 10), SendStatus::Sent);  // symbol 3: shard 1

  engine.process_pending();
  engine.process_pending();
  EXPECT_TRUE(engine.move_in_progress(0));
  EXPECT_EQ(engine.commands_handled(3), 0u);  // still queued behind the move

  engine.poll([](const Event&) {});  // reads the old shard's marker
  engine.process_pending();

  EXPECT_FALSE(engine.move_in_progress(0));
  EXPECT_EQ(engine.commands_handled(3), 1u);
}

TEST_F(SymbolMove, MovingToTheShardItIsAlreadyOnDoesNothing) {
  EXPECT_EQ(engine.move_symbol(0, 0), SendStatus::Sent);

  EXPECT_FALSE(engine.move_in_progress(0));
  EXPECT_EQ(engine.process_pending(), 0u);
  EXPECT_EQ(engine.shard_of(0), 0u);
}

TEST_F(SymbolMove, UnknownSymbolOrShardIsRefused) {
  EXPECT_EQ(engine.move_symbol(5, 0), SendStatus::UnknownSymbol);
  EXPECT_EQ(engine.move_symbol(0, 2), SendStatus::UnknownSymbol);
  EXPECT_EQ(engine.process_pending(), 0u);
  EXPECT_EQ(engine.shard_of(0), 0u);
}

TEST(SymbolMoveRings, NothingChangesIfEitherShardsRingIsFull) {
  ShardedEngine engine({.books = five_books(), .shards = 2, .command_capacity = 2});

  // The destination's ring is full.
  ASSERT_EQ(engine.submit(1, 1, Side::Buy, 150, 1), SendStatus::Sent);
  ASSERT_EQ(engine.submit(2, 3, Side::Buy, 150, 1), SendStatus::Sent);
  EXPECT_EQ(engine.move_symbol(0, 1), SendStatus::RingFull);
  EXPECT_EQ(engine.shard_of(0), 0u);
  EXPECT_FALSE(engine.move_in_progress(0));

  // The source's ring is full.
  std::vector<Event> events;
  settle(engine, events);
  ASSERT_EQ(engine.submit(3, 0, Side::Buy, 150, 1), SendStatus::Sent);
  ASSERT_EQ(engine.submit(4, 2, Side::Buy, 150, 1), SendStatus::Sent);
  EXPECT_EQ(engine.move_symbol(0, 1), SendStatus::RingFull);
  EXPECT_EQ(engine.shard_of(0), 0u);

  // With room in both, the same call goes through.
  settle(engine, events);
  EXPECT_EQ(engine.move_symbol(0, 1), SendStatus::Sent);
  settle(engine, events);
  EXPECT_EQ(engine.shard_of(0), 1u);
  EXPECT_FALSE(engine.move_in_progress(0));
}

TEST(SymbolMoveLoads, LoadsAndCountersFollowTheSymbol) {
  ShardedEngine engine({.books = five_books(), .shards = 2, .loads = {500, 10, 10, 10, 10}});
  const std::size_t from = engine.shard_of(0);
  const std::size_t to = 1 - from;
  const std::uint64_t from_before = engine.shard_load(from);
  const std::uint64_t to_before = engine.shard_load(to);
  std::vector<Event> events;

  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(engine.submit(0, 0, Side::Buy, 150, 1), SendStatus::Sent);
  }
  ASSERT_EQ(engine.move_symbol(0, to), SendStatus::Sent);
  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(engine.submit(0, 0, Side::Buy, 150, 1), SendStatus::Sent);
  }
  settle(engine, events);

  EXPECT_EQ(engine.shard_load(from), from_before - 500);
  EXPECT_EQ(engine.shard_load(to), to_before + 500);
  EXPECT_EQ(engine.commands_handled(0), 7u);  // three on the old shard, four on the new
  EXPECT_EQ(engine.measured_loads()[0], 7u);
}

// --- With the shards on their own threads -----------------------------------------

// Polls until `target` commands have been answered or two minutes pass.
bool poll_until_answered(ShardedEngine& engine, std::vector<Event>& events,
                         std::size_t& answered, std::size_t target) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
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

// One symbol is moved between two running shards 300 times while orders for it
// keep arriving. Its answers must come back in exactly the order they were sent.
TEST(SymbolMoveThreads, ASymbolPingPongsBetweenRunningShardsWithoutLosingOrder) {
  constexpr int kRounds = 300;
  ShardedEngine engine({.books = five_books(),
                        .shards = 2,
                        .command_capacity = 16,
                        .event_capacity = 16,
                        .idle = IdleStrategy::Yield});
  engine.start();
  std::vector<Event> events;
  std::size_t answered = 0;
  const auto keep_reading = [&] {
    engine.poll([&](const Event& event) {
      events.push_back(event);
      if (answers_a_command(event)) {
        ++answered;
      }
    });
  };
  const auto send = [&](std::uint64_t tag, Side side) {
    while (engine.submit(tag, 0, side, 150, 10) != SendStatus::Sent) {
      keep_reading();
      std::this_thread::yield();
    }
  };

  std::uint64_t tag = 0;
  for (int round = 0; round < kRounds; ++round) {
    send(tag++, Side::Sell);
    send(tag++, Side::Buy);
    while (engine.move_symbol(0, round % 2 == 0 ? 1 : 0) != SendStatus::Sent) {
      keep_reading();
      std::this_thread::yield();
    }
    send(tag++, Side::Sell);
    send(tag++, Side::Buy);
    keep_reading();
  }
  ASSERT_TRUE(poll_until_answered(engine, events, answered, tag));
  engine.stop();

  std::uint64_t expected_tag = 0;
  std::size_t trades = 0;
  for (const Event& event : events) {
    ASSERT_TRUE(is_a_public_event(event));
    ASSERT_EQ(event.symbol, 0);
    if (answers_a_command(event)) {
      ASSERT_EQ(event.client_tag, expected_tag) << "answers out of order";
      ++expected_tag;
    } else {
      ++trades;
    }
  }
  EXPECT_EQ(expected_tag, tag);
  EXPECT_EQ(trades, static_cast<std::size_t>(2 * kRounds));
  EXPECT_TRUE(engine.book(0).empty());
  EXPECT_FALSE(engine.move_in_progress(0));
  EXPECT_EQ(engine.events_dropped(), 0u);
}

// The decisive check. Each symbol's commands are first run alone through a
// private single-book engine, giving the events that symbol must produce.
// Then all the commands are interleaved through three shard threads with
// 8-slot rings, and every so often a randomly chosen symbol is moved to a
// randomly chosen shard. For each symbol, the events must match exactly.
TEST(SymbolMoveThreads, EverySymbolBehavesAsIfAloneHoweverOftenItIsMoved) {
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
  constexpr std::size_t kShards = 3;

  // 1. The reference: one private engine per symbol, run by hand.
  std::vector<Command> script;
  std::vector<std::vector<Event>> expected(books.size());
  {
    std::vector<std::unique_ptr<MatchingEngine>> alone;
    for (const lob::BookConfig& book : books) {
      alone.push_back(std::make_unique<MatchingEngine>(lob::EngineConfig{.books = {book}}));
    }
    std::vector<std::vector<OrderId>> accepted(books.size());
    std::mt19937_64 rng(31);

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

  // 2. The same commands through three shard threads, with moves mixed in.
  ShardedEngine engine({.books = books,
                        .shards = kShards,
                        .command_capacity = 8,
                        .event_capacity = 8,
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

  std::mt19937_64 rng(77);
  std::size_t moves_started = 0;
  for (const Command& command : script) {
    if (rng() % 20 == 0) {  // one command in twenty is preceded by a move
      const auto symbol = static_cast<SymbolId>(rng() % books.size());
      const std::size_t to = rng() % kShards;
      if (engine.shard_of(symbol) != to) {
        ++moves_started;
      }
      while (engine.move_symbol(symbol, to) != SendStatus::Sent) {
        poll_once();
        std::this_thread::yield();
      }
    }
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
  ASSERT_GT(moves_started, 300u) << "too few moves to mean anything";

  // 3. Symbol by symbol, the two must agree.
  for (std::size_t symbol = 0; symbol < books.size(); ++symbol) {
    ASSERT_EQ(actual[symbol].size(), expected[symbol].size()) << "symbol " << symbol;
    for (std::size_t i = 0; i < expected[symbol].size(); ++i) {
      const Event& got = actual[symbol][i];
      const Event& want = expected[symbol][i];
      const bool same = got.client_tag == want.client_tag && got.order_id == want.order_id &&
                        got.maker_id == want.maker_id && got.price == want.price &&
                        got.quantity == want.quantity && got.resting == want.resting &&
                        got.side == want.side && got.type == want.type;
      ASSERT_TRUE(same) << "symbol " << symbol << ", event " << i << " differs";
    }
    EXPECT_FALSE(engine.move_in_progress(static_cast<SymbolId>(symbol)));
  }
  EXPECT_EQ(engine.events_dropped(), 0u);
  EXPECT_EQ(engine.commands_processed(), script.size());
}

// Stopping with moves under way and nobody reading events must not hang, and
// must leave every symbol with a shard that is running it.
TEST(SymbolMoveThreads, StoppingInTheMiddleOfMovesLeavesNothingStranded) {
  ShardedEngine engine({.books = five_books(),
                        .shards = 2,
                        .event_capacity = 4,
                        .idle = IdleStrategy::Yield});
  engine.start();
  for (std::uint64_t tag = 0; tag < 30; ++tag) {
    ASSERT_EQ(engine.submit(tag, static_cast<SymbolId>(tag % 5), Side::Buy, 150, 1),
              SendStatus::Sent);
  }
  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    ASSERT_EQ(engine.move_symbol(symbol, 1 - engine.shard_of(symbol)), SendStatus::Sent);
  }
  for (std::uint64_t tag = 30; tag < 60; ++tag) {
    ASSERT_EQ(engine.submit(tag, static_cast<SymbolId>(tag % 5), Side::Buy, 150, 1),
              SendStatus::Sent);
  }

  engine.stop();  // nobody has polled: the 4-slot event rings are long since full

  EXPECT_EQ(engine.commands_processed(), 60u);
  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    EXPECT_FALSE(engine.move_in_progress(symbol)) << "symbol " << symbol;
    EXPECT_EQ(engine.book(symbol).size(), 12u) << "symbol " << symbol;
  }

  // The engine still works afterwards, with every symbol on its new shard.
  engine.poll([](const Event&) {});
  engine.start();
  std::vector<Event> events;
  std::size_t answered = 0;
  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    ASSERT_EQ(engine.submit(100u + symbol, symbol, Side::Sell, 150, 12), SendStatus::Sent);
  }
  ASSERT_TRUE(poll_until_answered(engine, events, answered, 5));
  engine.stop();
  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    EXPECT_TRUE(engine.book(symbol).empty()) << "symbol " << symbol;
  }
}

// Here the event rings have plenty of room, so the old shard's marker is
// published but never read. At shutdown the new shard must not wait for a
// reader that is not coming.
TEST(SymbolMoveThreads, StoppingWithTheMarkerUnreadStillFinishesTheMove) {
  ShardedEngine engine({.books = five_books(), .shards = 2, .idle = IdleStrategy::Yield});
  engine.start();
  ASSERT_EQ(engine.submit(1, 0, Side::Buy, 150, 10), SendStatus::Sent);
  ASSERT_EQ(engine.move_symbol(0, 1), SendStatus::Sent);
  ASSERT_EQ(engine.submit(2, 0, Side::Buy, 150, 10), SendStatus::Sent);

  engine.stop();  // nobody has polled

  EXPECT_FALSE(engine.move_in_progress(0));
  EXPECT_EQ(engine.commands_processed(), 2u);
  EXPECT_EQ(engine.events_dropped(), 0u);
  EXPECT_EQ(engine.book(0).quantity_at(Side::Buy, 150), 20u);

  // Both answers are still there to be read, and the marker is not among them.
  std::vector<Event> events;
  engine.poll([&](const Event& event) { events.push_back(event); });
  ASSERT_EQ(events.size(), 2u);
  for (const Event& event : events) {
    EXPECT_EQ(event.type, EventType::Accepted);
  }
}

// The same, but the engine is simply destroyed.
TEST(SymbolMoveThreads, DestroyingTheEngineInTheMiddleOfMovesDoesNotHang) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    ShardedEngine engine({.books = five_books(),
                          .shards = 2,
                          .event_capacity = 2,
                          .idle = IdleStrategy::Yield});
    engine.start();
    for (std::uint64_t tag = 0; tag < 20; ++tag) {
      ASSERT_EQ(engine.submit(tag, static_cast<SymbolId>(tag % 5), Side::Buy, 150, 1),
                SendStatus::Sent);
    }
    for (SymbolId symbol = 0; symbol < 5; ++symbol) {
      ASSERT_EQ(engine.move_symbol(symbol, 1 - engine.shard_of(symbol)), SendStatus::Sent);
    }
  }
}

}  // namespace

// The public market data feed: best prices, depth and trades.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lob/book_mirror.hpp"
#include "lob/sharded_engine.hpp"

namespace {

using lob::BookMirror;
using lob::Event;
using lob::EventType;
using lob::IdleStrategy;
using lob::MarketData;
using lob::MarketDataType;
using lob::MatchingEngine;
using lob::OrderBook;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Quantity;
using lob::SendStatus;
using lob::ShardedEngine;
using lob::Side;
using lob::SymbolId;

// Tradable prices are 100..199.
constexpr lob::BookConfig kBook{
    .symbol = "TEST", .min_price = 100, .num_levels = 100, .max_orders = 256};

using Types = std::vector<MarketDataType>;

Types types_of(const std::vector<MarketData>& messages) {
  Types types;
  for (const MarketData& message : messages) {
    types.push_back(message.type);
  }
  return types;
}

// Everything the mirror says about depth and best prices must match the book.
void expect_mirror_matches_book(const BookMirror& mirror, const OrderBook& book) {
  for (Price price = 100; price < 200; ++price) {
    for (const Side side : {Side::Buy, Side::Sell}) {
      ASSERT_EQ(mirror.quantity_at(side, price), book.quantity_at(side, price))
          << "price " << price << (side == Side::Buy ? " bid" : " ask");
    }
  }
  ASSERT_EQ(mirror.best_bid(), book.best_bid());
  ASSERT_EQ(mirror.best_ask(), book.best_ask());

  // What the feed announced as the best prices, as against working it out
  // from the levels.
  const std::optional<Price> bid = book.best_bid();
  const std::optional<Price> ask = book.best_ask();
  ASSERT_EQ(mirror.announced_bid_quantity(), bid ? book.quantity_at(Side::Buy, *bid) : 0u);
  ASSERT_EQ(mirror.announced_ask_quantity(), ask ? book.quantity_at(Side::Sell, *ask) : 0u);
  if (bid) {
    ASSERT_EQ(mirror.announced_bid_price(), *bid);
  }
  if (ask) {
    ASSERT_EQ(mirror.announced_ask_price(), *ask);
  }
}

// --- One engine, run by hand --------------------------------------------------------

class Feed : public ::testing::Test {
 protected:
  // Runs the engine and returns the market data that produced.
  std::vector<MarketData> run() {
    engine.process_pending();
    engine.poll([&](const Event& event) { events.push_back(event); });
    std::vector<MarketData> messages;
    engine.poll_market_data([&](const MarketData& message) { messages.push_back(message); });
    return messages;
  }

  // Rests an order and returns its ID, discarding the market data.
  OrderId rest(Side side, Price price, Quantity quantity) {
    EXPECT_TRUE(engine.submit(0, 0, side, price, quantity));
    (void)run();
    return events.back().order_id;
  }

  MatchingEngine engine{{.books = {kBook, kBook}, .market_data_capacity = 4'096}};
  std::vector<Event> events;
};

TEST(FeedSwitch, IsOffUnlessAskedFor) {
  MatchingEngine engine({.books = {kBook}});
  ASSERT_TRUE(engine.submit(1, 0, Side::Buy, 150, 10));
  ASSERT_TRUE(engine.submit(2, 0, Side::Sell, 150, 4));
  ASSERT_TRUE(engine.request_snapshot(0));
  engine.process_pending();

  std::size_t messages = 0;
  engine.poll_market_data([&](const MarketData&) { ++messages; });
  EXPECT_EQ(messages, 0u);
  EXPECT_EQ(engine.market_data_dropped(), 0u);
}

TEST_F(Feed, ARestingOrderAnnouncesItsPriceAndTheNewBest) {
  ASSERT_TRUE(engine.submit(1, 0, Side::Buy, 150, 10));

  const std::vector<MarketData> messages = run();

  ASSERT_EQ(types_of(messages), (Types{MarketDataType::Level, MarketDataType::BestPrices}));
  EXPECT_EQ(messages[0].symbol, 0);
  EXPECT_EQ(messages[0].side, Side::Buy);
  EXPECT_EQ(messages[0].price, 150);
  EXPECT_EQ(messages[0].quantity, 10u);
  EXPECT_EQ(messages[0].sequence, 1u);

  EXPECT_EQ(messages[1].bid_price, 150);
  EXPECT_EQ(messages[1].bid_quantity, 10u);
  EXPECT_EQ(messages[1].ask_quantity, 0u);  // no asks
  EXPECT_EQ(messages[1].sequence, 2u);
}

TEST_F(Feed, AnOrderBehindTheBestChangesOnlyItsOwnLevel) {
  (void)rest(Side::Buy, 150, 10);
  ASSERT_TRUE(engine.submit(1, 0, Side::Buy, 140, 5));

  const std::vector<MarketData> messages = run();

  ASSERT_EQ(types_of(messages), (Types{MarketDataType::Level}));
  EXPECT_EQ(messages[0].price, 140);
  EXPECT_EQ(messages[0].quantity, 5u);
}

TEST_F(Feed, MoreAtTheBestPriceIsANewBestQuantity) {
  (void)rest(Side::Sell, 150, 10);
  ASSERT_TRUE(engine.submit(1, 0, Side::Sell, 150, 5));

  const std::vector<MarketData> messages = run();

  ASSERT_EQ(types_of(messages), (Types{MarketDataType::Level, MarketDataType::BestPrices}));
  EXPECT_EQ(messages[0].quantity, 15u);  // the total, not the 5 that was added
  EXPECT_EQ(messages[1].ask_price, 150);
  EXPECT_EQ(messages[1].ask_quantity, 15u);
}

// An order that takes two orders at 150 and part of one at 151. Every trade is
// printed, but each price's new total is given once, when the order is done
// with that price.
TEST_F(Feed, ASweepPrintsEveryTradeAndEachPriceOnce) {
  (void)rest(Side::Sell, 150, 4);
  (void)rest(Side::Sell, 150, 6);
  (void)rest(Side::Sell, 151, 20);
  ASSERT_TRUE(engine.submit(1, 0, Side::Buy, 151, 15));

  const std::vector<MarketData> messages = run();

  ASSERT_EQ(types_of(messages),
            (Types{MarketDataType::Trade, MarketDataType::Trade, MarketDataType::Level,
                   MarketDataType::Trade, MarketDataType::Level, MarketDataType::BestPrices}));
  EXPECT_EQ(messages[0].price, 150);
  EXPECT_EQ(messages[0].quantity, 4u);
  EXPECT_EQ(messages[0].side, Side::Buy);  // the incoming order was a buy
  EXPECT_EQ(messages[1].quantity, 6u);
  EXPECT_EQ(messages[2].side, Side::Sell);
  EXPECT_EQ(messages[2].price, 150);
  EXPECT_EQ(messages[2].quantity, 0u);  // nothing left at 150
  EXPECT_EQ(messages[3].price, 151);
  EXPECT_EQ(messages[3].quantity, 5u);
  EXPECT_EQ(messages[4].price, 151);
  EXPECT_EQ(messages[4].quantity, 15u);
  EXPECT_EQ(messages[5].ask_price, 151);
  EXPECT_EQ(messages[5].ask_quantity, 15u);
  EXPECT_EQ(messages[5].bid_quantity, 0u);
}

TEST_F(Feed, AnOrderThatTradesAndThenRestsAnnouncesItsOwnLevelToo) {
  (void)rest(Side::Sell, 150, 10);
  ASSERT_TRUE(engine.submit(1, 0, Side::Buy, 152, 25));

  const std::vector<MarketData> messages = run();

  ASSERT_EQ(types_of(messages), (Types{MarketDataType::Trade, MarketDataType::Level,
                                       MarketDataType::Level, MarketDataType::BestPrices}));
  EXPECT_EQ(messages[1].side, Side::Sell);
  EXPECT_EQ(messages[1].quantity, 0u);
  EXPECT_EQ(messages[2].side, Side::Buy);
  EXPECT_EQ(messages[2].price, 152);
  EXPECT_EQ(messages[2].quantity, 15u);
  EXPECT_EQ(messages[3].bid_price, 152);
  EXPECT_EQ(messages[3].bid_quantity, 15u);
  EXPECT_EQ(messages[3].ask_quantity, 0u);
}

TEST_F(Feed, ACancelBringsItsLevelAndTheBestUpToDate) {
  const OrderId first = rest(Side::Buy, 150, 10);
  (void)rest(Side::Buy, 150, 5);
  (void)rest(Side::Buy, 140, 7);

  ASSERT_TRUE(engine.cancel(1, 0, first));
  std::vector<MarketData> messages = run();
  ASSERT_EQ(types_of(messages), (Types{MarketDataType::Level, MarketDataType::BestPrices}));
  EXPECT_EQ(messages[0].price, 150);
  EXPECT_EQ(messages[0].quantity, 5u);
  EXPECT_EQ(messages[1].bid_quantity, 5u);

  ASSERT_TRUE(engine.cancel(2, 0, events[2].order_id));  // the order at 140, behind the best
  messages = run();
  ASSERT_EQ(types_of(messages), (Types{MarketDataType::Level}));
  EXPECT_EQ(messages[0].price, 140);
  EXPECT_EQ(messages[0].quantity, 0u);
}

TEST_F(Feed, CommandsThatChangeNothingSayNothing) {
  (void)rest(Side::Sell, 150, 10);

  ASSERT_TRUE(engine.submit(1, 0, Side::Buy, 99, 10));                    // price out of range
  ASSERT_TRUE(engine.submit(2, 0, Side::Buy, 150, 0));                    // no quantity
  ASSERT_TRUE(engine.submit(3, 0, Side::Buy, 150, 50, OrderType::FOK));   // cannot be filled
  ASSERT_TRUE(engine.submit(4, 0, Side::Buy, 149, 5, OrderType::IOC));    // nothing at its price
  ASSERT_TRUE(engine.cancel(5, 0, 123'456));                              // no such order
  ASSERT_TRUE(engine.submit(6, 7, Side::Buy, 150, 5));                    // no such symbol

  EXPECT_TRUE(run().empty());
}

TEST_F(Feed, EachSymbolIsNumberedOnItsOwn) {
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(engine.submit(0, 0, Side::Buy, 150 - i, 1));
    ASSERT_TRUE(engine.submit(0, 1, Side::Sell, 160 + i, 1));
  }

  std::uint64_t next[2] = {1, 1};
  for (const MarketData& message : run()) {
    ASSERT_LT(message.symbol, 2);
    EXPECT_EQ(message.sequence, next[message.symbol]++);
  }
  EXPECT_GT(next[0], 3u);
  EXPECT_EQ(next[0], next[1]);
}

TEST_F(Feed, ASnapshotIsAClearThenEveryLevelBestFirstThenTheBestPrices) {
  (void)rest(Side::Buy, 140, 1);
  (void)rest(Side::Buy, 150, 2);
  (void)rest(Side::Buy, 145, 3);
  (void)rest(Side::Sell, 170, 4);
  (void)rest(Side::Sell, 160, 5);
  ASSERT_TRUE(engine.request_snapshot(0));

  const std::vector<MarketData> messages = run();

  ASSERT_EQ(types_of(messages),
            (Types{MarketDataType::Clear, MarketDataType::Level, MarketDataType::Level,
                   MarketDataType::Level, MarketDataType::Level, MarketDataType::Level,
                   MarketDataType::BestPrices}));
  const Price expected_prices[] = {150, 145, 140, 160, 170};  // bids down, then asks up
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(messages[i + 1].price, expected_prices[i]);
    EXPECT_EQ(messages[i + 1].side, i < 3 ? Side::Buy : Side::Sell);
  }
  EXPECT_EQ(messages[6].bid_price, 150);
  EXPECT_EQ(messages[6].ask_price, 160);

  // A reader that has seen nothing but the snapshot knows the whole book.
  BookMirror mirror;
  for (const MarketData& message : messages) {
    mirror.apply(message);
  }
  EXPECT_TRUE(mirror.in_sync());
  expect_mirror_matches_book(mirror, engine.book(0));
}

// The feed never holds the engine up. With a 64-slot ring and nobody reading,
// 60 orders at 60 new best prices produce about twice that many messages.
TEST(FeedBackPressure, ASlowReaderLosesMessagesButCanCatchUpFromASnapshot) {
  MatchingEngine engine({.books = {kBook}, .market_data_capacity = 64});
  for (int i = 0; i < 60; ++i) {
    ASSERT_TRUE(engine.submit(0, 0, Side::Buy, static_cast<Price>(110 + i), 1));
  }
  engine.process_pending();
  engine.poll([](const Event&) {});

  EXPECT_EQ(engine.commands_processed(), 60u);  // the engine was not held up
  EXPECT_EQ(engine.book(0).size(), 60u);
  EXPECT_GT(engine.market_data_dropped(), 0u);

  // The reader gets what fitted, then sees from the numbering that it missed some.
  BookMirror mirror;
  engine.poll_market_data([&](const MarketData& message) { mirror.apply(message); });
  ASSERT_TRUE(engine.submit(0, 0, Side::Sell, 190, 1));
  engine.process_pending();
  engine.poll_market_data([&](const MarketData& message) { mirror.apply(message); });
  EXPECT_FALSE(mirror.in_sync());

  // A snapshot puts it right.
  ASSERT_TRUE(engine.request_snapshot(0));
  engine.process_pending();
  engine.poll_market_data([&](const MarketData& message) { mirror.apply(message); });
  EXPECT_TRUE(mirror.in_sync());
  expect_mirror_matches_book(mirror, engine.book(0));
}

// 6,000 random commands of every kind. After each one, a reader that has seen
// only the feed must know exactly what the book holds.
TEST(FeedAccuracy, TheMirrorMatchesTheBookAfterEveryCommand) {
  MatchingEngine engine({.books = {kBook}, .market_data_capacity = 1 << 14});
  BookMirror mirror;
  std::vector<OrderId> accepted;
  std::uint64_t trades_seen_privately = 0;
  std::mt19937_64 rng(99);

  for (int step = 0; step < 6'000; ++step) {
    if (rng() % 100 < 65 || accepted.empty()) {
      const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
      const Price price = 99 + static_cast<Price>(rng() % 102);  // some out of range
      const auto quantity = static_cast<Quantity>(rng() % 31);    // some zero
      const auto type = static_cast<OrderType>(rng() % 4);
      ASSERT_TRUE(engine.submit(0, 0, side, price, quantity, type));
    } else {
      ASSERT_TRUE(engine.cancel(0, 0, accepted[static_cast<std::size_t>(rng() % accepted.size())]));
    }
    engine.process_pending();
    engine.poll([&](const Event& event) {
      if (event.type == EventType::Accepted) {
        accepted.push_back(event.order_id);
      }
      trades_seen_privately += event.type == EventType::Trade ? 1u : 0u;
    });
    engine.poll_market_data([&](const MarketData& message) { mirror.apply(message); });

    ASSERT_TRUE(mirror.in_sync()) << "step " << step;
    expect_mirror_matches_book(mirror, engine.book(0));
    if (::testing::Test::HasFatalFailure()) {
      FAIL() << "the mirror and the book disagree after step " << step;
    }
  }
  EXPECT_EQ(mirror.trades(), trades_seen_privately);
  EXPECT_GT(mirror.trades(), 500u);
  EXPECT_EQ(engine.market_data_dropped(), 0u);
}

// --- Several shards -----------------------------------------------------------------

std::vector<lob::BookConfig> five_books() { return std::vector<lob::BookConfig>(5, kBook); }

// Runs the shards by hand until nothing is left to do, feeding all market data
// to the mirrors. Shard 0's ring is always read first.
void settle(ShardedEngine& engine, std::vector<BookMirror>& mirrors) {
  int idle_rounds = 0;
  while (idle_rounds < 2) {
    const std::size_t handled = engine.process_pending();
    const std::size_t polled = engine.poll([](const Event&) {});
    const std::size_t market_data = engine.poll_market_data(
        [&](const MarketData& message) { mirrors[message.symbol].apply(message); });
    idle_rounds = handled == 0 && polled == 0 && market_data == 0 ? idle_rounds + 1 : 0;
  }
}

TEST(FeedOnShards, EachShardPublishesItsOwnSymbols) {
  ShardedEngine engine({.books = five_books(), .shards = 2, .market_data_capacity = 1'024});
  for (SymbolId symbol = 0; symbol < 5; ++symbol) {
    ASSERT_EQ(engine.submit(0, symbol, Side::Buy, 150, 10), SendStatus::Sent);
  }
  engine.process_pending();

  for (std::size_t shard = 0; shard < 2; ++shard) {
    std::size_t messages = 0;
    engine.poll_market_data(shard, [&](const MarketData& message) {
      EXPECT_EQ(engine.shard_of(message.symbol), shard);
      ++messages;
    });
    EXPECT_GT(messages, 0u);
  }
}

// The awkward case for a reader. Symbol 1 moves from shard 1 to shard 0. Both
// shards do their work before any market data is read, and then shard 0's ring
// is read first. So the reader meets what the new shard said about the symbol
// before the last of what the old shard said.
//
// It comes out right for two reasons: the numbering carries on across the
// move, so the old shard's leftovers are recognisably older and are ignored;
// and the new shard starts with a fresh picture, so nothing they said is
// needed.
TEST(FeedOnShards, TheFeedFollowsASymbolToItsNewShard) {
  ShardedEngine engine({.books = five_books(), .shards = 2, .market_data_capacity = 1'024});
  std::vector<BookMirror> mirrors(5);
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(engine.submit(0, 1, Side::Buy, static_cast<Price>(140 + i), 10), SendStatus::Sent);
  }
  settle(engine, mirrors);
  const std::uint64_t numbered_so_far = mirrors[1].last_sequence();
  ASSERT_GT(numbered_so_far, 5u);

  ASSERT_EQ(engine.submit(0, 1, Side::Sell, 180, 3), SendStatus::Sent);   // handled by shard 1
  ASSERT_EQ(engine.move_symbol(1, 0), SendStatus::Sent);
  ASSERT_EQ(engine.submit(0, 1, Side::Sell, 144, 15), SendStatus::Sent);  // handled by shard 0

  // Both shards finish their part of the move. Events are read, because the
  // move needs that, but the market data is left sitting in both rings.
  for (int round = 0; round < 3; ++round) {
    engine.process_pending();
    engine.poll([](const Event&) {});
  }
  ASSERT_FALSE(engine.move_in_progress(1));
  ASSERT_EQ(engine.commands_handled(1), 7u);

  // Now read it, the new shard's ring first.
  std::size_t from_new_shard = 0;
  std::size_t from_old_shard = 0;
  engine.poll_market_data(0, [&](const MarketData& message) {
    mirrors[message.symbol].apply(message);
    ++from_new_shard;
  });
  engine.poll_market_data(1, [&](const MarketData& message) {
    mirrors[message.symbol].apply(message);
    ++from_old_shard;
  });
  ASSERT_GT(from_new_shard, 0u);
  ASSERT_GT(from_old_shard, 0u);

  EXPECT_EQ(engine.shard_of(1), 0u);
  EXPECT_GT(mirrors[1].last_sequence(), numbered_so_far);  // carried on, not started again
  EXPECT_TRUE(mirrors[1].in_sync());
  expect_mirror_matches_book(mirrors[1], engine.book(1));
  EXPECT_EQ(engine.market_data_dropped(), 0u);
}

TEST(FeedOnShards, ASnapshotCanBeAskedForByGroupSymbolId) {
  ShardedEngine engine({.books = five_books(), .shards = 2, .market_data_capacity = 1'024});
  std::vector<BookMirror> ignored(5);
  ASSERT_EQ(engine.submit(0, 3, Side::Buy, 150, 10), SendStatus::Sent);
  ASSERT_EQ(engine.submit(0, 3, Side::Sell, 160, 20), SendStatus::Sent);
  settle(engine, ignored);

  ASSERT_EQ(engine.request_snapshot(3), SendStatus::Sent);
  EXPECT_EQ(engine.request_snapshot(9), SendStatus::UnknownSymbol);
  engine.process_pending();

  BookMirror late_reader;
  engine.poll_market_data([&](const MarketData& message) {
    ASSERT_EQ(message.symbol, 3);
    late_reader.apply(message);
  });
  EXPECT_TRUE(late_reader.in_sync());
  expect_mirror_matches_book(late_reader, engine.book(3));
}

// Three shard threads, random orders for seven symbols, and a symbol moved to
// a random shard every twenty commands or so. One thread sends, reads the
// events and reads the feed. At the end every mirror must match its book.
TEST(FeedOnShards, MirrorsMatchTheBooksAfterRandomTrafficAndMoves) {
  constexpr std::size_t kSymbols = 7;
  constexpr std::size_t kShards = 3;
  ShardedEngine engine({.books = std::vector<lob::BookConfig>(kSymbols, kBook),
                        .shards = kShards,
                        .command_capacity = 64,
                        .event_capacity = 64,
                        .idle = IdleStrategy::Yield,
                        .market_data_capacity = 1 << 16});
  engine.start();
  std::vector<BookMirror> mirrors(kSymbols);
  std::vector<std::vector<OrderId>> accepted(kSymbols);
  std::size_t answered = 0;
  const auto read = [&] {
    engine.poll([&](const Event& event) {
      if (event.type == EventType::Accepted) {
        accepted[event.symbol].push_back(event.order_id);
      }
      answered += event.type != EventType::Trade ? 1u : 0u;
    });
    engine.poll_market_data(
        [&](const MarketData& message) { mirrors[message.symbol].apply(message); });
  };

  std::mt19937_64 rng(2024);
  constexpr std::size_t kCommands = 12'000;
  for (std::size_t i = 0; i < kCommands; ++i) {
    const auto symbol = static_cast<SymbolId>(rng() % kSymbols);
    if (rng() % 20 == 0) {
      const std::size_t to = rng() % kShards;
      while (engine.move_symbol(symbol, to) != SendStatus::Sent) {
        read();
        std::this_thread::yield();
      }
    }
    const bool cancel = rng() % 100 >= 65 && !accepted[symbol].empty();
    const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
    const Price price = 99 + static_cast<Price>(rng() % 102);
    const auto quantity = static_cast<Quantity>(rng() % 31);
    const auto type = static_cast<OrderType>(rng() % 4);
    const OrderId to_cancel =
        cancel ? accepted[symbol][static_cast<std::size_t>(rng() % accepted[symbol].size())] : 0;
    const auto send = [&] {
      return cancel ? engine.cancel(i, symbol, to_cancel)
                    : engine.submit(i, symbol, side, price, quantity, type);
    };
    while (send() != SendStatus::Sent) {
      read();
      std::this_thread::yield();
    }
    read();
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  while (answered < kCommands && std::chrono::steady_clock::now() < deadline) {
    read();
    std::this_thread::yield();
  }
  engine.stop();
  read();
  ASSERT_EQ(answered, kCommands);
  ASSERT_EQ(engine.market_data_dropped(), 0u);

  for (SymbolId symbol = 0; symbol < kSymbols; ++symbol) {
    EXPECT_TRUE(mirrors[symbol].in_sync()) << "symbol " << symbol;
    expect_mirror_matches_book(mirrors[symbol], engine.book(symbol));
    if (::testing::Test::HasFatalFailure()) {
      FAIL() << "symbol " << symbol << ": the mirror and the book disagree";
    }
  }
}

}  // namespace

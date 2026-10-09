// Several books on one engine: commands reach the right book, books do not
// affect one another, and every event says which book it came from.
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "lob/matching_engine.hpp"

namespace {

using lob::Command;
using lob::CommandType;
using lob::Event;
using lob::EventType;
using lob::kInvalidOrderId;
using lob::MatchingEngine;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Quantity;
using lob::Side;
using lob::SymbolId;

constexpr SymbolId kApple = 0;
constexpr SymbolId kMicrosoft = 1;
constexpr SymbolId kTesla = 2;
constexpr SymbolId kUnknown = 3;

// The books deliberately differ in price range and capacity.
const std::vector<lob::BookConfig> kBooks{
    {.symbol = "AAPL", .min_price = 100, .num_levels = 100, .max_orders = 64},
    {.symbol = "MSFT", .min_price = 400, .num_levels = 100, .max_orders = 64},
    {.symbol = "TSLA", .min_price = 100, .num_levels = 100, .max_orders = 2},
};

class MultiSymbol : public ::testing::Test {
 protected:
  // Runs the engine on this thread and returns the events that produced.
  std::vector<Event> run() {
    engine.process_pending();
    std::vector<Event> events;
    engine.poll([&](const Event& event) { events.push_back(event); });
    return events;
  }

  // Rests one order and returns the ID its book gave it.
  OrderId rest(SymbolId symbol, Side side, Price price, Quantity quantity) {
    EXPECT_TRUE(engine.submit(0, symbol, side, price, quantity));
    const std::vector<Event> events = run();
    EXPECT_EQ(events.size(), 1u);
    return events.empty() ? kInvalidOrderId : events[0].order_id;
  }

  MatchingEngine engine{{.books = kBooks}};
};

TEST_F(MultiSymbol, SymbolsAreNumberedInTheOrderTheyWereConfigured) {
  EXPECT_EQ(engine.symbol_count(), 3u);
  EXPECT_EQ(engine.symbol_id("AAPL"), kApple);
  EXPECT_EQ(engine.symbol_id("MSFT"), kMicrosoft);
  EXPECT_EQ(engine.symbol_id("TSLA"), kTesla);
  EXPECT_FALSE(engine.symbol_id("GOOG").has_value());
  EXPECT_FALSE(engine.symbol_id("").has_value());
  EXPECT_EQ(engine.book(kMicrosoft).symbol(), "MSFT");
}

TEST_F(MultiSymbol, AnOrderRestsOnlyInItsOwnBook) {
  ASSERT_TRUE(engine.submit(1, kMicrosoft, Side::Buy, 450, 10));

  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, EventType::Accepted);
  EXPECT_EQ(events[0].symbol, kMicrosoft);
  EXPECT_TRUE(engine.book(kApple).empty());
  EXPECT_EQ(engine.book(kMicrosoft).size(), 1u);
  EXPECT_TRUE(engine.book(kTesla).empty());
}

// A buy and a sell at the same price trade only if they are for the same symbol.
TEST_F(MultiSymbol, OrdersForDifferentSymbolsNeverTradeWithEachOther) {
  ASSERT_TRUE(engine.submit(1, kApple, Side::Sell, 150, 10));
  ASSERT_TRUE(engine.submit(2, kTesla, Side::Buy, 150, 10));
  std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].type, EventType::Accepted);
  EXPECT_EQ(events[1].type, EventType::Accepted);
  EXPECT_EQ(engine.book(kApple).size(), 1u);
  EXPECT_EQ(engine.book(kTesla).size(), 1u);

  ASSERT_TRUE(engine.submit(3, kApple, Side::Buy, 150, 10));
  events = run();

  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].type, EventType::Trade);
  EXPECT_EQ(events[0].symbol, kApple);
  EXPECT_EQ(events[1].type, EventType::Accepted);
  EXPECT_TRUE(engine.book(kApple).empty());
  EXPECT_EQ(engine.book(kTesla).size(), 1u);
}

TEST_F(MultiSymbol, EveryEventNamesItsSymbol) {
  const OrderId maker = rest(kMicrosoft, Side::Sell, 450, 10);
  ASSERT_TRUE(engine.submit(1, kMicrosoft, Side::Buy, 450, 4));
  ASSERT_TRUE(engine.cancel(2, kMicrosoft, maker));
  ASSERT_TRUE(engine.cancel(3, kMicrosoft, maker));
  ASSERT_TRUE(engine.submit(4, kMicrosoft, Side::Buy, 450, 0));

  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 5u);
  EXPECT_EQ(events[0].type, EventType::Trade);
  EXPECT_EQ(events[1].type, EventType::Accepted);
  EXPECT_EQ(events[2].type, EventType::Cancelled);
  EXPECT_EQ(events[3].type, EventType::CancelRejected);
  EXPECT_EQ(events[4].type, EventType::Rejected);
  for (const Event& event : events) {
    EXPECT_EQ(event.symbol, kMicrosoft);
  }
}

// Each book numbers its own orders, so the first order in two books gets the
// same ID. Cancelling it in one book must leave the other book's order alone.
TEST_F(MultiSymbol, OrderIdsBelongToTheirBook) {
  const OrderId apple_order = rest(kApple, Side::Buy, 150, 10);
  const OrderId tesla_order = rest(kTesla, Side::Buy, 150, 10);
  ASSERT_EQ(apple_order, tesla_order);

  ASSERT_TRUE(engine.cancel(1, kApple, apple_order));
  ASSERT_TRUE(engine.cancel(2, kApple, apple_order));
  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].type, EventType::Cancelled);
  EXPECT_EQ(events[1].type, EventType::CancelRejected);
  EXPECT_TRUE(engine.book(kApple).empty());
  ASSERT_TRUE(engine.book(kTesla).find(tesla_order) != nullptr);
  EXPECT_EQ(engine.book(kTesla).find(tesla_order)->quantity, 10u);
}

TEST_F(MultiSymbol, EachBookEnforcesItsOwnPriceRange) {
  ASSERT_TRUE(engine.submit(1, kApple, Side::Buy, 150, 10));      // valid for AAPL
  ASSERT_TRUE(engine.submit(2, kMicrosoft, Side::Buy, 150, 10));  // below MSFT's range
  ASSERT_TRUE(engine.submit(3, kMicrosoft, Side::Buy, 450, 10));  // valid for MSFT
  ASSERT_TRUE(engine.submit(4, kApple, Side::Buy, 450, 10));      // above AAPL's range

  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[0].type, EventType::Accepted);
  EXPECT_EQ(events[1].type, EventType::Rejected);
  EXPECT_EQ(events[2].type, EventType::Accepted);
  EXPECT_EQ(events[3].type, EventType::Rejected);
}

// TSLA holds only two orders. Filling it up must not stop the other books.
TEST_F(MultiSymbol, AFullBookDoesNotAffectTheOthers) {
  ASSERT_TRUE(engine.submit(1, kTesla, Side::Buy, 150, 10));
  ASSERT_TRUE(engine.submit(2, kTesla, Side::Buy, 150, 10));
  ASSERT_TRUE(engine.submit(3, kTesla, Side::Buy, 150, 10));
  ASSERT_TRUE(engine.submit(4, kApple, Side::Buy, 150, 10));

  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[0].type, EventType::Accepted);
  EXPECT_EQ(events[1].type, EventType::Accepted);
  EXPECT_EQ(events[2].type, EventType::Rejected);
  EXPECT_EQ(events[2].symbol, kTesla);
  EXPECT_EQ(events[3].type, EventType::Accepted);
  EXPECT_EQ(events[3].symbol, kApple);
}

TEST_F(MultiSymbol, CommandsForAnUnknownSymbolAreRefused) {
  const OrderId resting = rest(kApple, Side::Buy, 150, 10);

  ASSERT_TRUE(engine.submit(1, kUnknown, Side::Sell, 150, 10));
  ASSERT_TRUE(engine.submit(2, kUnknown, Side::Sell, 0, 10, OrderType::Market));
  ASSERT_TRUE(engine.cancel(3, kUnknown, resting));
  ASSERT_TRUE(engine.cancel(4, SymbolId{65'535}, resting));
  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[0].type, EventType::Rejected);
  EXPECT_EQ(events[0].symbol, kUnknown);
  EXPECT_EQ(events[0].client_tag, 1u);
  EXPECT_EQ(events[0].order_id, kInvalidOrderId);
  EXPECT_EQ(events[1].type, EventType::Rejected);
  EXPECT_EQ(events[2].type, EventType::CancelRejected);
  EXPECT_EQ(events[2].order_id, resting);
  EXPECT_EQ(events[3].type, EventType::CancelRejected);
  EXPECT_EQ(events[3].symbol, SymbolId{65'535});

  // Nothing was touched.
  EXPECT_EQ(engine.book(kApple).size(), 1u);
  EXPECT_TRUE(engine.book(kMicrosoft).empty());
  EXPECT_TRUE(engine.book(kTesla).empty());
}

// The decisive check that books are independent. Random commands for three
// symbols are interleaved through one engine. Then each symbol's commands are
// replayed, alone, through an engine that has only that one book. For every
// symbol, the events from the shared engine must match the events from its
// private engine exactly.
TEST(MultiSymbolIndependence, InterleavedSymbolsBehaveLikeSeparateEngines) {
  const std::vector<lob::BookConfig> books{
      {.symbol = "AAA", .min_price = 100, .num_levels = 60, .max_orders = 128},
      {.symbol = "BBB", .min_price = 100, .num_levels = 60, .max_orders = 16},
      {.symbol = "CCC", .min_price = 130, .num_levels = 60, .max_orders = 128},
  };
  constexpr int kCommands = 9'000;

  // 1. One shared engine, commands interleaved across symbols.
  MatchingEngine shared({.books = books});
  std::vector<std::vector<Command>> commands_for(books.size());
  std::vector<std::vector<Event>> shared_events(books.size());
  std::vector<std::vector<OrderId>> accepted(books.size());
  std::mt19937_64 rng(7);

  for (int i = 0; i < kCommands; ++i) {
    const auto symbol = static_cast<SymbolId>(rng() % books.size());
    Command command{};
    command.client_tag = static_cast<std::uint64_t>(i);
    command.symbol = symbol;
    if (rng() % 100 < 65 || accepted[symbol].empty()) {
      command.type = CommandType::Submit;
      command.side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
      command.price = 99 + static_cast<Price>(rng() % 95);  // 99..193: some invalid per book
      command.quantity = static_cast<Quantity>(rng() % 31);
      command.order_type = static_cast<OrderType>(rng() % 4);
      ASSERT_TRUE(shared.submit(command.client_tag, symbol, command.side, command.price,
                                command.quantity, command.order_type));
    } else {
      command.type = CommandType::Cancel;
      command.order_id =
          accepted[symbol][static_cast<std::size_t>(rng() % accepted[symbol].size())];
      ASSERT_TRUE(shared.cancel(command.client_tag, symbol, command.order_id));
    }
    commands_for[symbol].push_back(command);

    shared.process_pending();
    shared.poll([&](const Event& event) {
      shared_events[event.symbol].push_back(event);
      if (event.type == EventType::Accepted) {
        accepted[event.symbol].push_back(event.order_id);
      }
    });
  }

  // 2. One private engine per symbol, fed only that symbol's commands.
  for (SymbolId symbol = 0; symbol < books.size(); ++symbol) {
    MatchingEngine alone({.books = {books[symbol]}});
    std::vector<Event> alone_events;
    for (const Command& command : commands_for[symbol]) {
      if (command.type == CommandType::Submit) {
        ASSERT_TRUE(alone.submit(command.client_tag, 0, command.side, command.price,
                                 command.quantity, command.order_type));
      } else {
        ASSERT_TRUE(alone.cancel(command.client_tag, 0, command.order_id));
      }
      alone.process_pending();
      alone.poll([&](const Event& event) { alone_events.push_back(event); });
    }

    ASSERT_GT(alone_events.size(), 1'000u) << "symbol " << symbol << " was barely exercised";
    ASSERT_EQ(shared_events[symbol].size(), alone_events.size()) << "symbol " << symbol;
    for (std::size_t i = 0; i < alone_events.size(); ++i) {
      const Event& from_shared = shared_events[symbol][i];
      const Event& from_alone = alone_events[i];
      const bool same = from_shared.client_tag == from_alone.client_tag &&
                        from_shared.order_id == from_alone.order_id &&
                        from_shared.maker_id == from_alone.maker_id &&
                        from_shared.price == from_alone.price &&
                        from_shared.quantity == from_alone.quantity &&
                        from_shared.resting == from_alone.resting &&
                        from_shared.side == from_alone.side &&
                        from_shared.type == from_alone.type;
      ASSERT_TRUE(same) << "symbol " << symbol << ", event " << i << " differs";
      ASSERT_EQ(from_shared.symbol, symbol);
    }
    EXPECT_EQ(shared.book(symbol).size(), alone.book(0).size());
    EXPECT_EQ(shared.book(symbol).best_bid(), alone.book(0).best_bid());
    EXPECT_EQ(shared.book(symbol).best_ask(), alone.book(0).best_ask());
  }
}

}  // namespace

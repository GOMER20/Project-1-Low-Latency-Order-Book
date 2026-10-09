#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "lob/matching_engine.hpp"

namespace {

using lob::Command;
using lob::CommandType;
using lob::EngineConfig;
using lob::Event;
using lob::EventType;
using lob::IdleStrategy;
using lob::kInvalidOrderId;
using lob::MatchingEngine;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Quantity;
using lob::Side;

// Tradable prices are 100..199.
constexpr lob::BookConfig kBook{
    .symbol = "TEST", .min_price = 100, .num_levels = 100, .max_orders = 256};

// These tests use a single book, which is symbol 0.
constexpr lob::SymbolId kSymbol = 0;

// Every command is answered by exactly one event that is not a Trade.
bool answers_a_command(const Event& event) { return event.type != EventType::Trade; }

bool same(const Event& a, const Event& b) {
  return a.client_tag == b.client_tag && a.order_id == b.order_id && a.maker_id == b.maker_id &&
         a.price == b.price && a.quantity == b.quantity && a.resting == b.resting &&
         a.symbol == b.symbol && a.side == b.side && a.type == b.type;
}

// Polls until `target` commands have been answered. Returns false instead of
// hanging if the engine stops responding.
bool poll_until_answered(MatchingEngine& engine, std::vector<Event>& events,
                         std::size_t& answered, std::size_t target) {
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

// --- The engine run by hand on the test's own thread: what it says -----------

class EngineByHand : public ::testing::Test {
 protected:
  // Handles everything queued and returns the events that produced.
  std::vector<Event> run() {
    engine.process_pending();
    std::vector<Event> events;
    engine.poll([&](const Event& event) { events.push_back(event); });
    return events;
  }

  MatchingEngine engine{{.books = {kBook}}};
};

TEST_F(EngineByHand, SubmitIsAnsweredWithAnAcceptedEvent) {
  ASSERT_TRUE(engine.submit(7, kSymbol, Side::Buy, 150, 10));

  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, EventType::Accepted);
  EXPECT_EQ(events[0].client_tag, 7u);
  EXPECT_NE(events[0].order_id, kInvalidOrderId);
  EXPECT_EQ(events[0].price, 150);
  EXPECT_EQ(events[0].quantity, 0u);
  EXPECT_EQ(events[0].resting, 10u);
  EXPECT_EQ(events[0].side, Side::Buy);
  EXPECT_TRUE(engine.book(kSymbol).find(events[0].order_id) != nullptr);
}

TEST_F(EngineByHand, SubmitThatTradesReportsItsTradesBeforeItsAccepted) {
  ASSERT_TRUE(engine.submit(1, kSymbol, Side::Sell, 150, 10));
  const OrderId maker = run().at(0).order_id;

  ASSERT_TRUE(engine.submit(2, kSymbol, Side::Buy, 155, 25));
  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 2u);
  const Event& trade = events[0];
  const Event& accepted = events[1];

  EXPECT_EQ(trade.type, EventType::Trade);
  EXPECT_EQ(trade.client_tag, 2u);
  EXPECT_EQ(trade.maker_id, maker);
  EXPECT_EQ(trade.price, 150);
  EXPECT_EQ(trade.quantity, 10u);
  EXPECT_EQ(trade.side, Side::Buy);

  EXPECT_EQ(accepted.type, EventType::Accepted);
  EXPECT_EQ(accepted.client_tag, 2u);
  EXPECT_EQ(accepted.order_id, trade.order_id);
  EXPECT_EQ(accepted.price, 155);
  EXPECT_EQ(accepted.quantity, 10u);
  EXPECT_EQ(accepted.resting, 15u);
}

TEST_F(EngineByHand, SubmitTheBookRefusesIsAnsweredWithRejected) {
  ASSERT_TRUE(engine.submit(1, kSymbol, Side::Buy, 99, 10));   // below the price range
  ASSERT_TRUE(engine.submit(2, kSymbol, Side::Sell, 150, 0));  // zero quantity

  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].type, EventType::Rejected);
  EXPECT_EQ(events[0].client_tag, 1u);
  EXPECT_EQ(events[0].order_id, kInvalidOrderId);
  EXPECT_EQ(events[1].type, EventType::Rejected);
  EXPECT_EQ(events[1].client_tag, 2u);
  EXPECT_TRUE(engine.book(kSymbol).empty());
}

TEST_F(EngineByHand, CancelIsAnsweredWithCancelledOnlyWhileTheOrderIsLive) {
  ASSERT_TRUE(engine.submit(1, kSymbol, Side::Buy, 150, 10));
  const OrderId id = run().at(0).order_id;

  ASSERT_TRUE(engine.cancel(2, kSymbol, id));
  ASSERT_TRUE(engine.cancel(3, kSymbol, id));
  ASSERT_TRUE(engine.cancel(4, kSymbol, 123'456));
  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0].type, EventType::Cancelled);
  EXPECT_EQ(events[0].client_tag, 2u);
  EXPECT_EQ(events[0].order_id, id);
  EXPECT_EQ(events[1].type, EventType::CancelRejected);
  EXPECT_EQ(events[1].client_tag, 3u);
  EXPECT_EQ(events[2].type, EventType::CancelRejected);
  EXPECT_EQ(events[2].order_id, 123'456u);
  EXPECT_TRUE(engine.book(kSymbol).empty());
}

// Orders that never rest are still answered with Accepted; `resting` is zero
// and whatever they did not fill is gone.
TEST_F(EngineByHand, MarketIocAndFokOrdersTravelThroughTheEngine) {
  ASSERT_TRUE(engine.submit(1, kSymbol, Side::Sell, 150, 10));
  (void)run();

  ASSERT_TRUE(engine.submit(2, kSymbol, Side::Buy, 150, 50, OrderType::FOK));     // cannot fill: killed
  ASSERT_TRUE(engine.submit(3, kSymbol, Side::Buy, 150, 4, OrderType::IOC));      // takes 4
  ASSERT_TRUE(engine.submit(4, kSymbol, Side::Buy, 0, 100, OrderType::Market));   // takes the last 6
  const std::vector<Event> events = run();

  ASSERT_EQ(events.size(), 5u);
  EXPECT_EQ(events[0].type, EventType::Accepted);
  EXPECT_EQ(events[0].client_tag, 2u);
  EXPECT_EQ(events[0].quantity, 0u);
  EXPECT_EQ(events[0].resting, 0u);

  EXPECT_EQ(events[1].type, EventType::Trade);
  EXPECT_EQ(events[1].quantity, 4u);
  EXPECT_EQ(events[2].type, EventType::Accepted);
  EXPECT_EQ(events[2].client_tag, 3u);
  EXPECT_EQ(events[2].quantity, 4u);
  EXPECT_EQ(events[2].resting, 0u);

  EXPECT_EQ(events[3].type, EventType::Trade);
  EXPECT_EQ(events[3].quantity, 6u);
  EXPECT_EQ(events[4].type, EventType::Accepted);
  EXPECT_EQ(events[4].client_tag, 4u);
  EXPECT_EQ(events[4].quantity, 6u);
  EXPECT_EQ(events[4].resting, 0u);

  EXPECT_TRUE(engine.book(kSymbol).empty());
}

TEST_F(EngineByHand, CommandsAreHandledInTheOrderTheyWereSent) {
  for (std::uint64_t tag = 1; tag <= 5; ++tag) {
    ASSERT_TRUE(engine.submit(tag, kSymbol, Side::Buy, 150, 1));
  }

  EXPECT_EQ(engine.process_pending(), 5u);
  EXPECT_EQ(engine.commands_processed(), 5u);
  EXPECT_EQ(engine.process_pending(), 0u);

  std::vector<std::uint64_t> tags;
  engine.poll([&](const Event& event) { tags.push_back(event.client_tag); });
  EXPECT_EQ(tags, (std::vector<std::uint64_t>{1, 2, 3, 4, 5}));
}

TEST_F(EngineByHand, CountsTheCommandsHandledForItsSymbol) {
  EXPECT_EQ(engine.commands_handled(kSymbol), 0u);
  ASSERT_TRUE(engine.submit(1, kSymbol, Side::Buy, 150, 10));
  ASSERT_TRUE(engine.submit(2, kSymbol, Side::Buy, 99, 10));  // rejected, but still handled
  ASSERT_TRUE(engine.cancel(3, kSymbol, 12'345));             // refused, but still handled
  ASSERT_TRUE(engine.submit(4, 5, Side::Buy, 150, 10));       // no such symbol: not counted

  EXPECT_EQ(engine.commands_handled(kSymbol), 0u);  // nothing has been handled yet
  (void)run();

  EXPECT_EQ(engine.commands_handled(kSymbol), 3u);
  EXPECT_EQ(engine.commands_processed(), 4u);
}

TEST(EngineRings, FullCommandRingRefusesCommandsUntilTheEngineCatchesUp) {
  MatchingEngine engine({.books = {kBook}, .command_capacity = 4});
  for (std::uint64_t tag = 0; tag < 4; ++tag) {
    EXPECT_TRUE(engine.submit(tag, kSymbol, Side::Buy, 150, 1));
  }
  EXPECT_FALSE(engine.submit(4, kSymbol, Side::Buy, 150, 1));
  EXPECT_FALSE(engine.cancel(5, kSymbol, 1));

  engine.process_pending();

  EXPECT_TRUE(engine.submit(6, kSymbol, Side::Buy, 150, 1));
}

// When the engine is run by hand, nothing can drain the event ring while it
// works, so events that do not fit are counted rather than waited for.
TEST(EngineRings, RunByHandCountsEventsThatDoNotFit) {
  MatchingEngine engine({.books = {kBook}, .event_capacity = 2});
  for (std::uint64_t tag = 0; tag < 5; ++tag) {
    ASSERT_TRUE(engine.submit(tag, kSymbol, Side::Buy, 150, 1));
  }

  engine.process_pending();

  std::size_t delivered = 0;
  engine.poll([&](const Event&) { ++delivered; });
  EXPECT_EQ(delivered, 2u);
  EXPECT_EQ(engine.events_dropped(), 3u);
  EXPECT_EQ(engine.book(kSymbol).size(), 5u);
}

// --- The engine on its own thread --------------------------------------------

TEST(EngineThread, AnswersCommandsOnItsOwnThread) {
  MatchingEngine engine({.books = {kBook}, .idle = IdleStrategy::Yield});
  engine.start();
  EXPECT_TRUE(engine.running());

  ASSERT_TRUE(engine.submit(1, kSymbol, Side::Sell, 150, 10));
  ASSERT_TRUE(engine.submit(2, kSymbol, Side::Buy, 150, 4));
  std::vector<Event> events;
  std::size_t answered = 0;
  ASSERT_TRUE(poll_until_answered(engine, events, answered, 2));

  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0].type, EventType::Accepted);
  EXPECT_EQ(events[1].type, EventType::Trade);
  EXPECT_EQ(events[1].maker_id, events[0].order_id);
  EXPECT_EQ(events[2].type, EventType::Accepted);

  ASSERT_TRUE(engine.cancel(3, kSymbol, events[0].order_id));
  ASSERT_TRUE(poll_until_answered(engine, events, answered, 3));
  EXPECT_EQ(events.back().type, EventType::Cancelled);

  engine.stop();
  EXPECT_FALSE(engine.running());
  EXPECT_TRUE(engine.book(kSymbol).empty());
  EXPECT_EQ(engine.commands_processed(), 3u);
  EXPECT_EQ(engine.events_dropped(), 0u);
}

TEST(EngineThread, SpinningEngineAnswersToo) {
  MatchingEngine engine({.books = {kBook}, .idle = IdleStrategy::Spin});
  engine.start();

  ASSERT_TRUE(engine.submit(1, kSymbol, Side::Buy, 150, 10));
  std::vector<Event> events;
  std::size_t answered = 0;
  ASSERT_TRUE(poll_until_answered(engine, events, answered, 1));
  engine.stop();

  EXPECT_EQ(events[0].type, EventType::Accepted);
  EXPECT_EQ(engine.book(kSymbol).size(), 1u);
}

TEST(EngineThread, StopHandlesEverythingThatWasAlreadyQueued) {
  MatchingEngine engine({.books = {kBook}, .idle = IdleStrategy::Yield});
  for (std::uint64_t tag = 0; tag < 50; ++tag) {
    ASSERT_TRUE(engine.submit(tag, kSymbol, Side::Buy, 150, 1));
  }

  engine.start();
  engine.stop();

  std::size_t answered = 0;
  engine.poll([&](const Event& event) {
    if (answers_a_command(event)) {
      ++answered;
    }
  });
  EXPECT_EQ(answered, 50u);
  EXPECT_EQ(engine.commands_processed(), 50u);
  EXPECT_EQ(engine.events_dropped(), 0u);
  EXPECT_EQ(engine.book(kSymbol).size(), 50u);
}

// Nobody polls here, so the 4-slot event ring fills up. stop() must still
// return, and every event must be accounted for as delivered or dropped.
TEST(EngineThread, StopReturnsEvenIfNobodyIsReadingEvents) {
  MatchingEngine engine(
      {.books = {kBook}, .event_capacity = 4, .idle = IdleStrategy::Yield});
  for (std::uint64_t tag = 0; tag < 40; ++tag) {
    ASSERT_TRUE(engine.submit(tag, kSymbol, Side::Buy, 150, 1));
  }

  engine.start();
  engine.stop();

  std::size_t delivered = 0;
  engine.poll([&](const Event&) { ++delivered; });
  EXPECT_EQ(delivered, 4u);
  EXPECT_EQ(engine.events_dropped(), 36u);
  EXPECT_EQ(engine.commands_processed(), 40u);
  EXPECT_EQ(engine.book(kSymbol).size(), 40u);
}

TEST(EngineThread, CanBeStartedAgainAfterStopping) {
  MatchingEngine engine({.books = {kBook}, .idle = IdleStrategy::Yield});
  std::vector<Event> events;
  std::size_t answered = 0;

  engine.start();
  ASSERT_TRUE(engine.submit(1, kSymbol, Side::Buy, 150, 10));
  ASSERT_TRUE(poll_until_answered(engine, events, answered, 1));
  engine.stop();

  engine.start();
  ASSERT_TRUE(engine.submit(2, kSymbol, Side::Sell, 150, 10));
  ASSERT_TRUE(poll_until_answered(engine, events, answered, 2));
  engine.stop();

  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[1].type, EventType::Trade);
  EXPECT_TRUE(engine.book(kSymbol).empty());
}

// The same 6,000 random commands, spread over three different books, are run
// twice: once by hand, one command at a time, and once through the engine
// thread with 4-slot rings, so that both the gateway and the engine are
// constantly blocked on a full ring. The two event streams must be identical,
// event for event.
TEST(EngineThread, ThreadedRunMatchesRunningByHandUnderBackPressure) {
  const std::vector<lob::BookConfig> books{
      {.symbol = "AAA", .min_price = 100, .num_levels = 100, .max_orders = 256},
      {.symbol = "BBB", .min_price = 100, .num_levels = 40, .max_orders = 32},
      {.symbol = "CCC", .min_price = 150, .num_levels = 100, .max_orders = 256},
  };
  constexpr int kCommands = 6'000;
  struct Known {
    lob::SymbolId symbol;
    OrderId id;
  };

  std::vector<Command> script;
  std::vector<Event> expected;
  std::vector<std::size_t> expected_resting;
  {
    MatchingEngine by_hand({.books = books});
    std::mt19937_64 rng(42);
    std::vector<Known> accepted;

    for (int i = 0; i < kCommands; ++i) {
      Command command{};
      command.client_tag = static_cast<std::uint64_t>(i);
      // One command in fifty names a symbol the engine does not have.
      command.symbol = rng() % 50 == 0 ? lob::SymbolId{7} : static_cast<lob::SymbolId>(rng() % 3);
      if (rng() % 100 < 65 || accepted.empty()) {
        command.type = CommandType::Submit;
        command.side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
        command.price = 99 + static_cast<Price>(rng() % 152);  // 99..250: some invalid per book
        command.quantity = static_cast<Quantity>(rng() % 31);  // 0 is invalid
        command.order_type = static_cast<OrderType>(rng() % 4);
        ASSERT_TRUE(by_hand.submit(command.client_tag, command.symbol, command.side,
                                   command.price, command.quantity, command.order_type));
      } else {
        // Usually cancel a known order in its own book; one time in ten, send
        // its ID to whichever book was picked above instead.
        const Known& known = accepted[static_cast<std::size_t>(rng() % accepted.size())];
        command.type = CommandType::Cancel;
        command.order_id = known.id;
        if (rng() % 10 != 0) {
          command.symbol = known.symbol;
        }
        ASSERT_TRUE(by_hand.cancel(command.client_tag, command.symbol, command.order_id));
      }
      script.push_back(command);

      by_hand.process_pending();
      by_hand.poll([&](const Event& event) {
        expected.push_back(event);
        if (event.type == EventType::Accepted) {
          accepted.push_back({event.symbol, event.order_id});
        }
      });
    }
    ASSERT_EQ(by_hand.events_dropped(), 0u);
    for (lob::SymbolId symbol = 0; symbol < books.size(); ++symbol) {
      expected_resting.push_back(by_hand.book(symbol).size());
    }
  }

  MatchingEngine engine({.books = books,
                         .command_capacity = 4,
                         .event_capacity = 4,
                         .idle = IdleStrategy::Yield});
  engine.start();
  std::vector<Event> actual;
  std::size_t answered = 0;
  const auto poll_once = [&] {
    engine.poll([&](const Event& event) {
      actual.push_back(event);
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
    while (!send()) {
      poll_once();  // keep reading, or the engine stalls on a full event ring
      std::this_thread::yield();
    }
    poll_once();
  }
  ASSERT_TRUE(poll_until_answered(engine, actual, answered, script.size()));
  engine.stop();

  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    ASSERT_TRUE(same(actual[i], expected[i])) << "event " << i << " differs";
  }
  EXPECT_EQ(engine.events_dropped(), 0u);
  EXPECT_EQ(engine.commands_processed(), script.size());
  for (lob::SymbolId symbol = 0; symbol < books.size(); ++symbol) {
    EXPECT_EQ(engine.book(symbol).size(), expected_resting[symbol]);
  }
}

}  // namespace

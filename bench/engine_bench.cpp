// The whole pipeline: commands cross one ring to the engine thread, and the
// events they cause cross another ring back.
#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

#include "lob/matching_engine.hpp"

namespace {

using lob::Event;
using lob::EventType;
using lob::OrderId;
using lob::Side;
using lob::SymbolId;

constexpr lob::Price kPrice = 30'000;

// A realistically sized book: 65,536 price levels and room for 65,536 orders,
// about 6 MB each.
constexpr lob::BookConfig kBook{
    .symbol = "BENCH", .min_price = 0, .num_levels = 1 << 16, .max_orders = 1 << 16};

// One command at a time: send it, then wait for its answer. The time per
// iteration is the full round trip through both rings and the book. Commands
// alternate between adding an order and cancelling it.
void BM_Engine_RoundTrip(benchmark::State& state) {
  lob::MatchingEngine engine({.books = {kBook}});
  engine.start();
  OrderId resting = lob::kInvalidOrderId;
  std::uint64_t tag = 0;

  for (auto _ : state) {
    if (resting == lob::kInvalidOrderId) {
      while (!engine.submit(tag, 0, Side::Buy, kPrice, 100)) {
      }
    } else {
      while (!engine.cancel(tag, 0, resting)) {
      }
    }
    bool answered = false;
    while (!answered) {
      engine.poll([&](const Event& event) {
        resting = event.type == EventType::Accepted ? event.order_id : lob::kInvalidOrderId;
        answered = true;
      });
    }
    ++tag;
  }
  engine.stop();
}
BENCHMARK(BM_Engine_RoundTrip)->UseRealTime();

// Sustained throughput: commands are sent as fast as the ring accepts them,
// without waiting for answers, and events are read as they arrive.
//
// The workload is fixed in advance: a sell that rests, then a buy that fills
// it, over and over. What is sent never depends on what has come back, so the
// result does not hinge on how the two threads happen to interleave.
//
// The argument is the number of symbols. Each sell and buy pair goes to the
// next symbol in turn, so with more symbols the engine hops between more books.
void BM_Engine_Throughput(benchmark::State& state) {
  const auto symbols = static_cast<std::size_t>(state.range(0));
  lob::MatchingEngine engine(
      {.books = std::vector<lob::BookConfig>(symbols, kBook)});
  engine.start();
  std::uint64_t sent = 0;
  std::uint64_t answered = 0;
  std::size_t symbol = 0;
  const auto on_event = [&](const Event& event) {
    if (event.type != EventType::Trade) {
      ++answered;
    }
  };

  for (auto _ : state) {
    const bool first_of_pair = (sent & 1) == 0;
    while (!engine.submit(sent, static_cast<SymbolId>(symbol),
                          first_of_pair ? Side::Sell : Side::Buy, kPrice, 100)) {
      engine.poll(on_event);
    }
    engine.poll(on_event);
    ++sent;
    if (!first_of_pair && ++symbol == symbols) {
      symbol = 0;
    }
  }
  while (answered < sent) {
    engine.poll(on_event);
  }
  engine.stop();
  state.SetItemsProcessed(static_cast<std::int64_t>(sent));
}
BENCHMARK(BM_Engine_Throughput)->Arg(1)->Arg(8)->Arg(64)->UseRealTime();

// The engine's own cost per command, with no second thread involved: this
// thread sends 256 orders, runs the engine, reads the answers, then cancels
// them all the same way.
//
// The argument is EngineConfig::time_one_in, to show what timing commands
// costs: 0 is no timing, 64 the default, and 1 a stopwatch on every command.
void BM_Engine_PerCommand(benchmark::State& state) {
  constexpr int kBatch = 256;
  lob::MatchingEngine engine(
      {.books = {kBook}, .time_one_in = static_cast<std::uint32_t>(state.range(0))});
  std::vector<OrderId> resting;
  resting.reserve(kBatch);
  std::uint64_t tag = 0;

  for (auto _ : state) {
    for (int i = 0; i < kBatch; ++i) {
      bool sent = engine.submit(tag++, 0, Side::Buy, kPrice, 100);
      benchmark::DoNotOptimize(sent);
    }
    engine.process_pending();
    engine.poll([&](const Event& event) { resting.push_back(event.order_id); });
    for (const OrderId id : resting) {
      bool sent = engine.cancel(tag++, 0, id);
      benchmark::DoNotOptimize(sent);
    }
    engine.process_pending();
    engine.poll([](const Event&) {});
    resting.clear();
  }
  state.SetItemsProcessed(state.iterations() * 2 * kBatch);
}
BENCHMARK(BM_Engine_PerCommand)->Arg(0)->Arg(64)->Arg(1);

// The same loop with the market data feed off (0) and on (1). Every command
// here changes the quantity at the best price, so with the feed on each one
// publishes two messages: the price's new total and the new best prices. That
// is the feed at its most expensive.
void BM_Engine_MarketDataFeed(benchmark::State& state) {
  constexpr int kBatch = 256;
  const bool feed_on = state.range(0) != 0;
  lob::MatchingEngine engine(
      {.books = {kBook}, .market_data_capacity = feed_on ? std::size_t{1} << 12 : 0});
  std::vector<OrderId> resting;
  resting.reserve(kBatch);
  std::uint64_t tag = 0;
  std::uint64_t messages = 0;
  const auto read_feed = [&] {
    messages += engine.poll_market_data([](const lob::MarketData&) {});
  };

  for (auto _ : state) {
    for (int i = 0; i < kBatch; ++i) {
      bool sent = engine.submit(tag++, 0, Side::Buy, kPrice, 100);
      benchmark::DoNotOptimize(sent);
    }
    engine.process_pending();
    engine.poll([&](const Event& event) { resting.push_back(event.order_id); });
    read_feed();
    for (const OrderId id : resting) {
      bool sent = engine.cancel(tag++, 0, id);
      benchmark::DoNotOptimize(sent);
    }
    engine.process_pending();
    engine.poll([](const Event&) {});
    read_feed();
    resting.clear();
  }
  state.SetItemsProcessed(state.iterations() * 2 * kBatch);
  state.counters["messages_per_command"] =
      static_cast<double>(messages) / static_cast<double>(state.iterations() * 2 * kBatch);
}
BENCHMARK(BM_Engine_MarketDataFeed)->Arg(0)->Arg(1);

}  // namespace

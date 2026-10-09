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

}  // namespace

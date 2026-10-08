// The whole pipeline: commands cross one ring to the engine thread, and the
// events they cause cross another ring back.
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

#include "lob/matching_engine.hpp"

namespace {

using lob::Event;
using lob::EventType;
using lob::OrderId;
using lob::Side;

constexpr lob::BookConfig kBook{
    .symbol = "BENCH", .min_price = 0, .num_levels = 1 << 16, .max_orders = 1 << 20};
constexpr lob::Price kPrice = 30'000;

// One command at a time: send it, then wait for its answer. The time per
// iteration is the full round trip through both rings and the book. Commands
// alternate between adding an order and cancelling it.
void BM_Engine_RoundTrip(benchmark::State& state) {
  lob::MatchingEngine engine({.book = kBook});
  engine.start();
  OrderId resting = lob::kInvalidOrderId;
  std::uint64_t tag = 0;

  for (auto _ : state) {
    if (resting == lob::kInvalidOrderId) {
      while (!engine.submit(tag, Side::Buy, kPrice, 100)) {
      }
    } else {
      while (!engine.cancel(tag, resting)) {
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

// Sustained throughput: commands are sent without waiting for their answers,
// and events are read as they arrive. An order is cancelled as soon as its ID
// is known, so the book stays small.
void BM_Engine_Throughput(benchmark::State& state) {
  lob::MatchingEngine engine({.book = kBook});
  engine.start();
  std::vector<OrderId> to_cancel;
  to_cancel.reserve(1 << 17);
  std::uint64_t sent = 0;
  std::uint64_t answered = 0;
  const auto on_event = [&](const Event& event) {
    if (event.type == EventType::Accepted) {
      to_cancel.push_back(event.order_id);
    }
    answered += event.type != EventType::Trade ? 1 : 0;
  };

  for (auto _ : state) {
    bool pushed = false;
    while (!pushed) {
      if (!to_cancel.empty()) {
        pushed = engine.cancel(sent, to_cancel.back());
        if (pushed) {
          to_cancel.pop_back();
        }
      } else {
        pushed = engine.submit(sent, Side::Buy, kPrice, 100);
      }
      engine.poll(on_event);
    }
    ++sent;
  }
  while (answered < sent) {
    engine.poll(on_event);
  }
  engine.stop();
  state.SetItemsProcessed(static_cast<std::int64_t>(sent));
}
BENCHMARK(BM_Engine_Throughput)->UseRealTime();

}  // namespace

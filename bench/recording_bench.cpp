// What recording a session costs the thread sending commands, and how fast a
// recording replays.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "lob/matching_engine.hpp"
#include "lob/replay.hpp"

namespace {

using lob::Command;
using lob::CommandType;
using lob::Event;
using lob::EventType;
using lob::OrderId;
using lob::Side;

constexpr lob::Price kPrice = 30'000;
constexpr lob::BookConfig kBook{
    .symbol = "BENCH", .min_price = 0, .num_levels = 1 << 16, .max_orders = 1 << 16};

// Somewhere to record to that costs nothing to write: these benchmarks are
// about the sender, and at tens of millions of commands a second a real file
// would be a gigabyte before the benchmark was over.
const char* const kNowhere = "/dev/null";

// The engine's cost per command on one thread, with recording off (0) and on
// (1). The loop is the same as BM_Engine_PerCommand: 256 orders, then 256
// cancels. With recording on, each command is also copied into the
// recorder's ring, and the recorder's own thread is taking them out again.
void BM_Recording_PerCommand(benchmark::State& state) {
  constexpr int kBatch = 256;
  lob::MatchingEngine engine(
      {.books = {kBook}, .record_to = state.range(0) != 0 ? kNowhere : ""});
  std::vector<OrderId> resting;
  resting.reserve(kBatch);
  std::uint64_t tag = 0;
  std::uint64_t refused = 0;

  for (auto _ : state) {
    for (int i = 0; i < kBatch; ++i) {
      if (!engine.submit(tag++, 0, Side::Buy, kPrice, 100)) {
        ++refused;
      }
    }
    engine.process_pending();
    engine.poll([&](const Event& event) { resting.push_back(event.order_id); });
    for (const OrderId id : resting) {
      if (!engine.cancel(tag++, 0, id)) {
        ++refused;
      }
    }
    engine.process_pending();
    engine.poll([](const Event&) {});
    resting.clear();
  }
  state.SetItemsProcessed(state.iterations() * 2 * kBatch);
  // Commands turned away because the recorder had no room. Not zero would
  // mean the numbers above are for a different workload than they claim.
  state.counters["refused"] = static_cast<double>(refused);
}
BENCHMARK(BM_Recording_PerCommand)->Arg(0)->Arg(1);

// The whole pipeline with an engine thread, as in BM_Engine_Throughput with
// one symbol: recording off (0) and on (1).
void BM_Recording_Throughput(benchmark::State& state) {
  lob::MatchingEngine engine(
      {.books = {kBook}, .record_to = state.range(0) != 0 ? kNowhere : ""});
  engine.start();
  std::uint64_t sent = 0;
  std::uint64_t answered = 0;
  const auto on_event = [&](const Event& event) {
    if (event.type != EventType::Trade) {
      ++answered;
    }
  };

  for (auto _ : state) {
    const Side side = (sent & 1) == 0 ? Side::Sell : Side::Buy;
    while (!engine.submit(sent, 0, side, kPrice, 100)) {
      engine.poll(on_event);
    }
    engine.poll(on_event);
    ++sent;
  }
  while (answered < sent) {
    engine.poll(on_event);
  }
  engine.stop();
  state.SetItemsProcessed(static_cast<std::int64_t>(sent));
}
BENCHMARK(BM_Recording_Throughput)->Arg(0)->Arg(1)->UseRealTime();

// Recording to a real file: two million commands, 64 MB, written by the
// recorder's thread while this one sends. The time includes waiting for the
// last of them to reach the file.
void BM_Recording_ToFile(benchmark::State& state) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "lob_recording_bench.rec").string();
  {
    lob::MatchingEngine engine({.books = {kBook}, .record_to = path});
    std::uint64_t sent = 0;
    for (auto _ : state) {
      const Side side = (sent & 1) == 0 ? Side::Sell : Side::Buy;
      while (!engine.submit(sent, 0, side, kPrice, 100)) {
        engine.process_pending();
        engine.poll([](const Event&) {});
      }
      if ((++sent & 255) == 0) {
        engine.process_pending();
        engine.poll([](const Event&) {});
      }
    }
    engine.flush_recording();
    state.SetItemsProcessed(static_cast<std::int64_t>(sent));
    state.SetBytesProcessed(static_cast<std::int64_t>(sent * sizeof(lob::RecordedCommand)));
    state.counters["recording"] = engine.recording() ? 1.0 : 0.0;
  }
  std::remove(path.c_str());
}
BENCHMARK(BM_Recording_ToFile)->Iterations(1 << 21)->UseRealTime();

// Replaying a session that is already in memory into an engine run by hand:
// 65,536 commands, a sell that rests and then a buy that fills it, over and
// over. See `items_per_second` for commands replayed per second.
void BM_Replay_ByHand(benchmark::State& state) {
  constexpr std::uint64_t kCommands = 1 << 16;
  std::vector<Command> commands;
  commands.reserve(kCommands);
  for (std::uint64_t tag = 0; tag < kCommands; ++tag) {
    Command command{};
    command.client_tag = tag;
    command.price = kPrice;
    command.quantity = 100;
    command.side = (tag & 1) == 0 ? Side::Sell : Side::Buy;
    command.type = CommandType::Submit;
    commands.push_back(command);
  }
  lob::MatchingEngine engine({.books = {kBook}});
  std::uint64_t events = 0;

  for (auto _ : state) {
    lob::ReplayResult result = lob::replay(commands, engine, [&](const Event&) { ++events; });
    benchmark::DoNotOptimize(result);
  }
  benchmark::DoNotOptimize(events);
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kCommands));
}
BENCHMARK(BM_Replay_ByHand);

}  // namespace

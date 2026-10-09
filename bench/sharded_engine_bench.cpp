// Does adding engine threads add capacity? Each shard gets a feeder thread of
// its own, which sends a fixed workload and reads that shard's events.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "lob/sharded_engine.hpp"

namespace {

using lob::Event;
using lob::EventType;
using lob::SendStatus;
using lob::Side;
using lob::SymbolId;

constexpr lob::BookConfig kBook{
    .symbol = "BENCH", .min_price = 0, .num_levels = 1 << 16, .max_orders = 1 << 16};
constexpr lob::Price kPrice = 30'000;

// The work of one feeder: a sell that rests, then a buy that fills it, for the
// one symbol on its shard, reading that shard's events as it goes.
class Feeder {
 public:
  Feeder(lob::ShardedEngine& engine, std::size_t shard)
      : engine_(engine), shard_(shard), symbol_(static_cast<SymbolId>(shard)) {}

  void send_one() noexcept {
    const Side side = (sent_ & 1) == 0 ? Side::Sell : Side::Buy;
    while (engine_.submit(sent_, symbol_, side, kPrice, 100) != SendStatus::Sent) {
      read_events();
    }
    read_events();
    ++sent_;
  }

  void wait_for_answers() noexcept {
    while (answered_ < sent_) {
      read_events();
    }
  }

  [[nodiscard]] std::uint64_t sent() const noexcept { return sent_; }

 private:
  void read_events() noexcept {
    engine_.poll(shard_, [this](const Event& event) {
      if (event.type != EventType::Trade) {
        ++answered_;
      }
    });
  }

  lob::ShardedEngine& engine_;
  std::size_t shard_;
  SymbolId symbol_;
  std::uint64_t sent_ = 0;
  std::uint64_t answered_ = 0;
};

// The argument is the number of shards. There is one symbol and one feeder per
// shard, so N shards means 2N busy threads. The benchmark's own thread feeds
// shard 0 and the timing loop runs there; the other feeders run for the same
// length of time. `items_per_second` is the total across all shards.
void BM_Sharded_Throughput(benchmark::State& state) {
  const auto shards = static_cast<std::size_t>(state.range(0));
  lob::ShardedEngine engine(
      {.books = std::vector<lob::BookConfig>(shards, kBook), .shards = shards});
  engine.start();

  std::atomic<bool> go{false};
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> sent_by_others{0};
  std::vector<std::thread> others;
  for (std::size_t shard = 1; shard < shards; ++shard) {
    others.emplace_back([&, shard] {
      Feeder feeder(engine, shard);
      while (!go.load(std::memory_order_acquire)) {
      }
      while (!done.load(std::memory_order_relaxed)) {
        feeder.send_one();
      }
      feeder.wait_for_answers();
      sent_by_others.fetch_add(feeder.sent(), std::memory_order_relaxed);
    });
  }

  Feeder feeder(engine, 0);
  go.store(true, std::memory_order_release);
  for (auto _ : state) {
    feeder.send_one();
  }
  done.store(true, std::memory_order_relaxed);
  feeder.wait_for_answers();
  for (std::thread& other : others) {
    other.join();
  }
  engine.stop();

  state.SetItemsProcessed(static_cast<std::int64_t>(feeder.sent() + sent_by_others.load()));
}
BENCHMARK(BM_Sharded_Throughput)->Arg(1)->Arg(2)->Arg(4)->UseRealTime();

// How long one move takes when both shards are otherwise idle: the "let go"
// and "take up" commands crossing the command rings, the old shard's marker
// crossing its event ring, and the new shard picking the book up. A symbol is
// passed back and forth between two shards.
void BM_Sharded_MoveSymbol(benchmark::State& state) {
  lob::ShardedEngine engine({.books = std::vector<lob::BookConfig>(2, kBook), .shards = 2});
  engine.start();
  const auto read_events = [&] { engine.poll([](const Event&) {}); };
  std::size_t destination = 1;

  for (auto _ : state) {
    while (engine.move_symbol(0, destination) != SendStatus::Sent) {
      read_events();
    }
    while (engine.move_in_progress(0)) {
      read_events();  // the move cannot finish until the marker has been read
    }
    destination = 1 - destination;
  }
  engine.stop();
}
BENCHMARK(BM_Sharded_MoveSymbol)->UseRealTime();

// What one look by rebalance() costs: reading every symbol's counter on every
// shard, working out each shard's load and deciding. The argument is the
// number of symbols, spread over four shards. The shards are idle and even, so
// no move is ever started.
void BM_Sharded_RebalanceLook(benchmark::State& state) {
  const auto symbols = static_cast<std::size_t>(state.range(0));
  const lob::BookConfig small_book{
      .symbol = "BENCH", .min_price = 0, .num_levels = 64, .max_orders = 16};
  lob::ShardedEngine engine({.books = std::vector<lob::BookConfig>(symbols, small_book),
                             .shards = 4,
                             .rebalance = {.min_sample = 0}});
  for (auto _ : state) {
    auto moved = engine.rebalance();
    benchmark::DoNotOptimize(moved);
  }
}
BENCHMARK(BM_Sharded_RebalanceLook)->Arg(16)->Arg(256)->Arg(4096);

}  // namespace

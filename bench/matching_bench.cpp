#include <cstdint>

#include <benchmark/benchmark.h>

#include "lob/order_book.hpp"

namespace {

constexpr lob::BookConfig kConfig{
    .symbol = "BENCH", .min_price = 0, .num_levels = 1 << 16, .max_orders = 1 << 16};

// One order rests, then one incoming order fully fills it.
void BM_Match_RestThenFill(benchmark::State& state) {
  lob::OrderBook book(kConfig);
  std::uint64_t traded = 0;
  const auto on_trade = [&](const lob::Trade& trade) { traded += trade.quantity; };

  for (auto _ : state) {
    lob::SubmitResult maker = book.submit(lob::Side::Sell, 30'000, 100, on_trade);
    benchmark::DoNotOptimize(maker);
    lob::SubmitResult taker = book.submit(lob::Side::Buy, 30'000, 100, on_trade);
    benchmark::DoNotOptimize(taker);
  }
  benchmark::DoNotOptimize(traded);
}
BENCHMARK(BM_Match_RestThenFill);

// N orders rest on N consecutive price levels, then one incoming order sweeps
// them all. Reported per order: one rest plus one fill.
void BM_Match_SweepLevels(benchmark::State& state) {
  const std::int64_t levels = state.range(0);
  lob::OrderBook book(kConfig);
  std::uint64_t traded = 0;
  const auto on_trade = [&](const lob::Trade& trade) { traded += trade.quantity; };

  for (auto _ : state) {
    for (std::int64_t i = 0; i < levels; ++i) {
      lob::SubmitResult maker = book.submit(lob::Side::Sell, 30'000 + i, 100, on_trade);
      benchmark::DoNotOptimize(maker);
    }
    lob::SubmitResult taker = book.submit(lob::Side::Buy, 30'000 + levels,
                                          static_cast<lob::Quantity>(100 * levels), on_trade);
    benchmark::DoNotOptimize(taker);
  }
  benchmark::DoNotOptimize(traded);
  state.SetItemsProcessed(state.iterations() * levels);
}
BENCHMARK(BM_Match_SweepLevels)->Arg(1)->Arg(8)->Arg(64);

}  // namespace

#include <benchmark/benchmark.h>

#include "lob/order_book.hpp"

static void BM_OrderBook_AddCancel(benchmark::State& state) {
  lob::OrderBook book(
      {.symbol = "BENCH", .min_price = 0, .num_levels = 1 << 16, .max_orders = 1 << 16});
  for (auto _ : state) {
    lob::OrderId id = book.add(lob::Side::Buy, 30'000, 100);
    benchmark::DoNotOptimize(id);
    book.cancel(id);
  }
}
BENCHMARK(BM_OrderBook_AddCancel);

// The worst case for a linear scan: when the best bid is cancelled, the next
// best is 200,000 empty price levels away. The bitmap finds it without scanning.
static void BM_OrderBook_BestBidAcrossSparseBook(benchmark::State& state) {
  lob::OrderBook book({.symbol = "BENCH",
                       .min_price = 0,
                       .num_levels = lob::LevelBitmap::kMaxBits,
                       .max_orders = 1024});
  lob::OrderId far_bid = book.add(lob::Side::Buy, 1, 100);
  benchmark::DoNotOptimize(far_bid);
  for (auto _ : state) {
    lob::OrderId top_bid = book.add(lob::Side::Buy, 200'000, 100);
    benchmark::DoNotOptimize(book.best_bid());
    book.cancel(top_bid);
    benchmark::DoNotOptimize(book.best_bid());
  }
}
BENCHMARK(BM_OrderBook_BestBidAcrossSparseBook);

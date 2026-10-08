#include <benchmark/benchmark.h>

#include "lob/order_pool.hpp"

static void BM_OrderPool_AllocateDeallocate(benchmark::State& state) {
  lob::OrderPool pool(1 << 16);
  for (auto _ : state) {
    lob::OrderIndex index = pool.allocate();
    benchmark::DoNotOptimize(index);
    pool.deallocate(index);
  }
}
BENCHMARK(BM_OrderPool_AllocateDeallocate);

// Baseline: the general-purpose heap serving the same 64-byte object.
static void BM_Heap_NewDelete(benchmark::State& state) {
  for (auto _ : state) {
    auto* order = new lob::Order;
    benchmark::DoNotOptimize(order);
    delete order;
  }
}
BENCHMARK(BM_Heap_NewDelete);

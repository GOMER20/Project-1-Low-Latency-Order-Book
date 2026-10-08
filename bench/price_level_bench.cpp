#include <list>

#include <benchmark/benchmark.h>

#include "lob/price_level.hpp"

// Add an order to the back of the queue, then cancel it: the two operations
// that dominate real order flow.
static void BM_IntrusiveLevel_AddCancel(benchmark::State& state) {
  lob::OrderPool pool(1 << 16);
  lob::PriceLevel level;
  for (auto _ : state) {
    const lob::OrderIndex index = pool.allocate();
    pool[index].quantity = 100;
    level.push_back(pool, index);
    benchmark::DoNotOptimize(level);
    level.erase(pool, index);
    pool.deallocate(index);
  }
}
BENCHMARK(BM_IntrusiveLevel_AddCancel);

// Baseline: the same workload on std::list, which heap-allocates a node per order.
static void BM_StdList_AddCancel(benchmark::State& state) {
  std::list<lob::Order> queue;
  lob::Order order{};
  order.quantity = 100;
  for (auto _ : state) {
    queue.push_back(order);
    auto position = std::prev(queue.end());
    benchmark::DoNotOptimize(position);
    queue.erase(position);
  }
}
BENCHMARK(BM_StdList_AddCancel);

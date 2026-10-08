// Headline latency numbers, measured on a deep book whose orders and free
// slots are scattered through memory, as they would be after a long session.
// The other benchmark files time the building blocks on a small, hot book.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

#include "lob/order_book.hpp"

namespace {

using lob::OrderBook;
using lob::OrderId;
using lob::Price;
using lob::Side;

constexpr lob::BookConfig kConfig{
    .symbol = "BENCH", .min_price = 0, .num_levels = 1 << 16, .max_orders = 1 << 20};
constexpr Price kMid = 1 << 15;
constexpr Price kDepth = 256;              // price levels in use on each side of the mid
constexpr std::size_t kResting = 1 << 16;  // the book holds between 1x and 2x this many orders
constexpr lob::Quantity kLot = 100;

// xorshift64*: a few cycles per number, so generating order flow inside the
// timed loop does not distort the result.
struct Rng {
  using result_type = std::uint64_t;
  static constexpr result_type min() noexcept { return 0; }
  static constexpr result_type max() noexcept { return ~result_type{0}; }

  result_type operator()() noexcept {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
  }

  result_type state = 0x9E3779B97F4A7C15ULL;
};

const auto kIgnoreTrades = [](const lob::Trade&) {};

// A resting order: bids go below the mid and asks above it, so it never trades.
lob::SubmitResult submit_passive(OrderBook& book, std::uint64_t random) {
  const Side side = (random & 1) != 0 ? Side::Buy : Side::Sell;
  const Price offset = 1 + static_cast<Price>((random >> 1) % static_cast<std::uint64_t>(kDepth));
  return book.submit(side, side == Side::Buy ? kMid - offset : kMid + offset, kLot, kIgnoreTrades);
}

// An incoming order priced to trade with exactly one resting order.
lob::SubmitResult submit_aggressive(OrderBook& book, std::uint64_t random) {
  const bool buy = (random & 1) != 0;
  return book.submit(buy ? Side::Buy : Side::Sell, buy ? kMid + kDepth : kMid - kDepth, kLot,
                     kIgnoreTrades);
}

// Tops the book up to 2 * kResting orders and shuffles the IDs, so later
// cancels hit orders in an order unrelated to where they sit in memory.
void refill(OrderBook& book, std::vector<OrderId>& ids, Rng& rng) {
  while (ids.size() < 2 * kResting) {
    ids.push_back(submit_passive(book, rng()).id);
  }
  std::shuffle(ids.begin(), ids.end(), rng);
}

// Cancels a random half of the book, leaving kResting orders and a pool whose
// free slots are scattered.
void trim(OrderBook& book, std::vector<OrderId>& ids, Rng& rng) {
  std::shuffle(ids.begin(), ids.end(), rng);
  while (ids.size() > kResting) {
    book.cancel(ids.back());
    ids.pop_back();
  }
}

// Insert one resting order into a book already holding 65k-131k orders.
void BM_Insert_DeepBook(benchmark::State& state) {
  OrderBook book(kConfig);
  Rng rng;
  std::vector<OrderId> ids;
  ids.reserve(2 * kResting);
  refill(book, ids, rng);
  trim(book, ids, rng);

  for (auto _ : state) {
    if (ids.size() == 2 * kResting) [[unlikely]] {
      state.PauseTiming();
      trim(book, ids, rng);
      state.ResumeTiming();
    }
    lob::SubmitResult result = submit_passive(book, rng());
    benchmark::DoNotOptimize(result);
    ids.push_back(result.id);
  }
}
BENCHMARK(BM_Insert_DeepBook);

// Cancel one randomly chosen order from a book holding 65k-131k orders.
void BM_Cancel_DeepBook(benchmark::State& state) {
  OrderBook book(kConfig);
  Rng rng;
  std::vector<OrderId> ids;
  ids.reserve(2 * kResting);
  refill(book, ids, rng);

  for (auto _ : state) {
    if (ids.size() == kResting) [[unlikely]] {
      state.PauseTiming();
      refill(book, ids, rng);
      state.ResumeTiming();
    }
    bool cancelled = book.cancel(ids.back());
    benchmark::DoNotOptimize(cancelled);
    ids.pop_back();
  }
}
BENCHMARK(BM_Cancel_DeepBook);

// One incoming order that crosses the spread and fully fills one resting
// order, against a book holding 65k-131k orders.
void BM_Match_DeepBook(benchmark::State& state) {
  OrderBook book(kConfig);
  Rng rng;
  const auto top_up = [&] {
    while (book.size() < 2 * kResting) {
      (void)submit_passive(book, rng());
    }
  };
  top_up();

  for (auto _ : state) {
    if (book.size() <= kResting) [[unlikely]] {
      state.PauseTiming();
      top_up();
      state.ResumeTiming();
    }
    lob::SubmitResult result = submit_aggressive(book, rng());
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_Match_DeepBook);

// A steady-state mix of order flow on a deep book: 48% new resting orders,
// 48% cancels of a randomly chosen order, 4% incoming orders that trade.
// A few percent of the cancels arrive after their order has already traded,
// as happens in a real market.
class MixedFlow {
 public:
  MixedFlow() : book_(kConfig) {
    ids_.reserve(4 * kResting);
    while (ids_.size() < kResting) {
      ids_.push_back(submit_passive(book_, rng_()).id);
    }
  }

  void step() noexcept {
    const std::uint64_t random = rng_();
    const std::uint64_t roll = (random >> 40) % 100;
    if (roll < 48 || ids_.empty()) {
      ids_.push_back(submit_passive(book_, random).id);
    } else if (roll < 96) {
      // Multiply-shift picks an index without a 64-bit division.
      const std::size_t pick =
          static_cast<std::size_t>((static_cast<std::uint32_t>(random) * ids_.size()) >> 32);
      book_.cancel(ids_[pick]);
      ids_[pick] = ids_.back();
      ids_.pop_back();
    } else {
      lob::SubmitResult result = submit_aggressive(book_, random);
      benchmark::DoNotOptimize(result);
    }
  }

 private:
  OrderBook book_;
  Rng rng_;
  std::vector<OrderId> ids_;
};

void BM_MixedFlow(benchmark::State& state) {
  MixedFlow flow;
  for (auto _ : state) {
    flow.step();
  }
}
BENCHMARK(BM_MixedFlow);

// Tail latency of the same mix. Each operation is timed individually, so every
// sample (and the mean reported for this benchmark) includes the cost of two
// clock reads, typically 20-50 ns. The percentiles are an upper bound.
void BM_MixedFlow_Percentiles(benchmark::State& state) {
  using Clock = std::chrono::steady_clock;
  constexpr std::size_t kMaxNanos = 4'096;

  MixedFlow flow;
  std::array<std::uint64_t, kMaxNanos + 1> histogram{};  // 1 ns buckets; last one is overflow
  std::uint64_t samples = 0;

  for (auto _ : state) {
    const Clock::time_point start = Clock::now();
    flow.step();
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    ++histogram[std::min(static_cast<std::size_t>(nanos), kMaxNanos)];
    ++samples;
  }

  const auto percentile = [&](double fraction) {
    const auto target = static_cast<std::uint64_t>(fraction * static_cast<double>(samples));
    std::uint64_t seen = 0;
    for (std::size_t nanos = 0; nanos <= kMaxNanos; ++nanos) {
      seen += histogram[nanos];
      if (seen > target) {
        return static_cast<double>(nanos);
      }
    }
    return static_cast<double>(kMaxNanos);
  };
  state.counters["p50_ns"] = percentile(0.50);
  state.counters["p99_ns"] = percentile(0.99);
  state.counters["p99.9_ns"] = percentile(0.999);
}
BENCHMARK(BM_MixedFlow_Percentiles);

}  // namespace

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "lob/spsc_ring.hpp"
#include "lob/trade.hpp"

namespace {

constexpr std::size_t kCapacity = 1 << 16;

// Baseline: the same bounded ring, protected by a mutex.
template <typename T>
class MutexRing {
 public:
  explicit MutexRing(std::size_t capacity) : slots_(capacity) {}

  bool try_push(const T& value) {
    const std::lock_guard lock(mutex_);
    if (tail_ - head_ == slots_.size()) {
      return false;
    }
    slots_[tail_++ % slots_.size()] = value;
    return true;
  }

  bool try_pop(T& out) {
    const std::lock_guard lock(mutex_);
    if (head_ == tail_) {
      return false;
    }
    out = slots_[head_++ % slots_.size()];
    return true;
  }

 private:
  std::mutex mutex_;
  std::vector<T> slots_;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
};

// The cost of the two operations themselves, with no second thread involved.
void BM_SpscRing_PushPop_OneThread(benchmark::State& state) {
  lob::SpscRing<lob::Trade> ring(kCapacity);
  lob::Trade trade{};
  for (auto _ : state) {
    bool pushed = ring.try_push(trade);
    benchmark::DoNotOptimize(pushed);
    bool popped = ring.try_pop(trade);
    benchmark::DoNotOptimize(popped);
  }
}
BENCHMARK(BM_SpscRing_PushPop_OneThread);

// One thread pushes Trade events as fast as it can while this thread pops
// them. The time per iteration is the time per event delivered end to end.
template <typename Queue>
void transfer_between_two_threads(benchmark::State& state) {
  Queue queue(kCapacity);
  std::atomic<bool> stop{false};

  std::thread producer([&] {
    lob::Trade trade{};
    while (!stop.load(std::memory_order_relaxed)) {
      if (queue.try_push(trade)) {
        ++trade.maker_id;
      }
    }
  });

  lob::Trade trade{};
  std::uint64_t checksum = 0;
  for (auto _ : state) {
    while (!queue.try_pop(trade)) {
    }
    checksum += trade.maker_id;
  }

  stop.store(true, std::memory_order_relaxed);
  producer.join();
  benchmark::DoNotOptimize(checksum);
  state.SetItemsProcessed(state.iterations());
}

void BM_SpscRing_TwoThreads(benchmark::State& state) {
  transfer_between_two_threads<lob::SpscRing<lob::Trade>>(state);
}
BENCHMARK(BM_SpscRing_TwoThreads)->UseRealTime();

void BM_MutexRing_TwoThreads(benchmark::State& state) {
  transfer_between_two_threads<MutexRing<lob::Trade>>(state);
}
BENCHMARK(BM_MutexRing_TwoThreads)->UseRealTime();

}  // namespace

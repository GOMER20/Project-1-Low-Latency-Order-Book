#include <cstddef>
#include <thread>

#include <gtest/gtest.h>

#include "lob/matching_engine.hpp"
#include "lob/thread_affinity.hpp"

#if defined(__linux__)
#include <sched.h>
#endif

namespace {

using lob::Event;
using lob::MatchingEngine;
using lob::Side;

constexpr lob::BookConfig kBook{
    .symbol = "TEST", .min_price = 100, .num_levels = 100, .max_orders = 64};

// A CPU this process is certainly allowed to run on: the one it is on now.
int a_usable_cpu() {
#if defined(__linux__)
  return sched_getcpu();
#else
  return 0;
#endif
}

// Sends one order and waits for its answer, to show the engine is alive.
bool engine_answers(MatchingEngine& engine) {
  if (!engine.submit(1, Side::Buy, 150, 10)) {
    return false;
  }
  std::size_t answers = 0;
  for (int attempt = 0; attempt < 10'000'000 && answers == 0; ++attempt) {
    engine.poll([&](const Event&) { ++answers; });
    std::this_thread::yield();
  }
  return answers == 1;
}

TEST(ThreadAffinity, RefusesCpusThatCannotExist) {
  EXPECT_FALSE(lob::pin_current_thread_to_cpu(-1));
  EXPECT_FALSE(lob::pin_current_thread_to_cpu(1 << 20));
}

#if defined(__linux__)
// Done on a throwaway thread so the test runner's own thread is left alone.
TEST(ThreadAffinity, PinnedThreadStaysOnItsCpu) {
  bool pinned = false;
  int cpu_before = -1;
  int cpu_after = -1;

  std::thread worker([&] {
    cpu_before = sched_getcpu();
    pinned = lob::pin_current_thread_to_cpu(cpu_before);
    for (int i = 0; i < 100; ++i) {
      std::this_thread::yield();  // give the scheduler every chance to move us
    }
    cpu_after = sched_getcpu();
  });
  worker.join();

  EXPECT_TRUE(pinned);
  EXPECT_EQ(cpu_after, cpu_before);
}
#endif

TEST(EnginePinning, UnpinnedByDefault) {
  MatchingEngine engine({.book = kBook, .idle = lob::IdleStrategy::Yield});
  engine.start();
  EXPECT_FALSE(engine.pinned());
  EXPECT_TRUE(engine_answers(engine));
  engine.stop();
}

// On Linux the engine must end up pinned; elsewhere pinning is unavailable and
// the engine must say so. It has to work in both cases.
TEST(EnginePinning, ReportsWhetherThePinTookEffect) {
  MatchingEngine engine({.book = kBook,
                         .idle = lob::IdleStrategy::Yield,
                         .pin_to_cpu = a_usable_cpu()});
  engine.start();
  EXPECT_EQ(engine.pinned(), lob::kCanPinThreads);
  EXPECT_TRUE(engine_answers(engine));
  engine.stop();
}

TEST(EnginePinning, RunsUnpinnedIfTheCpuDoesNotExist) {
  MatchingEngine engine(
      {.book = kBook, .idle = lob::IdleStrategy::Yield, .pin_to_cpu = 1 << 20});
  engine.start();
  EXPECT_FALSE(engine.pinned());
  EXPECT_TRUE(engine_answers(engine));
  engine.stop();
}

}  // namespace

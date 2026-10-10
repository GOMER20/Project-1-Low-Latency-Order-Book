// Proves the central claim of the design: once a book is constructed, no
// operation on it touches the heap. Global operator new is replaced with a
// counting version.
//
// That replacement applies to the whole program, so this file is built into
// a test binary of its own. It is left out of sanitizer builds, because the
// sanitizer runtimes replace operator new themselves.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include "lob/matching_engine.hpp"
#include "lob/order_book.hpp"
#include "lob/recording.hpp"
#include "lob/replay.hpp"

namespace {

std::atomic<std::size_t> g_allocations{0};

void* counted_allocate(std::size_t size) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size != 0 ? size : 1)) {
    return memory;
  }
  throw std::bad_alloc();
}

void* counted_allocate(std::size_t size, std::align_val_t alignment) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  void* memory = nullptr;
  if (posix_memalign(&memory, static_cast<std::size_t>(alignment), size != 0 ? size : 1) == 0) {
    return memory;
  }
  throw std::bad_alloc();
}

}  // namespace

// Every form the engine can reach is replaced explicitly, rather than relying
// on the array forms to forward to the scalar ones.
void* operator new(std::size_t size) { return counted_allocate(size); }
void* operator new[](std::size_t size) { return counted_allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return counted_allocate(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return counted_allocate(size, alignment);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

namespace {

using lob::OrderBook;
using lob::OrderId;
using lob::Side;

TEST(HotPath, ConstructionIsTheOnlyTimeTheBookAllocates) {
  const std::size_t before_construction = g_allocations.load();
  OrderBook book({.symbol = "ALLOC", .min_price = 0, .num_levels = 1'024, .max_orders = 256});
  const std::size_t after_construction = g_allocations.load();
  ASSERT_GT(after_construction, before_construction);  // the counter really is wired in

  // Fixed storage for IDs and trades, so the test itself does not allocate.
  std::array<OrderId, 256> ids{};
  std::size_t id_count = 0;
  std::size_t trade_count = 0;
  const auto on_trade = [&](const lob::Trade&) { ++trade_count; };

  for (int round = 0; round < 200; ++round) {
    // Fill the book to capacity with resting orders on both sides.
    id_count = 0;
    for (int i = 0; i < 128; ++i) {
      ids[id_count++] = book.submit(Side::Buy, 400 + (i % 50), 10, on_trade).id;
      ids[id_count++] = book.submit(Side::Sell, 600 + (i % 50), 10, on_trade).id;
    }
    // A full book rejects further orders.
    (void)book.submit(Side::Buy, 100, 10, on_trade);
    // Cancel a third of them, some twice.
    for (std::size_t i = 0; i < id_count; i += 3) {
      (void)book.cancel(ids[i]);
      (void)book.cancel(ids[i]);
    }
    // Sweep each side with one large incoming order, then clear what is left.
    const OrderId buy = book.submit(Side::Buy, 1'000, 100'000, on_trade).id;
    (void)book.cancel(buy);
    const OrderId sell = book.submit(Side::Sell, 0, 100'000, on_trade).id;
    (void)book.cancel(sell);
    // Queries and direct executions.
    (void)book.best_bid();
    (void)book.best_ask();
    (void)book.quantity_at(Side::Buy, 400);
    (void)book.execute(ids[1], 5);
  }

  EXPECT_EQ(g_allocations.load(), after_construction);
  EXPECT_GT(trade_count, 0u);
  EXPECT_TRUE(book.empty());
}

// Recording adds a ring and a writer thread to the engine, and neither may
// touch the heap once the engine is built. The count covers every thread in
// the program, so this holds the writer thread to it as well as the sender.
// Replaying a recording that is already in memory must not allocate either.
TEST(HotPath, RecordingAndReplayingASessionAllocateNothing) {
  constexpr std::uint64_t kCommands = 50'000;
  const std::string path =
      ::testing::TempDir() + "lob_" + std::to_string(::getpid()) + "_allocation_test.rec";
  const lob::BookConfig book{
      .symbol = "ALLOC", .min_price = 0, .num_levels = 1'024, .max_orders = 256};
  std::uint64_t events = 0;
  const auto count_event = [&](const lob::Event&) { ++events; };

  {
    lob::MatchingEngine engine({.books = {book}, .record_to = path});
    ASSERT_TRUE(engine.recording());
    const std::size_t before = g_allocations.load();
    for (std::uint64_t tag = 0; tag < kCommands; ++tag) {
      const Side side = tag % 2 == 0 ? Side::Buy : Side::Sell;
      while (!engine.submit(tag, 0, side, 500, 10)) {
        engine.process_pending();
        engine.poll(count_event);
      }
      if (tag % 64 == 0) {
        engine.process_pending();
        engine.poll(count_event);
      }
      if (tag % 10'000 == 0) {
        engine.flush_recording();
      }
    }
    engine.process_pending();
    engine.poll(count_event);
    engine.flush_recording();
    EXPECT_EQ(g_allocations.load(), before);
    EXPECT_EQ(engine.commands_recorded(), kCommands);
  }

  const std::optional<lob::Recording> recording = lob::Recording::load(path);
  std::remove(path.c_str());
  ASSERT_TRUE(recording.has_value());
  ASSERT_EQ(recording->commands().size(), kCommands);

  lob::MatchingEngine again({.books = recording->books()});
  std::uint64_t replayed_events = 0;
  const std::size_t before = g_allocations.load();
  const lob::ReplayResult result = lob::replay(
      recording->commands(), again, [&](const lob::Event&) { ++replayed_events; });
  EXPECT_EQ(g_allocations.load(), before);
  EXPECT_EQ(result.commands, kCommands);
  EXPECT_EQ(replayed_events, events);
  EXPECT_GT(events, kCommands);  // every command was answered, and half of them traded
}

}  // namespace

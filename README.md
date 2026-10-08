# Project 1 — Low-Latency Limit Order Book Matching Engine

[![CI](https://github.com/GOMER20/Project-1-Low-Latency-Order-Book/actions/workflows/ci.yml/badge.svg)](https://github.com/GOMER20/Project-1-Low-Latency-Order-Book/actions/workflows/ci.yml)

A limit order book is the data structure at the heart of an exchange. It holds
every resting buy and sell order for an instrument, and when a new order
arrives it matches it against the best-priced orders on the other side, oldest
first at each price. That rule is called price-time priority.

"Low latency" means doing that in tens of nanoseconds, every time. This C++20
implementation gets there by removing the things that make latency
unpredictable: it never allocates memory after start-up, never searches for an
order or a price, and keeps each order in a single CPU cache line. On a laptop
it inserts, cancels or matches an order in about 20 ns.

The book itself is single-threaded. `MatchingEngine` runs it on a thread of its
own, taking orders in and sending trades out through lock-free ring buffers, so
no other thread can ever block it.

## Design

| Piece | File | What it does |
|---|---|---|
| `Order` | [include/lob/order.hpp](include/lob/order.hpp) | One resting order in exactly one 64-byte cache line. Trivial and standard-layout. |
| `OrderPool` | [include/lob/order_pool.hpp](include/lob/order_pool.hpp) | All orders allocated once at start-up. Allocate and free are O(1) through a LIFO free list threaded through the orders themselves. |
| `PriceLevel` | [include/lob/price_level.hpp](include/lob/price_level.hpp) | The FIFO queue at one price, as an intrusive doubly linked list of 32-bit pool indices. 16 bytes per level. |
| `LevelBitmap` | [include/lob/level_bitmap.hpp](include/lob/level_bitmap.hpp) | One bit per price level in three tiers. Finds the best price among 262,144 levels with three count-zero instructions, however sparse the book. |
| `OrderBook` | [include/lob/order_book.hpp](include/lob/order_book.hpp) | Flat array of levels per side, indexed by `price - min_price`. Implements `submit` for limit, market, IOC and FOK orders, plus `add`, `cancel` and `execute`. |
| `Trade` | [include/lob/trade.hpp](include/lob/trade.hpp) | The 40-byte event emitted for every fill. |
| `SpscRing` | [include/lob/spsc_ring.hpp](include/lob/spsc_ring.hpp) | A wait-free queue between one producer thread and one consumer thread. Each side's counter has its own cache line, and each side caches the other's counter so it rarely reads it. |
| `Command`, `Event` | [include/lob/messages.hpp](include/lob/messages.hpp) | The 32-byte request and 48-byte response that cross the rings. |
| `MatchingEngine` | [include/lob/matching_engine.hpp](include/lob/matching_engine.hpp) | An `OrderBook` on its own thread, between a command ring and an event ring. The thread can be pinned to a CPU. |

Design rules followed throughout:

- No dynamic allocation on the hot path: no `new`, `std::string`, `shared_ptr` or container growth.
- No searching: cancel, execute and best-price lookup are all O(1).
- No hash map: an order ID carries its own pool slot in its low 32 bits. The high 32 bits are a sequence number, so an ID stops working once its slot is recycled.
- Trade handlers are template parameters, so they inline. There is no `std::function` and no virtual call.

## Usage

```cpp
#include "lob/order_book.hpp"

// Prices are integer ticks. This book accepts 10'000 .. 29'999.
lob::OrderBook book({.symbol = "AAPL",
                     .min_price = 10'000,
                     .num_levels = 20'000,
                     .max_orders = 1 << 20});

const auto on_trade = [](const lob::Trade& trade) {
  // trade.maker_id, trade.taker_id, trade.price, trade.quantity, trade.taker_side
};

const lob::SubmitResult ask = book.submit(lob::Side::Sell, 15'000, 100, on_trade);  // rests
const lob::SubmitResult bid = book.submit(lob::Side::Buy, 15'000, 40, on_trade);    // trades 40 @ 15'000

book.cancel(ask.id);  // cancels the remaining 60
```

`submit` returns the order's ID, the quantity filled immediately and the quantity
left resting. An ID of `lob::kInvalidOrderId` means the order was rejected.

### Order types

`submit` takes an optional order type before the trade handler. Without one, the
order is a limit order.

| Type | Trades | Unfilled remainder |
|---|---|---|
| `OrderType::Limit` | At its limit price or better | Rests in the book |
| `OrderType::Market` | At any price; the price argument is ignored | Discarded |
| `OrderType::IOC` (immediate-or-cancel) | At its limit price or better | Discarded |
| `OrderType::FOK` (fill-or-kill) | In full immediately, or not at all | Nothing is left: it is all or nothing |

```cpp
book.submit(lob::Side::Buy, 0, 500, lob::OrderType::Market, on_trade);
book.submit(lob::Side::Buy, 15'000, 500, lob::OrderType::FOK, on_trade);
```

A fill-or-kill order first checks that enough quantity is available. The check
adds up each price level's running total and hops between occupied levels with
the bitmap, so it never walks a queue of orders.

### Running the book on its own thread

`MatchingEngine` owns a book and a thread. One thread sends it commands, and one
thread (the same or another) reads the events that come back:

```cpp
#include "lob/matching_engine.hpp"

lob::MatchingEngine engine({.book = {.symbol = "AAPL",
                                     .min_price = 10'000,
                                     .num_levels = 20'000,
                                     .max_orders = 1 << 20}});
engine.start();

// Gateway thread. The tag (here 1) comes back on every event this order causes.
// submit and cancel return false if the command ring is full.
while (!engine.submit(1, lob::Side::Buy, 15'000, 100)) {
}

// Publisher thread: everything the engine has reported so far, in order.
engine.poll([&](const lob::Event& event) {
  // event.type is Accepted, Rejected, Trade, Cancelled or CancelRejected.
  // An Accepted event carries the order's ID, which is what cancel() takes.
});

engine.stop();
```

`submit` on the engine takes the same optional order type as the book:
`engine.submit(2, lob::Side::Buy, 0, 500, lob::OrderType::Market)`.

### Pinning the engine thread to a core

On Linux, the engine thread can be bound to one CPU so the scheduler never
moves it:

```cpp
lob::MatchingEngine engine({.book = book_config, .pin_to_cpu = 3});
engine.start();
if (!engine.pinned()) {
  // The CPU does not exist, is not available to this process, or the platform
  // cannot pin threads (macOS). The engine is running anyway, unpinned.
}
```

Pinning only stops the engine from leaving that core. Keeping everything else
off the core is a system setting, such as `isolcpus` or cpusets.

Every command is answered by exactly one `Accepted`, `Rejected`, `Cancelled` or
`CancelRejected` event. An order that trades produces its `Trade` events first
and its `Accepted` last.

`lob::SpscRing<T>` can also be used on its own, with `try_push`, `try_pop` and
`drain`, for any trivially copyable `T`.

## Build

Dependencies on Ubuntu or WSL:

```bash
sudo apt update && sudo apt install -y build-essential cmake ninja-build git libgtest-dev libbenchmark-dev
```

On macOS:

```bash
brew install cmake ninja
```

If GoogleTest or Google Benchmark are not installed, CMake downloads pinned
versions automatically.

Configure, build and test:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Debug build with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLOB_ENABLE_SANITIZERS=ON
cmake --build build-debug && ctest --test-dir build-debug --output-on-failure
```

Debug build with ThreadSanitizer, which checks the ring buffer's memory ordering:

```bash
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLOB_ENABLE_TSAN=ON
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure
```

## Tests

Every push and pull request is built and tested by GitHub Actions on Linux with
GCC and Clang, under ASan/UBSan and ThreadSanitizer, and on macOS. The workflow
is in [.github/workflows/ci.yml](.github/workflows/ci.yml).

| File | Covers |
|---|---|
| [tests/order_pool_test.cpp](tests/order_pool_test.cpp) | Exhaustion, LIFO reuse, alignment, drain and refill. |
| [tests/price_level_test.cpp](tests/price_level_test.cpp) | FIFO order; erase at head, middle and tail; recycled slots. |
| [tests/level_bitmap_test.cpp](tests/level_bitmap_test.cpp) | All three tiers, boundaries, and a randomised check against `std::vector<bool>`. |
| [tests/order_book_test.cpp](tests/order_book_test.cpp) | `add`, `cancel`, `execute`, rejections, stale and forged IDs. |
| [tests/matching_test.cpp](tests/matching_test.cpp) | Price priority, time priority, trade prices, remainders. |
| [tests/edge_case_test.cpp](tests/edge_case_test.cpp) | Partial fills, queue jumping, full cancellations, crossing the spread. |
| [tests/model_test.cpp](tests/model_test.cpp) | 66,000 random operations, covering all four order types, compared step by step against a naive `std::map` reference book. |
| [tests/allocation_test.cpp](tests/allocation_test.cpp) | Replaces global `operator new` and asserts that nothing allocates after construction. |
| [tests/spsc_ring_test.cpp](tests/spsc_ring_test.cpp) | FIFO order, full and empty, wrap-around, and two-thread transfers that check nothing is lost, reordered or torn. |
| [tests/matching_engine_test.cpp](tests/matching_engine_test.cpp) | Every event type, start, stop and restart, shutdown with nobody reading, and 5,000 random commands through 4-slot rings compared event for event against a single-threaded run. |
| [tests/order_type_test.cpp](tests/order_type_test.cpp) | Market, IOC and FOK orders: what trades, what is discarded, and that they work when the book is full. |
| [tests/thread_affinity_test.cpp](tests/thread_affinity_test.cpp) | Pinning requests that succeed, fail and are impossible; the engine runs in every case. |

## Benchmarks

```bash
./build/lob_bench
```

To run only the headline numbers:

```bash
./build/lob_bench --benchmark_filter='DeepBook|MixedFlow'
```

| Benchmark | Measures |
|---|---|
| `BM_Insert_DeepBook` | One resting order inserted into a book of 65k-131k orders. |
| `BM_Cancel_DeepBook` | One randomly chosen order cancelled from that book. |
| `BM_Match_DeepBook` | One incoming limit order that crosses and fully fills one resting order. |
| `BM_MatchMarket_DeepBook`, `BM_MatchIoc_DeepBook`, `BM_MatchFok_DeepBook` | The same fill by a market, IOC and FOK order. |
| `BM_MixedFlow` | A steady mix: 48% inserts, 48% cancels, 4% trades. |
| `BM_MixedFlow_Percentiles` | p50, p99 and p99.9 of the same mix. Each sample includes two clock reads, so these are upper bounds. |
| `BM_Match_SweepLevels/N` | One order sweeping N price levels; see `items_per_second`. |
| `BM_OrderPool_*`, `BM_Heap_NewDelete` | The pool against `new` / `delete`. |
| `BM_IntrusiveLevel_*`, `BM_StdList_*` | The intrusive queue against `std::list`. |
| `BM_SpscRing_TwoThreads`, `BM_MutexRing_TwoThreads` | Time per `Trade` delivered from one thread to another, through the ring and through the same ring guarded by a mutex. |
| `BM_Engine_RoundTrip` | One command sent to the engine thread and its answer received: both rings plus the book. |
| `BM_Engine_Throughput` | Commands per second through the whole pipeline when the sender does not wait for answers. |

The deep-book benchmarks shuffle their orders first, so resting orders and free
slots are scattered through memory rather than laid out in insertion order.

### Results

Google Benchmark 1.9.4, Release build, Apple clang 21, on a MacBook Pro
(Intel Core i9-9880H, 2.3 GHz, 16 MB L3). The machine was an ordinary laptop
with other programs running: no core isolation, no pinning.

| Benchmark | Time |
|---|---|
| `BM_Insert_DeepBook` | 20.6 ns |
| `BM_Cancel_DeepBook` | 18.0 ns |
| `BM_Match_DeepBook` | 21.9 ns |
| `BM_MatchMarket_DeepBook`, `BM_MatchIoc_DeepBook`, `BM_MatchFok_DeepBook` | 19 to 22 ns, 19 to 20 ns and about 22 ns |
| `BM_MixedFlow` | 26.0 ns |
| `BM_MixedFlow_Percentiles` | p50 75 ns, p99 137 ns, p99.9 193 ns |
| `BM_Match_SweepLevels/64` | 1,320 ns for 64 rests and 64 fills, about 21 ns per order |
| `BM_OrderPool_AllocateDeallocate` vs `BM_Heap_NewDelete` | 1.22 ns vs 181 ns |
| `BM_IntrusiveLevel_AddCancel` vs `BM_StdList_AddCancel` | 2.33 ns vs 197 ns |
| `BM_SpscRing_TwoThreads` vs `BM_MutexRing_TwoThreads` | 2.5 ns vs 106 ns per event (about 400 million vs 9 million events per second) |
| `BM_Engine_RoundTrip` | about 250 ns from sending a command to receiving its answer |
| `BM_Engine_Throughput` | 16 to 19 million commands per second |

Results depend on the CPU, and the percentiles also depend on how quiet the
machine is; run them on the hardware you care about.

## Limits and trade-offs

- **One symbol per book, one thread per book.** The book has no locks or atomics.
- **The ring buffer is strictly one producer and one consumer.** A second thread on either side is a data race. It carries trivially copyable types only.
- **The engine therefore takes commands from one thread and reports events to one thread.** Several gateways would each need their own ring.
- **A slow event reader stalls the engine.** Events are never dropped while it runs, so a full event ring makes it wait. At shutdown only, events nobody is reading are discarded and counted, so that `stop()` always returns.
- **Core pinning is Linux only.** macOS has no way to bind a thread to a CPU, so a pin request there is reported as not honoured.
- **By default the engine busy-waits**, occupying one core even when idle.
- **The price range is fixed at construction** and holds at most 262,144 levels. Orders outside it are rejected.
- **Four order types: limit, market, IOC and FOK.** There are no stop or iceberg orders and no self-trade prevention.
- **A full book rejects new limit orders**, including one that would have traded, because a slot is reserved before matching. Market, IOC and FOK orders never rest, so they are still accepted.
- **Order IDs are assigned by the book** and are unique for its first 2^32 orders.
- **The trade handler must not throw or call back into the book.**
- **`add` rests an order without matching it** and can therefore cross the book. It exists for rebuilding a book from an already-matched feed.

## Roadmap

- [x] Phase 1 — Environment and boilerplate (CMake, GoogleTest, Google Benchmark)
- [x] Phase 2 — `Order` struct and pre-allocated `OrderPool`
- [x] Phase 3 — Price level and intrusive FIFO list
- [x] Phase 4 — `OrderBook` with `add`, `cancel`, `execute`
- [x] Phase 5 — Price-time priority matching and `Trade` events
- [x] Phase 6 — Edge-case tests and latency benchmarks
- [x] Phase 7 — Lock-free SPSC ring buffer
- [x] Phase 8 — Engine thread between a command ring and an event ring
- [x] Phase 9 — Market, IOC and FOK orders; core pinning for the engine thread
- [x] Phase 10 — Continuous integration on Linux and macOS
- [ ] Next — Multiple symbols

## Layout

```
include/lob/   header-only engine
tests/         GoogleTest suite
bench/         Google Benchmark suite
```

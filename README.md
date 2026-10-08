# Project 1 — Low-Latency Limit Order Book Matching Engine

A single-threaded limit order book and matching engine in C++20. It matches by
price-time priority and never touches the heap after start-up.

## Design

| Piece | File | What it does |
|---|---|---|
| `Order` | [include/lob/order.hpp](include/lob/order.hpp) | One resting order in exactly one 64-byte cache line. Trivial and standard-layout. |
| `OrderPool` | [include/lob/order_pool.hpp](include/lob/order_pool.hpp) | All orders allocated once at start-up. Allocate and free are O(1) through a LIFO free list threaded through the orders themselves. |
| `PriceLevel` | [include/lob/price_level.hpp](include/lob/price_level.hpp) | The FIFO queue at one price, as an intrusive doubly linked list of 32-bit pool indices. 16 bytes per level. |
| `LevelBitmap` | [include/lob/level_bitmap.hpp](include/lob/level_bitmap.hpp) | One bit per price level in three tiers. Finds the best price among 262,144 levels with three count-zero instructions, however sparse the book. |
| `OrderBook` | [include/lob/order_book.hpp](include/lob/order_book.hpp) | Flat array of levels per side, indexed by `price - min_price`. Implements `submit` (match, then rest), `add`, `cancel` and `execute`. |
| `Trade` | [include/lob/trade.hpp](include/lob/trade.hpp) | The 40-byte event emitted for every fill. |

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

## Tests

| File | Covers |
|---|---|
| [tests/order_pool_test.cpp](tests/order_pool_test.cpp) | Exhaustion, LIFO reuse, alignment, drain and refill. |
| [tests/price_level_test.cpp](tests/price_level_test.cpp) | FIFO order; erase at head, middle and tail; recycled slots. |
| [tests/level_bitmap_test.cpp](tests/level_bitmap_test.cpp) | All three tiers, boundaries, and a randomised check against `std::vector<bool>`. |
| [tests/order_book_test.cpp](tests/order_book_test.cpp) | `add`, `cancel`, `execute`, rejections, stale and forged IDs. |
| [tests/matching_test.cpp](tests/matching_test.cpp) | Price priority, time priority, trade prices, remainders. |
| [tests/edge_case_test.cpp](tests/edge_case_test.cpp) | Partial fills, queue jumping, full cancellations, crossing the spread. |
| [tests/model_test.cpp](tests/model_test.cpp) | 66,000 random operations compared step by step against a naive `std::map` reference book. |
| [tests/allocation_test.cpp](tests/allocation_test.cpp) | Replaces global `operator new` and asserts that nothing allocates after construction. |

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
| `BM_Match_DeepBook` | One incoming order that crosses and fully fills one resting order. |
| `BM_MixedFlow` | A steady mix: 48% inserts, 48% cancels, 4% trades. |
| `BM_MixedFlow_Percentiles` | p50, p99 and p99.9 of the same mix. Each sample includes two clock reads, so these are upper bounds. |
| `BM_Match_SweepLevels/N` | One order sweeping N price levels; see `items_per_second`. |
| `BM_OrderPool_*`, `BM_Heap_NewDelete` | The pool against `new` / `delete`. |
| `BM_IntrusiveLevel_*`, `BM_StdList_*` | The intrusive queue against `std::list`. |

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
| `BM_MixedFlow` | 26.0 ns |
| `BM_MixedFlow_Percentiles` | p50 75 ns, p99 137 ns, p99.9 193 ns |
| `BM_Match_SweepLevels/64` | 1,320 ns for 64 rests and 64 fills, about 21 ns per order |
| `BM_OrderPool_AllocateDeallocate` vs `BM_Heap_NewDelete` | 1.22 ns vs 181 ns |
| `BM_IntrusiveLevel_AddCancel` vs `BM_StdList_AddCancel` | 2.33 ns vs 197 ns |

Results depend on the CPU, and the percentiles also depend on how quiet the
machine is; run them on the hardware you care about.

## Limits and trade-offs

- **One symbol per book, one thread per book.** There are no locks or atomics.
- **The price range is fixed at construction** and holds at most 262,144 levels. Orders outside it are rejected.
- **Limit orders only.** There are no market, IOC or FOK orders and no self-trade prevention.
- **A full book rejects every new order**, including one that would have traded, because a slot is reserved before matching.
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
- [ ] Next — Lock-free SPSC ring buffers and an engine thread between them

## Layout

```
include/lob/   header-only engine
tests/         GoogleTest suite
bench/         Google Benchmark suite
```

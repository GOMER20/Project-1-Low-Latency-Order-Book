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

A book is single-threaded and handles one symbol. `MatchingEngine` runs one book
per symbol on a thread of its own, taking orders in and sending trades out
through lock-free ring buffers, so no other thread can ever block it.

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
| `MatchingEngine` | [include/lob/matching_engine.hpp](include/lob/matching_engine.hpp) | One `OrderBook` per symbol, all on one engine thread between a command ring and an event ring. The thread can be pinned to a CPU. |
| `ShardedEngine` | [include/lob/sharded_engine.hpp](include/lob/sharded_engine.hpp) | Several `MatchingEngine`s side by side, each with its own thread and rings, to use more than one core. It owns every book, decides which shard runs each symbol, and can move a symbol to another shard while running. |
| `assign_shards`, `choose_rebalancing_move` | [include/lob/shard_assignment.hpp](include/lob/shard_assignment.hpp) | The two placement decisions: which shard each symbol starts on, given how busy each is expected to be, and which single symbol to move when the shards have drifted out of balance. |

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

### Running books on their own thread

`MatchingEngine` owns one book per symbol and a thread to run them on. One
thread sends it commands, and one thread (the same or another) reads the events
that come back:

```cpp
#include "lob/matching_engine.hpp"

lob::MatchingEngine engine({.books = {
    {.symbol = "AAPL", .min_price = 10'000, .num_levels = 20'000, .max_orders = 1 << 20},
    {.symbol = "MSFT", .min_price = 30'000, .num_levels = 20'000, .max_orders = 1 << 20},
}});

// A symbol's ID is its position in the list. Look it up once, not per order.
const lob::SymbolId aapl = *engine.symbol_id("AAPL");

engine.start();

// Gateway thread. The tag (here 1) comes back on every event this order causes.
// submit and cancel return false if the command ring is full.
while (!engine.submit(1, aapl, lob::Side::Buy, 15'000, 100)) {
}

// Publisher thread: everything the engine has reported so far, in order.
engine.poll([&](const lob::Event& event) {
  // event.type is Accepted, Rejected, Trade, Cancelled or CancelRejected.
  // event.symbol says which book it came from.
  // An Accepted event carries the order's ID, which is what cancel() takes:
  //   engine.cancel(2, event.symbol, event.order_id);
});

engine.stop();
```

Every command is answered by exactly one `Accepted`, `Rejected`, `Cancelled` or
`CancelRejected` event. An order that trades produces its `Trade` events first
and its `Accepted` last.

Books are independent: each has its own price range, capacity and order IDs.
An order is therefore identified by its symbol and its order ID together, and a
command for a symbol the engine does not have is answered with `Rejected` or
`CancelRejected`. Routing a command to its book is one array index; there is no
string comparison or hashing on the hot path.

`submit` on the engine takes the same optional order type as the book:
`engine.submit(2, aapl, lob::Side::Buy, 0, 500, lob::OrderType::Market)`.

### Pinning the engine thread to a core

On Linux, the engine thread can be bound to one CPU so the scheduler never
moves it:

```cpp
lob::MatchingEngine engine({.books = {book_config}, .pin_to_cpu = 3});
engine.start();
if (!engine.pinned()) {
  // The CPU does not exist, is not available to this process, or the platform
  // cannot pin threads (macOS). The engine is running anyway, unpinned.
}
```

Pinning only stops the engine from leaving that core. Keeping everything else
off the core is a system setting, such as `isolcpus` or cpusets.

`lob::SpscRing<T>` can also be used on its own, with `try_push`, `try_pop` and
`drain`, for any trivially copyable `T`.

### Using more than one core

One engine thread runs all of its books, so it uses one core. `ShardedEngine`
runs several engines side by side and deals the symbols out among them:

```cpp
#include "lob/sharded_engine.hpp"

lob::ShardedEngine engine({.books = books,                  // e.g. 400 symbols
                           .shards = 4,                     // four engine threads
                           .pin_to_cpus = {2, 3, 4, 5}});   // optional, Linux only
engine.start();

const lob::SymbolId aapl = *engine.symbol_id("AAPL");
if (engine.submit(1, aapl, lob::Side::Buy, 15'000, 100) == lob::SendStatus::RingFull) {
  // That symbol's shard is busy: try again.
}

engine.poll([&](const lob::Event& event) { /* events from every shard */ });
```

Symbol IDs are the same whatever the number of shards; a small table turns an
ID into its shard, so routing is still one array lookup. Shards share nothing:
there is no lock, and no cache line that two engine threads both write.

By default the symbols are dealt to the shards in turn. If a few symbols carry
most of the traffic, that can leave one shard doing most of the work. Tell the
engine how busy each symbol is expected to be, and it spreads the busy ones out
so that every shard carries a similar total:

```cpp
lob::ShardedEngine engine({.books = books,
                           .shards = 4,
                           .loads = orders_per_symbol_yesterday});  // one number per book
```

The numbers can be in any unit. The engine also measures them for you:
`engine.measured_loads()` returns how many commands each symbol actually
received, in the form `loads` expects, so one session's traffic can lay out the
next. The rule used is "largest first": take the symbols from busiest to
quietest and give each to the shard carrying the least so far. The busiest
shard then never carries more than 4/3 of what the best possible split would
give it.

A symbol can also be moved to another shard while the engine is running, for
when one becomes busy in the middle of a session:

```cpp
if (engine.move_symbol(aapl, 2) == lob::SendStatus::Sent) {
  // Orders for AAPL already go to shard 2. Keep polling: the move finishes
  // once the old shard's events have been read.
}
while (engine.move_in_progress(aapl)) {
  engine.poll([&](const lob::Event& event) { /* ... */ });
}
```

The book, its resting orders and their IDs are untouched; only the thread
running it changes. The group owns every book and a shard just holds pointers
to the ones it is running, so nothing is copied.

How the handover stays correct:

1. A "let go" command goes into the old shard's queue and a "take up" command
   into the new shard's. They travel in the same queues as orders, so every
   order sent before the move is handled by the old shard and every order sent
   after it by the new one.
2. The old shard's last word on the symbol is an internal marker in its event
   stream.
3. The new shard does not start on the symbol until `poll` has read that
   marker. The symbol's events therefore stay in order, and the two threads
   never touch the book at the same time.

Call `move_symbol` from the thread that feeds both shards involved.

The engine can also decide the moves itself. Ask it to look at the load every
so many commands, and it will move a symbol whenever one shard is carrying
clearly more than its share:

```cpp
lob::ShardedEngine engine({.books = books,
                           .shards = 4,
                           .rebalance = {.every = 100'000}});  // look every 100,000 commands
```

Each look measures how many commands every symbol has received, with older
traffic counting for less, and adds up what each shard is carrying. Then:

- **If the busiest shard is within 25% of its fair share, nothing moves.** The
  tolerance is configurable. A move pauses a shard, so small differences are
  not worth one.
- **Otherwise one symbol moves from the busiest shard to the quietest:** the
  one that narrows the gap between them the most.
- **A move must at least halve that gap.** That rules out symbols too quiet to
  matter and symbols so busy that moving them would only move the problem.
- **Only one move is under way at a time.**

Because older traffic fades rather than vanishes, a single burst does not cause
a move, but a lasting shift is acted on within a few looks.

By default a symbol's load is the number of commands handled for it, which
treats every command as equal work. That is wrong when one symbol's orders
sweep many price levels and another's simply rest. Ask for time instead:

```cpp
lob::ShardedEngine engine({.books = books,
                           .shards = 4,
                           .rebalance = {.every = 100'000,
                                         .measure = lob::LoadMeasure::Time}});
```

The engine thread then balances on how long it has spent on each symbol. It
does not put a stopwatch on every command, which would cost about ten
nanoseconds each. It times one in 64, chosen at random intervals so that no
pattern in the order flow can keep the stopwatch on one symbol, and scales the
total up. A timed command that had to wait for a slow event reader is left out,
and one that took wildly longer than usual, because the thread was interrupted,
is capped. `engine.time_spent(symbol)` reports the estimate. To decide the
timing yourself, leave `every` at zero and call `engine.rebalance()` when it
suits you. Either way it must run on the thread that feeds every shard, so turn
`every` on only if one thread does.

Two things differ from a single engine:

- **Threads.** Each shard's rings still take one thread on each end. One thread
  may feed every shard, or each shard may have a feeder of its own; the same
  goes for reading events with `poll(shard, ...)`.
- **Order.** Events for one symbol arrive in the order they happened, even
  when the symbol is moved between shards. Events
  for symbols on different shards have no order relative to each other.

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
| [tests/allocation_test.cpp](tests/allocation_test.cpp) | Replaces global `operator new` and asserts that nothing allocates after construction. Built as its own binary and left out of sanitizer builds, whose runtimes replace `operator new` themselves. |
| [tests/spsc_ring_test.cpp](tests/spsc_ring_test.cpp) | FIFO order, full and empty, wrap-around, and two-thread transfers that check nothing is lost, reordered or torn. |
| [tests/matching_engine_test.cpp](tests/matching_engine_test.cpp) | Every event type, start, stop and restart, shutdown with nobody reading, and 6,000 random commands for three books through 4-slot rings compared event for event against a single-threaded run. |
| [tests/multi_symbol_test.cpp](tests/multi_symbol_test.cpp) | Routing by symbol, unknown symbols, per-book order IDs and limits, and 9,000 interleaved commands checked against running each symbol alone. |
| [tests/sharded_engine_test.cpp](tests/sharded_engine_test.cpp) | How symbols are dealt to shards, routing and symbol IDs, one feeder thread per shard, and 12,000 random commands through three shard threads compared, symbol by symbol, with each symbol running alone. |
| [tests/shard_assignment_test.cpp](tests/shard_assignment_test.cpp) | Equal loads dealt in turn, busy symbols kept apart, no shard left empty, and 400 random cases checked against the best possible split. |
| [tests/symbol_move_test.cpp](tests/symbol_move_test.cpp) | Moving a symbol between shards: orders and IDs survive, events stay in order, queued and repeated moves, full rings, stopping or destroying the engine mid-move, and 12,000 random commands with hundreds of random moves compared, symbol by symbol, with each symbol running alone. |
| [tests/load_by_time_test.cpp](tests/load_by_time_test.cpp) | Timing a sample of commands: expensive commands told from cheap ones, a sample agreeing with timing everything, regular order patterns not fooling the sampling, interruptions capped, and rebalancing by time catching an imbalance that counting commands cannot see. |
| [tests/rebalance_test.cpp](tests/rebalance_test.cpp) | The rebalancing decision on its own, including 300 random cases showing that repeated moves only ever improve things and always stop; and the engine moving a busy symbol off an overloaded shard, settling, following a shift in traffic, and doing it all unprompted. |
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
| `BM_Engine_PerCommand/N` | The engine's cost per command on one thread, timing one command in N: 0 for no timing, 64 the default, 1 for every command. Divide the time shown by 512, or read `items_per_second`. |
| `BM_Engine_Throughput/N` | Commands per second through the whole pipeline when the sender does not wait for answers, with the orders spread over N symbols. |
| `BM_Sharded_Throughput/N` | Total commands per second across N shards, each with its own feeder thread: 2N busy threads in all. |
| `BM_Sharded_MoveSymbol` | How long one move takes between two otherwise idle shards. |
| `BM_Sharded_RebalanceLook/N` | What one look by `rebalance()` costs with N symbols on four shards. |

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
| `BM_Engine_Throughput/1`, `/8`, `/64` | 12 to 13 million commands per second, the same with 1, 8 or 64 symbols |
| `BM_Sharded_Throughput/1`, `/2`, `/4` | 9 to 15 million commands per second with one shard; roughly 1.2 to 1.6 times that with two and 1.8 to 2.7 times with four. These readings vary a lot from run to run: four shards means eight busy threads on this laptop's eight cores. |
| `BM_Sharded_MoveSymbol` | about 450 to 530 ns per move |
| `BM_Sharded_RebalanceLook/16`, `/256`, `/4096` | about 85 ns, 1.1 µs and 18 µs per look. At one look per 100,000 commands with 256 symbols, that is about a hundredth of a nanosecond per command. |
| `BM_Engine_PerCommand/0`, `/64`, `/1` | Timing one command in 64 costs nothing measurable next to no timing at all. Timing every command adds about 45% to the engine's cost per command, which is why it samples. |

Results depend on the CPU, and the percentiles also depend on how quiet the
machine is; run them on the hardware you care about.

## Limits and trade-offs

- **One symbol per book, one thread per book.** The book has no locks or atomics.
- **One engine thread runs all of its symbols.** `ShardedEngine` uses more cores by running several engines, each with its own symbols and rings. An engine holds at most 65,535 symbols.
- **Rebalancing is off unless you ask for it.** Symbols are dealt in turn, or by the loads you supply, at start-up. After that a symbol moves only when you call `move_symbol` or `rebalance`, or set `RebalanceConfig::every`.
- **Rebalancing is cautious, not optimal.** It moves one symbol at a time, from the busiest shard to the quietest, and only if that at least halves the gap between them. It will not split up two shards that are each dominated by one busy symbol, and it only ever compares the busiest shard with the quietest.
- **Automatic rebalancing needs a single feeder thread.** It runs inside `submit` and `cancel` and may move a symbol, which only the thread feeding every shard may do. With a feeder thread per shard, leave it off.
- **Time spent is an estimate.** It comes from timing one command in 64, so it is noisier than a count, and it is in the CPU counter's own units, good for comparing symbols but not for reading as seconds. Commands handled is still the default measure.
- **The stopwatch can be fooled in one direction.** A sample that took far longer than usual is capped at 64 times the recent average, on the assumption that the thread was interrupted. A symbol that suddenly becomes genuinely expensive is therefore undercounted for its first few samples.
- **A move pauses its destination.** The new shard waits, for all of its symbols, until the old shard has worked through its queue and `poll` has read its events. Idle shards hand over in about half a microsecond; a backlog on the old shard, or a slow reader, makes it longer.
- **A move needs the events to be read.** If nothing calls `poll`, a move does not finish until the engine is stopped. If the engine is stopped mid-move, the move still completes, but the symbol's events from either side of it may then be read out of order.
- **`move_symbol` must be called by the thread that feeds both shards.** If each shard has its own feeder thread, both must pause for the call.
- **Load-based assignment is good, not optimal.** The busiest shard carries at most 4/3 of the best possible, and a single symbol busier than all the others together still fills a shard on its own.
- **There is no ordering between shards.** Events for one symbol are in order; events for symbols on different shards are not ordered relative to each other.
- **Order IDs are unique within a symbol, not across symbols.** A cancel must name both.
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
- [x] Phase 11 — Multiple symbols on one engine
- [x] Phase 12 — Several engine threads, each with its own share of the symbols
- [x] Phase 13 — Assigning symbols to shards by expected or measured load
- [x] Phase 14 — Moving a symbol between shards while the engine is running
- [x] Phase 15 — Rebalancing automatically from measured load
- [x] Phase 16 — Measuring load by time spent rather than commands handled
- [ ] Next — A market data feed: publishing the best prices and depth as they change

## Layout

```
include/lob/   header-only engine
tests/         GoogleTest suite
bench/         Google Benchmark suite
```

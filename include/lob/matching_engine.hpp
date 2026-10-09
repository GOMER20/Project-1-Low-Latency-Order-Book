#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "lob/compiler.hpp"
#include "lob/messages.hpp"
#include "lob/order_book.hpp"
#include "lob/spsc_ring.hpp"
#include "lob/thread_affinity.hpp"

namespace lob {

inline constexpr int kNoPinning = -1;

enum class IdleStrategy : std::uint8_t {
  Spin,   // busy-wait for the next command: lowest latency, occupies a whole core
  Yield,  // give up the time slice when there is nothing to do
};

struct EngineConfig {
  std::vector<BookConfig> books;           // one per symbol; a symbol's ID is its position here
  std::size_t command_capacity = 1 << 16;  // inbound ring; rounded up to a power of two
  std::size_t event_capacity = 1 << 16;    // outbound ring; rounded up to a power of two
  IdleStrategy idle = IdleStrategy::Spin;
  int pin_to_cpu = kNoPinning;             // CPU to bind the engine thread to (Linux only)

  // Time one command in this many, on average, to estimate how long each
  // symbol's commands take; see time_spent(). 0 turns timing off.
  std::uint32_t time_one_in = 64;
};

// Decides how much of one timed command to count, and keeps `typical_x256` up
// to date: a running average of what a timed command takes, held as 256 times
// its value so that it stays meaningful on clocks so coarse that most commands
// measure as zero or one tick.
//
// A sample is capped at 64 times that average. Now and then the thread is
// interrupted in the middle of a timed command, and the stopwatch reads
// thousands of times too long; uncapped, one such reading would pass for a
// great deal of work. A symbol whose commands really are that expensive is
// not held down for long, because each capped sample still pulls the average
// up, by a factor of nearly five, and within a few samples its commands are
// counted in full.
[[nodiscard]] inline std::uint64_t cap_timed_sample(std::uint64_t elapsed,
                                                    std::uint64_t& typical_x256) noexcept {
  if (typical_x256 == 0) {
    typical_x256 = elapsed * 256;  // nothing to compare with yet
    return elapsed;
  }
  const std::uint64_t ceiling = std::max<std::uint64_t>(typical_x256 / 4, 1);  // 64 x typical
  const std::uint64_t counted = std::min(elapsed, ceiling);
  // Move the average a sixteenth of the way towards this sample.
  typical_x256 = typical_x256 - typical_x256 / 16 + counted * 16;
  return counted;
}

// The three milestones of moving one symbol from one engine to another. Each
// holds the number of the latest move to have reached it; the moves of a
// symbol are numbered 1, 2, 3 and so on.
struct MigrationGate {
  std::atomic<std::uint32_t> detached{0};   // the old engine has let go of the book
  std::atomic<std::uint32_t> delivered{0};  // everything the old engine said has been read
  std::atomic<std::uint32_t> attached{0};   // the new engine has taken the book up
};

// Shared by a group of engines that can pass symbols between them: every
// symbol's book, and its gate. The engines do not own the books.
struct SymbolDirectory {
  explicit SymbolDirectory(std::size_t symbols) : books(symbols, nullptr), gates(symbols) {}

  std::vector<OrderBook*> books;     // by SymbolId
  std::vector<MigrationGate> gates;  // by SymbolId
};

// One order book per symbol, all running on a single engine thread that is fed
// and read through two SPSC rings:
//
//   gateway thread --Command--> [ engine thread: OrderBooks ] --Event--> publisher thread
//
// The books stay single-threaded and lock-free; the rings are the only things
// shared between threads. Each ring allows exactly one thread on each end, so:
//
//   submit(), cancel()   one gateway thread
//   poll()               one publisher thread
//   start(), stop()      one controlling thread
//
// The gateway and publisher may be the same thread.
//
// A symbol is named by its SymbolId: its position in EngineConfig::books. That
// makes routing a command to its book one array index, with no string
// comparison or hashing on the hot path. Use symbol_id() to turn a name into
// an ID once, up front. Books are independent: each has its own price range,
// capacity and order IDs, so an order is identified by its symbol and its
// order ID together.
//
// Events are never dropped while the engine is running: if the event ring is
// full, the engine waits for the publisher. The one exception is shutdown, to
// guarantee stop() returns even if nobody is reading: events that still cannot
// be delivered once stop() has been called are discarded and counted in
// events_dropped(). To lose nothing, poll until every command has been
// answered, then call stop().
class MatchingEngine {
 public:
  static constexpr std::size_t kMaxSymbols = std::numeric_limits<SymbolId>::max();

  // An engine with books of its own. All memory for every book and both rings
  // is acquired here.
  explicit MatchingEngine(const EngineConfig& config)
      : books_(config.books.size(), nullptr),
        handled_(config.books.size()),
        timed_(config.books.size()),
        commands_(config.command_capacity),
        events_(config.event_capacity),
        idle_(config.idle),
        pin_to_cpu_(config.pin_to_cpu),
        time_one_in_(config.time_one_in),
        sample_countdown_(config.time_one_in != 0 ? 1 : 0) {
    assert(!config.books.empty() && config.books.size() <= kMaxSymbols);
    owned_.reserve(config.books.size());
    for (std::size_t symbol = 0; symbol < config.books.size(); ++symbol) {
      owned_.push_back(std::make_unique<OrderBook>(config.books[symbol]));
      books_[symbol] = owned_.back().get();
    }
  }

  // One engine of a group that can pass symbols between its members; this is
  // how ShardedEngine builds its shards. The books belong to the directory's
  // owner and must outlive the engine. The engine starts out running the
  // symbols listed in `running`; config.books is ignored.
  MatchingEngine(const EngineConfig& config, SymbolDirectory& directory,
                 std::span<const SymbolId> running)
      : books_(directory.books.size(), nullptr),
        handled_(directory.books.size()),
        timed_(directory.books.size()),
        directory_(&directory),
        commands_(config.command_capacity),
        events_(config.event_capacity),
        idle_(config.idle),
        pin_to_cpu_(config.pin_to_cpu),
        time_one_in_(config.time_one_in),
        sample_countdown_(config.time_one_in != 0 ? 1 : 0) {
    for (const SymbolId symbol : running) {
      books_[symbol] = directory.books[symbol];
    }
  }

  ~MatchingEngine() { stop(); }

  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;

  // --- Controlling thread ----------------------------------------------------

  // Returns once the engine thread is up, so pinned() can be trusted from
  // then on.
  void start() {
    assert(!thread_.joinable());
    stop_requested_.store(false, std::memory_order_release);
    started_.store(false, std::memory_order_release);
    pinned_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    while (!started_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  // Processes every command that was queued before the call, then joins the
  // engine thread. Safe to call when the engine is not running.
  void stop() {
    request_stop();
    join();
  }

  // The two halves of stop(), for stopping several engines together: ask them
  // all to stop first, then wait for each.
  void request_stop() noexcept {
    if (thread_.joinable()) {
      stop_requested_.store(true, std::memory_order_release);
    }
  }

  void join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] bool running() const noexcept { return thread_.joinable(); }

  // Whether the engine thread is bound to the CPU asked for in the config.
  // False if no CPU was asked for, or if the request could not be honoured;
  // the engine runs either way, so check this if pinning matters to you.
  [[nodiscard]] bool pinned() const noexcept { return pinned_.load(std::memory_order_acquire); }

  // --- Symbols: any thread ---------------------------------------------------

  [[nodiscard]] std::size_t symbol_count() const noexcept { return books_.size(); }

  // The ID of the book with this symbol name, among the books this engine was
  // built with. A linear search over the names: do it once at start-up, not
  // per order.
  [[nodiscard]] std::optional<SymbolId> symbol_id(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < owned_.size(); ++index) {
      if (owned_[index]->symbol() == name) {
        return static_cast<SymbolId>(index);
      }
    }
    return std::nullopt;
  }

  // --- Gateway thread --------------------------------------------------------
  // All of these return false if the command ring is full; the caller decides
  // whether to retry. A command for a symbol the engine is not running is
  // accepted here and answered with Rejected or CancelRejected.

  [[nodiscard]] bool submit(std::uint64_t client_tag, SymbolId symbol, Side side, Price price,
                            Quantity quantity,
                            OrderType order_type = OrderType::Limit) noexcept {
    Command command{};
    command.client_tag = client_tag;
    command.price = price;
    command.quantity = quantity;
    command.symbol = symbol;
    command.side = side;
    command.type = CommandType::Submit;
    command.order_type = order_type;
    return commands_.try_push(command);
  }

  [[nodiscard]] bool cancel(std::uint64_t client_tag, SymbolId symbol,
                            OrderId order_id) noexcept {
    Command command{};
    command.client_tag = client_tag;
    command.order_id = order_id;
    command.symbol = symbol;
    command.type = CommandType::Cancel;
    return commands_.try_push(command);
  }

  // The two halves of moving a symbol to another engine of the same group.
  // Only for engines built with a SymbolDirectory; ShardedEngine calls these.
  //
  // detach: once everything queued ahead of it has been handled, the engine
  // stops running the symbol and publishes a Handoff event as its last word
  // on it.
  [[nodiscard]] bool detach(SymbolId symbol, std::uint32_t move_number) noexcept {
    return commands_.try_push(migration_command(CommandType::Detach, symbol, move_number));
  }

  // attach: the engine takes the symbol up, but not before the old engine's
  // Handoff has been read from its event ring. Until then this command, and
  // everything queued behind it, waits.
  [[nodiscard]] bool attach(SymbolId symbol, std::uint32_t move_number) noexcept {
    return commands_.try_push(migration_command(CommandType::Attach, symbol, move_number));
  }

  // Whether the command ring is full right now. Only the gateway thread adds
  // to the ring, so if it sees room, its next push will succeed.
  [[nodiscard]] bool command_ring_full() const noexcept {
    return commands_.size() == commands_.capacity();
  }

  // --- Publisher thread ------------------------------------------------------

  // Passes every event published so far to `visit`, in order. Returns how many.
  template <std::invocable<const Event&> Visitor>
  std::size_t poll(Visitor&& visit) noexcept {
    return events_.drain(std::forward<Visitor>(visit));
  }

  // --- Engine thread ---------------------------------------------------------

  // Handles the commands currently queued and returns how many it got through.
  // The engine thread calls this in a loop. If the engine has not been
  // started, the caller may call it instead to run the engine by hand on its
  // own thread; nothing can drain events while it runs, so events that do not
  // fit in the event ring are then discarded and counted.
  //
  // It stops early at an Attach whose symbol has not been handed over yet,
  // leaving that command and everything behind it queued for the next call.
  std::size_t process_pending() noexcept {
    // Both of these live in locals while the batch runs, not in members: a
    // member would be written to memory once per command, which measurably
    // slows the engine thread.
    std::uint64_t orders = 0;
    std::uint32_t countdown = sample_countdown_;
    const std::size_t count = commands_.drain_while([&](const Command& command) {
      if (command.type == CommandType::Submit || command.type == CommandType::Cancel) [[likely]] {
        if (--countdown != 0) [[likely]] {
          handle_order(command);
        } else {
          countdown = handle_order_timed(command);
        }
        ++orders;
        return true;
      }
      return handle_migration(command);
    });
    sample_countdown_ = countdown;
    if (orders != 0) {
      commands_processed_.store(commands_processed_.load(std::memory_order_relaxed) + orders,
                                std::memory_order_release);
    }
    return count;
  }

  // --- Any thread ------------------------------------------------------------

  // Submits and cancels handled so far.
  [[nodiscard]] std::uint64_t commands_processed() const noexcept {
    return commands_processed_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t events_dropped() const noexcept {
    return events_dropped_.load(std::memory_order_acquire);
  }

  // How many commands the engine has handled for one symbol, whether they
  // were accepted or refused. A measure of how busy the symbol is.
  [[nodiscard]] std::uint64_t commands_handled(SymbolId symbol) const noexcept {
    assert(symbol < handled_.size());
    return handled_[symbol].load(std::memory_order_relaxed);
  }

  // An estimate of how long the engine has spent handling one symbol's
  // commands, in cycle_ticks(). Unlike commands_handled(), it tells an order
  // that sweeps fifty price levels apart from one that just rests.
  //
  // It is an estimate because only one command in EngineConfig::time_one_in is
  // timed, and the total is scaled up from those. That keeps the cost on the
  // hot path to a countdown. A timed command that had to wait for the event
  // ring is left out, and one that took wildly longer than usual is capped;
  // see cap_timed_sample(). Always 0 if timing is off.
  [[nodiscard]] std::uint64_t time_spent(SymbolId symbol) const noexcept {
    assert(symbol < timed_.size());
    return timed_[symbol].load(std::memory_order_relaxed) * time_one_in_;
  }

  // Only meaningful while the engine thread is not running, and only for a
  // symbol this engine is running.
  [[nodiscard]] const OrderBook& book(SymbolId symbol) const noexcept {
    assert(symbol < books_.size() && books_[symbol] != nullptr);
    return *books_[symbol];
  }

 private:
  static Command migration_command(CommandType type, SymbolId symbol,
                                   std::uint32_t move_number) noexcept {
    Command command{};
    command.quantity = move_number;
    command.symbol = symbol;
    command.type = type;
    return command;
  }

  void idle() const noexcept {
    if (idle_ == IdleStrategy::Yield) {
      std::this_thread::yield();
    } else {
      cpu_relax();
    }
  }

  void run() noexcept {
    if (pin_to_cpu_ != kNoPinning) {
      pinned_.store(pin_current_thread_to_cpu(pin_to_cpu_), std::memory_order_release);
    }
    started_.store(true, std::memory_order_release);

    while (!stop_requested_.load(std::memory_order_acquire)) {
      if (process_pending() == 0) {
        idle();
      }
    }

    // Shutting down: honour everything queued before stop() was called. If
    // that includes taking up a symbol another engine has not let go of yet,
    // wait for it: that engine is shutting down too and will get there.
    draining_ = true;
    do {
      process_pending();
      if (waiting_to_attach_) {
        idle();
      }
    } while (waiting_to_attach_);
    draining_ = false;
  }

  // Returns false if the command cannot be handled yet and must stay queued.
  bool handle_migration(const Command& command) noexcept {
    if (command.type == CommandType::Detach) {
      handle_detach(command);
      return true;
    }
    return command.type != CommandType::Attach || handle_attach(command);
  }

  // Handles one command with a stopwatch on it, and returns how many commands
  // to let pass before the next one is timed.
  //
  // The gap is random, between 1 and twice the configured average, rather
  // than fixed. A fixed gap could fall into step with a pattern in the order
  // flow and keep timing the same symbol.
  std::uint32_t handle_order_timed(const Command& command) noexcept {
    waited_while_timing_ = false;
    const std::uint64_t before = cycle_ticks();
    handle_order(command);
    const std::uint64_t elapsed = cycle_ticks() - before;

    // Time spent waiting for a full event ring says how slow the reader is,
    // not how much work the symbol is, so such a sample is thrown away.
    if (!waited_while_timing_ && command.symbol < timed_.size()) {
      const std::uint64_t counted = cap_timed_sample(elapsed, typical_sample_x256_);
      std::atomic<std::uint64_t>& timed = timed_[command.symbol];
      timed.store(timed.load(std::memory_order_relaxed) + counted, std::memory_order_relaxed);
    }

    sample_random_ = sample_random_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return 1 + static_cast<std::uint32_t>((sample_random_ >> 33) % (2 * time_one_in_ - 1));
  }

  void handle_order(const Command& command) noexcept {
    // Routing is one bounds check and one array index. A symbol this engine
    // is not running has no book here, and the command is answered as refused.
    OrderBook* const book = command.symbol < books_.size() ? books_[command.symbol] : nullptr;

    if (book != nullptr) [[likely]] {
      // Only this thread writes the counter, so a plain add is enough.
      std::atomic<std::uint64_t>& handled = handled_[command.symbol];
      handled.store(handled.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    Event event{};
    event.client_tag = command.client_tag;
    event.symbol = command.symbol;

    if (command.type == CommandType::Submit) {
      SubmitResult result{kInvalidOrderId, 0, 0};
      if (book != nullptr) [[likely]] {
        result = book->submit(command.side, command.price, command.quantity, command.order_type,
                              [&](const Trade& trade) {
                                Event fill{};
                                fill.client_tag = command.client_tag;
                                fill.order_id = trade.taker_id;
                                fill.maker_id = trade.maker_id;
                                fill.price = trade.price;
                                fill.quantity = trade.quantity;
                                fill.symbol = command.symbol;
                                fill.side = trade.taker_side;
                                fill.type = EventType::Trade;
                                publish(fill);
                              });
      }
      event.order_id = result.id;
      event.price = command.price;
      event.quantity = result.filled;
      event.resting = result.resting;
      event.side = command.side;
      event.type = result.id != kInvalidOrderId ? EventType::Accepted : EventType::Rejected;
    } else {
      const bool cancelled = book != nullptr && book->cancel(command.order_id);
      event.order_id = command.order_id;
      event.type = cancelled ? EventType::Cancelled : EventType::CancelRejected;
    }
    publish(event);
  }

  // Stop running a symbol. Everything queued for it ahead of this command has
  // been handled, because commands are handled in order.
  void handle_detach(const Command& command) noexcept {
    assert(directory_ != nullptr && command.symbol < books_.size());
    books_[command.symbol] = nullptr;
    directory_->gates[command.symbol].detached.store(command.quantity,
                                                    std::memory_order_release);

    // The last thing this engine says about the symbol. Whoever reads the
    // event ring reaches it only after every earlier event for the symbol.
    Event handoff{};
    handoff.symbol = command.symbol;
    handoff.quantity = command.quantity;
    handoff.type = EventType::Handoff;
    if (!publish(handoff, /*count_if_discarded=*/false)) {
      // It could not be published, which only happens when events are being
      // discarded anyway. Nobody will ever read it, so open the gate here
      // rather than leave the symbol stranded between engines.
      directory_->gates[command.symbol].delivered.store(command.quantity,
                                                       std::memory_order_release);
    }
  }

  // Take a symbol up, if it has been handed over.
  //
  // Normally that means the old engine's Handoff has been read from its event
  // ring, so that everything it said about the symbol is delivered before this
  // engine says anything. During shutdown nobody may be reading, so it is
  // enough that the old engine has let go.
  bool handle_attach(const Command& command) noexcept {
    assert(directory_ != nullptr && command.symbol < books_.size());
    MigrationGate& gate = directory_->gates[command.symbol];
    const std::uint32_t move_number = command.quantity;
    const bool handed_over =
        gate.delivered.load(std::memory_order_acquire) >= move_number ||
        (draining_ && gate.detached.load(std::memory_order_acquire) >= move_number);
    if (!handed_over) {
      waiting_to_attach_ = true;
      return false;
    }
    books_[command.symbol] = directory_->books[command.symbol];
    gate.attached.store(move_number, std::memory_order_release);
    waiting_to_attach_ = false;
    return true;
  }

  // Returns false if the event had to be discarded: the ring is full and the
  // engine is not running, or is shutting down.
  bool publish(const Event& event, bool count_if_discarded = true) noexcept {
    while (!events_.try_push(event)) {
      waited_while_timing_ = true;
      if (stop_requested_.load(std::memory_order_acquire)) {
        if (count_if_discarded) {
          events_dropped_.store(events_dropped_.load(std::memory_order_relaxed) + 1,
                                std::memory_order_release);
        }
        return false;
      }
      cpu_relax();
    }
    return true;
  }

  std::vector<std::unique_ptr<OrderBook>> owned_;    // books this engine created, if any
  std::vector<OrderBook*> books_;                    // the book it runs for each SymbolId, or
                                                     // nullptr; changed only by the engine thread
  std::vector<std::atomic<std::uint64_t>> handled_;  // commands handled, per symbol
  std::vector<std::atomic<std::uint64_t>> timed_;    // ticks spent on the timed ones, per symbol
  SymbolDirectory* directory_ = nullptr;             // set only for a member of a group
  SpscRing<Command> commands_;
  SpscRing<Event> events_;
  IdleStrategy idle_;
  int pin_to_cpu_;
  std::uint32_t time_one_in_;

  // Touched only by whichever thread is running the engine. The countdown is
  // read and written once per batch; the rest only when a command is timed.
  //
  // With timing off the countdown starts at zero, so counting down wraps it
  // round instead of ever reaching zero, and no command is timed. (Strictly,
  // one is every four billion; time_spent() still reports nothing.)
  std::uint32_t sample_countdown_;  // commands still to pass before the next timed one
  std::uint64_t sample_random_ = 0x9E3779B97F4A7C15ULL;
  std::uint64_t typical_sample_x256_ = 0;  // see cap_timed_sample()
  bool waited_while_timing_ = false;  // the timed command had to wait for the event ring

  // Touched only by whichever thread is running the engine, and only when a
  // symbol is changing hands.
  bool waiting_to_attach_ = false;  // the queue is stopped at an Attach that is not ready
  bool draining_ = false;           // the engine is shutting down

  // True whenever the engine thread should not be (or is not) running.
  alignas(kCacheLineSize) std::atomic<bool> stop_requested_{true};
  std::atomic<bool> started_{false};
  std::atomic<bool> pinned_{false};
  std::atomic<std::uint64_t> commands_processed_{0};
  std::atomic<std::uint64_t> events_dropped_{0};

  std::thread thread_;
};

}  // namespace lob

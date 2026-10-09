#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
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

  // All memory for every book and both rings is acquired here.
  explicit MatchingEngine(const EngineConfig& config)
      : handled_(config.books.size()),
        commands_(config.command_capacity),
        events_(config.event_capacity),
        idle_(config.idle),
        pin_to_cpu_(config.pin_to_cpu) {
    assert(!config.books.empty() && config.books.size() <= kMaxSymbols);
    books_.reserve(config.books.size());
    for (const BookConfig& book : config.books) {
      books_.push_back(std::make_unique<OrderBook>(book));
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
    if (!thread_.joinable()) {
      return;
    }
    stop_requested_.store(true, std::memory_order_release);
    thread_.join();
  }

  [[nodiscard]] bool running() const noexcept { return thread_.joinable(); }

  // Whether the engine thread is bound to the CPU asked for in the config.
  // False if no CPU was asked for, or if the request could not be honoured;
  // the engine runs either way, so check this if pinning matters to you.
  [[nodiscard]] bool pinned() const noexcept { return pinned_.load(std::memory_order_acquire); }

  // --- Symbols: any thread ---------------------------------------------------

  [[nodiscard]] std::size_t symbol_count() const noexcept { return books_.size(); }

  // The ID of the book with this symbol name, if there is one. A linear search
  // over the names: do it once at start-up, not per order.
  [[nodiscard]] std::optional<SymbolId> symbol_id(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < books_.size(); ++index) {
      if (books_[index]->symbol() == name) {
        return static_cast<SymbolId>(index);
      }
    }
    return std::nullopt;
  }

  // --- Gateway thread --------------------------------------------------------
  // Both return false if the command ring is full; the caller decides whether
  // to retry. A command for a symbol the engine does not have is accepted here
  // and answered with Rejected or CancelRejected.

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

  // --- Publisher thread ------------------------------------------------------

  // Passes every event published so far to `visit`, in order. Returns how many.
  template <std::invocable<const Event&> Visitor>
  std::size_t poll(Visitor&& visit) noexcept {
    return events_.drain(std::forward<Visitor>(visit));
  }

  // --- Engine thread ---------------------------------------------------------

  // Handles every command currently queued and returns how many there were.
  // The engine thread calls this in a loop. If the engine has not been
  // started, the caller may call it instead to run the engine by hand on its
  // own thread; nothing can drain events while it runs, so events that do not
  // fit in the event ring are then discarded and counted.
  std::size_t process_pending() noexcept {
    const std::size_t count =
        commands_.drain([this](const Command& command) { handle(command); });
    if (count != 0) {
      commands_processed_.store(commands_processed_.load(std::memory_order_relaxed) + count,
                                std::memory_order_release);
    }
    return count;
  }

  // --- Any thread ------------------------------------------------------------

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

  // Only meaningful while the engine thread is not running.
  [[nodiscard]] const OrderBook& book(SymbolId symbol) const noexcept {
    assert(symbol < books_.size());
    return *books_[symbol];
  }

 private:
  void run() noexcept {
    if (pin_to_cpu_ != kNoPinning) {
      pinned_.store(pin_current_thread_to_cpu(pin_to_cpu_), std::memory_order_release);
    }
    started_.store(true, std::memory_order_release);

    while (!stop_requested_.load(std::memory_order_acquire)) {
      if (process_pending() == 0) {
        if (idle_ == IdleStrategy::Yield) {
          std::this_thread::yield();
        } else {
          cpu_relax();
        }
      }
    }
    process_pending();  // honour everything queued before stop() was called
  }

  void handle(const Command& command) noexcept {
    // Routing is one bounds check and one array index. A symbol the engine
    // does not have gets no book, and the command is answered as refused.
    OrderBook* const book =
        command.symbol < books_.size() ? books_[command.symbol].get() : nullptr;

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

  void publish(const Event& event) noexcept {
    while (!events_.try_push(event)) {
      if (stop_requested_.load(std::memory_order_acquire)) {
        events_dropped_.store(events_dropped_.load(std::memory_order_relaxed) + 1,
                              std::memory_order_release);
        return;
      }
      cpu_relax();
    }
  }

  std::vector<std::unique_ptr<OrderBook>> books_;    // indexed by SymbolId
  std::vector<std::atomic<std::uint64_t>> handled_;  // commands handled, per symbol
  SpscRing<Command> commands_;
  SpscRing<Event> events_;
  IdleStrategy idle_;
  int pin_to_cpu_;

  // True whenever the engine thread should not be (or is not) running.
  alignas(kCacheLineSize) std::atomic<bool> stop_requested_{true};
  std::atomic<bool> started_{false};
  std::atomic<bool> pinned_{false};
  std::atomic<std::uint64_t> commands_processed_{0};
  std::atomic<std::uint64_t> events_dropped_{0};

  std::thread thread_;
};

}  // namespace lob

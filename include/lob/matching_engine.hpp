#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

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
  BookConfig book;
  std::size_t command_capacity = 1 << 16;  // inbound ring; rounded up to a power of two
  std::size_t event_capacity = 1 << 16;    // outbound ring; rounded up to a power of two
  IdleStrategy idle = IdleStrategy::Spin;
  int pin_to_cpu = kNoPinning;             // CPU to bind the engine thread to (Linux only)
};

// An order book running on its own thread, fed and read through two SPSC rings:
//
//   gateway thread --Command--> [ engine thread: OrderBook ] --Event--> publisher thread
//
// The book stays single-threaded and lock-free; the rings are the only things
// shared between threads. Each ring allows exactly one thread on each end, so:
//
//   submit(), cancel()   one gateway thread
//   poll()               one publisher thread
//   start(), stop()      one controlling thread
//
// The gateway and publisher may be the same thread.
//
// Events are never dropped while the engine is running: if the event ring is
// full, the engine waits for the publisher. The one exception is shutdown, to
// guarantee stop() returns even if nobody is reading: events that still cannot
// be delivered once stop() has been called are discarded and counted in
// events_dropped(). To lose nothing, poll until every command has been
// answered, then call stop().
class MatchingEngine {
 public:
  explicit MatchingEngine(const EngineConfig& config)
      : book_(config.book),
        commands_(config.command_capacity),
        events_(config.event_capacity),
        idle_(config.idle),
        pin_to_cpu_(config.pin_to_cpu) {}

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

  // --- Gateway thread --------------------------------------------------------
  // Both return false if the command ring is full; the caller decides whether
  // to retry.

  [[nodiscard]] bool submit(std::uint64_t client_tag, Side side, Price price, Quantity quantity,
                            OrderType order_type = OrderType::Limit) noexcept {
    Command command{};
    command.client_tag = client_tag;
    command.price = price;
    command.quantity = quantity;
    command.side = side;
    command.type = CommandType::Submit;
    command.order_type = order_type;
    return commands_.try_push(command);
  }

  [[nodiscard]] bool cancel(std::uint64_t client_tag, OrderId order_id) noexcept {
    Command command{};
    command.client_tag = client_tag;
    command.order_id = order_id;
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

  // Only meaningful while the engine thread is not running.
  [[nodiscard]] const OrderBook& book() const noexcept { return book_; }

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
    Event event{};
    event.client_tag = command.client_tag;

    if (command.type == CommandType::Submit) {
      const SubmitResult result = book_.submit(
          command.side, command.price, command.quantity, command.order_type,
          [&](const Trade& trade) {
            Event fill{};
            fill.client_tag = command.client_tag;
            fill.order_id = trade.taker_id;
            fill.maker_id = trade.maker_id;
            fill.price = trade.price;
            fill.quantity = trade.quantity;
            fill.side = trade.taker_side;
            fill.type = EventType::Trade;
            publish(fill);
          });
      event.order_id = result.id;
      event.price = command.price;
      event.quantity = result.filled;
      event.resting = result.resting;
      event.side = command.side;
      event.type = result.id != kInvalidOrderId ? EventType::Accepted : EventType::Rejected;
    } else {
      event.order_id = command.order_id;
      event.type =
          book_.cancel(command.order_id) ? EventType::Cancelled : EventType::CancelRejected;
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

  OrderBook book_;
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

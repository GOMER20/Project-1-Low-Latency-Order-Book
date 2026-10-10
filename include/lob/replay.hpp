#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "lob/matching_engine.hpp"
#include "lob/messages.hpp"
#include "lob/sharded_engine.hpp"

// Replaying a recorded session.
//
// replay() feeds a session's commands, in their recorded order, to an engine.
// Given books like the ones the session ran with, the engine then says and
// does exactly what the original did:
//
//   - every symbol's events are the same, in the same order: the same order
//     IDs, the same fills against the same resting orders, the same refusals;
//   - every symbol's market data is the same, message for message, down to
//     the sequence numbers;
//   - the books end up holding the same orders in the same queues.
//
// "Every symbol's", because that is as far as order goes. Symbols are
// independent, and on a ShardedEngine two symbols on different shards never
// had an order between them to reproduce.
//
// The engine replayed into need not be the same shape as the one recorded. A
// session recorded on eight shards replays on one engine run by hand, which is
// the convenient way to step through it in a debugger.
//
// What is not reproduced is time. A recording holds the order of the commands,
// not when they were sent; a replay goes as fast as the engine will take them.

namespace lob {

struct ReplayOptions {
  // What to do with a recorded move of a symbol from one shard to another.
  //
  // Which shard runs a symbol changes nothing that the symbol says or does,
  // with one exception: on arriving, the symbol puts a fresh picture of itself
  // on the market data feed. So by default a move is replayed as just that, a
  // request for a snapshot, which works on any engine and keeps the feed
  // exact.
  //
  // Set this to have a ShardedEngine really move the symbol, to the shard it
  // went to in the session, at the same point in the stream of commands. That
  // is for looking into the moving itself. Build the engine with the same
  // number of shards and the same ShardedConfig::loads as the original, and
  // with RebalanceConfig::every left at 0: the recorded moves already include
  // every move that rebalancing made.
  bool repeat_moves = false;
};

// What a replay did.
struct ReplayResult {
  std::uint64_t commands = 0;  // orders, cancels and snapshot requests sent to the engine
  std::uint64_t moves = 0;     // recorded moves replayed, either way
  std::uint64_t skipped = 0;   // commands a ShardedEngine could not take: see replay()
  std::uint64_t events = 0;    // events the engine answered with
};

namespace detail {

enum class Replayed : std::uint8_t {
  Sent,
  Full,     // a ring is full: try again
  Skipped,  // the engine will never take this command
};

[[nodiscard]] constexpr Replayed replayed(bool queued) noexcept {
  return queued ? Replayed::Sent : Replayed::Full;
}

[[nodiscard]] constexpr Replayed replayed(SendStatus status) noexcept {
  switch (status) {
    case SendStatus::Sent:
      return Replayed::Sent;
    case SendStatus::RingFull:
      return Replayed::Full;
    case SendStatus::UnknownSymbol:
      break;
  }
  return Replayed::Skipped;
}

// Sends one recorded command.

[[nodiscard]] inline Replayed replay_one(MatchingEngine& engine, const Command& command,
                                         const ReplayOptions&) noexcept {
  switch (command.type) {
    case CommandType::Submit:
      return replayed(engine.submit(command.client_tag, command.symbol, command.side,
                                    command.price, command.quantity, command.order_type));
    case CommandType::Cancel:
      return replayed(engine.cancel(command.client_tag, command.symbol, command.order_id));
    case CommandType::Snapshot:
    case CommandType::Move:
      return replayed(engine.request_snapshot(command.symbol));
    case CommandType::Detach:
    case CommandType::Attach:
      break;  // never recorded
  }
  return Replayed::Skipped;
}

[[nodiscard]] inline Replayed replay_one(ShardedEngine& engine, const Command& command,
                                         const ReplayOptions& options) noexcept {
  switch (command.type) {
    case CommandType::Submit:
      return replayed(engine.submit(command.client_tag, command.symbol, command.side,
                                    command.price, command.quantity, command.order_type));
    case CommandType::Cancel:
      return replayed(engine.cancel(command.client_tag, command.symbol, command.order_id));
    case CommandType::Move:
      // A move that this engine cannot repeat, because it has no such shard or
      // the symbol is already there, still owes the feed its snapshot.
      if (options.repeat_moves && command.symbol < engine.symbol_count() &&
          command.quantity < engine.shard_count() &&
          command.quantity != engine.shard_of(command.symbol)) {
        return replayed(engine.move_symbol(command.symbol, command.quantity));
      }
      [[fallthrough]];
    case CommandType::Snapshot:
      return replayed(engine.request_snapshot(command.symbol));
    case CommandType::Detach:
    case CommandType::Attach:
      break;  // never recorded
  }
  return Replayed::Skipped;
}

template <typename Engine, typename OnEvent, typename ReadFeed>
ReplayResult run_replay(std::span<const Command> commands, Engine& engine, OnEvent& on_event,
                        ReadFeed&& read_feed, const ReplayOptions& options) {
  // An engine with no thread of its own is run from here, one command at a
  // time: nothing else is reading its events while it works, so they are read
  // after every command, before they can pile up.
  const bool by_hand = !engine.running();
  ReplayResult result;

  const auto pump = [&]() -> std::size_t {
    std::size_t progress = by_hand ? engine.process_pending() : 0;
    progress += engine.poll([&](const Event& event) {
      ++result.events;
      on_event(event);
    });
    progress += read_feed();
    return progress;
  };

  constexpr std::uint64_t kReadEvery = 64;  // commands between reads, on a running engine
  std::uint64_t sent = 0;
  for (const Command& command : commands) {
    Replayed outcome = replay_one(engine, command, options);
    while (outcome == Replayed::Full) {
      // Reading events is what lets the engine get on and make room.
      if (pump() == 0) {
        std::this_thread::yield();
      }
      outcome = replay_one(engine, command, options);
    }
    if (outcome == Replayed::Skipped) {
      ++result.skipped;
      continue;
    }
    if (command.type == CommandType::Move) {
      ++result.moves;
    } else {
      ++result.commands;
    }
    if (by_hand || ++sent % kReadEvery == 0) {
      pump();
    }
  }

  // Wait for the engine to finish everything, then collect what is left.
  while (engine.commands_pending() != 0) {
    if (pump() == 0) {
      std::this_thread::yield();
    }
  }
  pump();
  return result;
}

}  // namespace detail

// Feeds `commands` to `engine` in order, passing every event the engine
// answers with to `on_event`, and returns once the engine has dealt with them
// all. `commands` is usually Recording::commands(), or the first part of it to
// stop the session at a chosen point.
//
// The engine may be running on its own thread or threads, or not started, in
// which case the replay runs it by hand on the calling thread. Either way the
// calling thread must be the only one sending the engine commands and reading
// its events while the replay lasts.
//
// For an exact replay, build the engine with the recorded books
// (Recording::books()) and start from a fresh one.
//
// A command for a symbol the engine does not have is treated as that engine
// treats any such command. A MatchingEngine takes it and answers that it is
// refused, which is what it did in the session. A ShardedEngine will not take
// it at all, so it is skipped, and counted in ReplayResult::skipped.
//
// The market data feed, if the engine has one, is left alone; use the other
// form to read it.
template <typename Engine, std::invocable<const Event&> OnEvent>
ReplayResult replay(std::span<const Command> commands, Engine& engine, OnEvent&& on_event,
                    const ReplayOptions& options = {}) {
  return detail::run_replay(
      commands, engine, on_event, []() -> std::size_t { return 0; }, options);
}

// The same, also passing every market data message to `on_market_data`. The
// calling thread must then be the only one reading the engine's market data.
template <typename Engine, std::invocable<const Event&> OnEvent,
          std::invocable<const MarketData&> OnMarketData>
ReplayResult replay(std::span<const Command> commands, Engine& engine, OnEvent&& on_event,
                    OnMarketData&& on_market_data, const ReplayOptions& options = {}) {
  return detail::run_replay(
      commands, engine, on_event,
      [&]() -> std::size_t { return engine.poll_market_data(on_market_data); }, options);
}

// A fingerprint of everything an engine said during a session: feed it every
// event, and every market data message if there is a feed. Two runs that
// produce equal digests said the same things, so comparing a replay with the
// original takes one line and no stored output.
//
// It follows the same rule as replay(): the order of a symbol's events counts,
// and the order between symbols does not. Add each symbol's events in the
// order they arrive. Market data may be added in any order at all: every
// message carries its own sequence number, which is part of what is compared.
// That matters across a move between shards, when a reader of two rings can
// meet a symbol's messages out of order.
class SessionDigest {
 public:
  // `symbols` is the number of books in the session.
  explicit SessionDigest(std::size_t symbols)
      : events_(symbols + 1, kSeed), market_data_(symbols + 1, kSeed) {}

  void add(const Event& event) noexcept {
    std::uint64_t& chain = events_[slot(event.symbol)];
    chain = mix(chain, event.client_tag);
    chain = mix(chain, event.order_id);
    chain = mix(chain, event.maker_id);
    chain = mix(chain, static_cast<std::uint64_t>(event.price));
    chain = mix(chain, (std::uint64_t{event.quantity} << 32) | event.resting);
    chain = mix(chain, (std::uint64_t{event.symbol} << 16) |
                           (static_cast<std::uint64_t>(event.side) << 8) |
                           static_cast<std::uint64_t>(event.type));
    ++count_;
  }

  void add(const MarketData& message) noexcept {
    std::uint64_t one = mix(kSeed, message.sequence);
    one = mix(one, static_cast<std::uint64_t>(message.price));
    one = mix(one, message.quantity);
    one = mix(one, static_cast<std::uint64_t>(message.bid_price));
    one = mix(one, message.bid_quantity);
    one = mix(one, static_cast<std::uint64_t>(message.ask_price));
    one = mix(one, message.ask_quantity);
    one = mix(one, (std::uint64_t{message.symbol} << 16) |
                       (static_cast<std::uint64_t>(message.side) << 8) |
                       static_cast<std::uint64_t>(message.type));
    // Added up rather than chained, so that the order does not matter.
    market_data_[slot(message.symbol)] += one;
    ++count_;
  }

  // One number for the whole session.
  [[nodiscard]] std::uint64_t value() const noexcept {
    std::uint64_t total = mix(kSeed, count_);
    for (const std::uint64_t chain : events_) {
      total = mix(total, chain);
    }
    for (const std::uint64_t chain : market_data_) {
      total = mix(total, chain);
    }
    return total;
  }

  // How many events and messages have been added.
  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

  friend bool operator==(const SessionDigest&, const SessionDigest&) = default;

 private:
  static constexpr std::uint64_t kSeed = 0x9E3779B97F4A7C15ULL;

  // Folds one more value into a chain. Both steps can be undone, so two
  // chains that differ stay different whatever is added to both.
  [[nodiscard]] static constexpr std::uint64_t mix(std::uint64_t chain,
                                                   std::uint64_t item) noexcept {
    chain = (chain ^ item) * 0xFF51AFD7ED558CCDULL;
    return chain ^ (chain >> 33);
  }

  // Anything for a symbol outside the session shares the last slot.
  [[nodiscard]] std::size_t slot(SymbolId symbol) const noexcept {
    return std::min<std::size_t>(symbol, events_.size() - 1);
  }

  std::vector<std::uint64_t> events_;       // one chain per symbol
  std::vector<std::uint64_t> market_data_;  // one sum per symbol
  std::uint64_t count_ = 0;
};

}  // namespace lob

#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "lob/matching_engine.hpp"

namespace lob {

struct ShardedConfig {
  std::vector<BookConfig> books;           // one per symbol; a symbol's ID is its position here
  std::size_t shards = 1;                  // engine threads; never more than there are symbols
  std::size_t command_capacity = 1 << 16;  // per shard
  std::size_t event_capacity = 1 << 16;    // per shard
  IdleStrategy idle = IdleStrategy::Spin;
  std::vector<int> pin_to_cpus = {};       // optional: the CPU for each shard, in order (Linux only)
};

// What became of a command handed to a ShardedEngine.
enum class SendStatus : std::uint8_t {
  Sent,           // queued for its shard
  RingFull,       // the shard's command ring is full; try again
  UnknownSymbol,  // no such symbol; retrying will not help
};

// Several MatchingEngines side by side, to use more than one core:
//
//                     +--Command--> [ shard 0 thread: its books ] --Event--+
//   gateway thread(s) +--Command--> [ shard 1 thread: its books ] --Event--+ publisher thread(s)
//                     +--Command--> [ shard 2 thread: its books ] --Event--+
//
// Each symbol lives on exactly one shard, and each shard is a complete
// MatchingEngine with its own thread and its own pair of rings. Shards share
// nothing, so there is still no lock anywhere and no cache line that two
// engine threads both write.
//
// Symbols are dealt to the shards in turn: symbol 0 to shard 0, symbol 1 to
// shard 1, and so on round the shards. Callers use one set of symbol IDs for
// the whole group; a small table turns an ID into its shard and its position
// there, so routing is still one array lookup.
//
// Threads. Every ring still has exactly one thread on each end, so:
//
//   submit(), cancel()   one thread per shard. One thread may feed every
//                        shard, or each shard may have a feeder of its own.
//   poll()               one thread per shard, on the same terms.
//   start(), stop()      one controlling thread
//
// Order. Events for one symbol arrive in the order they happened. Events for
// symbols on different shards have no order relative to each other.
class ShardedEngine {
 public:
  explicit ShardedEngine(const ShardedConfig& config) {
    assert(!config.books.empty());
    const std::size_t shard_count =
        std::clamp<std::size_t>(config.shards, 1, config.books.size());
    assert(config.pin_to_cpus.empty() || config.pin_to_cpus.size() >= shard_count);

    // Deal the symbols round the shards, remembering both directions.
    std::vector<std::vector<BookConfig>> books_of(shard_count);
    global_of_.resize(shard_count);
    routes_.reserve(config.books.size());
    for (std::size_t symbol = 0; symbol < config.books.size(); ++symbol) {
      const std::size_t shard = symbol % shard_count;
      routes_.push_back({static_cast<std::uint16_t>(shard),
                         static_cast<SymbolId>(books_of[shard].size())});
      books_of[shard].push_back(config.books[symbol]);
      global_of_[shard].push_back(static_cast<SymbolId>(symbol));
    }

    shards_.reserve(shard_count);
    for (std::size_t shard = 0; shard < shard_count; ++shard) {
      shards_.push_back(std::make_unique<MatchingEngine>(EngineConfig{
          .books = std::move(books_of[shard]),
          .command_capacity = config.command_capacity,
          .event_capacity = config.event_capacity,
          .idle = config.idle,
          .pin_to_cpu = config.pin_to_cpus.empty() ? kNoPinning : config.pin_to_cpus[shard],
      }));
    }
  }

  // --- Controlling thread ----------------------------------------------------

  // Returns once every shard's thread is up.
  void start() {
    for (const auto& shard : shards_) {
      shard->start();
    }
  }

  // Each shard handles everything already queued for it, then its thread is
  // joined. Safe to call when the shards are not running.
  void stop() {
    for (const auto& shard : shards_) {
      shard->stop();
    }
  }

  // Whether this shard's thread is bound to the CPU it was given.
  [[nodiscard]] bool pinned(std::size_t shard) const noexcept {
    return shards_[shard]->pinned();
  }

  // --- Layout: any thread ----------------------------------------------------

  [[nodiscard]] std::size_t shard_count() const noexcept { return shards_.size(); }
  [[nodiscard]] std::size_t symbol_count() const noexcept { return routes_.size(); }

  // The shard a symbol lives on. The symbol must exist.
  [[nodiscard]] std::size_t shard_of(SymbolId symbol) const noexcept {
    assert(symbol < routes_.size());
    return routes_[symbol].shard;
  }

  // The ID of the symbol with this name, if there is one. Do it once at
  // start-up, not per order.
  [[nodiscard]] std::optional<SymbolId> symbol_id(std::string_view name) const noexcept {
    for (std::size_t symbol = 0; symbol < routes_.size(); ++symbol) {
      const Route route = routes_[symbol];
      if (shards_[route.shard]->book(route.local).symbol() == name) {
        return static_cast<SymbolId>(symbol);
      }
    }
    return std::nullopt;
  }

  // --- Gateway threads -------------------------------------------------------

  [[nodiscard]] SendStatus submit(std::uint64_t client_tag, SymbolId symbol, Side side,
                                  Price price, Quantity quantity,
                                  OrderType order_type = OrderType::Limit) noexcept {
    if (symbol >= routes_.size()) [[unlikely]] {
      return SendStatus::UnknownSymbol;
    }
    const Route route = routes_[symbol];
    return shards_[route.shard]->submit(client_tag, route.local, side, price, quantity,
                                        order_type)
               ? SendStatus::Sent
               : SendStatus::RingFull;
  }

  [[nodiscard]] SendStatus cancel(std::uint64_t client_tag, SymbolId symbol,
                                  OrderId order_id) noexcept {
    if (symbol >= routes_.size()) [[unlikely]] {
      return SendStatus::UnknownSymbol;
    }
    const Route route = routes_[symbol];
    return shards_[route.shard]->cancel(client_tag, route.local, order_id)
               ? SendStatus::Sent
               : SendStatus::RingFull;
  }

  // --- Publisher threads -----------------------------------------------------

  // Passes every event one shard has published so far to `visit`, in order.
  // Events carry the group's symbol IDs, not the shard's own.
  template <std::invocable<const Event&> Visitor>
  std::size_t poll(std::size_t shard, Visitor&& visit) noexcept {
    const std::vector<SymbolId>& global = global_of_[shard];
    return shards_[shard]->poll([&](const Event& event) {
      assert(event.symbol < global.size());
      Event translated = event;
      translated.symbol = global[event.symbol];
      visit(translated);
    });
  }

  // The same for every shard in turn. Use this when one thread reads them all.
  template <std::invocable<const Event&> Visitor>
  std::size_t poll(Visitor&& visit) noexcept {
    std::size_t polled = 0;
    for (std::size_t shard = 0; shard < shards_.size(); ++shard) {
      polled += poll(shard, visit);
    }
    return polled;
  }

  // --- Running the shards by hand --------------------------------------------

  // Handles everything queued for every shard, on the calling thread. Only
  // for when the shards have not been started; see MatchingEngine.
  std::size_t process_pending() noexcept {
    std::size_t handled = 0;
    for (const auto& shard : shards_) {
      handled += shard->process_pending();
    }
    return handled;
  }

  // --- Any thread ------------------------------------------------------------

  [[nodiscard]] std::uint64_t commands_processed() const noexcept {
    std::uint64_t total = 0;
    for (const auto& shard : shards_) {
      total += shard->commands_processed();
    }
    return total;
  }

  [[nodiscard]] std::uint64_t events_dropped() const noexcept {
    std::uint64_t total = 0;
    for (const auto& shard : shards_) {
      total += shard->events_dropped();
    }
    return total;
  }

  // Only meaningful while the shards are not running.
  [[nodiscard]] const OrderBook& book(SymbolId symbol) const noexcept {
    assert(symbol < routes_.size());
    const Route route = routes_[symbol];
    return shards_[route.shard]->book(route.local);
  }

 private:
  // Where a symbol lives: which shard, and which of that shard's books.
  struct Route {
    std::uint16_t shard;
    SymbolId local;
  };

  std::vector<Route> routes_;                           // indexed by the group's SymbolId
  std::vector<std::vector<SymbolId>> global_of_;        // [shard][shard's SymbolId] -> group's
  std::vector<std::unique_ptr<MatchingEngine>> shards_;
};

}  // namespace lob

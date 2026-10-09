#pragma once

#include <algorithm>
#include <atomic>
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
#include "lob/shard_assignment.hpp"

namespace lob {

// What ShardedEngine::rebalance() counts as a symbol's load.
enum class LoadMeasure : std::uint8_t {
  // The number of commands handled for the symbol. Exact and repeatable, but
  // it treats every command as equal work.
  Commands,
  // The time the engine has spent on the symbol's commands. It tells a symbol
  // whose orders sweep many price levels from one whose orders just rest, but
  // it is an estimate from a sample of commands, and a few percent noisier.
  Time,
};

// When and how readily ShardedEngine::rebalance() moves a symbol.
struct RebalanceConfig {
  // Look at the load after every this-many commands have been sent, from
  // inside submit() and cancel(). 0 leaves it to the caller to call
  // rebalance(). Turn this on only if one thread feeds every shard.
  std::uint64_t every = 0;

  // Leave things alone while the busiest shard carries no more than its fair
  // share plus this much. Each move briefly pauses a shard, so small or
  // passing differences are not worth one.
  std::uint32_t tolerance_percent = 25;

  // Do not judge on fewer handled commands than this.
  std::uint64_t min_sample = 1'000;

  // What to balance: commands handled, or time spent.
  LoadMeasure measure = LoadMeasure::Commands;
};

struct ShardedConfig {
  std::vector<BookConfig> books;           // one per symbol; a symbol's ID is its position here
  std::size_t shards = 1;                  // engine threads; never more than there are symbols
  std::vector<std::uint64_t> loads = {};   // optional: how busy each symbol is expected to be
  RebalanceConfig rebalance = {};          // optional: evening out the shards as traffic shifts
  std::size_t command_capacity = 1 << 16;  // per shard
  std::size_t event_capacity = 1 << 16;    // per shard
  IdleStrategy idle = IdleStrategy::Spin;
  std::vector<int> pin_to_cpus = {};       // optional: the CPU for each shard, in order (Linux only)
  std::uint32_t time_one_in = 64;          // see EngineConfig::time_one_in; 0 turns timing off
};

// What became of a command handed to a ShardedEngine.
enum class SendStatus : std::uint8_t {
  Sent,           // queued for its shard
  RingFull,       // a command ring is full; try again
  UnknownSymbol,  // no such symbol (or, for a move, no such shard); retrying will not help
};

// Several MatchingEngines side by side, to use more than one core:
//
//                     +--Command--> [ shard 0 thread: its books ] --Event--+
//   gateway thread(s) +--Command--> [ shard 1 thread: its books ] --Event--+ publisher thread(s)
//                     +--Command--> [ shard 2 thread: its books ] --Event--+
//
// Each symbol is run by exactly one shard at a time, and each shard is a
// complete MatchingEngine with its own thread and its own pair of rings.
// Shards share nothing on the hot path, so there is still no lock anywhere
// and no cache line that two engine threads both write.
//
// The group owns every book. A shard only holds pointers to the books it is
// running, which is what lets a symbol change shards.
//
// Which shard a symbol starts on is decided in the constructor. By default
// the symbols are dealt in turn: symbol 0 to shard 0, symbol 1 to shard 1, and
// so on round the shards. If ShardedConfig::loads says how busy each symbol is
// expected to be, the busy ones are spread out so that every shard carries a
// similar total; see assign_shards(). measured_loads() reports what each
// symbol actually received, ready to be used as the loads for the next run.
//
// move_symbol() moves a symbol to another shard while the engine runs, and
// rebalance() uses it to even the shards out as traffic shifts: it measures
// what each symbol has been receiving and, if one shard is carrying clearly
// more than its share, moves one symbol off it.
//
// Callers use one set of symbol IDs for the whole group, wherever a symbol is
// running; a table of one entry per symbol says which shard that is.
//
// Threads. Every ring still has exactly one thread on each end, so:
//
//   submit(), cancel()   one thread per shard. One thread may feed every
//                        shard, or each shard may have a feeder of its own.
//   poll()               one thread per shard, on the same terms.
//   move_symbol()        the thread that feeds both shards involved
//   rebalance()          the thread that feeds every shard
//   start(), stop()      one controlling thread
//
// Order. Events for one symbol arrive in the order they happened, including
// across a move. Events for symbols on different shards have no order
// relative to each other.
class ShardedEngine {
 public:
  explicit ShardedEngine(const ShardedConfig& config)
      : directory_(config.books.size()),
        shard_of_(config.books.size()),
        moves_(config.books.size()),
        rebalance_(config.rebalance) {
    assert(!config.books.empty() && config.books.size() <= MatchingEngine::kMaxSymbols);
    const std::size_t shard_count =
        std::clamp<std::size_t>(config.shards, 1, config.books.size());
    assert(config.pin_to_cpus.empty() || config.pin_to_cpus.size() >= shard_count);

    // With no loads given, every symbol counts the same, which deals them out
    // in turn.
    assert(config.loads.empty() || config.loads.size() == config.books.size());
    loads_ = config.loads.size() == config.books.size()
                 ? config.loads
                 : std::vector<std::uint64_t>(config.books.size(), 1);
    const std::vector<std::uint16_t> shard_for = assign_shards(loads_, shard_count);

    // The group owns the books; the directory tells the shards where they are.
    books_.reserve(config.books.size());
    std::vector<std::vector<SymbolId>> running(shard_count);
    for (std::size_t symbol = 0; symbol < config.books.size(); ++symbol) {
      books_.push_back(std::make_unique<OrderBook>(config.books[symbol]));
      directory_.books[symbol] = books_.back().get();
      shard_of_[symbol].store(shard_for[symbol], std::memory_order_relaxed);
      running[shard_for[symbol]].push_back(static_cast<SymbolId>(symbol));
    }

    // Working space for rebalance(), so that it never allocates.
    balance_.seen.assign(config.books.size(), 0);
    balance_.measured.assign(config.books.size(), 0);
    balance_.load.assign(config.books.size(), 0);
    balance_.proposed.assign(config.books.size(), 0);
    balance_.where.assign(config.books.size(), 0);
    balance_.carried.assign(shard_count, 0);

    shards_.reserve(shard_count);
    for (std::size_t shard = 0; shard < shard_count; ++shard) {
      const EngineConfig shard_config{
          .books = {},
          .command_capacity = config.command_capacity,
          .event_capacity = config.event_capacity,
          .idle = config.idle,
          .pin_to_cpu = config.pin_to_cpus.empty() ? kNoPinning : config.pin_to_cpus[shard],
          .time_one_in = config.time_one_in,
      };
      shards_.push_back(
          std::make_unique<MatchingEngine>(shard_config, directory_, running[shard]));
    }
  }

  // Stops the shards together. Left to the shards' own destructors, one could
  // wait on another that had not yet been told to stop.
  ~ShardedEngine() { stop(); }

  ShardedEngine(const ShardedEngine&) = delete;
  ShardedEngine& operator=(const ShardedEngine&) = delete;

  // --- Controlling thread ----------------------------------------------------

  // Returns once every shard's thread is up.
  void start() {
    for (const auto& shard : shards_) {
      shard->start();
    }
  }

  // Each shard handles everything already queued for it, including any move
  // in progress, then its thread is joined. Safe to call when the shards are
  // not running.
  void stop() {
    // All are asked first: a shard finishing a move may be waiting on another.
    for (const auto& shard : shards_) {
      shard->request_stop();
    }
    for (const auto& shard : shards_) {
      shard->join();
    }
  }

  // Whether this shard's thread is bound to the CPU it was given.
  [[nodiscard]] bool pinned(std::size_t shard) const noexcept {
    return shards_[shard]->pinned();
  }

  // --- Layout: any thread ----------------------------------------------------

  [[nodiscard]] std::size_t shard_count() const noexcept { return shards_.size(); }
  [[nodiscard]] std::size_t symbol_count() const noexcept { return books_.size(); }

  // The shard a symbol's commands currently go to. The symbol must exist.
  [[nodiscard]] std::size_t shard_of(SymbolId symbol) const noexcept {
    assert(symbol < shard_of_.size());
    return shard_of_[symbol].load(std::memory_order_acquire);
  }

  // The total expected load of the symbols currently assigned to a shard: the
  // sum of ShardedConfig::loads over them, or simply how many there are if no
  // loads were given.
  [[nodiscard]] std::uint64_t shard_load(std::size_t shard) const noexcept {
    std::uint64_t total = 0;
    for (std::size_t symbol = 0; symbol < shard_of_.size(); ++symbol) {
      if (shard_of_[symbol].load(std::memory_order_acquire) == shard) {
        total += loads_[symbol];
      }
    }
    return total;
  }

  // The ID of the symbol with this name, if there is one. Do it once at
  // start-up, not per order.
  [[nodiscard]] std::optional<SymbolId> symbol_id(std::string_view name) const noexcept {
    for (std::size_t symbol = 0; symbol < books_.size(); ++symbol) {
      if (books_[symbol]->symbol() == name) {
        return static_cast<SymbolId>(symbol);
      }
    }
    return std::nullopt;
  }

  // --- Gateway threads -------------------------------------------------------

  [[nodiscard]] SendStatus submit(std::uint64_t client_tag, SymbolId symbol, Side side,
                                  Price price, Quantity quantity,
                                  OrderType order_type = OrderType::Limit) noexcept {
    if (symbol >= shard_of_.size()) [[unlikely]] {
      return SendStatus::UnknownSymbol;
    }
    MatchingEngine& shard = *shards_[shard_of_[symbol].load(std::memory_order_relaxed)];
    if (!shard.submit(client_tag, symbol, side, price, quantity, order_type)) {
      return SendStatus::RingFull;
    }
    count_towards_rebalance();
    return SendStatus::Sent;
  }

  [[nodiscard]] SendStatus cancel(std::uint64_t client_tag, SymbolId symbol,
                                  OrderId order_id) noexcept {
    if (symbol >= shard_of_.size()) [[unlikely]] {
      return SendStatus::UnknownSymbol;
    }
    MatchingEngine& shard = *shards_[shard_of_[symbol].load(std::memory_order_relaxed)];
    if (!shard.cancel(client_tag, symbol, order_id)) {
      return SendStatus::RingFull;
    }
    count_towards_rebalance();
    return SendStatus::Sent;
  }

  // Moves a symbol to another shard while the engine is running. The book, its
  // resting orders and their IDs are untouched; only the thread running it
  // changes.
  //
  // How it works. A "let go" command goes into the old shard's queue and a
  // "take up" command into the new shard's, and from then on the symbol's
  // commands go to the new shard. Both travel in the same queues as orders, so
  // every order sent before the move is handled by the old shard, and every
  // order sent after it by the new one. The old shard's last word on the
  // symbol is an internal marker in its event stream. The new shard does not
  // start on the symbol until that marker has been read by poll(), so the
  // symbol's events stay in order.
  //
  // What it costs. The move is finished only when the old shard has worked
  // through its queue and poll() has read its events. Until then the new shard
  // is paused at the "take up" command, for all of its symbols. So keep
  // polling, and expect a brief pause on the destination. move_in_progress()
  // says when it is over.
  //
  // Who may call it. Only the thread that feeds both the old shard and the new
  // one. If the two shards have different feeder threads, stop both from
  // submitting for the length of the call.
  //
  // Returns Sent if the move was started (or the symbol is already there),
  // RingFull if either shard's command ring is full, in which case nothing has
  // changed and the call can be repeated, and UnknownSymbol if there is no
  // such symbol or shard.
  [[nodiscard]] SendStatus move_symbol(SymbolId symbol, std::size_t to_shard) noexcept {
    if (symbol >= shard_of_.size() || to_shard >= shards_.size()) [[unlikely]] {
      return SendStatus::UnknownSymbol;
    }
    const std::size_t from_shard = shard_of_[symbol].load(std::memory_order_relaxed);
    if (from_shard == to_shard) {
      return SendStatus::Sent;
    }
    MatchingEngine& from = *shards_[from_shard];
    MatchingEngine& to = *shards_[to_shard];
    // This thread is the only one adding to either ring, so if both have room
    // now, both pushes below are certain to succeed.
    if (from.command_ring_full() || to.command_ring_full()) {
      return SendStatus::RingFull;
    }

    const std::uint32_t move_number = moves_[symbol].load(std::memory_order_relaxed) + 1;
    moves_[symbol].store(move_number, std::memory_order_release);
    const bool let_go_queued = from.detach(symbol, move_number);
    shard_of_[symbol].store(static_cast<std::uint16_t>(to_shard), std::memory_order_release);
    const bool take_up_queued = to.attach(symbol, move_number);
    assert(let_go_queued && take_up_queued);
    static_cast<void>(let_go_queued);
    static_cast<void>(take_up_queued);
    return SendStatus::Sent;
  }

  // Whether the symbol's latest move is still under way: it was started, and
  // its new shard has not taken it up yet. Any thread.
  [[nodiscard]] bool move_in_progress(SymbolId symbol) const noexcept {
    assert(symbol < moves_.size());
    return directory_.gates[symbol].attached.load(std::memory_order_acquire) !=
           moves_[symbol].load(std::memory_order_acquire);
  }

  // Evens the shards out according to the traffic each symbol has actually
  // been receiving. One call takes one look and starts at most one move;
  // returns the symbol it moved, if any.
  //
  // A symbol's "load" is a running total of the commands handled for it, or
  // of the time spent on them if RebalanceConfig::measure says so. Older
  // traffic counts for less: at each look that reaches a decision, the total
  // so far is cut by a quarter and what has arrived since is added.
  // So a burst in one symbol does not by itself cause a move, while a lasting
  // shift shows up within a few looks.
  //
  // If the busiest shard is carrying more than its fair share plus the
  // configured tolerance, one symbol is moved from it to the quietest shard:
  // the one that narrows the gap between the two the most, provided it at
  // least halves it. See choose_rebalancing_move().
  //
  // It does nothing while an earlier move of its own is still under way, or
  // until min_sample commands have been handled since the last look, so it is
  // cheap and safe to call often. It never allocates.
  //
  // Who may call it: only the thread that feeds every shard, because it may
  // call move_symbol(). With RebalanceConfig::every set, submit() and
  // cancel() call it for you.
  std::optional<SymbolId> rebalance() noexcept {
    if (shards_.size() < 2) {
      return std::nullopt;
    }
    // One move at a time: each pauses its destination, and the load is not
    // worth measuring while a symbol is in flight.
    if (balance_.moving) {
      if (move_in_progress(balance_.moved)) {
        return std::nullopt;
      }
      balance_.moving = false;
    }

    // Whatever is being balanced, there must have been enough commands since
    // the last look for the picture to mean anything.
    std::uint64_t commands = 0;
    for (std::size_t symbol = 0; symbol < books_.size(); ++symbol) {
      commands += commands_handled(static_cast<SymbolId>(symbol));
    }
    if (commands - balance_.commands_seen < rebalance_.min_sample) {
      return std::nullopt;  // too little to judge on: keep counting
    }

    const bool by_time = rebalance_.measure == LoadMeasure::Time;
    for (std::size_t symbol = 0; symbol < books_.size(); ++symbol) {
      const auto id = static_cast<SymbolId>(symbol);
      balance_.measured[symbol] = by_time ? time_spent(id) : commands_handled(id);
      const std::uint64_t recent = balance_.measured[symbol] - balance_.seen[symbol];
      // Older traffic fades: keep three quarters of the total, add what is new.
      balance_.proposed[symbol] = balance_.load[symbol] - balance_.load[symbol] / 4 + recent;
      balance_.where[symbol] = shard_of_[symbol].load(std::memory_order_relaxed);
    }

    const std::optional<RebalancingMove> move = choose_rebalancing_move(
        balance_.proposed, balance_.where, balance_.carried, rebalance_.tolerance_percent);
    if (move) {
      const auto symbol = static_cast<SymbolId>(move->symbol);
      if (move_symbol(symbol, move->to) != SendStatus::Sent) {
        return std::nullopt;  // a ring is full: this look does not count
      }
      balance_.moving = true;
      balance_.moved = symbol;
      ++balance_.moves_started;
    }
    // The look counts: adopt the new totals, and measure afresh from here.
    balance_.load.swap(balance_.proposed);
    balance_.seen.swap(balance_.measured);
    balance_.commands_seen = commands;
    return move ? std::optional<SymbolId>(static_cast<SymbolId>(move->symbol)) : std::nullopt;
  }

  // How many moves rebalance() has started. Gateway thread only.
  [[nodiscard]] std::uint64_t rebalancing_moves() const noexcept {
    return balance_.moves_started;
  }

  // --- Publisher threads -----------------------------------------------------

  // Passes every event one shard has published so far to `visit`, in order,
  // and returns how many. Reading is also what lets a move finish.
  template <std::invocable<const Event&> Visitor>
  std::size_t poll(std::size_t shard, Visitor&& visit) noexcept {
    std::size_t passed_on = 0;
    shards_[shard]->poll([&](const Event& event) {
      if (event.type == EventType::Handoff) [[unlikely]] {
        // The old shard's last word on a symbol it has let go. Everything it
        // said before has just been passed on, so the new shard may start.
        directory_.gates[event.symbol].delivered.store(event.quantity,
                                                      std::memory_order_release);
        return;
      }
      visit(event);
      ++passed_on;
    });
    return passed_on;
  }

  // The same for every shard in turn. Use this when one thread reads them all.
  template <std::invocable<const Event&> Visitor>
  std::size_t poll(Visitor&& visit) noexcept {
    std::size_t passed_on = 0;
    for (std::size_t shard = 0; shard < shards_.size(); ++shard) {
      passed_on += poll(shard, visit);
    }
    return passed_on;
  }

  // --- Running the shards by hand --------------------------------------------

  // Handles what is queued for every shard, on the calling thread, and returns
  // how many commands it got through. Only for when the shards have not been
  // started; see MatchingEngine.
  //
  // A move needs both this and poll() to finish, so with a move under way,
  // call the two alternately until neither has anything left to do.
  std::size_t process_pending() noexcept {
    std::size_t handled = 0;
    for (const auto& shard : shards_) {
      handled += shard->process_pending();
    }
    return handled;
  }

  // --- Any thread ------------------------------------------------------------

  // Submits and cancels handled so far, over all shards.
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

  // How many commands have been handled for one symbol so far, on whichever
  // shards it has run.
  [[nodiscard]] std::uint64_t commands_handled(SymbolId symbol) const noexcept {
    assert(symbol < books_.size());
    std::uint64_t total = 0;
    for (const auto& shard : shards_) {
      total += shard->commands_handled(symbol);
    }
    return total;
  }

  // An estimate of how long the shards have spent on one symbol's commands so
  // far, in cycle_ticks(). See MatchingEngine::time_spent().
  [[nodiscard]] std::uint64_t time_spent(SymbolId symbol) const noexcept {
    assert(symbol < books_.size());
    std::uint64_t total = 0;
    for (const auto& shard : shards_) {
      total += shard->time_spent(symbol);
    }
    return total;
  }

  // Every symbol's load so far, in symbol order: commands handled, or time
  // spent. Pass it as ShardedConfig::loads to balance the next engine on real
  // traffic.
  [[nodiscard]] std::vector<std::uint64_t> measured_loads(
      LoadMeasure measure = LoadMeasure::Commands) const {
    std::vector<std::uint64_t> loads(books_.size());
    for (std::size_t symbol = 0; symbol < books_.size(); ++symbol) {
      const auto id = static_cast<SymbolId>(symbol);
      loads[symbol] = measure == LoadMeasure::Time ? time_spent(id) : commands_handled(id);
    }
    return loads;
  }

  // Only meaningful while the shards are not running.
  [[nodiscard]] const OrderBook& book(SymbolId symbol) const noexcept {
    assert(symbol < books_.size());
    return *books_[symbol];
  }

 private:
  // The automatic trigger. With it off, this is one test of a value that
  // never changes.
  void count_towards_rebalance() noexcept {
    if (rebalance_.every != 0 && ++balance_.sent_since_look >= rebalance_.every) {
      balance_.sent_since_look = 0;
      static_cast<void>(rebalance());
    }
  }

  // Everything rebalance() needs between calls. Used only by the thread that
  // feeds the shards.
  struct Balance {
    std::vector<std::uint64_t> seen;      // each symbol's measure at the last decision
    std::vector<std::uint64_t> measured;  // the same, as of this look
    std::vector<std::uint64_t> load;      // each symbol's running total, older traffic fading
    std::vector<std::uint64_t> proposed;  // what those totals become if this look counts
    std::vector<std::uint16_t> where;     // each symbol's shard, as of this look
    std::vector<std::uint64_t> carried;   // each shard's load
    std::uint64_t commands_seen = 0;      // commands handled in all, at the last decision
    std::uint64_t sent_since_look = 0;
    std::uint64_t moves_started = 0;
    SymbolId moved = 0;                  // the symbol rebalance() last moved
    bool moving = false;                 // whether that move may still be under way
  };

  std::vector<std::unique_ptr<OrderBook>> books_;         // every book, by SymbolId
  SymbolDirectory directory_;                             // what the shards share
  std::vector<std::uint64_t> loads_;                      // expected load, by SymbolId
  std::vector<std::atomic<std::uint16_t>> shard_of_;      // where each symbol's commands go
  std::vector<std::atomic<std::uint32_t>> moves_;         // how many times each has been moved
  RebalanceConfig rebalance_;
  Balance balance_;
  std::vector<std::unique_ptr<MatchingEngine>> shards_;   // last: destroyed first
};

}  // namespace lob

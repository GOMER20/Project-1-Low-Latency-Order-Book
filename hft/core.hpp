#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "hft/stats.hpp"
#include "hft/wire.hpp"
#include "lob/level_bitmap.hpp"
#include "lob/matching_engine.hpp"
#include "lob/recording.hpp"
#include "lob/replay.hpp"

// HFT: the matching engine as a service.
//
// Core is everything except the socket. It is given one message at a time and
// appends its answers to an Outbox; Server (server.hpp) is the part that moves
// bytes. Keeping the two apart means the whole of HFT's behaviour can be
// tested, and simulated, without a connection.
//
// What Core adds to the order books underneath:
//
//   - A client's view of its orders. The engine reports a trade to whoever
//     sent the incoming order; Core also tells the owner of the resting order,
//     and keeps each resting order's tag and remaining quantity to do it.
//   - The house. The program using HFT says what the market's own bids and
//     offers should be (a Quote), and Core keeps real orders resting in the
//     book to match. Client orders trade with those and with each other, in
//     strict price and time priority.
//   - Statistics: what it was asked to do, what came of it, and how long each
//     message took.
//
// One thread does everything. The engine has no thread of its own here: each
// message is turned into commands, and each command is run to completion
// before the next is sent. With one client on a local socket there is nothing
// for a second thread to do but burn a core while idle, and HFT is meant to
// sit inside a desktop application for days.

namespace hft {

struct CoreOptions {
  // Where to keep recordings of sessions. Empty records nothing.
  std::string data_dir = {};
  std::size_t recording_capacity = 1 << 16;

  // The most commands to record in one session; 0 for no limit. A session
  // that goes on past it is recorded up to that point and no further, so
  // that an HFT left running for days cannot fill the disk.
  std::uint64_t recording_limit = 0;
};

class Core {
 public:
  explicit Core(CoreOptions options = {}) : options_(std::move(options)) {}

  ~Core() { finish(); }

  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  // Handles one message and appends whatever it has to say in reply to `out`.
  // Returns false if the message cannot be understood at all: an Error has
  // then been appended, and the connection should be closed once it is sent.
  [[nodiscard]] bool handle(Kind kind, std::span<const std::byte> payload, Outbox& out) {
    const auto began = std::chrono::steady_clock::now();
    switch (kind) {
      case Kind::Hello:
        return on_hello(payload, out);
      case Kind::Ping: {
        Ping ping{};
        if (!take(payload, ping)) {
          return fail(out, "Ping is too short");
        }
        out.send(Kind::Pong, Pong{ping.value});
        return true;
      }
      case Kind::StatsRequest:
        out.send(Kind::Stats, to_json(stats()));
        return true;
      case Kind::Shutdown:
        shutdown_requested_ = true;
        return true;
      default:
        break;
    }
    if (engine_ == nullptr) {
      return fail(out, "the first message must be Hello");
    }

    switch (kind) {
      case Kind::Order: {
        Order order{};
        if (!take(payload, order)) {
          return fail(out, "Order is too short");
        }
        on_order(order, out);
        timed(stats_.order_time, began);
        return true;
      }
      case Kind::Cancel: {
        Cancel cancel{};
        if (!take(payload, cancel)) {
          return fail(out, "Cancel is too short");
        }
        on_cancel(cancel, out);
        timed(stats_.order_time, began);
        return true;
      }
      case Kind::Quote:
        if (!on_quote(payload, out)) {
          return fail(out, "Quote is the wrong size");
        }
        timed(stats_.quote_time, began);
        return true;
      case Kind::DepthRequest: {
        DepthRequest request{};
        if (!take(payload, request)) {
          return fail(out, "DepthRequest is too short");
        }
        on_depth(request, out);
        return true;
      }
      default:
        return fail(out, "unknown kind of message");
    }
  }

  // Set by a Shutdown message.
  [[nodiscard]] bool shutdown_requested() const noexcept { return shutdown_requested_; }

  [[nodiscard]] bool running() const noexcept { return engine_ != nullptr; }
  [[nodiscard]] std::size_t symbol_count() const noexcept { return books_.size(); }

  // The statistics, brought up to date.
  [[nodiscard]] const Stats& stats() {
    if (engine_ != nullptr) {
      stats_.digest = digest_.value();
      stats_.events_dropped = engine_->events_dropped();
      stats_.recording = engine_->recording();
      stats_.commands_recorded = engine_->commands_recorded();
      // While the recording is still going it covers the whole session, so
      // its fingerprint is the session's.
      stats_.recorded_digest = stats_.recording_stopped ? digest_at_limit_ : stats_.digest;
    }
    return stats_;
  }

  // Makes the recording of the session so far complete on disk.
  void finish() {
    if (engine_ != nullptr) {
      engine_->flush_recording();
    }
  }

  // For tests: the book underneath.
  [[nodiscard]] const lob::OrderBook& book(std::uint16_t symbol) const {
    return engine_->book(symbol);
  }

  // The engine is sized so that the events of any one command fit its ring;
  // a replay must be given the same room. See start_session().
  [[nodiscard]] static std::size_t event_capacity_for(std::span<const lob::BookConfig> books) {
    std::size_t most_orders = 0;
    for (const lob::BookConfig& book : books) {
      most_orders = std::max(most_orders, book.max_orders);
    }
    return std::max<std::size_t>(4'096, most_orders + 64);
  }

 private:
  // What Core remembers about an order resting in a book. Found by the slot
  // number in the low half of the order's ID, so looking one up is an array
  // index: no hashing, and no memory beyond what start_session() set aside.
  struct Resting {
    std::uint64_t order_id = 0;  // 0: nothing here
    std::uint64_t tag = 0;
    std::uint32_t remaining = 0;
    bool house = false;
  };

  struct HouseOrder {
    std::int64_t price;
    std::uint64_t order_id;
    std::uint32_t remaining;
    lob::Side side;
  };

  struct Symbol {
    std::vector<Resting> resting;   // by order slot
    std::vector<HouseOrder> house;  // the house's resting orders, in no particular order
  };

  // The most house orders in one symbol that a Quote looks at. The house never
  // has anything like this many; if it somehow did, the rest would be dealt
  // with by the next Quote.
  static constexpr std::size_t kMostHouseOrders = 8 * kMaxLevels;

  // The command being run, which is what tells an event's meaning: the engine
  // answers every command before the next is sent.
  struct Running {
    bool house = false;
    std::uint64_t tag = 0;
    std::uint32_t quantity = 0;
    std::uint32_t filled = 0;
  };

  [[nodiscard]] static bool fail(Outbox& out, std::string_view why) {
    out.send(Kind::Error, why);
    return false;
  }

  void timed(Histogram& histogram, std::chrono::steady_clock::time_point began) {
    const auto now = std::chrono::steady_clock::now();
    histogram.record(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - began).count()));
    stats_.count_message(now);
  }

  // --- Hello: which books to run ------------------------------------------------

  [[nodiscard]] bool on_hello(std::span<const std::byte> payload, Outbox& out) {
    Hello hello{};
    if (!take(payload, hello)) {
      return fail(out, "Hello is too short");
    }
    if (hello.version != kProtocolVersion) {
      return fail(out, "this HFT speaks a different version of the protocol");
    }
    if (hello.book_count == 0 || hello.book_count > lob::MatchingEngine::kMaxSymbols ||
        payload.size() != std::size_t{hello.book_count} * sizeof(HelloBook)) {
      return fail(out, "Hello does not hold the books it says it does");
    }

    std::vector<HelloBook> wanted(hello.book_count);
    for (HelloBook& book : wanted) {
      static_cast<void>(take(payload, book));
      if (book.symbol[0] == '\0' || book.num_levels == 0 ||
          book.num_levels > lob::LevelBitmap::kMaxBits || book.max_orders == 0 ||
          book.max_orders >= lob::kNullIndex) {
        return fail(out, "Hello asks for a book that cannot be built");
      }
    }

    const bool same_books =
        engine_ != nullptr && wanted.size() == wanted_.size() &&
        std::equal(wanted.begin(), wanted.end(), wanted_.begin(),
                   [](const HelloBook& a, const HelloBook& b) {
                     return std::memcmp(&a, &b, sizeof(HelloBook)) == 0;
                   });
    const bool resumed = same_books && (hello.flags & kHelloFresh) == 0;
    if (!resumed) {
      start_session(std::move(wanted));
    }

    Welcome welcome{};
    welcome.version = kProtocolVersion;
    welcome.book_count = static_cast<std::uint32_t>(books_.size());
    welcome.session = stats_.session;
    welcome.resumed = resumed ? 1 : 0;
    out.send(Kind::Welcome, welcome);
    return true;
  }

  // Throws the old books away, if there were any, and builds empty ones.
  void start_session(std::vector<HelloBook> wanted) {
    finish();
    engine_.reset();  // closes the old session's recording

    wanted_ = std::move(wanted);
    names_.clear();
    names_.reserve(wanted_.size());  // the books' names point into this: it must not move
    books_.clear();
    symbols_.clear();
    symbols_.resize(wanted_.size());

    const std::uint64_t session = stats_.session + 1;
    stats_ = Stats{};
    stats_.session = session;
    stats_.started = std::chrono::steady_clock::now();
    stats_.started_wall = std::chrono::system_clock::now();
    stats_.symbols.resize(wanted_.size());

    for (std::size_t index = 0; index < wanted_.size(); ++index) {
      const HelloBook& book = wanted_[index];
      const char* const end = std::find(std::begin(book.symbol), std::end(book.symbol), '\0');
      names_.emplace_back(book.symbol, static_cast<std::size_t>(end - book.symbol));
      books_.push_back(lob::BookConfig{.symbol = names_.back(),
                                       .min_price = book.min_price,
                                       .num_levels = book.num_levels,
                                       .max_orders = book.max_orders});
      symbols_[index].resting.resize(book.max_orders);
      symbols_[index].house.reserve(kMostHouseOrders);
      stats_.symbols[index].name = names_.back();
    }

    if (!options_.data_dir.empty()) {
      stats_.recording_file = options_.data_dir + "/hft-session-" + file_stamp(stats_.started_wall) +
                              "-" + std::to_string(session) + ".rec";
    }
    engine_ = std::make_unique<lob::MatchingEngine>(lob::EngineConfig{
        .books = books_,
        // Commands are run one at a time, so the ring never holds more than one.
        .command_capacity = 64,
        // Run by hand, the engine discards events that do not fit, and one
        // order can trade with every order resting in its book.
        .event_capacity = event_capacity_for(books_),
        .time_one_in = 0,
        .record_to = stats_.recording_file,
        .recording_capacity = options_.recording_capacity,
        .recording_limit = options_.recording_limit,
    });
    digest_ = lob::SessionDigest(books_.size());
    digest_at_limit_ = 0;
    stats_.recording_limit = options_.recording_limit;
  }

  [[nodiscard]] static std::string file_stamp(std::chrono::system_clock::time_point when) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &utc);
    return buffer;
  }

  // --- Running a command ----------------------------------------------------------

  // Sends one command to the engine, lets the engine deal with it, and reads
  // everything it had to say.
  template <typename Send>
  void run(Send&& send, Outbox& out) {
    // The command ring is empty, so only one thing can refuse a command: the
    // recording has fallen behind. Wait for it; nothing may go unrecorded.
    while (!send()) {
      ++stats_.recorder_waits;
      std::this_thread::yield();
    }
    ++stats_.engine_commands;
    engine_->process_pending();
    engine_->poll([&](const lob::Event& event) { on_event(event, out); });

    // That was the last command the recording has room for. Note what the
    // session's fingerprint is now: it is what a replay of the recording
    // should arrive at.
    if (stats_.engine_commands == options_.recording_limit && !stats_.recording_file.empty()) {
      digest_at_limit_ = digest_.value();
      stats_.recording_stopped = true;
    }
  }

  [[nodiscard]] Resting& resting_at(std::uint16_t symbol, std::uint64_t order_id) {
    return symbols_[symbol].resting[order_id & 0xFFFF'FFFFu];
  }

  // The entry for an order, if it is resting.
  [[nodiscard]] Resting* find_resting(std::uint16_t symbol, std::uint64_t order_id) {
    const std::uint64_t slot = order_id & 0xFFFF'FFFFu;
    std::vector<Resting>& table = symbols_[symbol].resting;
    if (order_id == 0 || slot >= table.size() || table[slot].order_id != order_id) {
      return nullptr;
    }
    return &table[slot];
  }

  void forget(std::uint16_t symbol, Resting& entry) {
    if (entry.house) {
      std::vector<HouseOrder>& house = symbols_[symbol].house;
      const auto it = std::find_if(house.begin(), house.end(), [&](const HouseOrder& order) {
        return order.order_id == entry.order_id;
      });
      if (it != house.end()) {
        *it = house.back();
        house.pop_back();
      }
      --stats_.resting_house_orders;
    } else {
      --stats_.resting_client_orders;
    }
    entry = Resting{};
  }

  void on_event(const lob::Event& event, Outbox& out) {
    ++stats_.engine_events;
    digest_.add(event);
    switch (event.type) {
      case lob::EventType::Trade:
        on_trade(event, out);
        break;
      case lob::EventType::Accepted:
        if (event.resting != 0) {
          resting_at(event.symbol, event.order_id) =
              Resting{event.order_id, running_.tag, event.resting, running_.house};
          if (running_.house) {
            symbols_[event.symbol].house.push_back(
                HouseOrder{event.price, event.order_id, event.resting, event.side});
            ++stats_.resting_house_orders;
          } else {
            ++stats_.resting_client_orders;
          }
        }
        if (!running_.house) {
          Accepted accepted{};
          accepted.tag = running_.tag;
          accepted.order_id = event.order_id;
          accepted.filled = event.quantity;
          accepted.resting = event.resting;
          accepted.symbol = event.symbol;
          out.send(Kind::Accepted, accepted);
          ++stats_.accepted;
        }
        break;
      case lob::EventType::Rejected:
        if (running_.house) {
          ++stats_.house_rejected;
        } else {
          Rejected rejected{};
          rejected.tag = running_.tag;
          rejected.symbol = event.symbol;
          out.send(Kind::Rejected, rejected);
          ++stats_.rejected;
        }
        break;
      case lob::EventType::Cancelled:
        if (Resting* const entry = find_resting(event.symbol, event.order_id)) {
          forget(event.symbol, *entry);
        }
        if (!running_.house) {
          send_cancel_answer(Kind::Cancelled, running_.tag, event.order_id, event.symbol, out);
          ++stats_.cancelled;
        }
        break;
      case lob::EventType::CancelRejected:
        if (!running_.house) {
          send_cancel_answer(Kind::CancelRejected, running_.tag, event.order_id, event.symbol,
                             out);
          ++stats_.cancel_rejected;
        }
        break;
      case lob::EventType::Handoff:
        break;  // only between the shards of a ShardedEngine
    }
  }

  // One trade: the order being run took `event.quantity` from a resting order.
  void on_trade(const lob::Event& event, Outbox& out) {
    Resting* const maker = find_resting(event.symbol, event.maker_id);
    const bool maker_is_house = maker == nullptr || maker->house;
    const bool taker_is_house = running_.house;
    running_.filled += event.quantity;

    ++stats_.trades;
    stats_.shares += event.quantity;
    stats_.notional += event.price * static_cast<std::int64_t>(event.quantity);
    if (!maker_is_house && !taker_is_house) {
      ++stats_.trades_between_clients;
    }
    SymbolStats& symbol = stats_.symbols[event.symbol];
    ++symbol.trades;
    symbol.shares += event.quantity;
    symbol.last_price = event.price;

    if (!taker_is_house) {
      Fill fill{};
      fill.tag = running_.tag;
      fill.order_id = event.order_id;
      fill.price = event.price;
      fill.quantity = event.quantity;
      fill.remaining = running_.quantity - running_.filled;
      fill.symbol = event.symbol;
      fill.side = static_cast<std::uint8_t>(event.side);
      fill.maker = 0;
      fill.with_house = maker_is_house ? 1 : 0;
      out.send(Kind::Fill, fill);
      ++stats_.fills;
    }
    if (maker == nullptr) {
      return;  // cannot happen: every resting order is in the table
    }

    maker->remaining -= std::min(maker->remaining, event.quantity);
    if (maker->house) {
      for (HouseOrder& order : symbols_[event.symbol].house) {
        if (order.order_id == maker->order_id) {
          order.remaining = maker->remaining;
          break;
        }
      }
    } else {
      Fill fill{};
      fill.tag = maker->tag;
      fill.order_id = maker->order_id;
      fill.price = event.price;
      fill.quantity = event.quantity;
      fill.remaining = maker->remaining;
      fill.symbol = event.symbol;
      fill.side = static_cast<std::uint8_t>(lob::opposite(event.side));
      fill.maker = 1;
      fill.with_house = taker_is_house ? 1 : 0;
      out.send(Kind::Fill, fill);
      ++stats_.fills;
    }
    if (maker->remaining == 0) {
      forget(event.symbol, *maker);
    }
  }

  static void send_cancel_answer(Kind kind, std::uint64_t tag, std::uint64_t order_id,
                                 std::uint16_t symbol, Outbox& out) {
    CancelAnswer answer{};
    answer.tag = tag;
    answer.order_id = order_id;
    answer.symbol = symbol;
    out.send(kind, answer);
  }

  // --- The client's orders --------------------------------------------------------

  void on_order(const Order& order, Outbox& out) {
    ++stats_.orders;
    if (order.symbol >= books_.size() || order.side > 1 || order.type > 3) {
      Rejected rejected{};
      rejected.tag = order.tag;
      rejected.symbol = order.symbol;
      out.send(Kind::Rejected, rejected);
      ++stats_.rejected;
      return;
    }
    ++stats_.symbols[order.symbol].orders;
    running_ = Running{false, order.tag, order.quantity, 0};
    run(
        [&] {
          return engine_->submit(order.tag, order.symbol, static_cast<lob::Side>(order.side),
                                 order.price, order.quantity,
                                 static_cast<lob::OrderType>(order.type));
        },
        out);
  }

  void on_cancel(const Cancel& cancel, Outbox& out) {
    ++stats_.cancels;
    // A client may only cancel its own orders. The house's are not its to
    // touch, however it came by their IDs.
    const Resting* const entry =
        cancel.symbol < books_.size() ? find_resting(cancel.symbol, cancel.order_id) : nullptr;
    if (entry == nullptr || entry->house) {
      send_cancel_answer(Kind::CancelRejected, cancel.tag, cancel.order_id, cancel.symbol, out);
      ++stats_.cancel_rejected;
      return;
    }
    running_ = Running{false, cancel.tag, 0, 0};
    run([&] { return engine_->cancel(cancel.tag, cancel.symbol, cancel.order_id); }, out);
  }

  // --- The house ------------------------------------------------------------------

  struct Wanted {
    std::int64_t price;
    std::uint32_t quantity;
  };
  using WantedSide = std::array<Wanted, kMaxLevels>;

  static constexpr std::uint8_t kNotWanted = 0xFF;

  // Which of the wanted levels on a side is at this price, or kNotWanted.
  [[nodiscard]] static std::uint8_t level_at(const WantedSide& side, std::size_t count,
                                             std::int64_t price) {
    for (std::size_t index = 0; index < count; ++index) {
      if (side[index].price == price) {
        return side[index].quantity != 0 ? static_cast<std::uint8_t>(index) : kNotWanted;
      }
    }
    return kNotWanted;
  }

  // Makes the house's resting orders in one symbol match what is asked for.
  // Returns false only if the message is the wrong size to be a Quote.
  [[nodiscard]] bool on_quote(std::span<const std::byte> payload, Outbox& out) {
    Quote quote{};
    if (!take(payload, quote) ||
        payload.size() !=
            (std::size_t{quote.bid_levels} + quote.ask_levels) * sizeof(QuoteLevel)) {
      return false;
    }
    ++stats_.quotes;
    if (quote.symbol >= books_.size() || quote.bid_levels > kMaxLevels ||
        quote.ask_levels > kMaxLevels) {
      ++stats_.quotes_refused;
      return true;
    }

    std::array<WantedSide, 2> wanted;  // bids, then asks
    const std::array<std::size_t, 2> counts = {quote.bid_levels, quote.ask_levels};
    for (std::size_t side = 0; side < 2; ++side) {
      for (std::size_t index = 0; index < counts[side]; ++index) {
        QuoteLevel level{};
        static_cast<void>(take(payload, level));
        wanted[side][index] = Wanted{level.price, level.quantity};
      }
    }
    // Each side must run strictly away from its best price, and the best bid
    // must be below the best offer. Otherwise the house would be asked to rest
    // two amounts at one price, or to bid at or above its own offer, where its
    // two sides would trade with each other. Such a quote is ignored whole.
    bool sound = counts[0] == 0 || counts[1] == 0 || wanted[0][0].price < wanted[1][0].price;
    for (std::size_t index = 1; index < counts[0]; ++index) {
      sound = sound && wanted[0][index].price < wanted[0][index - 1].price;
    }
    for (std::size_t index = 1; index < counts[1]; ++index) {
      sound = sound && wanted[1][index].price > wanted[1][index - 1].price;
    }
    if (!sound) {
      ++stats_.quotes_refused;
      return true;
    }
    ++stats_.symbols[quote.symbol].quotes;

    // One pass over the house's orders in this symbol: which wanted level each
    // is at, if any, and how much the house has at each wanted level in all.
    const std::vector<HouseOrder>& house = symbols_[quote.symbol].house;
    const std::size_t orders = std::min(house.size(), kMostHouseOrders);
    std::array<std::uint8_t, kMostHouseOrders> level_of;
    std::array<std::array<std::uint64_t, kMaxLevels>, 2> have{};
    for (std::size_t index = 0; index < orders; ++index) {
      const std::size_t side = house[index].side == lob::Side::Buy ? 0 : 1;
      level_of[index] = level_at(wanted[side], counts[side], house[index].price);
      if (level_of[index] != kNotWanted) {
        have[side][level_of[index]] += house[index].remaining;
      }
    }

    // What has to go: orders at prices that are not wanted, and every order at
    // a price where the house has more than is wanted. (There is no making an
    // order smaller, so too much at a price means starting that price again.)
    std::array<std::uint64_t, kMostHouseOrders> stale;
    std::size_t stale_count = 0;
    for (std::size_t index = 0; index < orders; ++index) {
      const std::size_t side = house[index].side == lob::Side::Buy ? 0 : 1;
      const std::uint8_t level = level_of[index];
      if (level == kNotWanted || have[side][level] > wanted[side][level].quantity) {
        stale[stale_count++] = house[index].order_id;
      }
    }
    for (std::size_t side = 0; side < 2; ++side) {
      for (std::size_t level = 0; level < counts[side]; ++level) {
        if (have[side][level] > wanted[side][level].quantity) {
          have[side][level] = 0;  // all of it is about to be cancelled
        }
      }
    }

    // Take away first, on both sides, and only then add. Done the other way
    // about, a new bid could meet one of the house's own stale offers on its
    // way in.
    for (std::size_t index = 0; index < stale_count; ++index) {
      running_ = Running{true, 0, 0, 0};
      ++stats_.house_cancels;
      run([&] { return engine_->cancel(0, quote.symbol, stale[index]); }, out);
    }
    for (std::size_t side = 0; side < 2; ++side) {
      const lob::Side book_side = side == 0 ? lob::Side::Buy : lob::Side::Sell;
      for (std::size_t level = 0; level < counts[side]; ++level) {
        const Wanted& want = wanted[side][level];
        if (want.quantity <= have[side][level]) {
          continue;
        }
        const auto more = static_cast<std::uint32_t>(want.quantity - have[side][level]);
        running_ = Running{true, 0, more, 0};
        ++stats_.house_orders;
        run(
            [&] {
              return engine_->submit(0, quote.symbol, book_side, want.price, more,
                                     lob::OrderType::Limit);
            },
            out);
      }
    }
    return true;
  }

  // --- Looking at a book ----------------------------------------------------------

  void on_depth(const DepthRequest& request, Outbox& out) {
    ++stats_.depth_requests;
    const std::size_t limit = std::min<std::size_t>(request.levels, kMaxLevels);
    std::array<std::array<DepthLevel, kMaxLevels>, 2> levels{};
    std::array<std::size_t, 2> counts = {0, 0};
    if (request.symbol < books_.size()) {
      const lob::OrderBook& book = engine_->book(request.symbol);
      for (std::size_t side = 0; side < 2; ++side) {
        book.for_each_level(side == 0 ? lob::Side::Buy : lob::Side::Sell,
                            [&](lob::Price price, std::uint64_t quantity) {
                              if (counts[side] < limit) {
                                levels[side][counts[side]++] = DepthLevel{price, quantity};
                              }
                            });
      }
    }
    Depth depth{};
    depth.symbol = request.symbol;
    depth.bid_levels = static_cast<std::uint8_t>(counts[0]);
    depth.ask_levels = static_cast<std::uint8_t>(counts[1]);
    out.begin(Kind::Depth);
    out.put(depth);
    for (std::size_t side = 0; side < 2; ++side) {
      for (std::size_t index = 0; index < counts[side]; ++index) {
        out.put(levels[side][index]);
      }
    }
    out.end();
  }

  CoreOptions options_;
  std::vector<HelloBook> wanted_;        // the books as the client asked for them
  std::vector<std::string> names_;       // owns the names that books_ points into
  std::vector<lob::BookConfig> books_;
  std::vector<Symbol> symbols_;
  std::unique_ptr<lob::MatchingEngine> engine_;
  lob::SessionDigest digest_{0};
  std::uint64_t digest_at_limit_ = 0;  // the fingerprint when the recording reached its limit
  Stats stats_;
  Running running_;
  bool shutdown_requested_ = false;
};

// --- Checking a recording ---------------------------------------------------------

struct Verification {
  bool loaded = false;         // the file is a recording
  bool damaged = false;        // it stops part-way through a command
  std::uint64_t commands = 0;
  std::uint64_t events = 0;
  std::uint64_t trades = 0;
  std::uint64_t digest = 0;    // equal to the session's own if the replay matched it
};

// Replays a recorded session from nothing and reports the fingerprint of what
// the engine did. Compare it with `recorded_digest` in the session's
// statistics: if they are equal, the replay did exactly what the session did,
// for as much of the session as was recorded.
[[nodiscard]] inline Verification verify_recording(const std::string& path) {
  Verification result;
  const std::optional<lob::Recording> recording = lob::Recording::load(path);
  if (!recording) {
    return result;
  }
  result.loaded = true;
  result.damaged = recording->damaged();

  lob::MatchingEngine engine(lob::EngineConfig{
      .books = recording->books(),
      .command_capacity = 64,
      .event_capacity = Core::event_capacity_for(recording->books()),
      .time_one_in = 0,
  });
  lob::SessionDigest digest(recording->books().size());
  const lob::ReplayResult replayed =
      lob::replay(recording->commands(), engine, [&](const lob::Event& event) {
        digest.add(event);
        if (event.type == lob::EventType::Trade) {
          ++result.trades;
        }
      });
  result.commands = replayed.commands;
  result.events = replayed.events;
  result.digest = digest.value();
  return result;
}

}  // namespace hft

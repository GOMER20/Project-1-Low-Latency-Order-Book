// Recording a session to a file, and replaying it into a fresh engine.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "lob/recording.hpp"
#include "lob/replay.hpp"
#include "lob/sharded_engine.hpp"

namespace {

using lob::BookConfig;
using lob::Command;
using lob::CommandType;
using lob::Event;
using lob::EventType;
using lob::IdleStrategy;
using lob::MarketData;
using lob::MatchingEngine;
using lob::OrderBook;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Quantity;
using lob::Recording;
using lob::ReplayResult;
using lob::SendStatus;
using lob::SessionDigest;
using lob::ShardedEngine;
using lob::Side;
using lob::SymbolId;

// --- Helpers -------------------------------------------------------------------

// A file name of the running test's own, removed again when the test ends.
// The process ID is part of it: two builds' tests may well be running at the
// same time, and the same test in each must not share a file.
class TempFile {
 public:
  explicit TempFile(const std::string& suffix = "") {
    const auto* const test = ::testing::UnitTest::GetInstance()->current_test_info();
    path_ = ::testing::TempDir() + "lob_" + std::to_string(::getpid()) + "_" +
            test->test_suite_name() + "_" + test->name() + suffix + ".rec";
    std::remove(path_.c_str());
  }
  ~TempFile() { std::remove(path_.c_str()); }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  std::string path_;
};

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Symbol i trades at prices 1000 + 100 i up to 64 ticks above that, so that a
// replay which muddled the books up would show.
std::vector<BookConfig> make_books(std::size_t count) {
  static const char* const kNames[] = {"AAA", "BBB", "CCC", "DDD", "EEE", "FFF", "GGG", "HHH"};
  std::vector<BookConfig> books;
  for (std::size_t symbol = 0; symbol < count; ++symbol) {
    books.push_back({.symbol = kNames[symbol % 8],
                     .min_price = 1'000 + 100 * static_cast<Price>(symbol),
                     .num_levels = 64,
                     .max_orders = 512});
  }
  return books;
}

[[nodiscard]] bool sent(bool queued) { return queued; }
[[nodiscard]] bool sent(SendStatus status) { return status == SendStatus::Sent; }

void expect_same(const Event& a, const Event& b, const std::string& where) {
  EXPECT_EQ(a.client_tag, b.client_tag) << where;
  EXPECT_EQ(a.order_id, b.order_id) << where;
  EXPECT_EQ(a.maker_id, b.maker_id) << where;
  EXPECT_EQ(a.price, b.price) << where;
  EXPECT_EQ(a.quantity, b.quantity) << where;
  EXPECT_EQ(a.resting, b.resting) << where;
  EXPECT_EQ(a.symbol, b.symbol) << where;
  EXPECT_EQ(a.side, b.side) << where;
  EXPECT_EQ(a.type, b.type) << where;
}

void expect_same_events(const std::vector<Event>& original, const std::vector<Event>& replayed,
                        const std::string& what) {
  ASSERT_EQ(original.size(), replayed.size()) << what;
  for (std::size_t index = 0; index < original.size(); ++index) {
    expect_same(original[index], replayed[index], what + ", event " + std::to_string(index));
    if (::testing::Test::HasFailure()) {
      return;  // one difference is enough; the rest would only bury it
    }
  }
}

// Market data is compared in sequence order. That is the order it arrives in,
// except that a reader of two shards' rings can meet a symbol's messages out
// of order just after the symbol has moved.
void expect_same_feed(std::vector<MarketData> original, std::vector<MarketData> replayed,
                      const std::string& what) {
  const auto by_sequence = [](const MarketData& a, const MarketData& b) {
    return a.sequence < b.sequence;
  };
  std::sort(original.begin(), original.end(), by_sequence);
  std::sort(replayed.begin(), replayed.end(), by_sequence);
  ASSERT_EQ(original.size(), replayed.size()) << what;
  for (std::size_t index = 0; index < original.size(); ++index) {
    const MarketData& a = original[index];
    const MarketData& b = replayed[index];
    const std::string where = what + ", message " + std::to_string(index);
    ASSERT_EQ(a.sequence, index + 1) << where;  // none missing, none twice
    ASSERT_EQ(a.sequence, b.sequence) << where;
    ASSERT_EQ(a.type, b.type) << where;
    ASSERT_EQ(a.symbol, b.symbol) << where;
    ASSERT_EQ(a.side, b.side) << where;
    ASSERT_EQ(a.price, b.price) << where;
    ASSERT_EQ(a.quantity, b.quantity) << where;
    ASSERT_EQ(a.bid_price, b.bid_price) << where;
    ASSERT_EQ(a.bid_quantity, b.bid_quantity) << where;
    ASSERT_EQ(a.ask_price, b.ask_price) << where;
    ASSERT_EQ(a.ask_quantity, b.ask_quantity) << where;
  }
}

// The same orders, with the same IDs and quantities, in the same queues.
void expect_same_book(const OrderBook& original, const OrderBook& replayed,
                      const BookConfig& config) {
  struct Resting {
    OrderId id;
    Quantity quantity;
    bool operator==(const Resting&) const = default;
  };
  ASSERT_EQ(original.size(), replayed.size()) << config.symbol;
  ASSERT_EQ(original.best_bid(), replayed.best_bid()) << config.symbol;
  ASSERT_EQ(original.best_ask(), replayed.best_ask()) << config.symbol;
  for (std::size_t level = 0; level < config.num_levels; ++level) {
    const Price price = config.min_price + static_cast<Price>(level);
    for (const Side side : {Side::Buy, Side::Sell}) {
      std::vector<Resting> first;
      std::vector<Resting> second;
      original.for_each_order(side, price, [&](const lob::Order& order) {
        first.push_back({order.id, order.quantity});
      });
      replayed.for_each_order(side, price, [&](const lob::Order& order) {
        second.push_back({order.id, order.quantity});
      });
      ASSERT_TRUE(first == second) << config.symbol << " at " << price;
    }
  }
}

// Everything an engine said, sorted out by symbol. Answers to commands for a
// symbol the session did not have all go in one extra place at the end.
struct Capture {
  explicit Capture(std::size_t symbols)
      : events(symbols + 1), feed(symbols + 1), digest(symbols) {}

  void add(const Event& event) {
    events[std::min<std::size_t>(event.symbol, events.size() - 1)].push_back(event);
    in_order.push_back(event);
    digest.add(event);
  }
  void add(const MarketData& message) {
    feed[std::min<std::size_t>(message.symbol, feed.size() - 1)].push_back(message);
    digest.add(message);
  }

  std::vector<std::vector<Event>> events;     // by symbol, in the order they arrived
  std::vector<Event> in_order;                // all of them, in the order they arrived
  std::vector<std::vector<MarketData>> feed;  // by symbol
  SessionDigest digest;
};

void expect_same_session(const Capture& original, const Capture& replayed) {
  ASSERT_EQ(original.events.size(), replayed.events.size());
  for (std::size_t symbol = 0; symbol < original.events.size(); ++symbol) {
    const std::string what = "symbol " + std::to_string(symbol);
    expect_same_events(original.events[symbol], replayed.events[symbol], what);
    expect_same_feed(original.feed[symbol], replayed.feed[symbol], what);
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
  EXPECT_EQ(original.digest.count(), replayed.digest.count());
  EXPECT_EQ(original.digest.value(), replayed.digest.value());
  EXPECT_TRUE(original.digest == replayed.digest);
}

// Replays into `engine`, capturing what it says.
template <typename Engine>
ReplayResult replay_into(Engine& engine, std::span<const Command> commands, Capture& capture,
                         const lob::ReplayOptions& options = {}) {
  return lob::replay(
      commands, engine, [&](const Event& event) { capture.add(event); },
      [&](const MarketData& message) { capture.add(message); }, options);
}

// Someone trading at random against an engine, the way a real client would:
// what it sends next depends on the answers it has had so far. It cancels
// orders it has been told are resting, so on a running engine no two sessions
// are alike. That is what makes them worth recording.
template <typename Engine>
class Trader {
 public:
  Trader(Engine& engine, const std::vector<BookConfig>& books, Capture& capture,
         std::uint32_t seed)
      : engine_(engine), books_(books), capture_(capture), live_(books.size()), random_(seed) {}

  // Sends this many commands, reading answers as it goes.
  void trade(int commands) {
    for (int sent_so_far = 0; sent_so_far < commands; ++sent_so_far) {
      // Never far ahead of the engine: what it is sent should depend on what
      // it has answered, and the market data ring must never overflow, or
      // there would be nothing exact to compare.
      while (unanswered_ >= kWindow) {
        read();
      }
      step();
      read();
    }
  }

  // Waits until the engine has answered everything.
  void finish() {
    while (engine_.commands_pending() != 0 || unanswered_ != 0) {
      read();
    }
    read();
  }

  // Makes this fraction of the orders go to symbols 0 and 4, to give
  // rebalancing something to do.
  void favour_two_symbols(int percent) { favoured_percent_ = percent; }

  [[nodiscard]] std::uint64_t moves_started() const { return moves_started_; }

 private:
  static constexpr int kWindow = 64;

  int pick(int low, int high) { return std::uniform_int_distribution<int>(low, high)(random_); }

  void read() {
    if (!engine_.running()) {
      engine_.process_pending();
    }
    std::size_t got = engine_.poll([&](const Event& event) { on_event(event); });
    got += engine_.poll_market_data([&](const MarketData& message) { capture_.add(message); });
    if (got == 0 && engine_.running()) {
      std::this_thread::yield();
    }
  }

  void on_event(const Event& event) {
    capture_.add(event);
    if (event.type != EventType::Trade) {
      --unanswered_;
    }
    if (event.type == EventType::Accepted && event.resting != 0) {
      live_.at(event.symbol).push_back(event.order_id);
    }
  }

  void order(SymbolId symbol, Side side, Price price, Quantity quantity, OrderType type) {
    while (!sent(engine_.submit(next_tag_, symbol, side, price, quantity, type))) {
      read();
    }
    ++next_tag_;
    ++unanswered_;
  }

  void cancel(SymbolId symbol, OrderId id) {
    while (!sent(engine_.cancel(next_tag_, symbol, id))) {
      read();
    }
    ++next_tag_;
    ++unanswered_;
  }

  void move(SymbolId symbol) {
    if constexpr (requires { engine_.move_symbol(symbol, std::size_t{0}); }) {
      const auto to = static_cast<std::size_t>(
          pick(0, static_cast<int>(engine_.shard_count()) - 1));
      const bool is_move = engine_.shard_of(symbol) != to;
      while (!sent(engine_.move_symbol(symbol, to))) {
        read();
      }
      if (is_move) {
        ++moves_started_;
      }
    } else {
      while (!sent(engine_.request_snapshot(symbol))) {
        read();
      }
    }
  }

  // A command for a symbol that does not exist. One engine takes it and
  // answers that it is refused; a ShardedEngine will not take it at all, so
  // there it is not sent.
  bool stray() {
    if constexpr (requires { engine_.move_symbol(SymbolId{0}, std::size_t{0}); }) {
      return false;
    } else {
      const auto nowhere = static_cast<SymbolId>(books_.size() + static_cast<std::size_t>(pick(0, 2)));
      switch (pick(0, 2)) {
        case 0:
          order(nowhere, Side::Buy, 1'020, 5, OrderType::Limit);
          break;
        case 1:
          cancel(nowhere, static_cast<OrderId>(pick(1, 1'000)));
          break;
        default:
          while (!sent(engine_.request_snapshot(nowhere))) {
            read();
          }
          break;
      }
      return true;
    }
  }

  void step() {
    if (pick(0, 99) == 0 && stray()) {
      return;
    }
    const int symbols = static_cast<int>(books_.size());
    auto symbol = static_cast<SymbolId>(pick(0, symbols - 1));
    if (pick(0, 99) < favoured_percent_) {
      symbol = static_cast<SymbolId>(pick(0, 1) == 0 ? 0 : std::min(4, symbols - 1));
    }
    const Price base = books_[symbol].min_price;
    const Side side = pick(0, 1) == 0 ? Side::Buy : Side::Sell;
    // Bids at 20..36 ticks above the bottom and asks at 28..44, so they meet.
    const Price price = base + (side == Side::Buy ? pick(20, 36) : pick(28, 44));
    const auto quantity = static_cast<Quantity>(pick(1, 40));

    const int roll = pick(0, 99);
    if (roll < 55) {
      order(symbol, side, price, quantity, OrderType::Limit);
    } else if (roll < 63) {
      static constexpr OrderType kTypes[] = {OrderType::Market, OrderType::IOC, OrderType::FOK};
      order(symbol, side, price, static_cast<Quantity>(quantity * 3), kTypes[pick(0, 2)]);
    } else if (roll < 66) {
      // Refused: a price off the book, or nothing to trade.
      if (pick(0, 1) == 0) {
        order(symbol, side, base - 5, quantity, OrderType::Limit);
      } else {
        order(symbol, side, price, 0, OrderType::Limit);
      }
    } else if (roll < 90) {
      std::vector<OrderId>& live = live_[symbol];
      if (live.empty()) {
        cancel(symbol, 12'345);
      } else {
        const auto chosen = static_cast<std::size_t>(pick(0, static_cast<int>(live.size()) - 1));
        const OrderId id = live[chosen];
        live[chosen] = live.back();
        live.pop_back();
        cancel(symbol, id);
      }
    } else if (roll < 93) {
      // An order that never was, or one that has already gone.
      cancel(symbol, static_cast<OrderId>(pick(1, 1'000'000)));
    } else if (roll < 97) {
      while (!sent(engine_.request_snapshot(symbol))) {
        read();
      }
    } else {
      move(symbol);
    }
  }

  Engine& engine_;
  const std::vector<BookConfig>& books_;
  Capture& capture_;
  std::vector<std::vector<OrderId>> live_;  // orders it believes are resting, by symbol
  std::mt19937 random_;
  std::uint64_t next_tag_ = 1;
  int unanswered_ = 0;
  int favoured_percent_ = 0;
  std::uint64_t moves_started_ = 0;
};

std::uint64_t count_of(std::span<const Command> commands, CommandType type) {
  return static_cast<std::uint64_t>(
      std::count_if(commands.begin(), commands.end(),
                    [type](const Command& command) { return command.type == type; }));
}

// --- The recording itself ------------------------------------------------------

TEST(Recorder, WritesEveryCommandInTheOrderItWasSent) {
  const TempFile file;
  const std::vector<BookConfig> books = make_books(2);
  {
    MatchingEngine engine({.books = books, .record_to = file.path()});
    ASSERT_TRUE(engine.recording());
    ASSERT_TRUE(engine.submit(11, 0, Side::Buy, 1'020, 7));
    ASSERT_TRUE(engine.submit(12, 1, Side::Sell, 1'130, 9, OrderType::IOC));
    ASSERT_TRUE(engine.cancel(13, 0, 0xABCDEF0123456789ULL));
    ASSERT_TRUE(engine.request_snapshot(1));
    ASSERT_TRUE(engine.submit(14, 1, Side::Buy, -3, 0, OrderType::FOK));
    engine.flush_recording();
    EXPECT_EQ(engine.commands_recorded(), 5u);
  }

  const std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  EXPECT_FALSE(recording->damaged());
  const std::span<const Command> commands = recording->commands();
  ASSERT_EQ(commands.size(), 5u);

  EXPECT_EQ(commands[0].type, CommandType::Submit);
  EXPECT_EQ(commands[0].client_tag, 11u);
  EXPECT_EQ(commands[0].symbol, 0);
  EXPECT_EQ(commands[0].side, Side::Buy);
  EXPECT_EQ(commands[0].price, 1'020);
  EXPECT_EQ(commands[0].quantity, 7u);
  EXPECT_EQ(commands[0].order_type, OrderType::Limit);

  EXPECT_EQ(commands[1].type, CommandType::Submit);
  EXPECT_EQ(commands[1].client_tag, 12u);
  EXPECT_EQ(commands[1].symbol, 1);
  EXPECT_EQ(commands[1].side, Side::Sell);
  EXPECT_EQ(commands[1].price, 1'130);
  EXPECT_EQ(commands[1].quantity, 9u);
  EXPECT_EQ(commands[1].order_type, OrderType::IOC);

  EXPECT_EQ(commands[2].type, CommandType::Cancel);
  EXPECT_EQ(commands[2].client_tag, 13u);
  EXPECT_EQ(commands[2].symbol, 0);
  EXPECT_EQ(commands[2].order_id, 0xABCDEF0123456789ULL);

  EXPECT_EQ(commands[3].type, CommandType::Snapshot);
  EXPECT_EQ(commands[3].symbol, 1);

  EXPECT_EQ(commands[4].type, CommandType::Submit);
  EXPECT_EQ(commands[4].price, -3);  // a negative price comes back negative
  EXPECT_EQ(commands[4].quantity, 0u);
  EXPECT_EQ(commands[4].order_type, OrderType::FOK);
}

TEST(Recorder, KeepsTheBooksTheSessionRanWith) {
  const TempFile file;
  const std::vector<BookConfig> books = {
      {.symbol = "A", .min_price = -50, .num_levels = 128, .max_orders = 16},
      {.symbol = "EIGHTCHR", .min_price = 1'000'000'000'000, .num_levels = 64, .max_orders = 32},
      {.symbol = "FAR-TOO-LONG", .min_price = 0, .num_levels = 256, .max_orders = 64},
  };
  { MatchingEngine engine({.books = books, .record_to = file.path()}); }

  const std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  EXPECT_TRUE(recording->commands().empty());
  ASSERT_EQ(recording->books().size(), 3u);
  EXPECT_EQ(recording->books()[0].symbol, "A");
  EXPECT_EQ(recording->books()[0].min_price, -50);
  EXPECT_EQ(recording->books()[0].num_levels, 128u);
  EXPECT_EQ(recording->books()[0].max_orders, 16u);
  EXPECT_EQ(recording->books()[1].symbol, "EIGHTCHR");
  EXPECT_EQ(recording->books()[1].min_price, 1'000'000'000'000);
  EXPECT_EQ(recording->books()[2].symbol, "FAR-TOO-");  // what the book itself keeps
  EXPECT_EQ(recording->books()[2].max_orders, 64u);

  // They are enough to build the same engine again.
  MatchingEngine rebuilt({.books = recording->books()});
  EXPECT_EQ(rebuilt.symbol_count(), 3u);
  EXPECT_EQ(rebuilt.symbol_id("EIGHTCHR"), std::optional<SymbolId>(1));
  EXPECT_EQ(rebuilt.book(2).symbol(), "FAR-TOO-");
}

TEST(Recorder, LeavesOutCommandsTheEngineRefused) {
  const TempFile file;
  int taken = 0;
  {
    // Nothing runs the engine, so its ring of four fills up.
    MatchingEngine engine(
        {.books = make_books(1), .command_capacity = 4, .record_to = file.path()});
    for (std::uint64_t tag = 0; tag < 10; ++tag) {
      if (engine.submit(tag, 0, Side::Buy, 1'020, 1)) {
        ++taken;
      }
    }
  }
  ASSERT_EQ(taken, 4);

  const std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  ASSERT_EQ(recording->commands().size(), 4u);
  for (std::size_t index = 0; index < 4; ++index) {
    EXPECT_EQ(recording->commands()[index].client_tag, index);
  }
}

TEST(Recorder, LosesNothingWhenItFallsBehind) {
  constexpr std::uint64_t kCommands = 20'000;
  const TempFile file;
  std::uint64_t refused = 0;
  {
    // Room for two commands waiting to be written: the sender is far quicker
    // than that, so it is refused again and again and has to try again.
    MatchingEngine engine(
        {.books = make_books(1), .record_to = file.path(), .recording_capacity = 2});
    for (std::uint64_t tag = 0; tag < kCommands; ++tag) {
      while (!engine.submit(tag, 0, Side::Buy, 1'020, 1, OrderType::IOC)) {
        ++refused;
        engine.process_pending();
        engine.poll([](const Event&) {});
      }
    }
    engine.process_pending();
    EXPECT_EQ(engine.commands_processed(), kCommands);  // refused ones never reached the engine
    EXPECT_TRUE(engine.recording());
  }
  EXPECT_GT(refused, 0u);

  const std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  ASSERT_FALSE(recording->damaged());
  ASSERT_EQ(recording->commands().size(), kCommands);  // none lost, none twice
  for (std::uint64_t index = 0; index < kCommands; ++index) {
    ASSERT_EQ(recording->commands()[index].client_tag, index);
  }
}

TEST(Recorder, StoppingTheEngineLeavesACompleteRecording) {
  constexpr std::uint64_t kCommands = 5'000;
  const TempFile file;
  MatchingEngine engine(
      {.books = make_books(1), .idle = IdleStrategy::Yield, .record_to = file.path()});
  engine.start();
  for (std::uint64_t tag = 0; tag < kCommands; ++tag) {
    while (!engine.submit(tag, 0, Side::Buy, 1'020, 1, OrderType::IOC)) {
      engine.poll([](const Event&) {});
    }
    engine.poll([](const Event&) {});
  }
  engine.stop();

  // The engine is still alive and the file still open, and it is all there.
  EXPECT_EQ(engine.commands_recorded(), kCommands);
  const std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  EXPECT_FALSE(recording->damaged());
  EXPECT_EQ(recording->commands().size(), kCommands);
}

TEST(Recorder, FlushingMakesTheFileReadableWhileTheSessionGoesOn) {
  const TempFile file;
  MatchingEngine engine({.books = make_books(1), .record_to = file.path()});

  engine.flush_recording();  // nothing yet: must not hang
  ASSERT_TRUE(Recording::load(file.path()).has_value());
  EXPECT_TRUE(Recording::load(file.path())->commands().empty());

  for (std::uint64_t tag = 0; tag < 3; ++tag) {
    ASSERT_TRUE(engine.submit(tag, 0, Side::Buy, 1'020, 1));
  }
  engine.flush_recording();
  EXPECT_EQ(Recording::load(file.path())->commands().size(), 3u);

  for (std::uint64_t tag = 3; tag < 10; ++tag) {
    ASSERT_TRUE(engine.submit(tag, 0, Side::Buy, 1'020, 1));
  }
  engine.flush_recording();
  engine.flush_recording();
  EXPECT_EQ(Recording::load(file.path())->commands().size(), 10u);
  EXPECT_EQ(engine.commands_recorded(), 10u);
}

TEST(Recorder, AFlushCoversEverythingRecordedBeforeItWhileMoreArrives) {
  const TempFile file;
  const std::vector<BookConfig> books = make_books(1);
  lob::SessionRecorder recorder(file.path(), books, 1 << 12);
  std::atomic<std::uint64_t> recorded{0};
  std::atomic<bool> done{false};

  // One thread records as fast as it can, so the recorder never has a quiet
  // moment in which it would have written everything out anyway...
  std::thread sender([&] {
    Command command{};
    command.type = CommandType::Submit;
    std::uint64_t count = 0;
    while (!done.load(std::memory_order_acquire)) {
      if (recorder.can_record()) {
        command.client_tag = count;
        recorder.record(command);
        recorded.store(++count, std::memory_order_release);
      }
    }
  });

  // ...while this one flushes, and each time must find in the file at least
  // what had been recorded when it asked.
  const std::uintmax_t before_commands = sizeof(lob::RecordingHeader) + sizeof(lob::RecordedBook);
  int fell_short = 0;
  for (int round = 0; round < 300; ++round) {
    const std::uint64_t recorded_before = recorded.load(std::memory_order_acquire);
    recorder.flush();
    const std::uintmax_t size = std::filesystem::file_size(file.path());
    if (size < before_commands + recorded_before * sizeof(lob::RecordedCommand)) {
      ++fell_short;
    }
  }
  done.store(true, std::memory_order_release);
  sender.join();
  EXPECT_EQ(fell_short, 0);
  EXPECT_GT(recorded.load(), 300u);
  EXPECT_TRUE(recorder.ok());
}

TEST(Recorder, UsedOnItsOwnWritesOutWhatIsLeftWhenItIsDestroyed) {
  // No engine here to flush it first: the recorder's own destructor has to
  // finish the job.
  const TempFile file;
  const std::vector<BookConfig> books = make_books(2);
  for (int round = 0; round < 50; ++round) {
    {
      lob::SessionRecorder recorder(file.path(), books, 64);
      ASSERT_TRUE(recorder.ok());
      for (std::uint64_t tag = 0; tag < 40; ++tag) {
        Command command{};
        command.client_tag = tag;
        command.price = 1'020;
        command.quantity = 1;
        command.symbol = static_cast<SymbolId>(tag % 2);
        command.type = CommandType::Submit;
        ASSERT_TRUE(recorder.can_record());
        recorder.record(command);
      }
    }
    const std::optional<Recording> recording = Recording::load(file.path());
    ASSERT_TRUE(recording.has_value());
    ASSERT_EQ(recording->commands().size(), 40u) << "round " << round;
    ASSERT_EQ(recording->commands()[39].client_tag, 39u);
  }
}

TEST(Recorder, StopsAtItsLimitAndWhatItHasReplaysAsTheSessionSoFar) {
  // Limits either side of the 1,024 commands the writer hands over at a time,
  // and one that the session never reaches.
  for (const std::uint64_t limit : {std::uint64_t{1}, std::uint64_t{100}, std::uint64_t{1'024},
                                    std::uint64_t{1'500}, std::uint64_t{5'000}}) {
    constexpr std::uint64_t kCommands = 3'000;
    const TempFile file("_" + std::to_string(limit));
    std::vector<Event> live;
    std::vector<std::size_t> events_after;  // how many events there had been after each command
    {
      MatchingEngine engine(
          {.books = make_books(1), .record_to = file.path(), .recording_limit = limit});
      for (std::uint64_t tag = 0; tag < kCommands; ++tag) {
        // Each sell fills the buy before it: two commands, three events.
        while (!engine.submit(tag, 0, tag % 2 == 0 ? Side::Buy : Side::Sell, 1'020, 1)) {
          engine.process_pending();
          engine.poll([&](const Event& event) { live.push_back(event); });
        }
        engine.process_pending();
        engine.poll([&](const Event& event) { live.push_back(event); });
        events_after.push_back(live.size());
      }
      engine.flush_recording();
      const std::uint64_t expected = std::min(limit, kCommands);
      EXPECT_EQ(engine.commands_recorded(), expected) << "limit " << limit;
      EXPECT_EQ(engine.recording_at_limit(), limit <= kCommands) << "limit " << limit;
      EXPECT_TRUE(engine.recording());  // the file is good; it is just finished
    }

    const std::optional<Recording> recording = Recording::load(file.path());
    ASSERT_TRUE(recording.has_value());
    EXPECT_FALSE(recording->damaged());
    const std::uint64_t expected = std::min(limit, kCommands);
    ASSERT_EQ(recording->commands().size(), expected) << "limit " << limit;
    for (std::uint64_t index = 0; index < expected; ++index) {
      ASSERT_EQ(recording->commands()[index].client_tag, index);
    }

    MatchingEngine again({.books = recording->books()});
    std::vector<Event> replayed;
    lob::replay(recording->commands(), again,
                [&](const Event& event) { replayed.push_back(event); });
    const std::vector<Event> so_far(
        live.begin(), live.begin() + static_cast<std::ptrdiff_t>(events_after[expected - 1]));
    expect_same_events(so_far, replayed, "limit " + std::to_string(limit));
  }
}

TEST(Recorder, ALimitOfNoneNeverStops) {
  const TempFile file;
  MatchingEngine engine({.books = make_books(1), .record_to = file.path()});
  for (std::uint64_t tag = 0; tag < 5'000; ++tag) {
    while (!engine.submit(tag, 0, Side::Buy, 1'020, 1, OrderType::IOC)) {
      engine.process_pending();
      engine.poll([](const Event&) {});
    }
    engine.process_pending();
    engine.poll([](const Event&) {});
  }
  engine.flush_recording();
  EXPECT_EQ(engine.commands_recorded(), 5'000u);
  EXPECT_FALSE(engine.recording_at_limit());
  EXPECT_FALSE(MatchingEngine({.books = make_books(1)}).recording_at_limit());  // not recording at all
}

TEST(ShardedRecorder, StopsAtItsLimitToo) {
  const TempFile file;
  ShardedEngine engine({.books = make_books(4),
                        .shards = 2,
                        .record_to = file.path(),
                        .recording_limit = 10});
  for (std::uint64_t tag = 0; tag < 40; ++tag) {
    ASSERT_EQ(engine.submit(tag, static_cast<SymbolId>(tag % 4), Side::Buy, 1'020 + 100 * static_cast<Price>(tag % 4), 1,
                            OrderType::IOC),
              SendStatus::Sent);
    engine.process_pending();
    engine.poll([](const Event&) {});
  }
  engine.flush_recording();
  EXPECT_EQ(engine.commands_recorded(), 10u);
  EXPECT_TRUE(engine.recording_at_limit());
  EXPECT_EQ(Recording::load(file.path())->commands().size(), 10u);
}

TEST(Recorder, IsOffUnlessAFileIsNamed) {
  MatchingEngine engine({.books = make_books(1)});
  EXPECT_FALSE(engine.recording());
  ASSERT_TRUE(engine.submit(1, 0, Side::Buy, 1'020, 1));
  engine.flush_recording();  // harmless
  EXPECT_EQ(engine.commands_recorded(), 0u);

  ShardedEngine sharded({.books = make_books(2), .shards = 2});
  EXPECT_FALSE(sharded.recording());
  sharded.flush_recording();
  EXPECT_EQ(sharded.commands_recorded(), 0u);
}

TEST(Recorder, AFileThatCannotBeOpenedIsReportedAndTheEngineCarriesOn) {
  const std::string nowhere = ::testing::TempDir() + "lob-no-such-directory/session.rec";
  MatchingEngine engine({.books = make_books(1), .record_to = nowhere});
  EXPECT_FALSE(engine.recording());

  // Far more commands than the recorder has room for: it must keep taking
  // them, or the engine would be stuck.
  std::uint64_t answered = 0;
  for (std::uint64_t tag = 0; tag < 200'000; ++tag) {
    while (!engine.submit(tag, 0, Side::Buy, 1'020, 1, OrderType::IOC)) {
      engine.process_pending();
      answered += engine.poll([](const Event&) {});
    }
  }
  engine.process_pending();
  answered += engine.poll([](const Event&) {});
  engine.flush_recording();  // must not hang either
  EXPECT_EQ(answered, 200'000u);
  EXPECT_EQ(engine.commands_recorded(), 0u);
  EXPECT_FALSE(Recording::load(nowhere).has_value());
}

// A recording of `count` orders for one symbol, tags 0, 1, 2 and so on.
void record_orders(const std::string& path, std::uint64_t count) {
  MatchingEngine engine({.books = make_books(1), .record_to = path});
  for (std::uint64_t tag = 0; tag < count; ++tag) {
    ASSERT_TRUE(engine.submit(tag, 0, tag % 2 == 0 ? Side::Buy : Side::Sell, 1'030, 5));
  }
}

TEST(RecordingFile, HasTheSizeItsLayoutSays) {
  const TempFile file;
  record_orders(file.path(), 10);
  EXPECT_EQ(read_file(file.path()).size(),
            sizeof(lob::RecordingHeader) + sizeof(lob::RecordedBook) +
                10 * sizeof(lob::RecordedCommand));
}

TEST(RecordingFile, IsTurnedDownIfItIsNotARecording) {
  const TempFile file;
  EXPECT_FALSE(Recording::load(file.path()).has_value());  // no such file

  write_file(file.path(), "");
  EXPECT_FALSE(Recording::load(file.path()).has_value());

  write_file(file.path(), std::string(4'096, 'x'));
  EXPECT_FALSE(Recording::load(file.path()).has_value());

  record_orders(file.path(), 4);
  const std::string good = read_file(file.path());
  ASSERT_TRUE(Recording::load(file.path()).has_value());

  // Each of the header's fields in turn.
  const auto with_byte = [&](std::size_t offset, char value) {
    std::string bytes = good;
    bytes[offset] = value;
    return bytes;
  };
  write_file(file.path(), with_byte(0, 'X'));  // the magic
  EXPECT_FALSE(Recording::load(file.path()).has_value());
  write_file(file.path(), with_byte(12, 99));  // the version
  EXPECT_FALSE(Recording::load(file.path()).has_value());
  write_file(file.path(), with_byte(20, 24));  // the size of a command
  EXPECT_FALSE(Recording::load(file.path()).has_value());

  // Written on a machine with the other byte order.
  std::string swapped = good;
  std::reverse(swapped.begin() + 8, swapped.begin() + 12);
  write_file(file.path(), swapped);
  EXPECT_FALSE(Recording::load(file.path()).has_value());

  // No books, or more books than the file goes on to describe.
  std::string no_books = good;
  std::fill(no_books.begin() + 16, no_books.begin() + 20, '\0');
  write_file(file.path(), no_books);
  EXPECT_FALSE(Recording::load(file.path()).has_value());
  write_file(file.path(), good.substr(0, sizeof(lob::RecordingHeader) + 10));
  EXPECT_FALSE(Recording::load(file.path()).has_value());
  write_file(file.path(), good.substr(0, 10));
  EXPECT_FALSE(Recording::load(file.path()).has_value());

  write_file(file.path(), good);  // and the untouched bytes still load
  EXPECT_TRUE(Recording::load(file.path()).has_value());
}

TEST(RecordingFile, CutShortKeepsEverythingBeforeTheCut) {
  const TempFile file;
  record_orders(file.path(), 10);
  const std::string whole = read_file(file.path());

  // The program died part-way through writing the tenth command.
  write_file(file.path(), whole.substr(0, whole.size() - 5));
  std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  EXPECT_TRUE(recording->damaged());
  ASSERT_EQ(recording->commands().size(), 9u);
  EXPECT_EQ(recording->commands()[8].client_tag, 8u);

  // What is left replays like any other recording.
  MatchingEngine engine({.books = recording->books()});
  const ReplayResult result = lob::replay(recording->commands(), engine, [](const Event&) {});
  EXPECT_EQ(result.commands, 9u);

  // Cut cleanly between two commands, there is nothing to say it was cut.
  write_file(file.path(), whole.substr(0, whole.size() - sizeof(lob::RecordedCommand)));
  recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  EXPECT_FALSE(recording->damaged());
  EXPECT_EQ(recording->commands().size(), 9u);

  // Cut straight after the books: a session in which nothing was sent.
  write_file(file.path(),
             whole.substr(0, sizeof(lob::RecordingHeader) + sizeof(lob::RecordedBook)));
  recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  EXPECT_FALSE(recording->damaged());
  EXPECT_TRUE(recording->commands().empty());
}

TEST(RecordingFile, StopsAtSomethingThatIsNotACommand) {
  const TempFile file;
  record_orders(file.path(), 10);
  const std::string good = read_file(file.path());
  const std::size_t fourth = sizeof(lob::RecordingHeader) + sizeof(lob::RecordedBook) +
                             3 * sizeof(lob::RecordedCommand);

  // Offsets within a command: side 22, type 23, order type 24, and the
  // unused bytes from 25.
  struct Damage {
    std::size_t offset;
    char value;
  };
  for (const Damage damage :
       {Damage{22, 2},   // not a side
        Damage{23, 2},   // Detach: never recorded
        Damage{23, 3},   // Attach: never recorded
        Damage{23, 77},  // not a command type at all
        Damage{24, 4},   // not an order type
        Damage{31, 1}})  // something in the unused bytes
  {
    std::string bytes = good;
    bytes[fourth + damage.offset] = damage.value;
    write_file(file.path(), bytes);
    const std::optional<Recording> recording = Recording::load(file.path());
    ASSERT_TRUE(recording.has_value());
    EXPECT_TRUE(recording->damaged()) << "offset " << damage.offset;
    EXPECT_EQ(recording->commands().size(), 3u) << "offset " << damage.offset;
  }

  // A symbol the session did not have (offset 20) is not damage: an engine can
  // be sent such a command, and answers it.
  std::string bytes = good;
  bytes[fourth + 20] = 7;
  write_file(file.path(), bytes);
  const std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  EXPECT_FALSE(recording->damaged());
  ASSERT_EQ(recording->commands().size(), 10u);
  EXPECT_EQ(recording->commands()[3].symbol, 7);
}

TEST(RecordingFile, HoldsMoreCommandsThanAreReadAtOnce) {
  // The reader takes 4,096 commands at a time; this is a few blocks and a bit.
  constexpr std::uint64_t kCommands = 3 * 4'096 + 17;
  const TempFile file;
  {
    MatchingEngine engine({.books = make_books(1), .record_to = file.path()});
    for (std::uint64_t tag = 0; tag < kCommands; ++tag) {
      while (!engine.submit(tag, 0, Side::Buy, 1'020, 1, OrderType::IOC)) {
        engine.process_pending();
        engine.poll([](const Event&) {});
      }
    }
  }
  const std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  EXPECT_FALSE(recording->damaged());
  ASSERT_EQ(recording->commands().size(), kCommands);
  for (std::uint64_t index = 0; index < kCommands; ++index) {
    ASSERT_EQ(recording->commands()[index].client_tag, index);
  }
}

// --- Recording a sharded engine -------------------------------------------------

TEST(ShardedRecorder, PutsEveryShardsCommandsAndTheMovesInOneRecording) {
  const TempFile file;
  {
    ShardedEngine engine({.books = make_books(4), .shards = 2, .record_to = file.path()});
    ASSERT_TRUE(engine.recording());
    ASSERT_EQ(engine.shard_of(0), 0u);
    ASSERT_EQ(engine.shard_of(1), 1u);

    ASSERT_EQ(engine.submit(1, 0, Side::Buy, 1'020, 5), SendStatus::Sent);
    ASSERT_EQ(engine.submit(2, 1, Side::Sell, 1'130, 5), SendStatus::Sent);
    ASSERT_EQ(engine.move_symbol(0, 1), SendStatus::Sent);
    ASSERT_EQ(engine.submit(3, 0, Side::Sell, 1'020, 5), SendStatus::Sent);
    ASSERT_EQ(engine.move_symbol(0, 1), SendStatus::Sent);  // already there: not a move
    ASSERT_EQ(engine.move_symbol(0, 7), SendStatus::UnknownSymbol);
    ASSERT_EQ(engine.submit(4, 9, Side::Buy, 1'020, 5), SendStatus::UnknownSymbol);
    ASSERT_EQ(engine.request_snapshot(3), SendStatus::Sent);
    ASSERT_EQ(engine.cancel(5, 2, 42), SendStatus::Sent);
    ASSERT_EQ(engine.move_symbol(0, 0), SendStatus::Sent);
    engine.flush_recording();
    EXPECT_EQ(engine.commands_recorded(), 7u);
  }

  const std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  ASSERT_EQ(recording->books().size(), 4u);
  const std::span<const Command> commands = recording->commands();
  ASSERT_EQ(commands.size(), 7u);

  const CommandType expected[] = {CommandType::Submit, CommandType::Submit, CommandType::Move,
                                  CommandType::Submit, CommandType::Snapshot,
                                  CommandType::Cancel, CommandType::Move};
  for (std::size_t index = 0; index < 7; ++index) {
    EXPECT_EQ(commands[index].type, expected[index]) << index;
  }
  EXPECT_EQ(commands[2].symbol, 0);
  EXPECT_EQ(commands[2].quantity, 1u);  // the shard it went to
  EXPECT_EQ(commands[3].client_tag, 3u);
  EXPECT_EQ(commands[6].symbol, 0);
  EXPECT_EQ(commands[6].quantity, 0u);
}

TEST(ShardedRecorder, AMoveIsNotStartedIfItCannotBeRecorded) {
  const TempFile file;
  // Room for two commands waiting to be written. Whether there is room at any
  // moment depends on the writer, so keep trying moves and check that each one
  // either happened and was recorded, or did neither.
  ShardedEngine engine(
      {.books = make_books(2), .shards = 2, .record_to = file.path(), .recording_capacity = 2});
  std::uint64_t started = 0;
  for (int attempt = 0; attempt < 20'000; ++attempt) {
    const std::size_t from = engine.shard_of(0);
    const SendStatus status = engine.move_symbol(0, 1 - from);
    if (status == SendStatus::Sent) {
      ++started;
      ASSERT_EQ(engine.shard_of(0), 1 - from);
    } else {
      ASSERT_EQ(status, SendStatus::RingFull);
      ASSERT_EQ(engine.shard_of(0), from);  // nothing changed
    }
    engine.process_pending();
    engine.poll([](const Event&) {});
  }
  engine.flush_recording();

  const std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  EXPECT_EQ(recording->commands().size(), started);
  EXPECT_EQ(count_of(recording->commands(), CommandType::Move), started);
  EXPECT_GT(started, 0u);
}

TEST(ShardedRecorder, RecordsTheMovesRebalancingMakes) {
  const TempFile file;
  std::uint64_t moves = 0;
  {
    // Symbols 0 and 2 share shard 0 and get all the traffic.
    ShardedEngine engine({.books = make_books(4),
                          .shards = 2,
                          .rebalance = {.every = 200, .min_sample = 100},
                          .record_to = file.path()});
    for (std::uint64_t tag = 0; tag < 4'000; ++tag) {
      const auto symbol = static_cast<SymbolId>(tag % 2 == 0 ? 0 : 2);
      const Price price = 1'020 + 100 * static_cast<Price>(symbol);
      while (engine.submit(tag, symbol, Side::Buy, price, 1, OrderType::IOC) !=
             SendStatus::Sent) {
        engine.process_pending();
        engine.poll([](const Event&) {});
      }
      engine.process_pending();
      engine.poll([](const Event&) {});
    }
    moves = engine.rebalancing_moves();
  }
  ASSERT_GT(moves, 0u);

  const std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  EXPECT_EQ(count_of(recording->commands(), CommandType::Move), moves);
  EXPECT_EQ(count_of(recording->commands(), CommandType::Submit), 4'000u);
}

// --- Replaying -----------------------------------------------------------------

class Replay : public ::testing::Test {
 protected:
  static constexpr std::size_t kFeed = std::size_t{1} << 16;

  // Runs a random session on one engine with a thread of its own, and leaves
  // the recording in `file`.
  void run_live_session(std::size_t symbols, int commands, std::uint32_t seed) {
    books = make_books(symbols);
    live = std::make_unique<Capture>(symbols);
    original = std::make_unique<MatchingEngine>(lob::EngineConfig{
        .books = books,
        .idle = IdleStrategy::Yield,
        .market_data_capacity = kFeed,
        .record_to = file.path()});
    ASSERT_TRUE(original->recording());
    original->start();
    Trader<MatchingEngine> trader(*original, books, *live, seed);
    trader.trade(commands);
    trader.finish();
    original->stop();
    ASSERT_EQ(original->events_dropped(), 0u);
    ASSERT_EQ(original->market_data_dropped(), 0u);

    std::optional<Recording> loaded = Recording::load(file.path());
    ASSERT_TRUE(loaded.has_value());
    ASSERT_FALSE(loaded->damaged());
    recording = std::make_unique<Recording>(std::move(*loaded));
    ASSERT_GE(recording->commands().size(), static_cast<std::size_t>(commands));
  }

  // How many of the recorded commands were for symbols from `first` on.
  [[nodiscard]] std::uint64_t commands_from_symbol(std::size_t first) const {
    return static_cast<std::uint64_t>(
        std::count_if(recording->commands().begin(), recording->commands().end(),
                      [first](const Command& command) { return command.symbol >= first; }));
  }

  // Commands the trader sent for symbols that do not exist.
  [[nodiscard]] std::uint64_t strays() const { return commands_from_symbol(books.size()); }

  TempFile file;
  std::vector<BookConfig> books;
  std::unique_ptr<Capture> live;
  std::unique_ptr<MatchingEngine> original;
  std::unique_ptr<Recording> recording;
};

TEST_F(Replay, ReproducesASessionExactly) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(3, 20'000, 2024));

  // The session was a real one: orders rested, traded, were refused and
  // cancelled, and snapshots were asked for.
  const auto count = [&](EventType type) {
    return std::count_if(live->in_order.begin(), live->in_order.end(),
                         [type](const Event& event) { return event.type == type; });
  };
  ASSERT_GT(count(EventType::Accepted), 1'000);
  ASSERT_GT(count(EventType::Trade), 1'000);
  ASSERT_GT(count(EventType::Rejected), 100);
  ASSERT_GT(count(EventType::Cancelled), 1'000);
  ASSERT_GT(count(EventType::CancelRejected), 100);
  ASSERT_GT(count_of(recording->commands(), CommandType::Snapshot), 100u);
  ASSERT_GT(strays(), 50u);  // and commands for symbols that do not exist

  // A fresh engine, built from nothing but the file, run by hand.
  MatchingEngine again({.books = recording->books(), .market_data_capacity = kFeed});
  Capture replayed(books.size());
  const ReplayResult result = replay_into(again, recording->commands(), replayed);

  EXPECT_EQ(result.commands, recording->commands().size());
  EXPECT_EQ(result.moves, 0u);
  EXPECT_EQ(result.skipped, 0u);
  EXPECT_EQ(result.events, live->in_order.size());
  EXPECT_EQ(again.events_dropped(), 0u);
  EXPECT_EQ(again.market_data_dropped(), 0u);

  // One engine puts all its events in one order, so the whole stream must
  // match, not just each symbol's part of it.
  expect_same_events(live->in_order, replayed.in_order, "the whole session");
  expect_same_session(*live, replayed);
  for (std::size_t symbol = 0; symbol < books.size(); ++symbol) {
    expect_same_book(original->book(static_cast<SymbolId>(symbol)),
                     again.book(static_cast<SymbolId>(symbol)), books[symbol]);
  }
}

TEST_F(Replay, ReproducesItOnARunningEngineToo) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(3, 10'000, 7));

  MatchingEngine again({.books = recording->books(),
                        .idle = IdleStrategy::Yield,
                        .market_data_capacity = kFeed});
  again.start();
  Capture replayed(books.size());
  const ReplayResult result = replay_into(again, recording->commands(), replayed);
  again.stop();

  EXPECT_EQ(result.commands, recording->commands().size());
  EXPECT_EQ(result.events, live->in_order.size());
  ASSERT_EQ(again.market_data_dropped(), 0u);
  expect_same_events(live->in_order, replayed.in_order, "the whole session");
  expect_same_session(*live, replayed);
  for (std::size_t symbol = 0; symbol < books.size(); ++symbol) {
    expect_same_book(original->book(static_cast<SymbolId>(symbol)),
                     again.book(static_cast<SymbolId>(symbol)), books[symbol]);
  }
}

TEST_F(Replay, ReproducesItOnShardsWithoutAFeedReader) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(6, 10'000, 99));

  // A different shape of engine altogether, and this time only the events are
  // read: the three-argument form of replay() leaves the feed alone.
  ShardedEngine again({.books = recording->books(), .shards = 3, .idle = IdleStrategy::Yield});
  again.start();
  Capture replayed(books.size());
  const ReplayResult result = lob::replay(recording->commands(), again,
                                          [&](const Event& event) { replayed.add(event); });
  again.stop();

  // The one difference: a ShardedEngine will not take a command for a symbol
  // it does not have, where the single engine took it and refused it.
  ASSERT_GT(strays(), 20u);
  EXPECT_EQ(result.skipped, strays());
  EXPECT_EQ(result.commands, recording->commands().size() - strays());
  EXPECT_EQ(result.events, live->in_order.size() - live->events[books.size()].size());
  EXPECT_TRUE(replayed.events[books.size()].empty());
  for (std::size_t symbol = 0; symbol < books.size(); ++symbol) {
    expect_same_events(live->events[symbol], replayed.events[symbol],
                       "symbol " + std::to_string(symbol));
    expect_same_book(original->book(static_cast<SymbolId>(symbol)),
                     again.book(static_cast<SymbolId>(symbol)), books[symbol]);
  }
}

TEST_F(Replay, RecordingAReplayGivesTheSameFileByteForByte) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(3, 5'000, 31));

  const TempFile second("_again");
  {
    MatchingEngine again({.books = recording->books(), .record_to = second.path()});
    lob::replay(recording->commands(), again, [](const Event&) {});
  }
  const std::string first_bytes = read_file(file.path());
  ASSERT_GT(first_bytes.size(), 5'000 * sizeof(lob::RecordedCommand));
  EXPECT_TRUE(first_bytes == read_file(second.path()));
}

TEST_F(Replay, StoppingPartWayGivesTheSessionAsItStoodThen) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(2, 3'000, 5));
  const std::span<const Command> commands = recording->commands();

  // Run the whole thing by hand once, noting how many events there had been
  // after each command.
  std::vector<std::size_t> events_after;
  {
    MatchingEngine engine({.books = recording->books()});
    std::size_t events = 0;
    for (const Command& command : commands) {
      lob::replay(std::span<const Command>(&command, 1), engine,
                  [&](const Event&) { ++events; });
      events_after.push_back(events);
    }
    ASSERT_EQ(events, live->in_order.size());
  }

  for (const std::size_t stop_after : {std::size_t{0}, std::size_t{1}, std::size_t{777},
                                       commands.size() - 1, commands.size()}) {
    MatchingEngine engine({.books = recording->books()});
    std::vector<Event> events;
    const ReplayResult result = lob::replay(commands.first(stop_after), engine,
                                            [&](const Event& event) { events.push_back(event); });
    EXPECT_EQ(result.commands, stop_after);
    const std::size_t expected = stop_after == 0 ? 0 : events_after[stop_after - 1];
    const std::vector<Event> prefix(live->in_order.begin(),
                                    live->in_order.begin() + static_cast<std::ptrdiff_t>(expected));
    expect_same_events(prefix, events, "stopping after " + std::to_string(stop_after));
  }
}

TEST_F(Replay, AnEngineWithFewerBooksRefusesTheCommandsForTheRest) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(3, 5'000, 12));

  // Only the first two of the three books. Commands for the third are given
  // to the engine like any others, and it answers each as refused.
  const std::vector<BookConfig> two(recording->books().begin(), recording->books().begin() + 2);
  MatchingEngine again({.books = two});
  Capture replayed(2);
  const ReplayResult result = lob::replay(recording->commands(), again,
                                          [&](const Event& event) { replayed.add(event); });

  ASSERT_GT(commands_from_symbol(2), 1'000u);
  EXPECT_EQ(result.skipped, 0u);
  EXPECT_EQ(result.commands, recording->commands().size());
  std::uint64_t orders_for_the_rest = 0;
  for (const Command& command : recording->commands()) {
    if (command.symbol >= 2 && command.type != CommandType::Snapshot) {
      ++orders_for_the_rest;
    }
  }
  ASSERT_EQ(replayed.events[2].size(), orders_for_the_rest);
  for (const Event& event : replayed.events[2]) {
    ASSERT_TRUE(event.type == EventType::Rejected || event.type == EventType::CancelRejected);
  }
  // The symbols it does have are none the worse for it.
  expect_same_events(live->events[0], replayed.events[0], "symbol 0");
  expect_same_events(live->events[1], replayed.events[1], "symbol 1");
}

TEST_F(Replay, AShardedEngineSkipsCommandsForSymbolsItDoesNotHave) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(3, 5'000, 13));

  const std::vector<BookConfig> two(recording->books().begin(), recording->books().begin() + 2);
  ShardedEngine again({.books = two, .shards = 2});
  Capture replayed(2);
  const ReplayResult result = lob::replay(recording->commands(), again,
                                          [&](const Event& event) { replayed.add(event); },
                                          {.repeat_moves = true});

  ASSERT_GT(commands_from_symbol(2), 1'000u);
  EXPECT_EQ(result.skipped, commands_from_symbol(2));
  EXPECT_EQ(result.commands, recording->commands().size() - commands_from_symbol(2));
  EXPECT_TRUE(replayed.events[2].empty());
  expect_same_events(live->events[0], replayed.events[0], "symbol 0");
  expect_same_events(live->events[1], replayed.events[1], "symbol 1");
}

TEST(ReplayStray, ACommandForAnUnknownSymbolIsRefusedAgainAsItWasInTheSession) {
  const TempFile file;
  std::vector<Event> live;
  {
    MatchingEngine engine({.books = make_books(2), .record_to = file.path()});
    ASSERT_TRUE(engine.submit(1, 0, Side::Buy, 1'020, 5));
    ASSERT_TRUE(engine.submit(2, 9, Side::Buy, 1'020, 5));  // no such symbol
    ASSERT_TRUE(engine.cancel(3, 9, 77));
    ASSERT_TRUE(engine.request_snapshot(9));
    ASSERT_TRUE(engine.submit(4, 1, Side::Sell, 1'130, 5));
    engine.process_pending();
    engine.poll([&](const Event& event) { live.push_back(event); });
  }
  ASSERT_EQ(live.size(), 4u);
  ASSERT_EQ(live[1].type, EventType::Rejected);
  ASSERT_EQ(live[2].type, EventType::CancelRejected);

  const std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());
  EXPECT_FALSE(recording->damaged());
  ASSERT_EQ(recording->commands().size(), 5u);

  MatchingEngine again({.books = recording->books()});
  std::vector<Event> replayed;
  const ReplayResult result = lob::replay(recording->commands(), again,
                                          [&](const Event& event) { replayed.push_back(event); });
  EXPECT_EQ(result.commands, 5u);
  EXPECT_EQ(result.skipped, 0u);
  expect_same_events(live, replayed, "the session");

  // A sharded engine cannot be sent those three at all.
  ShardedEngine sharded({.books = recording->books(), .shards = 2});
  const ReplayResult on_shards = lob::replay(recording->commands(), sharded, [](const Event&) {},
                                             {.repeat_moves = true});
  EXPECT_EQ(on_shards.commands, 2u);
  EXPECT_EQ(on_shards.skipped, 3u);
}

TEST(ReplayByHand, ReadsTheEventsAfterEveryCommand) {
  // An engine run by hand throws away events that do not fit in its ring, so
  // a replay must never let them pile up. Here the ring holds two.
  std::vector<Command> commands;
  for (std::uint64_t tag = 0; tag < 1'000; ++tag) {
    Command command{};
    command.client_tag = tag;
    command.price = 1'020;
    command.quantity = 1;
    command.side = Side::Buy;
    command.type = CommandType::Submit;
    command.order_type = OrderType::IOC;  // nothing to trade with: one event each
    commands.push_back(command);
  }
  MatchingEngine engine({.books = make_books(1), .event_capacity = 2});
  std::uint64_t next_tag = 0;
  const ReplayResult result = lob::replay(commands, engine, [&](const Event& event) {
    EXPECT_EQ(event.client_tag, next_tag++);
  });
  EXPECT_EQ(result.commands, 1'000u);
  EXPECT_EQ(result.events, 1'000u);
  EXPECT_EQ(engine.events_dropped(), 0u);
}

TEST(ReplayRunning, KeepsGoingWhenTheRingsAreFull) {
  // Rings far smaller than the session, on a running engine: the replay has
  // to wait for room again and again, and must lose nothing doing it.
  std::vector<Command> commands;
  for (std::uint64_t tag = 0; tag < 20'000; ++tag) {
    Command command{};
    command.client_tag = tag;
    command.price = 1'020;
    command.quantity = 1;
    command.side = tag % 2 == 0 ? Side::Buy : Side::Sell;  // each sell fills the buy before it
    command.type = CommandType::Submit;
    commands.push_back(command);
  }
  MatchingEngine engine({.books = make_books(1),
                         .command_capacity = 4,
                         .event_capacity = 4,
                         .idle = IdleStrategy::Yield});
  engine.start();
  std::uint64_t answers = 0;
  std::uint64_t trades = 0;
  const ReplayResult result = lob::replay(commands, engine, [&](const Event& event) {
    if (event.type == EventType::Trade) {
      ++trades;
    } else {
      EXPECT_EQ(event.client_tag, answers++);
    }
  });
  engine.stop();
  EXPECT_EQ(result.commands, 20'000u);
  EXPECT_EQ(answers, 20'000u);
  EXPECT_EQ(trades, 10'000u);
  EXPECT_EQ(result.events, 30'000u);
  EXPECT_EQ(engine.events_dropped(), 0u);
}

TEST(ReplayRunning, KeepsGoingWhenAShardsRingsAreFull) {
  // The same on shards, with moves among the commands: a move needs room in
  // two rings at once, and has to wait for it like anything else.
  constexpr std::uint64_t kOrders = 20'000;
  constexpr std::size_t kSymbols = 4;
  std::vector<Command> commands;
  std::uint64_t moves = 0;
  for (std::uint64_t tag = 0; tag < kOrders; ++tag) {
    const auto symbol = static_cast<SymbolId>(tag % kSymbols);
    if (tag % 250 == 0) {
      // Each symbol in turn, to and fro between the two shards: eighty moves
      // recorded, of which the first four find the symbol already there. The
      // last four leave every symbol on shard 1.
      const std::uint64_t number = tag / 250;
      Command move{};
      move.symbol = static_cast<SymbolId>(number % kSymbols);
      move.quantity = static_cast<Quantity>((number / kSymbols) % 2);
      move.type = CommandType::Move;
      commands.push_back(move);
      ++moves;
    }
    Command command{};
    command.client_tag = tag;
    command.symbol = symbol;
    command.price = 1'020 + 100 * static_cast<Price>(symbol);
    command.quantity = 1;
    // Each symbol's orders alternate, so every sell fills the buy before it.
    command.side = (tag / kSymbols) % 2 == 0 ? Side::Buy : Side::Sell;
    command.type = CommandType::Submit;
    commands.push_back(command);
  }

  for (const bool running : {true, false}) {
    ShardedEngine engine({.books = make_books(kSymbols),
                          .shards = 2,
                          .command_capacity = 4,
                          .event_capacity = 4,
                          .idle = IdleStrategy::Yield});
    if (running) {
      engine.start();
    }
    std::vector<std::uint64_t> next_tag = {0, 1, 2, 3};  // by symbol
    std::uint64_t answers = 0;
    std::uint64_t trades = 0;
    const ReplayResult result = lob::replay(
        commands, engine,
        [&](const Event& event) {
          if (event.type == EventType::Trade) {
            ++trades;
            return;
          }
          EXPECT_EQ(event.type, EventType::Accepted);
          EXPECT_EQ(event.client_tag, next_tag[event.symbol]);
          next_tag[event.symbol] += kSymbols;
          ++answers;
        },
        {.repeat_moves = true});
    engine.stop();

    EXPECT_EQ(result.commands, kOrders) << running;
    EXPECT_EQ(result.moves, moves) << running;
    EXPECT_EQ(result.skipped, 0u) << running;
    EXPECT_EQ(answers, kOrders) << running;
    EXPECT_EQ(trades, kOrders / 2) << running;
    EXPECT_EQ(result.events, kOrders + kOrders / 2) << running;
    EXPECT_EQ(engine.events_dropped(), 0u) << running;
    EXPECT_EQ(engine.commands_pending(), 0u) << running;
    for (std::size_t symbol = 0; symbol < kSymbols; ++symbol) {
      EXPECT_TRUE(engine.book(static_cast<SymbolId>(symbol)).empty()) << symbol;
      EXPECT_FALSE(engine.move_in_progress(static_cast<SymbolId>(symbol))) << symbol;
      EXPECT_EQ(engine.shard_of(static_cast<SymbolId>(symbol)), 1u) << symbol;
    }
  }
}

TEST_F(Replay, ARecordingWithNothingInItReplaysAsNothing) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(1, 0, 1));
  ASSERT_TRUE(recording->commands().empty());
  MatchingEngine again({.books = recording->books()});
  const ReplayResult result = lob::replay(recording->commands(), again, [](const Event&) {});
  EXPECT_EQ(result.commands, 0u);
  EXPECT_EQ(result.events, 0u);
}

// --- Replaying a sharded session -----------------------------------------------

class ShardedReplay : public ::testing::Test {
 protected:
  static constexpr std::size_t kSymbols = 8;
  static constexpr std::size_t kShards = 4;
  static constexpr std::size_t kFeed = std::size_t{1} << 16;

  // A random session on four running shards, with symbols being moved about
  // both by the trader and by automatic rebalancing.
  void run_live_session(int commands, std::uint32_t seed) {
    books = make_books(kSymbols);
    live = std::make_unique<Capture>(kSymbols);
    original = std::make_unique<ShardedEngine>(lob::ShardedConfig{
        .books = books,
        .shards = kShards,
        .rebalance = {.every = 500, .min_sample = 200},
        .idle = IdleStrategy::Yield,
        .market_data_capacity = kFeed,
        .record_to = file.path()});
    ASSERT_TRUE(original->recording());
    original->start();
    Trader<ShardedEngine> trader(*original, books, *live, seed);
    trader.favour_two_symbols(60);
    trader.trade(commands);
    trader.finish();
    original->stop();
    // Stopping is enough for the recording to be complete.
    const std::uint64_t recorded_at_stop = original->commands_recorded();
    ASSERT_EQ(original->events_dropped(), 0u);
    ASSERT_EQ(original->market_data_dropped(), 0u);
    moves_made = trader.moves_started() + original->rebalancing_moves();

    std::optional<Recording> loaded = Recording::load(file.path());
    ASSERT_TRUE(loaded.has_value());
    ASSERT_FALSE(loaded->damaged());
    recording = std::make_unique<Recording>(std::move(*loaded));
    ASSERT_EQ(recording->commands().size(), recorded_at_stop);

    // The session did have moves in it, of both kinds, and all are recorded.
    ASSERT_GT(trader.moves_started(), 20u);
    ASSERT_GT(original->rebalancing_moves(), 0u);
    ASSERT_EQ(count_of(recording->commands(), CommandType::Move), moves_made);
  }

  void expect_same_books(const auto& again) {
    for (std::size_t symbol = 0; symbol < kSymbols; ++symbol) {
      expect_same_book(original->book(static_cast<SymbolId>(symbol)),
                       again.book(static_cast<SymbolId>(symbol)), books[symbol]);
    }
  }

  TempFile file;
  std::vector<BookConfig> books;
  std::unique_ptr<Capture> live;
  std::unique_ptr<ShardedEngine> original;
  std::unique_ptr<Recording> recording;
  std::uint64_t moves_made = 0;
};

TEST_F(ShardedReplay, ReproducesTheSessionOnOneEngineRunByHand) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(20'000, 4242));

  MatchingEngine again({.books = recording->books(), .market_data_capacity = kFeed});
  Capture replayed(kSymbols);
  const ReplayResult result = replay_into(again, recording->commands(), replayed);

  EXPECT_EQ(result.moves, moves_made);
  EXPECT_EQ(result.commands + result.moves, recording->commands().size());
  EXPECT_EQ(result.events, live->in_order.size());
  ASSERT_EQ(again.market_data_dropped(), 0u);
  // Every symbol's events and market data, including the snapshot each move
  // put on the feed, and the books at the end.
  expect_same_session(*live, replayed);
  expect_same_books(again);
}

TEST_F(ShardedReplay, ReproducesTheSessionOnShardsRunByHand) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(10'000, 77));

  ShardedEngine again(
      {.books = recording->books(), .shards = 3, .market_data_capacity = kFeed});
  Capture replayed(kSymbols);
  const ReplayResult result = replay_into(again, recording->commands(), replayed);

  EXPECT_EQ(result.moves, moves_made);
  ASSERT_EQ(again.market_data_dropped(), 0u);
  expect_same_session(*live, replayed);
  expect_same_books(again);
  // The moves were replayed as snapshots: nothing actually moved.
  for (std::size_t symbol = 0; symbol < kSymbols; ++symbol) {
    EXPECT_EQ(again.shard_of(static_cast<SymbolId>(symbol)), symbol % 3);
  }
}

TEST_F(ShardedReplay, CanRepeatTheMovesThemselves) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(20'000, 808));

  // The same shape as the original, rebalancing off: the recorded moves stand
  // in for it. This replay is recorded in its turn.
  const TempFile second("_again");
  for (const bool running : {true, false}) {
    ShardedEngine again({.books = recording->books(),
                         .shards = kShards,
                         .idle = IdleStrategy::Yield,
                         .market_data_capacity = kFeed,
                         .record_to = second.path()});
    if (running) {
      again.start();
    }
    Capture replayed(kSymbols);
    const ReplayResult result =
        replay_into(again, recording->commands(), replayed, {.repeat_moves = true});
    again.stop();

    EXPECT_EQ(result.moves, moves_made);
    ASSERT_EQ(again.market_data_dropped(), 0u);
    expect_same_session(*live, replayed);
    expect_same_books(again);
    // Every symbol finished on the shard it finished on in the session...
    for (std::size_t symbol = 0; symbol < kSymbols; ++symbol) {
      const auto id = static_cast<SymbolId>(symbol);
      EXPECT_EQ(again.shard_of(id), original->shard_of(id)) << "symbol " << symbol;
      EXPECT_FALSE(again.move_in_progress(id));
    }
    // ...and the two recordings are the same file: the same commands and the
    // same moves at the same points.
    EXPECT_TRUE(read_file(file.path()) == read_file(second.path()));
  }
}

TEST_F(ShardedReplay, AMoveTheEngineCannotRepeatStillSendsItsSnapshot) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(10'000, 2));

  // Two shards where the session had four: some recorded moves name a shard
  // this engine has not got, and some name the one the symbol is already on.
  ShardedEngine again(
      {.books = recording->books(), .shards = 2, .market_data_capacity = kFeed});
  Capture replayed(kSymbols);
  const ReplayResult result =
      replay_into(again, recording->commands(), replayed, {.repeat_moves = true});

  EXPECT_EQ(result.moves, moves_made);
  ASSERT_EQ(again.market_data_dropped(), 0u);
  expect_same_session(*live, replayed);
  expect_same_books(again);
}

TEST_F(ShardedReplay, OnAnEngineWithFewerBooksLeavesOutTheRest) {
  ASSERT_NO_FATAL_FAILURE(run_live_session(10'000, 314));

  // Half the books. Everything recorded for the other half is skipped, the
  // moves included, and the half that is there comes out as it did.
  const std::vector<BookConfig> half(recording->books().begin(), recording->books().begin() + 4);
  ShardedEngine again({.books = half, .shards = 2, .market_data_capacity = kFeed});
  Capture replayed(4);
  const ReplayResult result =
      replay_into(again, recording->commands(), replayed, {.repeat_moves = true});

  const auto for_the_rest = static_cast<std::uint64_t>(
      std::count_if(recording->commands().begin(), recording->commands().end(),
                    [](const Command& command) { return command.symbol >= 4; }));
  ASSERT_GT(for_the_rest, 1'000u);
  EXPECT_EQ(result.skipped, for_the_rest);
  EXPECT_EQ(result.commands + result.moves + result.skipped, recording->commands().size());
  EXPECT_TRUE(replayed.events[4].empty());
  ASSERT_EQ(again.market_data_dropped(), 0u);
  for (std::size_t symbol = 0; symbol < 4; ++symbol) {
    const std::string what = "symbol " + std::to_string(symbol);
    expect_same_events(live->events[symbol], replayed.events[symbol], what);
    expect_same_feed(live->feed[symbol], replayed.feed[symbol], what);
    expect_same_book(original->book(static_cast<SymbolId>(symbol)),
                     again.book(static_cast<SymbolId>(symbol)), books[symbol]);
  }
}

TEST(ShardedReplayByHand, ASessionRecordedByHandReplaysTheSame) {
  // Everything on the calling thread: recording, moving and replaying, with
  // no engine thread anywhere.
  const TempFile file;
  const std::vector<BookConfig> books = make_books(6);
  Capture live(books.size());
  ShardedEngine original({.books = books,
                          .shards = 3,
                          .market_data_capacity = std::size_t{1} << 16,
                          .record_to = file.path()});
  Trader<ShardedEngine> trader(original, books, live, 606);
  trader.trade(10'000);
  trader.finish();
  original.flush_recording();
  ASSERT_GT(trader.moves_started(), 20u);

  const std::optional<Recording> recording = Recording::load(file.path());
  ASSERT_TRUE(recording.has_value());

  ShardedEngine again({.books = recording->books(),
                       .shards = 3,
                       .market_data_capacity = std::size_t{1} << 16});
  Capture replayed(books.size());
  const ReplayResult result =
      replay_into(again, recording->commands(), replayed, {.repeat_moves = true});
  EXPECT_EQ(result.moves, trader.moves_started());
  EXPECT_EQ(again.commands_pending(), 0u);
  expect_same_session(live, replayed);
  for (std::size_t symbol = 0; symbol < books.size(); ++symbol) {
    const auto id = static_cast<SymbolId>(symbol);
    expect_same_book(original.book(id), again.book(id), books[symbol]);
    EXPECT_EQ(again.shard_of(id), original.shard_of(id));
  }
}

// --- The digest ----------------------------------------------------------------

Event some_event(std::uint64_t tag, SymbolId symbol) {
  Event event{};
  event.client_tag = tag;
  event.order_id = 1'000 + tag;
  event.maker_id = 2'000 + tag;
  event.price = 150;
  event.quantity = 10;
  event.resting = 3;
  event.symbol = symbol;
  event.side = Side::Buy;
  event.type = EventType::Accepted;
  return event;
}

MarketData some_message(std::uint64_t sequence, SymbolId symbol) {
  MarketData message{};
  message.sequence = sequence;
  message.price = 150;
  message.quantity = 10;
  message.bid_price = 149;
  message.bid_quantity = 5;
  message.ask_price = 151;
  message.ask_quantity = 6;
  message.symbol = symbol;
  message.side = Side::Buy;
  message.type = lob::MarketDataType::Level;
  return message;
}

TEST(Digest, IsTheSameForTheSameSessionAndStartsOutEqual) {
  SessionDigest first(2);
  SessionDigest second(2);
  EXPECT_TRUE(first == second);
  EXPECT_EQ(first.value(), second.value());
  for (std::uint64_t tag = 0; tag < 10; ++tag) {
    first.add(some_event(tag, static_cast<SymbolId>(tag % 2)));
    second.add(some_event(tag, static_cast<SymbolId>(tag % 2)));
    first.add(some_message(tag + 1, 0));
    second.add(some_message(tag + 1, 0));
  }
  EXPECT_TRUE(first == second);
  EXPECT_EQ(first.value(), second.value());
  EXPECT_EQ(first.count(), 20u);
  EXPECT_NE(first.value(), SessionDigest(2).value());
}

TEST(Digest, ChangesWithAnyFieldOfAnEvent) {
  const Event base = some_event(1, 0);
  SessionDigest reference(2);
  reference.add(base);

  std::vector<Event> variants(9, base);
  variants[0].client_tag += 1;
  variants[1].order_id += 1;
  variants[2].maker_id += 1;
  variants[3].price += 1;
  variants[4].quantity += 1;
  variants[5].resting += 1;
  variants[6].symbol = 1;
  variants[7].side = Side::Sell;
  variants[8].type = EventType::Rejected;
  for (std::size_t index = 0; index < variants.size(); ++index) {
    SessionDigest digest(2);
    digest.add(variants[index]);
    EXPECT_NE(digest.value(), reference.value()) << "field " << index;
    EXPECT_FALSE(digest == reference) << "field " << index;
  }
}

TEST(Digest, ChangesWithAnyFieldOfAMarketDataMessage) {
  const MarketData base = some_message(1, 0);
  SessionDigest reference(2);
  reference.add(base);

  std::vector<MarketData> variants(10, base);
  variants[0].sequence += 1;
  variants[1].price += 1;
  variants[2].quantity += 1;
  variants[3].bid_price += 1;
  variants[4].bid_quantity += 1;
  variants[5].ask_price += 1;
  variants[6].ask_quantity += 1;
  variants[7].symbol = 1;
  variants[8].side = Side::Sell;
  variants[9].type = lob::MarketDataType::Trade;
  for (std::size_t index = 0; index < variants.size(); ++index) {
    SessionDigest digest(2);
    digest.add(variants[index]);
    EXPECT_NE(digest.value(), reference.value()) << "field " << index;
  }
}

TEST(Digest, TellsTheFieldsApart) {
  // The same two numbers in each other's places are a different event.
  const auto digest_of = [](const Event& event) {
    SessionDigest digest(1);
    digest.add(event);
    return digest.value();
  };
  Event event = some_event(1, 0);
  event.client_tag = 5;
  event.order_id = 7;
  event.maker_id = 9;
  Event swapped = event;
  swapped.client_tag = 7;
  swapped.order_id = 5;
  EXPECT_NE(digest_of(event), digest_of(swapped));
  swapped = event;
  swapped.order_id = 9;
  swapped.maker_id = 7;
  EXPECT_NE(digest_of(event), digest_of(swapped));
  swapped = event;
  swapped.quantity = event.resting;
  swapped.resting = event.quantity;
  EXPECT_NE(digest_of(event), digest_of(swapped));

  const auto digest_of_message = [](const MarketData& message) {
    SessionDigest digest(1);
    digest.add(message);
    return digest.value();
  };
  const MarketData message = some_message(1, 0);
  MarketData other = message;
  other.price = static_cast<Price>(message.quantity);
  other.quantity = static_cast<std::uint64_t>(message.price);
  EXPECT_NE(digest_of_message(message), digest_of_message(other));
  other = message;
  other.bid_price = message.ask_price;
  other.ask_price = message.bid_price;
  EXPECT_NE(digest_of_message(message), digest_of_message(other));
  other = message;
  other.bid_quantity = message.ask_quantity;
  other.ask_quantity = message.bid_quantity;
  EXPECT_NE(digest_of_message(message), digest_of_message(other));
}

TEST(Digest, OrderCountsWithinASymbolButNotBetweenSymbols) {
  const Event a0 = some_event(1, 0);
  const Event b0 = some_event(2, 0);
  const Event a1 = some_event(3, 1);
  const Event b1 = some_event(4, 1);

  SessionDigest one(2);
  for (const Event& event : {a0, b0, a1, b1}) {
    one.add(event);
  }
  // The two symbols interleaved differently, each still in its own order.
  SessionDigest interleaved(2);
  for (const Event& event : {a1, a0, b1, b0}) {
    interleaved.add(event);
  }
  EXPECT_TRUE(one == interleaved);
  EXPECT_EQ(one.value(), interleaved.value());

  // One symbol's events the other way round.
  SessionDigest swapped(2);
  for (const Event& event : {b0, a0, a1, b1}) {
    swapped.add(event);
  }
  EXPECT_FALSE(one == swapped);
  EXPECT_NE(one.value(), swapped.value());
}

TEST(Digest, MarketDataMayBeAddedInAnyOrderButNoneMayBeMissing) {
  SessionDigest in_order(1);
  SessionDigest shuffled(1);
  SessionDigest one_short(1);
  SessionDigest one_twice(1);
  for (std::uint64_t sequence = 1; sequence <= 20; ++sequence) {
    in_order.add(some_message(sequence, 0));
    if (sequence != 7) {
      one_short.add(some_message(sequence, 0));
    }
    one_twice.add(some_message(sequence == 7 ? 8 : sequence, 0));
  }
  for (std::uint64_t sequence = 20; sequence >= 1; --sequence) {
    shuffled.add(some_message(sequence, 0));
  }
  EXPECT_TRUE(in_order == shuffled);
  EXPECT_EQ(in_order.value(), shuffled.value());
  EXPECT_NE(in_order.value(), one_short.value());
  EXPECT_NE(in_order.value(), one_twice.value());
}

TEST(Digest, TakesEventsForSymbolsOutsideTheSession) {
  SessionDigest digest(1);
  const std::uint64_t empty = digest.value();
  digest.add(some_event(1, 500));
  digest.add(some_message(1, 500));
  EXPECT_NE(digest.value(), empty);
  EXPECT_EQ(digest.count(), 2u);
}

}  // namespace

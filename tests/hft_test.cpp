// HFT: the matching engine as a service. Everything here goes through the same
// messages a real client sends; most of it straight into a Core, and the last
// part over a real socket to a Server on another thread.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "hft/client.hpp"
#include "hft/core.hpp"
#include "hft/server.hpp"
#include "hft/simulator.hpp"
#include "hft/stats.hpp"
#include "hft/wire.hpp"

namespace {

using hft::Kind;
using lob::OrderType;
using lob::Side;

// --- Helpers -------------------------------------------------------------------

// A directory of the running test's own, removed with everything in it when
// the test ends.
class TempDir {
 public:
  TempDir() {
    const auto* const test = ::testing::UnitTest::GetInstance()->current_test_info();
    path_ = ::testing::TempDir() + "lob_hft_" + std::to_string(::getpid()) + "_" + test->name();
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TempDir() { std::filesystem::remove_all(path_); }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }

  // The files in it whose names end with `suffix`.
  [[nodiscard]] std::vector<std::string> files(const std::string& suffix) const {
    std::vector<std::string> found;
    for (const auto& entry : std::filesystem::directory_iterator(path_)) {
      const std::string name = entry.path().string();
      if (name.ends_with(suffix)) {
        found.push_back(name);
      }
    }
    std::sort(found.begin(), found.end());
    return found;
  }

 private:
  std::string path_;
};

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Reply {
  Kind kind;
  std::vector<std::byte> payload;

  template <typename T>
  [[nodiscard]] T as() const {
    return hft::read_as<T>(payload);
  }
  [[nodiscard]] std::string text() const { return std::string(hft::text_of(payload)); }
};

using Replies = std::vector<Reply>;

std::vector<Kind> kinds_of(const Replies& replies) {
  std::vector<Kind> kinds;
  for (const Reply& reply : replies) {
    kinds.push_back(reply.kind);
  }
  return kinds;
}

// Two books. AAA takes prices 1 to 1,000 and 64 resting orders; BBB takes
// 5,000 to 5,099 and 16.
const std::vector<hft::BookSpec> kBooks = {
    {"AAA", 1, 1'000, 64},
    {"BBB", 5'000, 100, 16},
};

using Levels = std::vector<hft::HouseLevel>;

// A Core, spoken to as a client would speak to it.
class Hft : public ::testing::Test {
 protected:
  // Sends what has been said so far and returns the replies.
  Replies send() {
    Replies replies;
    understood = link.exchange(out, [&](Kind kind, std::span<const std::byte> payload) {
      replies.push_back(Reply{kind, {payload.begin(), payload.end()}});
    });
    return replies;
  }

  Replies hello(std::uint32_t flags = 0) {
    hft::say_hello(out, kBooks, flags);
    return send();
  }

  Replies order(std::uint64_t tag, std::uint16_t symbol, Side side, std::int64_t price,
                std::uint32_t quantity, OrderType type = OrderType::Limit) {
    hft::say_order(out, tag, symbol, side, price, quantity, type);
    return send();
  }

  // Rests an order and returns its ID.
  std::uint64_t rest(std::uint64_t tag, std::uint16_t symbol, Side side, std::int64_t price,
                     std::uint32_t quantity) {
    const Replies replies = order(tag, symbol, side, price, quantity);
    EXPECT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies.back().kind, Kind::Accepted);
    const auto accepted = replies.back().as<hft::Accepted>();
    EXPECT_EQ(accepted.resting, quantity);
    return accepted.order_id;
  }

  Replies cancel(std::uint64_t tag, std::uint16_t symbol, std::uint64_t order_id) {
    hft::say_cancel(out, tag, symbol, order_id);
    return send();
  }

  Replies quote(std::uint16_t symbol, const Levels& bids, const Levels& asks) {
    hft::say_quote(out, symbol, bids, asks);
    return send();
  }

  // The top of a book as HFT reports it: bids then asks, each (price, quantity).
  struct Book {
    std::vector<std::pair<std::int64_t, std::uint64_t>> bids;
    std::vector<std::pair<std::int64_t, std::uint64_t>> asks;
  };

  Book depth(std::uint16_t symbol, std::uint16_t levels = 32) {
    hft::say_depth_request(out, symbol, levels);
    const Replies replies = send();
    Book book;
    EXPECT_EQ(replies.size(), 1u);
    if (replies.size() != 1 || replies[0].kind != Kind::Depth) {
      ADD_FAILURE() << "no Depth came back";
      return book;
    }
    std::span<const std::byte> payload = replies[0].payload;
    hft::Depth header{};
    EXPECT_TRUE(hft::take(payload, header));
    EXPECT_EQ(header.symbol, symbol);
    EXPECT_EQ(payload.size(),
              (std::size_t{header.bid_levels} + header.ask_levels) * sizeof(hft::DepthLevel));
    for (std::size_t index = 0; index < std::size_t{header.bid_levels} + header.ask_levels; ++index) {
      hft::DepthLevel level{};
      EXPECT_TRUE(hft::take(payload, level));
      (index < header.bid_levels ? book.bids : book.asks).emplace_back(level.price, level.quantity);
    }
    return book;
  }

  hft::Core core;
  hft::DirectLink link{core};
  hft::Outbox out;
  bool understood = true;
};

using Side2 = std::vector<std::pair<std::int64_t, std::uint64_t>>;

// --- Hello ---------------------------------------------------------------------

TEST_F(Hft, MustBeToldWhichBooksToRunBeforeAnythingElse) {
  const Replies replies = order(1, 0, Side::Buy, 100, 10);
  EXPECT_FALSE(understood);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].kind, Kind::Error);
  EXPECT_FALSE(replies[0].text().empty());
  EXPECT_FALSE(core.running());
}

TEST_F(Hft, AnswersHelloWithAWelcome) {
  const Replies replies = hello();
  EXPECT_TRUE(understood);
  ASSERT_EQ(replies.size(), 1u);
  ASSERT_EQ(replies[0].kind, Kind::Welcome);
  const auto welcome = replies[0].as<hft::Welcome>();
  EXPECT_EQ(welcome.version, hft::kProtocolVersion);
  EXPECT_EQ(welcome.book_count, 2u);
  EXPECT_EQ(welcome.session, 1u);
  EXPECT_EQ(welcome.resumed, 0);
  EXPECT_TRUE(core.running());
  EXPECT_EQ(core.symbol_count(), 2u);
  EXPECT_EQ(core.book(0).symbol(), "AAA");
  EXPECT_EQ(core.book(1).symbol(), "BBB");
}

TEST_F(Hft, PingAndStatisticsWorkBeforeHello) {
  hft::say_ping(out, 0xFEEDFACE12345678ULL);
  hft::say_stats_request(out);
  const Replies replies = send();
  EXPECT_TRUE(understood);
  ASSERT_EQ(kinds_of(replies), (std::vector<Kind>{Kind::Pong, Kind::Stats}));
  EXPECT_EQ(replies[0].as<hft::Pong>().value, 0xFEEDFACE12345678ULL);
  EXPECT_NE(replies[1].text().find("\"engine\":\"HFT\""), std::string::npos);
}

TEST_F(Hft, TurnsDownAHelloItCannotAct) {
  // The wrong version.
  hft::Hello wrong_version{};
  wrong_version.version = hft::kProtocolVersion + 1;
  wrong_version.book_count = 1;
  out.begin(Kind::Hello);
  out.put(wrong_version);
  out.put(hft::HelloBook{{'A'}, 1, 10, 10});
  out.end();
  EXPECT_EQ(kinds_of(send()), std::vector<Kind>{Kind::Error});
  EXPECT_FALSE(understood);

  // Fewer books than it says, more than it says, and none.
  for (const std::uint32_t claimed : {0u, 1u, 3u}) {
    hft::Hello hello{};
    hello.version = hft::kProtocolVersion;
    hello.book_count = claimed;
    out.begin(Kind::Hello);
    out.put(hello);
    out.put(hft::HelloBook{{'A'}, 1, 10, 10});
    out.put(hft::HelloBook{{'B'}, 1, 10, 10});
    out.end();
    EXPECT_EQ(kinds_of(send()), std::vector<Kind>{Kind::Error}) << claimed;
    EXPECT_FALSE(understood);
  }

  // Books that cannot be built: no name, no prices, too many prices, no room.
  for (const hft::HelloBook& bad :
       {hft::HelloBook{{}, 1, 10, 10}, hft::HelloBook{{'A'}, 1, 0, 10},
        hft::HelloBook{{'A'}, 1, 300'000, 10}, hft::HelloBook{{'A'}, 1, 10, 0}}) {
    hft::Hello hello{};
    hello.version = hft::kProtocolVersion;
    hello.book_count = 1;
    out.begin(Kind::Hello);
    out.put(hello);
    out.put(bad);
    out.end();
    EXPECT_EQ(kinds_of(send()), std::vector<Kind>{Kind::Error});
    EXPECT_FALSE(understood);
  }
  EXPECT_FALSE(core.running());

  // A message too short to be what it claims, and one of no known kind.
  hello();
  ASSERT_TRUE(understood);
  out.send(Kind::Order, hft::Ping{1});
  EXPECT_EQ(kinds_of(send()), std::vector<Kind>{Kind::Error});
  EXPECT_FALSE(understood);
  out.send(static_cast<Kind>(9'999), hft::Ping{1});
  EXPECT_EQ(kinds_of(send()), std::vector<Kind>{Kind::Error});
  EXPECT_FALSE(understood);
}

// --- Orders --------------------------------------------------------------------

TEST_F(Hft, AnOrderWithNothingToTradeWithRests) {
  hello();
  const Replies replies = order(7, 0, Side::Buy, 100, 50);
  ASSERT_EQ(kinds_of(replies), std::vector<Kind>{Kind::Accepted});
  const auto accepted = replies[0].as<hft::Accepted>();
  EXPECT_EQ(accepted.tag, 7u);
  EXPECT_NE(accepted.order_id, 0u);
  EXPECT_EQ(accepted.filled, 0u);
  EXPECT_EQ(accepted.resting, 50u);
  EXPECT_EQ(accepted.symbol, 0);
  EXPECT_EQ(depth(0).bids, (Side2{{100, 50}}));
  EXPECT_TRUE(depth(0).asks.empty());
  EXPECT_EQ(core.stats().resting_client_orders, 1u);
}

TEST_F(Hft, TwoClientOrdersTradeAndBothAreTold) {
  hello();
  const std::uint64_t resting = rest(1, 0, Side::Sell, 100, 30);

  const Replies replies = order(2, 0, Side::Buy, 105, 50);
  ASSERT_EQ(kinds_of(replies), (std::vector<Kind>{Kind::Fill, Kind::Fill, Kind::Accepted}));

  // The order that arrived.
  const auto taker = replies[0].as<hft::Fill>();
  EXPECT_EQ(taker.tag, 2u);
  EXPECT_EQ(taker.price, 100);  // the resting order's price, not its own limit
  EXPECT_EQ(taker.quantity, 30u);
  EXPECT_EQ(taker.remaining, 20u);
  EXPECT_EQ(taker.symbol, 0);
  EXPECT_EQ(taker.side, 0);
  EXPECT_EQ(taker.maker, 0);
  EXPECT_EQ(taker.with_house, 0);

  // The order that was resting.
  const auto maker = replies[1].as<hft::Fill>();
  EXPECT_EQ(maker.tag, 1u);
  EXPECT_EQ(maker.order_id, resting);
  EXPECT_EQ(maker.price, 100);
  EXPECT_EQ(maker.quantity, 30u);
  EXPECT_EQ(maker.remaining, 0u);
  EXPECT_EQ(maker.side, 1);
  EXPECT_EQ(maker.maker, 1);
  EXPECT_EQ(maker.with_house, 0);

  const auto accepted = replies[2].as<hft::Accepted>();
  EXPECT_EQ(accepted.tag, 2u);
  EXPECT_EQ(accepted.order_id, taker.order_id);
  EXPECT_EQ(accepted.filled, 30u);
  EXPECT_EQ(accepted.resting, 20u);

  EXPECT_EQ(depth(0).bids, (Side2{{105, 20}}));
  EXPECT_TRUE(depth(0).asks.empty());
  const hft::Stats& stats = core.stats();
  EXPECT_EQ(stats.trades, 1u);
  EXPECT_EQ(stats.trades_between_clients, 1u);
  EXPECT_EQ(stats.shares, 30u);
  EXPECT_EQ(stats.notional, 3'000);
  EXPECT_EQ(stats.fills, 2u);
  EXPECT_EQ(stats.resting_client_orders, 1u);  // the 20 left of the second order
}

TEST_F(Hft, ARestingOrderIsTradedAwayInPiecesAndKeepsCount) {
  hello();
  const std::uint64_t resting = rest(1, 0, Side::Buy, 100, 100);

  std::uint32_t left = 100;
  for (const std::uint32_t piece : {30u, 30u, 40u}) {
    const Replies replies = order(10 + piece, 0, Side::Sell, 100, piece, OrderType::IOC);
    ASSERT_EQ(kinds_of(replies), (std::vector<Kind>{Kind::Fill, Kind::Fill, Kind::Accepted}));
    left -= piece;
    const auto maker = replies[1].as<hft::Fill>();
    EXPECT_EQ(maker.tag, 1u);
    EXPECT_EQ(maker.order_id, resting);
    EXPECT_EQ(maker.quantity, piece);
    EXPECT_EQ(maker.remaining, left);
    EXPECT_EQ(replies[2].as<hft::Accepted>().resting, 0u);
  }
  EXPECT_TRUE(depth(0).bids.empty());
  EXPECT_EQ(core.stats().resting_client_orders, 0u);

  // It is gone: cancelling it now is refused.
  EXPECT_EQ(kinds_of(cancel(99, 0, resting)), std::vector<Kind>{Kind::CancelRejected});
}

TEST_F(Hft, AnOrderTradesThroughSeveralPricesBestFirstAndOldestFirst) {
  hello();
  rest(1, 0, Side::Sell, 102, 10);
  rest(2, 0, Side::Sell, 100, 10);  // the best price
  rest(3, 0, Side::Sell, 101, 10);
  rest(4, 0, Side::Sell, 100, 10);  // the same price, later

  const Replies replies = order(9, 0, Side::Buy, 0, 35, OrderType::Market);
  std::vector<std::pair<std::uint64_t, std::int64_t>> makers;  // tag, price
  for (const Reply& reply : replies) {
    if (reply.kind == Kind::Fill && reply.as<hft::Fill>().maker == 1) {
      makers.emplace_back(reply.as<hft::Fill>().tag, reply.as<hft::Fill>().price);
    }
  }
  EXPECT_EQ(makers, (std::vector<std::pair<std::uint64_t, std::int64_t>>{
                        {2, 100}, {4, 100}, {3, 101}, {1, 102}}));
  ASSERT_EQ(replies.back().kind, Kind::Accepted);
  EXPECT_EQ(replies.back().as<hft::Accepted>().filled, 35u);
  EXPECT_EQ(replies.back().as<hft::Accepted>().resting, 0u);  // a market order never rests
  EXPECT_EQ(depth(0).asks, (Side2{{102, 5}}));
}

TEST_F(Hft, AFillOrKillOrderTradesInFullOrNotAtAll) {
  hello();
  rest(1, 0, Side::Sell, 100, 10);
  Replies replies = order(2, 0, Side::Buy, 100, 11, OrderType::FOK);
  ASSERT_EQ(kinds_of(replies), std::vector<Kind>{Kind::Accepted});
  EXPECT_EQ(replies[0].as<hft::Accepted>().filled, 0u);
  EXPECT_EQ(depth(0).asks, (Side2{{100, 10}}));

  replies = order(3, 0, Side::Buy, 100, 10, OrderType::FOK);
  ASSERT_EQ(kinds_of(replies), (std::vector<Kind>{Kind::Fill, Kind::Fill, Kind::Accepted}));
  EXPECT_EQ(replies[2].as<hft::Accepted>().filled, 10u);
}

TEST_F(Hft, RefusesOrdersThatMakeNoSense) {
  hello();
  struct Bad {
    std::uint16_t symbol;
    std::uint8_t side;
    std::uint8_t type;
    std::int64_t price;
    std::uint32_t quantity;
  };
  std::uint64_t tag = 100;
  for (const Bad& bad : {Bad{2, 0, 0, 100, 10},     // no such symbol
                         Bad{0, 2, 0, 100, 10},     // not a side
                         Bad{0, 0, 4, 100, 10},     // not a type of order
                         Bad{0, 0, 0, 0, 10},       // a price below the book
                         Bad{0, 0, 0, 1'001, 10},   // a price above the book
                         Bad{1, 0, 0, 100, 10},     // a good price for the other symbol
                         Bad{0, 0, 0, 100, 0}}) {   // nothing to trade
    hft::Order order{};
    order.tag = ++tag;
    order.symbol = bad.symbol;
    order.side = bad.side;
    order.type = bad.type;
    order.price = bad.price;
    order.quantity = bad.quantity;
    out.send(Kind::Order, order);
    const Replies replies = send();
    EXPECT_TRUE(understood);
    ASSERT_EQ(kinds_of(replies), std::vector<Kind>{Kind::Rejected}) << "tag " << tag;
    EXPECT_EQ(replies[0].as<hft::Rejected>().tag, tag);
    EXPECT_EQ(replies[0].as<hft::Rejected>().symbol, bad.symbol);
  }
  EXPECT_EQ(core.stats().rejected, 7u);
  EXPECT_EQ(core.stats().orders, 7u);
  EXPECT_EQ(core.stats().resting_client_orders, 0u);
}

TEST_F(Hft, ABookThatIsFullRefusesWhatWouldRest) {
  hello();
  for (std::uint64_t tag = 1; tag <= 16; ++tag) {  // BBB has room for 16
    rest(tag, 1, Side::Buy, 5'000 + static_cast<std::int64_t>(tag), 1);
  }
  EXPECT_EQ(kinds_of(order(17, 1, Side::Buy, 5'050, 1)), std::vector<Kind>{Kind::Rejected});
  // One that only trades is still welcome.
  const Replies replies = order(18, 1, Side::Sell, 5'016, 1, OrderType::IOC);
  EXPECT_EQ(kinds_of(replies), (std::vector<Kind>{Kind::Fill, Kind::Fill, Kind::Accepted}));
  // And that made room.
  EXPECT_EQ(kinds_of(order(19, 1, Side::Buy, 5'050, 1)), std::vector<Kind>{Kind::Accepted});
}

// --- Cancels -------------------------------------------------------------------

TEST_F(Hft, CancelsAnOrderOnceAndOnlyItsOwn) {
  hello();
  const std::uint64_t id = rest(1, 0, Side::Buy, 100, 50);
  const std::uint64_t other = rest(2, 0, Side::Buy, 99, 50);

  Replies replies = cancel(3, 0, id);
  ASSERT_EQ(kinds_of(replies), std::vector<Kind>{Kind::Cancelled});
  EXPECT_EQ(replies[0].as<hft::CancelAnswer>().tag, 3u);
  EXPECT_EQ(replies[0].as<hft::CancelAnswer>().order_id, id);
  EXPECT_EQ(replies[0].as<hft::CancelAnswer>().symbol, 0);
  EXPECT_EQ(depth(0).bids, (Side2{{99, 50}}));

  // Again, an ID that never was, the right ID in the wrong symbol, no such symbol.
  for (const auto& [symbol, order_id] :
       std::vector<std::pair<std::uint16_t, std::uint64_t>>{
           {0, id}, {0, 0}, {0, 0xDEADBEEF}, {1, other}, {7, other}}) {
    replies = cancel(4, symbol, order_id);
    EXPECT_TRUE(understood);
    ASSERT_EQ(kinds_of(replies), std::vector<Kind>{Kind::CancelRejected});
    EXPECT_EQ(replies[0].as<hft::CancelAnswer>().tag, 4u);
    EXPECT_EQ(replies[0].as<hft::CancelAnswer>().order_id, order_id);
  }
  EXPECT_EQ(depth(0).bids, (Side2{{99, 50}}));
  EXPECT_EQ(core.stats().cancelled, 1u);
  EXPECT_EQ(core.stats().cancel_rejected, 5u);
  EXPECT_EQ(core.stats().resting_client_orders, 1u);
}

// --- The house -----------------------------------------------------------------

TEST_F(Hft, TheHouseRestsWhatAQuoteAsksFor) {
  hello();
  EXPECT_TRUE(quote(0, {{100, 500}, {99, 700}, {98, 900}}, {{102, 400}, {103, 600}}).empty());
  EXPECT_EQ(depth(0).bids, (Side2{{100, 500}, {99, 700}, {98, 900}}));
  EXPECT_EQ(depth(0).asks, (Side2{{102, 400}, {103, 600}}));
  EXPECT_EQ(core.stats().resting_house_orders, 5u);
  EXPECT_EQ(core.stats().house_orders, 5u);
  EXPECT_EQ(core.stats().quotes, 1u);

  // The other symbol is untouched.
  EXPECT_TRUE(depth(1).bids.empty());
  EXPECT_TRUE(depth(1).asks.empty());
}

TEST_F(Hft, TheSameQuoteAgainChangesNothing) {
  hello();
  const Levels bids = {{100, 500}, {99, 700}};
  const Levels asks = {{102, 400}, {103, 600}};
  quote(0, bids, asks);
  const std::uint64_t commands = core.stats().engine_commands;
  for (int again = 0; again < 5; ++again) {
    quote(0, bids, asks);
  }
  EXPECT_EQ(core.stats().engine_commands, commands);  // nothing was sent to the engine
  EXPECT_EQ(core.stats().quotes, 6u);
  EXPECT_EQ(depth(0).bids, (Side2{{100, 500}, {99, 700}}));
}

TEST_F(Hft, AQuoteThatMovesTakesAwayOnlyWhatChanged) {
  hello();
  quote(0, {{100, 500}, {99, 700}, {98, 900}}, {{102, 400}, {103, 600}, {104, 800}});
  const std::uint64_t commands = core.stats().engine_commands;

  // The market moves up a tick. Prices that are in both ladders keep their
  // orders: 99 and 100 on the bid, 103 and 104 on the offer.
  quote(0, {{101, 300}, {100, 500}, {99, 700}}, {{103, 600}, {104, 800}, {105, 200}});
  EXPECT_EQ(depth(0).bids, (Side2{{101, 300}, {100, 500}, {99, 700}}));
  EXPECT_EQ(depth(0).asks, (Side2{{103, 600}, {104, 800}, {105, 200}}));
  EXPECT_EQ(core.stats().engine_commands - commands, 4u);  // two cancelled, two placed
  EXPECT_EQ(core.stats().resting_house_orders, 6u);
}

TEST_F(Hft, TheHouseNeverTradesWithItselfHoweverFarTheQuoteJumps) {
  hello();
  quote(0, {{100, 500}, {99, 700}}, {{102, 400}, {103, 600}});

  // Up through its own old offers, down through its own old bids, and back:
  // each time the new prices on one side sit where the other side just was.
  quote(0, {{103, 500}, {102, 700}}, {{105, 400}, {106, 600}});
  EXPECT_EQ(depth(0).bids, (Side2{{103, 500}, {102, 700}}));
  EXPECT_EQ(depth(0).asks, (Side2{{105, 400}, {106, 600}}));
  quote(0, {{97, 500}, {96, 700}}, {{99, 400}, {103, 600}});
  EXPECT_EQ(depth(0).bids, (Side2{{97, 500}, {96, 700}}));
  EXPECT_EQ(depth(0).asks, (Side2{{99, 400}, {103, 600}}));
  quote(0, {{100, 500}, {99, 700}}, {{102, 400}, {103, 600}});
  EXPECT_EQ(depth(0).bids, (Side2{{100, 500}, {99, 700}}));
  EXPECT_EQ(depth(0).asks, (Side2{{102, 400}, {103, 600}}));

  EXPECT_EQ(core.stats().trades, 0u);
  EXPECT_EQ(core.stats().resting_house_orders, 4u);
  EXPECT_EQ(core.stats().house_rejected, 0u);
}

TEST_F(Hft, AQuoteWithNewSizesTopsUpOrStartsThePriceAgain) {
  hello();
  quote(0, {{100, 500}}, {{102, 400}});

  // More wanted: an order for the difference joins the one already there.
  std::uint64_t commands = core.stats().engine_commands;
  quote(0, {{100, 800}}, {{102, 400}});
  EXPECT_EQ(depth(0).bids, (Side2{{100, 800}}));
  EXPECT_EQ(core.stats().engine_commands - commands, 1u);
  EXPECT_EQ(core.stats().resting_house_orders, 3u);

  // Less wanted: both orders at that price go, and one of the new size comes.
  commands = core.stats().engine_commands;
  quote(0, {{100, 200}}, {{102, 400}});
  EXPECT_EQ(depth(0).bids, (Side2{{100, 200}}));
  EXPECT_EQ(core.stats().engine_commands - commands, 3u);
  EXPECT_EQ(core.stats().resting_house_orders, 2u);

  // A level asked for with nothing in it is a level not asked for.
  quote(0, {{100, 0}}, {{102, 400}});
  EXPECT_TRUE(depth(0).bids.empty());
  EXPECT_EQ(depth(0).asks, (Side2{{102, 400}}));

  // No levels at all on a side: the house quotes nothing there.
  quote(0, {}, {});
  EXPECT_TRUE(depth(0).asks.empty());
  EXPECT_EQ(core.stats().resting_house_orders, 0u);
}

TEST_F(Hft, IgnoresAQuoteThatMakesNoSense) {
  hello();
  quote(0, {{100, 500}}, {{102, 400}});
  const std::uint64_t commands = core.stats().engine_commands;

  quote(0, {{102, 500}}, {{102, 400}});                 // the bid at the offer
  quote(0, {{105, 500}}, {{102, 400}});                 // the bid above the offer
  quote(0, {{100, 500}, {100, 300}}, {{102, 400}});     // one price twice
  quote(0, {{99, 500}, {100, 300}}, {{102, 400}});      // bids not best first
  quote(0, {{100, 500}}, {{103, 400}, {102, 400}});     // offers not best first
  quote(5, {{100, 500}}, {{102, 400}});                 // no such symbol
  EXPECT_TRUE(understood);
  EXPECT_EQ(core.stats().quotes_refused, 6u);
  EXPECT_EQ(core.stats().engine_commands, commands);
  EXPECT_EQ(depth(0).bids, (Side2{{100, 500}}));
  EXPECT_EQ(depth(0).asks, (Side2{{102, 400}}));

  // One that is the wrong size for what it says it holds is not a Quote at all.
  hft::Quote header{};
  header.symbol = 0;
  header.bid_levels = 2;
  header.ask_levels = 1;
  out.begin(Kind::Quote);
  out.put(header);
  out.put(hft::QuoteLevel{100, 500, 0});
  out.end();
  EXPECT_EQ(kinds_of(send()), std::vector<Kind>{Kind::Error});
  EXPECT_FALSE(understood);
}

TEST_F(Hft, AClientOrderTakesTheHousesLiquidityLevelByLevel) {
  hello();
  quote(0, {{100, 500}, {99, 700}}, {{102, 400}, {103, 600}, {104, 800}});

  const Replies replies = order(1, 0, Side::Buy, 103, 700);
  ASSERT_EQ(kinds_of(replies), (std::vector<Kind>{Kind::Fill, Kind::Fill, Kind::Accepted}));
  const auto first = replies[0].as<hft::Fill>();
  const auto second = replies[1].as<hft::Fill>();
  EXPECT_EQ(first.price, 102);
  EXPECT_EQ(first.quantity, 400u);
  EXPECT_EQ(first.remaining, 300u);
  EXPECT_EQ(first.maker, 0);
  EXPECT_EQ(first.with_house, 1);
  EXPECT_EQ(second.price, 103);
  EXPECT_EQ(second.quantity, 300u);
  EXPECT_EQ(second.remaining, 0u);
  EXPECT_EQ(second.with_house, 1);
  EXPECT_EQ(replies[2].as<hft::Accepted>().filled, 700u);
  EXPECT_EQ(replies[2].as<hft::Accepted>().resting, 0u);

  // What was taken is gone from the book until the house is asked to put it back.
  EXPECT_EQ(depth(0).asks, (Side2{{103, 300}, {104, 800}}));
  EXPECT_EQ(core.stats().trades, 2u);
  EXPECT_EQ(core.stats().trades_between_clients, 0u);
  EXPECT_EQ(core.stats().fills, 2u);  // the house is not sent messages
  EXPECT_EQ(core.stats().resting_house_orders, 4u);

  quote(0, {{100, 500}, {99, 700}}, {{102, 400}, {103, 600}, {104, 800}});
  EXPECT_EQ(depth(0).asks, (Side2{{102, 400}, {103, 600}, {104, 800}}));
}

TEST_F(Hft, WhenTheMarketComesToARestingOrderItFillsAtItsOwnPrice) {
  hello();
  quote(0, {{100, 500}}, {{104, 400}});
  const std::uint64_t id = rest(1, 0, Side::Buy, 102, 500);  // inside the spread
  EXPECT_EQ(depth(0).bids, (Side2{{102, 500}, {100, 500}}));

  // The market falls: the house now offers at 101, below the resting bid.
  const Replies replies = quote(0, {{99, 500}}, {{101, 300}, {102, 300}});
  ASSERT_EQ(kinds_of(replies), (std::vector<Kind>{Kind::Fill, Kind::Fill}));
  std::uint32_t left = 500;
  for (const Reply& reply : replies) {
    const auto fill = reply.as<hft::Fill>();
    left -= fill.quantity;
    EXPECT_EQ(fill.tag, 1u);
    EXPECT_EQ(fill.order_id, id);
    EXPECT_EQ(fill.price, 102);  // the resting order's own price, both times
    EXPECT_EQ(fill.side, 0);
    EXPECT_EQ(fill.maker, 1);
    EXPECT_EQ(fill.with_house, 1);
    EXPECT_EQ(fill.remaining, left);
  }
  EXPECT_EQ(replies[0].as<hft::Fill>().quantity, 300u);  // all the house offered at 101
  EXPECT_EQ(replies[1].as<hft::Fill>().quantity, 200u);  // and some of its 102s: 100 are left

  EXPECT_TRUE(depth(0).asks == (Side2{{102, 100}}));
  EXPECT_EQ(depth(0).bids, (Side2{{99, 500}}));
  EXPECT_EQ(core.stats().resting_client_orders, 0u);
  EXPECT_EQ(core.stats().trades, 2u);
}

TEST_F(Hft, AClientCannotCancelTheHousesOrders) {
  hello();
  quote(0, {{100, 500}}, {{102, 400}});
  // Order IDs are the same kind of number for everyone, so a client could hit
  // on the house's. Find them the way a nosy client might: try what its own
  // next order's ID would be near.
  const std::uint64_t mine = rest(1, 0, Side::Buy, 90, 10);
  int refused = 0;
  for (std::uint64_t sequence = 0; sequence < 8; ++sequence) {
    for (std::uint64_t slot = 0; slot < 8; ++slot) {
      const std::uint64_t guess = (sequence << 32) | slot;
      if (guess == mine) {
        continue;
      }
      const Replies replies = cancel(50, 0, guess);
      ASSERT_EQ(kinds_of(replies), std::vector<Kind>{Kind::CancelRejected});
      ++refused;
    }
  }
  EXPECT_EQ(refused, 63);
  EXPECT_EQ(depth(0).bids, (Side2{{100, 500}, {90, 10}}));
  EXPECT_EQ(depth(0).asks, (Side2{{102, 400}}));
  EXPECT_EQ(core.stats().resting_house_orders, 2u);
}

// --- Depth ---------------------------------------------------------------------

TEST_F(Hft, DepthAddsTheHouseAndTheClientsTogetherAndStopsWhereAsked) {
  hello();
  quote(0, {{100, 500}, {99, 700}, {98, 900}}, {{102, 400}, {103, 600}, {104, 800}});
  rest(1, 0, Side::Buy, 100, 50);
  rest(2, 0, Side::Sell, 103, 25);
  rest(3, 0, Side::Sell, 110, 5);

  EXPECT_EQ(depth(0).bids, (Side2{{100, 550}, {99, 700}, {98, 900}}));
  EXPECT_EQ(depth(0).asks, (Side2{{102, 400}, {103, 625}, {104, 800}, {110, 5}}));
  EXPECT_EQ(depth(0, 2).bids, (Side2{{100, 550}, {99, 700}}));
  EXPECT_EQ(depth(0, 2).asks, (Side2{{102, 400}, {103, 625}}));
  EXPECT_TRUE(depth(0, 0).bids.empty());

  // A symbol that does not exist has an empty book, not an error.
  EXPECT_TRUE(depth(9).bids.empty());
  EXPECT_TRUE(understood);
}

// --- Sessions ------------------------------------------------------------------

TEST_F(Hft, AClientThatComesBackFindsTheBooksAsItLeftThem) {
  hello();
  quote(0, {{100, 500}}, {{102, 400}});
  const std::uint64_t id = rest(1, 0, Side::Buy, 101, 10);

  Replies replies = hello();
  ASSERT_EQ(kinds_of(replies), std::vector<Kind>{Kind::Welcome});
  EXPECT_EQ(replies[0].as<hft::Welcome>().resumed, 1);
  EXPECT_EQ(replies[0].as<hft::Welcome>().session, 1u);
  EXPECT_EQ(depth(0).bids, (Side2{{101, 10}, {100, 500}}));
  EXPECT_EQ(kinds_of(cancel(2, 0, id)), std::vector<Kind>{Kind::Cancelled});

  // Asking for a fresh start empties them and begins a new session.
  replies = hello(hft::kHelloFresh);
  ASSERT_EQ(kinds_of(replies), std::vector<Kind>{Kind::Welcome});
  EXPECT_EQ(replies[0].as<hft::Welcome>().resumed, 0);
  EXPECT_EQ(replies[0].as<hft::Welcome>().session, 2u);
  EXPECT_TRUE(depth(0).bids.empty());
  EXPECT_TRUE(depth(0).asks.empty());
  EXPECT_EQ(core.stats().orders, 0u);
  EXPECT_EQ(core.stats().resting_house_orders, 0u);

  // So does asking for different books.
  const std::vector<hft::BookSpec> other = {{"ZZZ", 1, 50, 8}};
  hft::say_hello(out, other);
  replies = send();
  ASSERT_EQ(kinds_of(replies), std::vector<Kind>{Kind::Welcome});
  EXPECT_EQ(replies[0].as<hft::Welcome>().resumed, 0);
  EXPECT_EQ(replies[0].as<hft::Welcome>().session, 3u);
  EXPECT_EQ(replies[0].as<hft::Welcome>().book_count, 1u);
  EXPECT_EQ(core.book(0).symbol(), "ZZZ");
}

TEST_F(Hft, ShutdownIsNotedAndLeftToTheServer) {
  hello();
  EXPECT_FALSE(core.shutdown_requested());
  hft::say_shutdown(out);
  EXPECT_TRUE(send().empty());
  EXPECT_TRUE(core.shutdown_requested());
}

// --- A client keeping its own books ------------------------------------------------

// Random trading with a client that believes only what HFT tells it: every
// Accepted, Fill and Cancelled updates its own idea of what it has resting.
// At the end it cancels everything it thinks it has. If HFT ever told it
// something untrue, or failed to tell it something, a cancel is refused or an
// order is left behind in a book.
TEST_F(Hft, WhatAClientIsToldAlwaysAddsUpToWhatIsInTheBooks) {
  hello();
  std::mt19937 random(2026);
  const auto pick = [&](int low, int high) {
    return std::uniform_int_distribution<int>(low, high)(random);
  };

  struct Mine {
    std::uint16_t symbol;
    std::uint32_t remaining;
  };
  std::map<std::uint64_t, Mine> tags;                 // every order sent, by tag
  std::map<std::pair<std::uint16_t, std::uint64_t>, std::uint64_t> resting;  // (symbol, id) -> tag
  std::uint64_t next_tag = 0;
  std::int64_t mid[2] = {500, 5'050};

  const auto digest = [&](const Replies& replies) {
    for (const Reply& reply : replies) {
      if (reply.kind == Kind::Fill) {
        const auto fill = reply.as<hft::Fill>();
        ASSERT_TRUE(tags.contains(fill.tag));
        Mine& mine = tags[fill.tag];
        ASSERT_GE(mine.remaining, fill.quantity);
        mine.remaining -= fill.quantity;
        ASSERT_EQ(fill.remaining, mine.remaining) << "tag " << fill.tag;
        if (fill.maker == 1) {
          ASSERT_TRUE(resting.contains({fill.symbol, fill.order_id}));
          if (mine.remaining == 0) {
            resting.erase({fill.symbol, fill.order_id});
          }
        }
      } else if (reply.kind == Kind::Accepted) {
        const auto accepted = reply.as<hft::Accepted>();
        Mine& mine = tags[accepted.tag];
        if (accepted.resting != 0) {
          ASSERT_EQ(accepted.resting, mine.remaining);
          resting[{accepted.symbol, accepted.order_id}] = accepted.tag;
        } else {
          mine.remaining = 0;  // whatever did not trade was discarded
        }
      } else if (reply.kind == Kind::Cancelled) {
        const auto answer = reply.as<hft::CancelAnswer>();
        ASSERT_EQ(resting.erase({answer.symbol, answer.order_id}), 1u);
      } else if (reply.kind == Kind::Rejected) {
        tags[reply.as<hft::Rejected>().tag].remaining = 0;
      } else {
        ASSERT_EQ(reply.kind, Kind::CancelRejected);
        ADD_FAILURE() << "a cancel for an order the client believed was resting was refused";
      }
    }
  };

  for (int step = 0; step < 30'000; ++step) {
    const auto symbol = static_cast<std::uint16_t>(pick(0, 1));
    const int roll = pick(0, 99);
    if (roll < 25) {
      mid[symbol] += pick(-1, 1);
      mid[0] = std::clamp<std::int64_t>(mid[0], 100, 900);
      mid[1] = std::clamp<std::int64_t>(mid[1], 5'020, 5'080);
      Levels bids;
      Levels asks;
      for (int level = 0; level < 5; ++level) {
        bids.push_back({mid[symbol] - 1 - level, static_cast<std::uint32_t>(pick(1, 5) * 10)});
        asks.push_back({mid[symbol] + 1 + level, static_cast<std::uint32_t>(pick(1, 5) * 10)});
      }
      ASSERT_NO_FATAL_FAILURE(digest(quote(symbol, bids, asks)));
    } else if (roll < 80) {
      const Side side = pick(0, 1) == 0 ? Side::Buy : Side::Sell;
      const std::int64_t price = mid[symbol] + pick(-4, 4);
      const auto quantity = static_cast<std::uint32_t>(pick(1, 60));
      const auto type = static_cast<OrderType>(pick(0, 9) < 7 ? 0 : pick(1, 3));
      tags[++next_tag] = Mine{symbol, quantity};
      ASSERT_NO_FATAL_FAILURE(digest(order(next_tag, symbol, side, price, quantity, type)));
    } else if (!resting.empty()) {
      auto it = resting.begin();
      std::advance(it, pick(0, static_cast<int>(resting.size()) - 1));
      const auto [key, tag] = *it;
      ASSERT_NO_FATAL_FAILURE(digest(cancel(++next_tag, key.first, key.second)));
    }
    ASSERT_TRUE(understood);

    // What HFT counts as resting is what the books hold, at every step.
    if (step % 500 == 0) {
      const hft::Stats& stats = core.stats();
      ASSERT_EQ(stats.resting_client_orders + stats.resting_house_orders,
                core.book(0).size() + core.book(1).size());
      ASSERT_EQ(stats.resting_client_orders, resting.size());
    }
  }

  ASSERT_GT(core.stats().trades, 5'000u);
  ASSERT_GT(core.stats().trades_between_clients, 500u);
  ASSERT_GT(core.stats().cancelled, 1'000u);

  // Take the house away, then cancel everything the client believes it has.
  quote(0, {}, {});
  quote(1, {}, {});
  EXPECT_EQ(core.stats().resting_house_orders, 0u);
  while (!resting.empty()) {
    const auto [key, tag] = *resting.begin();
    ASSERT_NO_FATAL_FAILURE(digest(cancel(++next_tag, key.first, key.second)));
  }
  EXPECT_TRUE(core.book(0).empty());
  EXPECT_TRUE(core.book(1).empty());
  EXPECT_EQ(core.stats().resting_client_orders, 0u);
  EXPECT_EQ(core.stats().events_dropped, 0u);
}

// --- Statistics ----------------------------------------------------------------

TEST(HftHistogram, FindsPercentilesToWithinAnEighth) {
  hft::Histogram histogram;
  EXPECT_EQ(histogram.count(), 0u);
  EXPECT_EQ(histogram.percentile(0.5), 0u);
  EXPECT_EQ(histogram.max(), 0u);
  EXPECT_EQ(histogram.min(), 0u);

  for (std::uint64_t value = 1; value <= 10'000; ++value) {
    histogram.record(value);
  }
  EXPECT_EQ(histogram.count(), 10'000u);
  EXPECT_EQ(histogram.min(), 1u);
  EXPECT_EQ(histogram.max(), 10'000u);
  EXPECT_EQ(histogram.mean(), 5'000u);
  for (const double fraction : {0.1, 0.5, 0.9, 0.99, 0.999}) {
    const double exact = fraction * 10'000;
    const auto reported = static_cast<double>(histogram.percentile(fraction));
    EXPECT_GE(reported, exact) << fraction;  // never flatters
    EXPECT_LE(reported, exact * 1.126) << fraction;
  }
  EXPECT_EQ(histogram.percentile(1.0), 10'000u);
  EXPECT_EQ(histogram.percentile(0.0), 1u);
}

TEST(HftHistogram, KeepsSmallValuesExactAndSurvivesHugeOnes) {
  hft::Histogram small;
  for (std::uint64_t value = 0; value < 8; ++value) {
    small.record(value);
  }
  for (std::uint64_t value = 0; value < 8; ++value) {
    EXPECT_EQ(small.percentile((static_cast<double>(value) + 1) / 8), value);
  }

  hft::Histogram huge;
  huge.record(5);
  huge.record(~std::uint64_t{0});  // far beyond the last bucket
  EXPECT_EQ(huge.count(), 2u);
  EXPECT_EQ(huge.max(), ~std::uint64_t{0});
  EXPECT_EQ(huge.percentile(0.5), 5u);
  EXPECT_EQ(huge.percentile(1.0), ~std::uint64_t{0});

  // Every power of two and its neighbours land in a bucket whose top is not below them.
  for (int bit = 3; bit < 40; ++bit) {
    for (const std::uint64_t value : {(std::uint64_t{1} << bit) - 1, std::uint64_t{1} << bit,
                                      (std::uint64_t{1} << bit) + 1}) {
      hft::Histogram one;
      one.record(value);
      EXPECT_EQ(one.percentile(0.5), value);  // capped at the largest seen
    }
  }
}

TEST_F(Hft, CountsWhatItDidAndHowLongItTook) {
  hello();
  quote(0, {{100, 500}}, {{102, 400}});
  rest(1, 0, Side::Sell, 101, 30);
  order(2, 0, Side::Buy, 102, 100);            // 30 from the client order, 70 from the house
  order(3, 0, Side::Buy, 2'000, 10);           // refused
  const std::uint64_t id = rest(4, 1, Side::Buy, 5'010, 5);
  cancel(5, 1, id);
  cancel(6, 1, id);                            // refused
  depth(0);

  const hft::Stats& stats = core.stats();
  EXPECT_EQ(stats.session, 1u);
  EXPECT_EQ(stats.orders, 4u);
  EXPECT_EQ(stats.cancels, 2u);
  EXPECT_EQ(stats.quotes, 1u);
  EXPECT_EQ(stats.depth_requests, 1u);
  EXPECT_EQ(stats.accepted, 3u);
  EXPECT_EQ(stats.rejected, 1u);
  EXPECT_EQ(stats.cancelled, 1u);
  EXPECT_EQ(stats.cancel_rejected, 1u);
  EXPECT_EQ(stats.trades, 2u);
  EXPECT_EQ(stats.trades_between_clients, 1u);
  EXPECT_EQ(stats.shares, 100u);
  EXPECT_EQ(stats.notional, 30 * 101 + 70 * 102);
  EXPECT_EQ(stats.fills, 3u);  // the taker twice, the client maker once
  EXPECT_EQ(stats.house_orders, 2u);
  EXPECT_EQ(stats.events_dropped, 0u);
  EXPECT_EQ(stats.order_time.count(), 6u);  // four orders and two cancels
  EXPECT_EQ(stats.quote_time.count(), 1u);
  EXPECT_GT(stats.order_time.max(), 0u);
  EXPECT_EQ(stats.busiest_second, 7u);
  EXPECT_NE(stats.digest, 0u);
  EXPECT_FALSE(stats.recording);  // no data directory was given

  ASSERT_EQ(stats.symbols.size(), 2u);
  EXPECT_EQ(stats.symbols[0].name, "AAA");
  EXPECT_EQ(stats.symbols[0].orders, 3u);
  EXPECT_EQ(stats.symbols[0].trades, 2u);
  EXPECT_EQ(stats.symbols[0].shares, 100u);
  EXPECT_EQ(stats.symbols[0].last_price, 102);
  EXPECT_EQ(stats.symbols[0].quotes, 1u);
  EXPECT_EQ(stats.symbols[1].orders, 1u);
  EXPECT_EQ(stats.symbols[1].trades, 0u);

  // The same numbers, as the client is sent them and as a person reads them.
  hft::say_stats_request(out);
  const Replies replies = send();
  ASSERT_EQ(kinds_of(replies), std::vector<Kind>{Kind::Stats});
  const std::string json = replies[0].text();
  for (const char* expected :
       {"\"engine\":\"HFT\"", "\"session\":1", "\"orders\":4", "\"cancels\":2", "\"trades\":2",
        "\"trades_between_clients\":1", "\"shares\":100", "\"rejected\":1", "\"events_dropped\":0",
        "\"order_time\":{\"count\":6,", "\"p99_ns\":", "\"recording\":false",
        "{\"symbol\":\"AAA\",\"orders\":3,\"quotes\":1,\"trades\":2,\"shares\":100,\"last_price\":102}"}) {
    EXPECT_NE(json.find(expected), std::string::npos) << expected << "\n" << json;
  }
  EXPECT_EQ(json.front(), '{');
  EXPECT_EQ(json.back(), '}');
  EXPECT_EQ(std::count(json.begin(), json.end(), '{'), std::count(json.begin(), json.end(), '}'));
  EXPECT_EQ(std::count(json.begin(), json.end(), '['), std::count(json.begin(), json.end(), ']'));

  const std::string text = hft::to_text(stats);
  for (const char* expected : {"HFT session 1", "orders", "trades", "typical (half were faster)",
                               "events lost", "AAA", "value traded                      101.70"}) {
    EXPECT_NE(text.find(expected), std::string::npos) << expected << "\n" << text;
  }
}

TEST(HftStats, TheBusiestSecondIsCountedSecondBySecond) {
  hft::Stats stats;
  stats.started = std::chrono::steady_clock::now();
  const auto at = [&](int milliseconds) {
    stats.count_message(stats.started + std::chrono::milliseconds(milliseconds));
  };
  at(100);
  at(500);
  at(999);   // three in the first second
  EXPECT_EQ(stats.busiest_second, 3u);
  at(1'000);
  at(1'900);  // two in the next: not a new record, and not added to the three
  EXPECT_EQ(stats.busiest_second, 3u);
  for (int message = 0; message < 5; ++message) {
    at(7'000 + message);  // five, several seconds later
  }
  EXPECT_EQ(stats.busiest_second, 5u);
  at(60'000);
  EXPECT_EQ(stats.busiest_second, 5u);
}

TEST(HftStats, WritesNumbersAndNamesThatCannotBreakTheJson) {
  hft::Stats stats;
  stats.symbols.resize(1);
  stats.symbols[0].name = "A\"B\\C\n";
  stats.recording_file = "/tmp/with \"quotes\"/x.rec";
  const std::string json = hft::to_json(stats);
  EXPECT_NE(json.find("\"symbol\":\"A\\\"B\\\\C\\u000a\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"recording_file\":\"/tmp/with \\\"quotes\\\"/x.rec\""), std::string::npos);

  EXPECT_EQ(hft::detail::grouped(0), "0");
  EXPECT_EQ(hft::detail::grouped(999), "999");
  EXPECT_EQ(hft::detail::grouped(1'000), "1,000");
  EXPECT_EQ(hft::detail::grouped(1'234'567), "1,234,567");
  EXPECT_EQ(hft::detail::money(1'234'567.891), "1,234,567.89");
  EXPECT_EQ(hft::detail::money(0.5), "0.50");
  EXPECT_EQ(hft::detail::duration(740), "740 ns");
  EXPECT_EQ(hft::detail::duration(12'400), "12.4 us");
  EXPECT_EQ(hft::detail::duration(3'100'000), "3.1 ms");
  EXPECT_EQ(hft::detail::duration(2'500'000'000), "2.50 s");
}

// --- Recording, and checking the recording -----------------------------------------

TEST(HftRecording, ASessionIsRecordedAndReplaysToTheSameFingerprint) {
  const TempDir dir;
  std::uint64_t live_digest = 0;
  std::uint64_t live_trades = 0;
  std::uint64_t live_commands = 0;
  std::string file;
  {
    hft::Core core(hft::CoreOptions{.data_dir = dir.path()});
    hft::DirectLink link(core);
    hft::Simulator<hft::DirectLink> simulator(link, {.symbols = 4, .messages = 40'000, .seed = 7});
    const hft::SimulationResult result = simulator.run();
    ASSERT_TRUE(result.completed);
    EXPECT_EQ(result.errors, 0u);
    EXPECT_EQ(result.messages, 40'000u);
    core.finish();

    const hft::Stats& stats = core.stats();
    ASSERT_TRUE(stats.recording);
    ASSERT_GT(stats.trades, 5'000u);
    ASSERT_GT(stats.trades_between_clients, 500u);
    ASSERT_GT(stats.cancelled, 1'000u);
    EXPECT_EQ(stats.events_dropped, 0u);
    EXPECT_EQ(stats.house_rejected, 0u);
    EXPECT_EQ(stats.rejected, 0u);
    EXPECT_EQ(stats.commands_recorded, stats.engine_commands);
    live_digest = stats.digest;
    live_trades = stats.trades;
    live_commands = stats.engine_commands;
    file = stats.recording_file;
  }
  ASSERT_EQ(dir.files(".rec"), std::vector<std::string>{file});

  const hft::Verification replay = hft::verify_recording(file);
  ASSERT_TRUE(replay.loaded);
  EXPECT_FALSE(replay.damaged);
  EXPECT_EQ(replay.commands, live_commands);
  EXPECT_EQ(replay.trades, live_trades);
  EXPECT_EQ(replay.digest, live_digest);

  // A recording with its last command missing is a different session.
  std::filesystem::resize_file(file, std::filesystem::file_size(file) - 32);
  const hft::Verification shorter = hft::verify_recording(file);
  ASSERT_TRUE(shorter.loaded);
  EXPECT_EQ(shorter.commands, live_commands - 1);
  EXPECT_NE(shorter.digest, live_digest);

  EXPECT_FALSE(hft::verify_recording(dir.path() + "/no-such-file.rec").loaded);
}

TEST(HftRecording, ARecordingThatReachesItsLimitStillChecksOutAsFarAsItGoes) {
  const TempDir dir;
  constexpr std::uint64_t kLimit = 5'000;
  hft::Core core(hft::CoreOptions{.data_dir = dir.path(), .recording_limit = kLimit});
  hft::DirectLink link(core);
  hft::Simulator<hft::DirectLink> simulator(link, {.symbols = 3, .messages = 20'000, .seed = 3});
  ASSERT_TRUE(simulator.run().completed);
  core.finish();

  const hft::Stats& stats = core.stats();
  ASSERT_TRUE(stats.recording);
  ASSERT_GT(stats.engine_commands, 2 * kLimit);  // the session went well past it
  EXPECT_TRUE(stats.recording_stopped);
  EXPECT_EQ(stats.recording_limit, kLimit);
  EXPECT_EQ(stats.commands_recorded, kLimit);
  EXPECT_NE(stats.recorded_digest, stats.digest);  // the session did more after the recording stopped
  EXPECT_EQ(std::filesystem::file_size(stats.recording_file),
            sizeof(lob::RecordingHeader) + 3 * sizeof(lob::RecordedBook) +
                kLimit * sizeof(lob::RecordedCommand));

  const hft::Verification replay = hft::verify_recording(stats.recording_file);
  ASSERT_TRUE(replay.loaded);
  EXPECT_FALSE(replay.damaged);
  EXPECT_EQ(replay.commands, kLimit);
  EXPECT_EQ(replay.digest, stats.recorded_digest);

  const std::string json = hft::to_json(stats);
  EXPECT_NE(json.find("\"recording_stopped\":true"), std::string::npos);
  EXPECT_NE(json.find("\"recording_limit\":5000"), std::string::npos);
  EXPECT_NE(json.find("\"recorded_digest\":\"" + hft::detail::hex(stats.recorded_digest) + "\""),
            std::string::npos);
  EXPECT_NE(hft::to_text(stats).find("reached its limit of 5,000 commands"), std::string::npos);
}

TEST(HftRecording, WhileTheRecordingIsGoingItsFingerprintIsTheSessions) {
  const TempDir dir;
  hft::Core core(hft::CoreOptions{.data_dir = dir.path(), .recording_limit = 1'000'000});
  hft::DirectLink link(core);
  hft::Simulator<hft::DirectLink> simulator(link, {.symbols = 2, .messages = 2'000, .seed = 5});
  ASSERT_TRUE(simulator.run().completed);
  const hft::Stats& stats = core.stats();
  EXPECT_FALSE(stats.recording_stopped);
  EXPECT_EQ(stats.recorded_digest, stats.digest);
  EXPECT_NE(stats.digest, 0u);
  EXPECT_EQ(hft::to_text(stats).find("reached its limit"), std::string::npos);
}

TEST(HftRecording, EachSessionGetsARecordingOfItsOwn) {
  const TempDir dir;
  hft::Core core(hft::CoreOptions{.data_dir = dir.path()});
  hft::DirectLink link(core);
  hft::Outbox out;
  const auto send = [&] { return link.exchange(out, [](Kind, std::span<const std::byte>) {}); };

  hft::say_hello(out, kBooks);
  hft::say_order(out, 1, 0, Side::Buy, 100, 10);
  ASSERT_TRUE(send());
  const std::string first = core.stats().recording_file;
  const std::uint64_t first_digest = core.stats().digest;

  hft::say_hello(out, kBooks, hft::kHelloFresh);
  hft::say_order(out, 1, 0, Side::Buy, 100, 10);
  hft::say_order(out, 2, 0, Side::Sell, 100, 10);
  ASSERT_TRUE(send());
  const std::string second = core.stats().recording_file;
  const std::uint64_t second_digest = core.stats().digest;
  core.finish();

  ASSERT_NE(first, second);
  EXPECT_EQ(dir.files(".rec").size(), 2u);
  // The first was complete the moment its session ended.
  EXPECT_EQ(hft::verify_recording(first).commands, 1u);
  EXPECT_EQ(hft::verify_recording(first).digest, first_digest);
  EXPECT_EQ(hft::verify_recording(second).commands, 2u);
  EXPECT_EQ(hft::verify_recording(second).digest, second_digest);
  EXPECT_NE(first_digest, second_digest);
}

// --- Over the socket -----------------------------------------------------------

// A Server on a thread of its own, on a socket in the test's directory.
class HftServer : public ::testing::Test {
 protected:
  void SetUp() override {
    // A socket's path must be short, and the test's own directory name is not.
    socket_path = ::testing::TempDir() + "hft_" + std::to_string(::getpid()) + "_" +
                  std::to_string(counter++) + ".sock";
    start();
  }

  void TearDown() override {
    stop();
    ::unlink(socket_path.c_str());
  }

  void start(std::chrono::seconds stats_every = std::chrono::seconds(5)) {
    server = std::make_unique<hft::Server>(hft::ServerOptions{
        .socket_path = socket_path, .data_dir = dir.path(), .stats_every = stats_every});
    ASSERT_TRUE(server->listen()) << server->error();
    thread = std::thread([this] { exit_code = server->run(); });
  }

  void stop() {
    if (thread.joinable()) {
      server->request_stop();
      thread.join();
    }
  }

  Replies exchange(hft::SocketLink& link, hft::Outbox& out) {
    Replies replies;
    exchanged = link.exchange(
        out,
        [&](Kind kind, std::span<const std::byte> payload) {
          replies.push_back(Reply{kind, {payload.begin(), payload.end()}});
        },
        5'000);
    return replies;
  }

  static inline int counter = 0;
  TempDir dir;
  std::string socket_path;
  std::unique_ptr<hft::Server> server;
  std::thread thread;
  int exit_code = -1;
  bool exchanged = false;
};

TEST_F(HftServer, AClientConnectsTradesAndGetsItsAnswersInOrder) {
  hft::SocketLink link;
  ASSERT_TRUE(link.connect(socket_path));
  hft::Outbox out;
  hft::say_hello(out, kBooks);
  hft::say_quote(out, 0, Levels{{100, 500}}, Levels{{102, 400}});
  hft::say_order(out, 1, 0, Side::Buy, 102, 150);
  hft::say_order(out, 2, 0, Side::Sell, 101, 20);
  hft::say_depth_request(out, 0, 5);
  const Replies replies = exchange(link, out);
  ASSERT_TRUE(exchanged);
  ASSERT_EQ(kinds_of(replies), (std::vector<Kind>{Kind::Welcome, Kind::Fill, Kind::Accepted,
                                                  Kind::Accepted, Kind::Depth}));
  EXPECT_EQ(replies[1].as<hft::Fill>().quantity, 150u);
  EXPECT_EQ(replies[1].as<hft::Fill>().with_house, 1);
  EXPECT_EQ(replies[2].as<hft::Accepted>().tag, 1u);
  EXPECT_EQ(replies[3].as<hft::Accepted>().resting, 20u);
}

TEST_F(HftServer, HandlesAMessageHoweverItIsCutUpOnTheWay) {
  hft::SocketLink link;
  ASSERT_TRUE(link.connect(socket_path));
  hft::Outbox out;
  hft::say_hello(out, kBooks);
  hft::say_order(out, 1, 0, Side::Buy, 100, 10);
  hft::say_order(out, 2, 0, Side::Sell, 100, 4);
  hft::say_ping(out, 77);

  // A byte at a time.
  for (const std::byte byte : out.bytes()) {
    ASSERT_TRUE(link.send_bytes(std::span<const std::byte>(&byte, 1)));
  }
  std::vector<Kind> kinds;
  while (kinds.empty() || kinds.back() != Kind::Pong) {
    ASSERT_TRUE(link.receive([&](Kind kind, std::span<const std::byte>) { kinds.push_back(kind); },
                             5'000));
  }
  EXPECT_EQ(kinds, (std::vector<Kind>{Kind::Welcome, Kind::Accepted, Kind::Fill, Kind::Fill,
                                      Kind::Accepted, Kind::Pong}));
}

TEST_F(HftServer, AnswersThousandsOfMessagesSentInOneGo) {
  hft::SocketLink link;
  ASSERT_TRUE(link.connect(socket_path));
  hft::Outbox out;
  hft::say_hello(out, kBooks);
  constexpr std::uint64_t kOrders = 20'000;
  for (std::uint64_t tag = 1; tag <= kOrders; ++tag) {
    // Each buy rests and the sell after it takes it, so the book never fills.
    hft::say_order(out, tag, 0, tag % 2 == 1 ? Side::Buy : Side::Sell, 100, 1);
  }
  const Replies replies = exchange(link, out);
  ASSERT_TRUE(exchanged);
  std::uint64_t next_tag = 1;
  std::uint64_t fills = 0;
  for (const Reply& reply : replies) {
    if (reply.kind == Kind::Accepted) {
      ASSERT_EQ(reply.as<hft::Accepted>().tag, next_tag++);
    } else if (reply.kind == Kind::Fill) {
      ++fills;
    } else {
      ASSERT_EQ(reply.kind, Kind::Welcome);
    }
  }
  EXPECT_EQ(next_tag, kOrders + 1);
  EXPECT_EQ(fills, kOrders);  // two for each of the 10,000 trades
}

TEST_F(HftServer, KeepsTheBooksWhileTheClientIsAwayAndTakesItBack) {
  std::uint64_t id = 0;
  {
    hft::SocketLink link;
    ASSERT_TRUE(link.connect(socket_path));
    hft::Outbox out;
    hft::say_hello(out, kBooks);
    hft::say_order(out, 1, 0, Side::Buy, 100, 10);
    const Replies replies = exchange(link, out);
    ASSERT_TRUE(exchanged);
    id = replies.back().as<hft::Accepted>().order_id;
  }  // hangs up

  // The server notices the client has gone only when it next looks, so the
  // first try at connecting again may find it still busy with the old one.
  for (int attempt = 0; attempt < 200; ++attempt) {
    hft::SocketLink link;
    ASSERT_TRUE(link.connect(socket_path));
    hft::Outbox out;
    hft::say_hello(out, kBooks);
    hft::say_cancel(out, 2, 0, id);
    const Replies replies = exchange(link, out);
    if (!exchanged) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    ASSERT_EQ(kinds_of(replies), (std::vector<Kind>{Kind::Welcome, Kind::Cancelled}));
    EXPECT_EQ(replies[0].as<hft::Welcome>().resumed, 1);
    return;
  }
  FAIL() << "the server never took the client back";
}

TEST_F(HftServer, TurnsAwayASecondClientWhileOneIsConnected) {
  hft::SocketLink first;
  ASSERT_TRUE(first.connect(socket_path));
  hft::Outbox out;
  hft::say_hello(out, kBooks);
  exchange(first, out);
  ASSERT_TRUE(exchanged);

  hft::SocketLink second;
  ASSERT_TRUE(second.connect(socket_path));  // the connection is made, then closed
  hft::say_hello(out, kBooks);
  exchange(second, out);
  EXPECT_FALSE(exchanged);

  // The first is none the worse for it.
  hft::say_order(out, 1, 0, Side::Buy, 100, 10);
  EXPECT_EQ(kinds_of(exchange(first, out)), std::vector<Kind>{Kind::Accepted});
  EXPECT_TRUE(exchanged);
}

TEST_F(HftServer, SaysWhatWasWrongAndHangsUpOnNonsense) {
  // A header claiming a payload far too large to be a message.
  {
    hft::SocketLink link;
    ASSERT_TRUE(link.connect(socket_path));
    hft::Header header{};
    header.kind = static_cast<std::uint16_t>(Kind::Order);
    header.size = hft::kMaxPayload + 1;
    ASSERT_TRUE(link.send_bytes(std::as_bytes(std::span<const hft::Header>(&header, 1))));
    std::vector<Kind> kinds;
    while (link.receive([&](Kind kind, std::span<const std::byte>) { kinds.push_back(kind); },
                        5'000)) {
    }
    EXPECT_EQ(kinds, std::vector<Kind>{Kind::Error});  // and then the connection closed
  }
  // An order before Hello. The server is ready for the next client afterwards.
  for (int attempt = 0; attempt < 200; ++attempt) {
    hft::SocketLink link;
    ASSERT_TRUE(link.connect(socket_path));
    hft::Outbox out;
    hft::say_order(out, 1, 0, Side::Buy, 100, 10);
    ASSERT_TRUE(link.send_bytes(out.bytes()));
    out.clear();
    std::vector<Kind> kinds;
    while (link.receive([&](Kind kind, std::span<const std::byte>) { kinds.push_back(kind); },
                        1'000)) {
    }
    if (kinds.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;  // it was still letting go of the client before
    }
    EXPECT_EQ(kinds, std::vector<Kind>{Kind::Error});

    // And it really has hung up: nothing sent on this connection is answered
    // any more, however well-formed.
    hft::say_hello(out, kBooks);
    exchange(link, out);
    EXPECT_FALSE(exchanged);
    return;
  }
  FAIL() << "the server never answered";
}

TEST_F(HftServer, StopsWhenToldToAndLeavesItsRecordAndStatisticsBehind) {
  hft::SocketLink link;
  ASSERT_TRUE(link.connect(socket_path));
  hft::Outbox out;
  hft::say_hello(out, kBooks);
  hft::say_order(out, 1, 0, Side::Buy, 100, 10);
  hft::say_order(out, 2, 0, Side::Sell, 100, 10);
  exchange(link, out);
  ASSERT_TRUE(exchanged);

  hft::say_shutdown(out);
  ASSERT_TRUE(link.send_bytes(out.bytes()));
  thread.join();
  EXPECT_EQ(exit_code, 0);
  EXPECT_FALSE(std::filesystem::exists(socket_path));  // it tidied up after itself

  // The latest statistics under a fixed name, and the session's own beside
  // its recording.
  const std::string json = read_file(dir.path() + "/hft-stats.json");
  EXPECT_NE(json.find("\"engine\":\"HFT\""), std::string::npos);
  EXPECT_NE(json.find("\"trades\":1"), std::string::npos);
  EXPECT_NE(read_file(dir.path() + "/hft-stats.txt").find("HFT session 1"), std::string::npos);
  ASSERT_EQ(dir.files(".rec").size(), 1u);
  ASSERT_EQ(dir.files(".stats.json").size(), 1u);
  ASSERT_EQ(dir.files(".stats.txt").size(), 1u);
  EXPECT_TRUE(dir.files(".part").empty());
  EXPECT_EQ(read_file(dir.files(".stats.json")[0]), json);

  // And the recording replays to the fingerprint in those statistics.
  const hft::Verification replay = hft::verify_recording(dir.files(".rec")[0]);
  ASSERT_TRUE(replay.loaded);
  EXPECT_EQ(replay.commands, 2u);
  EXPECT_NE(json.find("\"digest\":\"" + hft::detail::hex(replay.digest) + "\""), std::string::npos);
}

TEST_F(HftServer, StopsPromptlyWhenAskedFromOutside) {
  const auto began = std::chrono::steady_clock::now();
  stop();
  EXPECT_EQ(exit_code, 0);
  EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(2));
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}

TEST_F(HftServer, WritesItsStatisticsWhileItRuns) {
  stop();
  start(std::chrono::seconds(0));  // every time it looks
  hft::SocketLink link;
  ASSERT_TRUE(link.connect(socket_path));
  hft::Outbox out;
  hft::say_hello(out, kBooks);
  hft::say_order(out, 1, 0, Side::Buy, 100, 10);
  exchange(link, out);
  ASSERT_TRUE(exchanged);

  for (int attempt = 0; attempt < 500; ++attempt) {
    if (read_file(dir.path() + "/hft-stats.json").find("\"orders\":1") != std::string::npos) {
      SUCCEED();
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  FAIL() << "the statistics file never showed the order";
}

TEST_F(HftServer, ASimulatedMarketOverTheSocketComesOutTheSameAsOneInProcess) {
  const hft::SimulationOptions options{.symbols = 3, .messages = 20'000, .seed = 11, .batch = 64};

  hft::SocketLink link;
  ASSERT_TRUE(link.connect(socket_path));
  hft::Simulator<hft::SocketLink> over_socket(link, options);
  const hft::SimulationResult remote = over_socket.run();
  ASSERT_TRUE(remote.completed);
  EXPECT_EQ(remote.errors, 0u);
  hft::Outbox out;
  hft::say_stats_request(out);
  const Replies replies = exchange(link, out);
  ASSERT_EQ(kinds_of(replies), std::vector<Kind>{Kind::Stats});
  const std::string json = replies[0].text();

  hft::Core core;
  hft::DirectLink direct(core);
  hft::Simulator<hft::DirectLink> in_process(direct, options);
  const hft::SimulationResult local = in_process.run();
  ASSERT_TRUE(local.completed);

  EXPECT_EQ(remote.replies, local.replies);
  EXPECT_EQ(remote.fills, local.fills);
  const hft::Stats& stats = core.stats();
  ASSERT_GT(stats.trades, 1'000u);
  EXPECT_NE(json.find("\"digest\":\"" + hft::detail::hex(stats.digest) + "\""), std::string::npos)
      << json;
  EXPECT_NE(json.find("\"trades\":" + std::to_string(stats.trades) + ","), std::string::npos);
}

TEST_F(HftServer, KeepsOnlyTheNewestSessionsFiles) {
  stop();
  // Files from eight earlier sessions, some with statistics and some without,
  // and two files that are not HFT's to touch.
  const auto touch = [&](const std::string& name) {
    std::ofstream(dir.path() + "/" + name) << "x";
  };
  for (int day = 1; day <= 8; ++day) {
    const std::string session = "hft-session-2026010" + std::to_string(day) + "-120000-1";
    touch(session + ".rec");
    if (day % 2 == 0) {
      touch(session + ".stats.json");
      touch(session + ".stats.txt");
    }
  }
  touch("notes.txt");
  touch("hft-session-keep-me.dat");

  start();
  hft::SocketLink link;
  ASSERT_TRUE(link.connect(socket_path));
  hft::Outbox out;
  hft::say_hello(out, kBooks);
  exchange(link, out);
  ASSERT_TRUE(exchanged);
  stop();

  // Five are kept: the four newest of the old ones and the one just run.
  const std::vector<std::string> recordings = dir.files(".rec");
  ASSERT_EQ(recordings.size(), 5u);
  for (int day = 5; day <= 8; ++day) {
    const std::string session = dir.path() + "/hft-session-2026010" + std::to_string(day) + "-120000-1";
    EXPECT_TRUE(std::filesystem::exists(session + ".rec")) << day;
    EXPECT_EQ(std::filesystem::exists(session + ".stats.json"), day % 2 == 0) << day;
  }
  for (int day = 1; day <= 4; ++day) {
    const std::string session = dir.path() + "/hft-session-2026010" + std::to_string(day) + "-120000-1";
    EXPECT_FALSE(std::filesystem::exists(session + ".rec")) << day;
    EXPECT_FALSE(std::filesystem::exists(session + ".stats.json")) << day;
    EXPECT_FALSE(std::filesystem::exists(session + ".stats.txt")) << day;
  }
  EXPECT_TRUE(std::filesystem::exists(dir.path() + "/notes.txt"));
  EXPECT_TRUE(std::filesystem::exists(dir.path() + "/hft-session-keep-me.dat"));
}

TEST(HftServerSetup, SaysWhyItCannotListen) {
  hft::Server no_path(hft::ServerOptions{.socket_path = ""});
  EXPECT_FALSE(no_path.listen());
  EXPECT_FALSE(no_path.error().empty());

  hft::Server too_long(hft::ServerOptions{.socket_path = std::string(500, 'x')});
  EXPECT_FALSE(too_long.listen());

  hft::Server nowhere(
      hft::ServerOptions{.socket_path = "/no-such-directory-for-hft/engine.sock"});
  EXPECT_FALSE(nowhere.listen());
  EXPECT_NE(nowhere.error().find("bind"), std::string::npos) << nowhere.error();
}

}  // namespace

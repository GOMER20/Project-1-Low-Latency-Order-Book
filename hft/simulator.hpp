#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "hft/client.hpp"
#include "hft/wire.hpp"

// A market to try HFT on when there is no real one: a handful of symbols whose
// prices wander, a house that keeps ten levels of bids and offers around each
// price, and traders who take that liquidity, rest orders of their own in and
// around the spread, trade with each other, and cancel.
//
// It talks to HFT in the same messages a real client would, through whichever
// link it is given, so what it measures is what a real client would get.
//
// The market is made up. It is busy in the ways that matter to a matching
// engine, but it says nothing about how any strategy would fare on a real one.

namespace hft {

struct SimulationOptions {
  std::size_t symbols = 8;
  std::uint64_t messages = 1'000'000;  // orders, cancels and quote updates to send
  std::uint32_t seed = 1;
  std::size_t batch = 1;               // messages sent before reading the answers
};

struct SimulationResult {
  bool completed = false;            // false if the link failed part-way
  std::uint64_t messages = 0;
  std::uint64_t replies = 0;
  std::uint64_t fills = 0;
  std::uint64_t errors = 0;          // Error messages from HFT: there should be none
  double seconds = 0;                // wall-clock time from the first message to the last reply
};

template <typename Link>
class Simulator {
 public:
  Simulator(Link& link, const SimulationOptions& options)
      : link_(link),
        options_(options),
        random_(options.seed),
        markets_(options.symbols),
        live_(options.symbols) {}

  // The books the simulated market trades: prices from 1 to 65,536 ticks, with
  // each symbol starting somewhere between 20.00 and 400.00.
  [[nodiscard]] std::vector<BookSpec> books() {
    static constexpr std::array<const char*, 12> kNames = {
        "AAA", "BBB", "CCC", "DDD", "EEE", "FFF", "GGG", "HHH", "III", "JJJ", "KKK", "LLL"};
    names_.clear();
    for (std::size_t index = 0; index < markets_.size(); ++index) {
      std::string name = kNames[index % kNames.size()];
      if (index >= kNames.size()) {
        name += std::to_string(index / kNames.size());
      }
      names_.push_back(name);
    }
    std::vector<BookSpec> specs;
    for (std::size_t index = 0; index < markets_.size(); ++index) {
      specs.push_back(BookSpec{names_[index], 1, 1u << 16, 1u << 14});
      markets_[index].mid = pick(2'000, 40'000);
    }
    return specs;
  }

  // Says Hello, then trades. Returns once every message has been answered.
  SimulationResult run() {
    SimulationResult result;
    Outbox requests;
    const std::vector<BookSpec> specs = books();
    say_hello(requests, specs, kHelloFresh);
    if (!link_.exchange(requests, [&](Kind kind, std::span<const std::byte> payload) {
          on_reply(kind, payload, result);
        })) {
      return result;
    }
    // Every symbol opens with a full book.
    for (std::size_t symbol = 0; symbol < markets_.size(); ++symbol) {
      quote(requests, symbol);
    }

    const auto began = std::chrono::steady_clock::now();
    std::size_t waiting = markets_.size();
    while (result.messages < options_.messages) {
      step(requests);
      ++result.messages;
      if (++waiting >= options_.batch) {
        waiting = 0;
        if (!link_.exchange(requests, [&](Kind kind, std::span<const std::byte> payload) {
              on_reply(kind, payload, result);
            })) {
          return result;
        }
      }
    }
    if (!link_.exchange(requests, [&](Kind kind, std::span<const std::byte> payload) {
          on_reply(kind, payload, result);
        })) {
      return result;
    }
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    result.completed = true;
    return result;
  }

 private:
  struct RestingOrder {
    std::uint16_t symbol;
    std::uint64_t order_id;
  };

  struct Market {
    std::int64_t mid = 10'000;   // in ticks
    std::int64_t half_spread = 1;
  };

  int pick(int low, int high) { return std::uniform_int_distribution<int>(low, high)(random_); }

  // How much the house shows at one price. It depends on nothing but the
  // symbol, the side and the price, so a level keeps its size while the market
  // moves around it, the way resting orders do. A one-tick move then changes
  // one level at each end of the ladder, not all twenty.
  [[nodiscard]] static std::uint32_t house_size(std::size_t symbol, bool bid, std::int64_t price) {
    std::uint64_t mixed = (static_cast<std::uint64_t>(price) * 0x9E3779B97F4A7C15ULL) ^
                          (symbol * 0xC2B2AE3D27D4EB4FULL) ^ (bid ? 0x165667B19E3779F9ULL : 0);
    mixed ^= mixed >> 29;
    mixed *= 0xBF58476D1CE4E5B9ULL;
    mixed ^= mixed >> 32;
    return static_cast<std::uint32_t>(100 * (2 + mixed % 14));
  }

  // The house's ten levels a side around the symbol's price.
  void quote(Outbox& out, std::size_t symbol) {
    const Market& market = markets_[symbol];
    std::array<HouseLevel, 10> bids{};
    std::array<HouseLevel, 10> asks{};
    for (int level = 0; level < 10; ++level) {
      const std::int64_t bid = market.mid - market.half_spread - level;
      const std::int64_t ask = market.mid + market.half_spread + level;
      bids[static_cast<std::size_t>(level)] = HouseLevel{bid, house_size(symbol, true, bid)};
      asks[static_cast<std::size_t>(level)] = HouseLevel{ask, house_size(symbol, false, ask)};
    }
    say_quote(out, static_cast<std::uint16_t>(symbol), bids, asks);
  }

  // One message: the market moving, or a trader doing something.
  void step(Outbox& out) {
    const auto symbol = static_cast<std::size_t>(pick(0, static_cast<int>(markets_.size()) - 1));
    Market& market = markets_[symbol];
    const lob::Side side = pick(0, 1) == 0 ? lob::Side::Buy : lob::Side::Sell;
    const bool buying = side == lob::Side::Buy;
    const auto id = static_cast<std::uint16_t>(symbol);
    const int roll = pick(0, 99);

    if (roll < 30) {
      // The price moves a tick or two, and now and then the spread changes.
      market.mid = std::clamp<std::int64_t>(market.mid + pick(-2, 2), 200, 60'000);
      if (pick(0, 9) == 0) {
        market.half_spread = pick(1, 3);
      }
      quote(out, symbol);
    } else if (roll < 50) {
      // Take what is on offer: at the market, or with a limit a few ticks through.
      const auto quantity = static_cast<std::uint32_t>(pick(1, 12) * 100);
      if (pick(0, 2) == 0) {
        say_order(out, ++tag_, id, side, 0, quantity, lob::OrderType::Market);
      } else {
        const std::int64_t through = market.half_spread + pick(0, 3);
        say_order(out, ++tag_, id, side, buying ? market.mid + through : market.mid - through,
                  quantity);
      }
    } else if (roll < 75 && resting_.size() < kRestingPerSymbol * markets_.size()) {
      // Rest an order: inside the spread, at the touch, or a little behind it.
      const std::int64_t away = market.half_spread + pick(-1, 4);
      const std::int64_t price = buying ? market.mid - away : market.mid + away;
      say_order(out, ++tag_, id, side, price, static_cast<std::uint32_t>(pick(1, 10) * 100));
    } else if (roll < 93) {
      // Cancel one. (This is also what happens instead of resting another
      // when the traders have enough orders out already.) Orders that have
      // traded away since they were noted are passed over, most of the time.
      bool sent = false;
      while (!sent && !resting_.empty()) {
        const auto chosen =
            static_cast<std::size_t>(pick(0, static_cast<int>(resting_.size()) - 1));
        const RestingOrder order = resting_[chosen];
        resting_[chosen] = resting_.back();
        resting_.pop_back();
        // One cancel in twenty is for an order that has already gone: traders
        // do send cancels that arrive too late.
        if (live_[order.symbol].erase(order.order_id) != 0 || pick(0, 19) == 0) {
          say_cancel(out, ++tag_, order.symbol, order.order_id);
          sent = true;
        }
      }
      if (!sent) {
        say_cancel(out, ++tag_, id, 1);  // nothing to cancel: HFT will say so
      }
    } else {
      // All now or nothing, and whatever-is-there-now orders.
      const std::int64_t through = market.half_spread + pick(0, 2);
      say_order(out, ++tag_, id, side, buying ? market.mid + through : market.mid - through,
                static_cast<std::uint32_t>(pick(1, 30) * 100),
                pick(0, 1) == 0 ? lob::OrderType::IOC : lob::OrderType::FOK);
    }
  }

  void on_reply(Kind kind, std::span<const std::byte> payload, SimulationResult& result) {
    ++result.replies;
    switch (kind) {
      case Kind::Accepted: {
        const Accepted accepted = read_as<Accepted>(payload);
        // Remember the orders that rest, to cancel later.
        if (accepted.resting != 0) {
          resting_.push_back(RestingOrder{accepted.symbol, accepted.order_id});
          live_[accepted.symbol].insert(accepted.order_id);
        }
        break;
      }
      case Kind::Fill: {
        ++result.fills;
        const Fill fill = read_as<Fill>(payload);
        if (fill.maker != 0 && fill.remaining == 0) {
          live_[fill.symbol].erase(fill.order_id);  // traded away completely
        }
        break;
      }
      case Kind::Error:
        ++result.errors;
        break;
      default:
        break;
    }
  }

  static constexpr std::size_t kRestingPerSymbol = 512;

  Link& link_;
  SimulationOptions options_;
  std::mt19937 random_;
  std::vector<Market> markets_;
  std::vector<std::string> names_;
  std::vector<RestingOrder> resting_;                    // orders noted as resting, to cancel
  std::vector<std::unordered_set<std::uint64_t>> live_;  // by symbol: those still resting
  std::uint64_t tag_ = 0;
};

}  // namespace hft

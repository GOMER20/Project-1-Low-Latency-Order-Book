// Randomised differential test. Thousands of random operations are applied to
// the real book and to a deliberately naive reference model built from
// std::map and std::deque. After every operation the two must agree on the
// trades produced, the outcome and the best prices; periodically every queue
// at every price is compared too.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "lob/order_book.hpp"

namespace {

using lob::kInvalidOrderId;
using lob::OrderBook;
using lob::OrderId;
using lob::Price;
using lob::Quantity;
using lob::Side;

constexpr Price kNoPrice = -1;

struct Fill {
  std::size_t maker_key;
  Price price;
  Quantity quantity;
};

struct Outcome {
  bool accepted;
  Quantity filled;
  Quantity resting;
  std::vector<Fill> fills;
};

// Orders are identified by a key: the number of orders accepted before them.
class ReferenceBook {
 public:
  ReferenceBook(Price min_price, Price num_levels, std::size_t max_orders)
      : min_price_(min_price), num_levels_(num_levels), max_orders_(max_orders) {}

  Outcome submit(Side side, Price price, Quantity quantity) {
    if (price < min_price_ || price >= min_price_ + num_levels_ || quantity == 0 ||
        live_.size() == max_orders_) {
      return {false, 0, 0, {}};
    }
    const std::size_t key = next_key_++;
    Outcome outcome{true, 0, quantity, {}};

    // Both sides are keyed so that begin() is the best price: asks by price,
    // bids by negated price.
    Levels& makers = side == Side::Buy ? asks_ : bids_;
    const Price threshold = side == Side::Buy ? price : -price;
    while (outcome.resting != 0 && !makers.empty() && makers.begin()->first <= threshold) {
      std::deque<Resting>& queue = makers.begin()->second;
      Resting& maker = queue.front();
      const Quantity fill = std::min(outcome.resting, maker.quantity);
      const Price maker_price = side == Side::Buy ? makers.begin()->first : -makers.begin()->first;
      outcome.fills.push_back({maker.key, maker_price, fill});
      outcome.filled += fill;
      outcome.resting -= fill;
      maker.quantity -= fill;
      if (maker.quantity == 0) {
        live_.erase(maker.key);
        queue.pop_front();
        if (queue.empty()) {
          makers.erase(makers.begin());
        }
      }
    }

    if (outcome.resting != 0) {
      const Price level_key = side == Side::Buy ? -price : price;
      (side == Side::Buy ? bids_ : asks_)[level_key].push_back({key, outcome.resting});
      live_[key] = {side, level_key};
    }
    return outcome;
  }

  bool cancel(std::size_t key) { return reduce(key, 0, true) != 0; }

  // Returns the quantity taken off the order.
  Quantity execute(std::size_t key, Quantity quantity) { return reduce(key, quantity, false); }

  Price best_bid() const { return bids_.empty() ? kNoPrice : -bids_.begin()->first; }
  Price best_ask() const { return asks_.empty() ? kNoPrice : asks_.begin()->first; }
  std::size_t size() const { return live_.size(); }
  std::size_t keys_issued() const { return next_key_; }

  // (key, quantity) for each order at a price, first-to-trade first.
  std::vector<std::pair<std::size_t, Quantity>> queue_at(Side side, Price price) const {
    const Levels& levels = side == Side::Buy ? bids_ : asks_;
    const auto level = levels.find(side == Side::Buy ? -price : price);
    std::vector<std::pair<std::size_t, Quantity>> result;
    if (level != levels.end()) {
      for (const Resting& order : level->second) {
        result.emplace_back(order.key, order.quantity);
      }
    }
    return result;
  }

 private:
  struct Resting {
    std::size_t key;
    Quantity quantity;
  };
  struct Location {
    Side side;
    Price level_key;
  };
  using Levels = std::map<Price, std::deque<Resting>>;

  Quantity reduce(std::size_t key, Quantity quantity, bool remove_all) {
    const auto location = live_.find(key);
    if (location == live_.end()) {
      return 0;
    }
    Levels& levels = location->second.side == Side::Buy ? bids_ : asks_;
    const auto level = levels.find(location->second.level_key);
    std::deque<Resting>& queue = level->second;
    for (auto order = queue.begin(); order != queue.end(); ++order) {
      if (order->key != key) {
        continue;
      }
      const Quantity taken = remove_all ? order->quantity : std::min(quantity, order->quantity);
      order->quantity -= taken;
      if (order->quantity == 0) {
        queue.erase(order);
        if (queue.empty()) {
          levels.erase(level);
        }
        live_.erase(location);
      }
      return taken;
    }
    return 0;
  }

  Price min_price_;
  Price num_levels_;
  std::size_t max_orders_;
  std::size_t next_key_ = 0;
  Levels bids_;
  Levels asks_;
  std::map<std::size_t, Location> live_;
};

void run_against_reference(std::uint64_t seed, Price num_levels, std::size_t max_orders,
                           int steps) {
  constexpr Price kMinPrice = 1'000;
  OrderBook book({.symbol = "MODEL",
                  .min_price = kMinPrice,
                  .num_levels = static_cast<std::size_t>(num_levels),
                  .max_orders = max_orders});
  ReferenceBook reference(kMinPrice, num_levels, max_orders);
  std::vector<OrderId> id_of_key;
  std::vector<lob::Trade> trades;
  std::mt19937_64 rng(seed);
  const auto pick = [&](std::uint64_t bound) { return rng() % bound; };

  const auto compare_whole_book = [&](int step) {
    for (Price price = kMinPrice; price < kMinPrice + num_levels; ++price) {
      for (const Side side : {Side::Buy, Side::Sell}) {
        const auto expected = reference.queue_at(side, price);
        std::vector<std::pair<OrderId, Quantity>> actual;
        book.for_each_order(side, price, [&](const lob::Order& order) {
          actual.emplace_back(order.id, order.quantity);
        });
        ASSERT_EQ(actual.size(), expected.size()) << "queue length, step " << step;
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < expected.size(); ++i) {
          ASSERT_EQ(actual[i].first, id_of_key[expected[i].first]) << "queue order, step " << step;
          ASSERT_EQ(actual[i].second, expected[i].second) << "quantity, step " << step;
          total += expected[i].second;
        }
        ASSERT_EQ(book.quantity_at(side, price), total) << "level total, step " << step;
      }
    }
  };

  for (int step = 0; step < steps; ++step) {
    const std::uint64_t action = pick(100);

    if (action < 55) {
      // Prices span the whole range (plus a little beyond it), so about half
      // of all orders cross and trade.
      const Side side = pick(2) == 0 ? Side::Buy : Side::Sell;
      const Price price = kMinPrice - 2 + static_cast<Price>(pick(static_cast<std::uint64_t>(num_levels) + 4));
      const Quantity quantity = pick(50) == 0 ? 0 : static_cast<Quantity>(1 + pick(50));

      const Outcome expected = reference.submit(side, price, quantity);
      trades.clear();
      const lob::SubmitResult actual = book.submit(
          side, price, quantity, [&](const lob::Trade& trade) { trades.push_back(trade); });

      ASSERT_EQ(actual.id != kInvalidOrderId, expected.accepted) << "acceptance, step " << step;
      ASSERT_EQ(actual.filled, expected.filled) << "filled, step " << step;
      ASSERT_EQ(actual.resting, expected.accepted ? expected.resting : 0u) << "resting, step " << step;
      ASSERT_EQ(trades.size(), expected.fills.size()) << "trade count, step " << step;
      for (std::size_t i = 0; i < trades.size(); ++i) {
        ASSERT_EQ(trades[i].maker_id, id_of_key[expected.fills[i].maker_key]) << "maker, step " << step;
        ASSERT_EQ(trades[i].taker_id, actual.id) << "taker, step " << step;
        ASSERT_EQ(trades[i].price, expected.fills[i].price) << "trade price, step " << step;
        ASSERT_EQ(trades[i].quantity, expected.fills[i].quantity) << "trade quantity, step " << step;
        ASSERT_EQ(trades[i].taker_side, side) << "taker side, step " << step;
      }
      if (expected.accepted) {
        id_of_key.push_back(actual.id);
      }
    } else if (action < 90) {
      // Cancel any order ever accepted, live or not, and sometimes a made-up ID.
      if (pick(20) == 0 || id_of_key.empty()) {
        ASSERT_FALSE(book.cancel(rng())) << "made-up ID, step " << step;
      } else {
        const std::size_t key = static_cast<std::size_t>(pick(id_of_key.size()));
        ASSERT_EQ(book.cancel(id_of_key[key]), reference.cancel(key)) << "cancel, step " << step;
      }
    } else if (!id_of_key.empty()) {
      const std::size_t key = static_cast<std::size_t>(pick(id_of_key.size()));
      const Quantity quantity = static_cast<Quantity>(1 + pick(30));
      ASSERT_EQ(book.execute(id_of_key[key], quantity), reference.execute(key, quantity))
          << "execute, step " << step;
    }

    ASSERT_EQ(book.best_bid().value_or(kNoPrice), reference.best_bid()) << "best bid, step " << step;
    ASSERT_EQ(book.best_ask().value_or(kNoPrice), reference.best_ask()) << "best ask, step " << step;
    ASSERT_EQ(book.size(), reference.size()) << "order count, step " << step;
    if (book.best_bid() && book.best_ask()) {
      ASSERT_LT(*book.best_bid(), *book.best_ask()) << "book is crossed, step " << step;
    }
    if (step % 500 == 499) {
      compare_whole_book(step);
      if (::testing::Test::HasFatalFailure()) {
        return;
      }
    }
  }
  compare_whole_book(steps);
}

TEST(ModelCheck, RoomyBookOverAWidePriceRange) {
  run_against_reference(/*seed=*/1, /*num_levels=*/200, /*max_orders=*/4'096, /*steps=*/20'000);
}

// Few levels, so queues grow long and most orders trade.
TEST(ModelCheck, DeepQueuesOverANarrowPriceRange) {
  run_against_reference(/*seed=*/2, /*num_levels=*/6, /*max_orders=*/4'096, /*steps=*/20'000);
}

// A tiny pool, so the book is often full and slots are recycled constantly.
TEST(ModelCheck, BookThatKeepsFillingUp) {
  run_against_reference(/*seed=*/3, /*num_levels=*/40, /*max_orders=*/24, /*steps=*/20'000);
}

// More than 4,096 levels, so the bitmap's top tier is exercised.
TEST(ModelCheck, SparseBookAcrossAllBitmapTiers) {
  run_against_reference(/*seed=*/4, /*num_levels=*/20'000, /*max_orders=*/1'024, /*steps=*/6'000);
}

}  // namespace

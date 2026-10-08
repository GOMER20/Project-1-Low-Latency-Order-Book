#pragma once

#include <vector>

#include <gtest/gtest.h>

#include "lob/order_book.hpp"

namespace lob_test {

// Tradable prices are 100..199.
inline constexpr lob::BookConfig kConfig{
    .symbol = "TEST", .min_price = 100, .num_levels = 100, .max_orders = 64};

inline constexpr lob::Price kNoPrice = -1;

// A book plus a record of every trade it has emitted.
class BookFixture : public ::testing::Test {
 protected:
  lob::SubmitResult submit(lob::Side side, lob::Price price, lob::Quantity quantity) {
    return book.submit(side, price, quantity,
                       [this](const lob::Trade& trade) { trades.push_back(trade); });
  }

  lob::Price best_bid() const { return book.best_bid().value_or(kNoPrice); }
  lob::Price best_ask() const { return book.best_ask().value_or(kNoPrice); }

  // IDs resting at a price, first-to-trade first.
  std::vector<lob::OrderId> queue_at(lob::Side side, lob::Price price) const {
    std::vector<lob::OrderId> ids;
    book.for_each_order(side, price, [&](const lob::Order& order) { ids.push_back(order.id); });
    return ids;
  }

  lob::OrderBook book{kConfig};
  std::vector<lob::Trade> trades;
};

}  // namespace lob_test

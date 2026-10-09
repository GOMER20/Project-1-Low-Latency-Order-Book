#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>

#include "lob/messages.hpp"

namespace lob {

// One symbol's prices and depth, rebuilt from the market data feed: what a
// reader of the feed keeps. It is the reference for how the feed is meant to
// be read, and what the tests use to check that the feed tells the truth.
//
// It is for the reading side, which has no need of the engine's constraints,
// so it uses ordinary containers and allocates as it goes.
//
// The one rule that matters is about sequence numbers. A message older than
// the last one applied is ignored. That is what keeps the mirror right when a
// symbol moves between shards, where the tail of the old shard's messages may
// be read after the fresh picture the new shard starts with.
class BookMirror {
 public:
  // Applies one message for this mirror's symbol.
  void apply(const MarketData& message) {
    if (message.sequence <= last_sequence_) {
      return;  // old news
    }
    if (message.sequence != last_sequence_ + 1 && message.type != MarketDataType::Clear) {
      in_sync_ = false;  // something was dropped in between
    }
    last_sequence_ = message.sequence;

    switch (message.type) {
      case MarketDataType::Clear:
        bids_.clear();
        asks_.clear();
        in_sync_ = true;  // a fresh picture starts here
        break;
      case MarketDataType::Level:
        if (message.side == Side::Buy) {
          set(bids_, message.price, message.quantity);
        } else {
          set(asks_, message.price, message.quantity);
        }
        break;
      case MarketDataType::BestPrices:
        best_ = message;
        break;
      case MarketDataType::Trade:
        ++trades_;
        traded_quantity_ += message.quantity;
        break;
    }
  }

  // Whether the mirror can be trusted: no message has gone missing since the
  // last fresh picture began. If not, ask for a snapshot.
  [[nodiscard]] bool in_sync() const noexcept { return in_sync_; }

  // The total resting at a price, as built up from Level messages.
  [[nodiscard]] std::uint64_t quantity_at(Side side, Price price) const {
    if (side == Side::Buy) {
      const auto level = bids_.find(price);
      return level != bids_.end() ? level->second : 0;
    }
    const auto level = asks_.find(price);
    return level != asks_.end() ? level->second : 0;
  }

  // The best prices according to the depth built up from Level messages.
  [[nodiscard]] std::optional<Price> best_bid() const {
    return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
  }
  [[nodiscard]] std::optional<Price> best_ask() const {
    return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
  }

  // The best prices as last announced by a BestPrices message. A quantity of
  // 0 means that side is empty.
  [[nodiscard]] Price announced_bid_price() const noexcept { return best_.bid_price; }
  [[nodiscard]] std::uint64_t announced_bid_quantity() const noexcept {
    return best_.bid_quantity;
  }
  [[nodiscard]] Price announced_ask_price() const noexcept { return best_.ask_price; }
  [[nodiscard]] std::uint64_t announced_ask_quantity() const noexcept {
    return best_.ask_quantity;
  }

  [[nodiscard]] std::size_t bid_levels() const noexcept { return bids_.size(); }
  [[nodiscard]] std::size_t ask_levels() const noexcept { return asks_.size(); }
  [[nodiscard]] std::uint64_t trades() const noexcept { return trades_; }
  [[nodiscard]] std::uint64_t traded_quantity() const noexcept { return traded_quantity_; }
  [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }

 private:
  template <typename Levels>
  static void set(Levels& levels, Price price, std::uint64_t quantity) {
    if (quantity == 0) {
      levels.erase(price);
    } else {
      levels[price] = quantity;
    }
  }

  std::map<Price, std::uint64_t, std::greater<>> bids_;  // best (highest) first
  std::map<Price, std::uint64_t> asks_;                  // best (lowest) first
  MarketData best_{};
  std::uint64_t last_sequence_ = 0;
  std::uint64_t trades_ = 0;
  std::uint64_t traded_quantity_ = 0;
  bool in_sync_ = true;
};

}  // namespace lob

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "lob/level_bitmap.hpp"
#include "lob/order_pool.hpp"
#include "lob/price_level.hpp"
#include "lob/trade.hpp"

namespace lob {

struct BookConfig {
  std::string_view symbol;  // at most 8 characters are kept
  Price min_price;          // lowest tradable price, in ticks
  std::size_t num_levels;   // tradable prices are [min_price, min_price + num_levels)
  std::size_t max_orders;   // capacity for resting orders
};

// What happened to an order passed to OrderBook::submit.
struct SubmitResult {
  OrderId id;        // kInvalidOrderId if the order was rejected
  Quantity filled;   // quantity that traded immediately
  Quantity resting;  // quantity left in the book; always 0 for Market, IOC and FOK
};

// A limit order book and matching engine for one symbol. All memory is
// acquired in the constructor; no operation allocates afterwards.
//
// Order IDs are assigned by the book and carry the order's pool slot in their
// low 32 bits, so finding an order from its ID is a single array index: no
// hash map. The high 32 bits are a sequence number, which makes an ID invalid
// once its slot has been recycled. IDs are unique for the first 2^32 orders.
class OrderBook {
 public:
  explicit OrderBook(const BookConfig& config)
      : pool_(config.max_orders),
        sides_{{BookSide(config.num_levels), BookSide(config.num_levels)}},
        min_price_(config.min_price),
        num_levels_(config.num_levels) {
    assert(config.num_levels > 0);
    std::copy_n(config.symbol.begin(),
                std::min(config.symbol.size(), sizeof(symbol_)), symbol_);
  }

  // Matches an incoming order against the opposite side of the book. What
  // happens to any quantity left over depends on `type`:
  //
  //   Limit   rests at its limit price until it is filled or cancelled
  //   Market  trades at any price; the remainder is discarded; `price` is ignored
  //   IOC     trades at its limit price or better; the remainder is discarded
  //   FOK     trades in full immediately, or does not trade at all
  //
  // Price-time priority: the best-priced level trades first, and within a
  // level the oldest order trades first. Every fill happens at the resting
  // order's price, so any price improvement goes to the incoming order.
  //
  // `on_trade` is called once per fill, after the book has been updated for
  // that fill. It must not throw and must not call back into the book.
  //
  // The order is rejected (id == kInvalidOrderId, no trades) if the quantity
  // is zero, if its price is outside the book's range (a Market order has no
  // price to check) or, for Limit orders only, if the book is full. Market,
  // IOC and FOK orders never rest, so they need no room in the book.
  //
  // A FOK order that cannot be filled in full is not a rejection: it gets an
  // ID and reports zero filled.
  template <std::invocable<const Trade&> OnTrade>
  [[nodiscard]] SubmitResult submit(Side side, Price price, Quantity quantity, OrderType type,
                                    OnTrade&& on_trade) noexcept {
    if (quantity == 0) [[unlikely]] {
      return {kInvalidOrderId, 0, 0};
    }
    std::size_t limit_level;
    if (type == OrderType::Market) {
      // The most aggressive level there is: a buy will pay the highest price.
      limit_level = side == Side::Buy ? num_levels_ - 1 : 0;
    } else {
      limit_level = level_of(price);
      if (limit_level >= num_levels_) [[unlikely]] {
        return {kInvalidOrderId, 0, 0};
      }
    }

    if (type == OrderType::Limit) {
      // The slot is reserved before matching so that the ID reported in the
      // trades is the same ID the remainder rests under.
      const OrderIndex index = pool_.allocate();
      if (index == kNullIndex) [[unlikely]] {
        return {kInvalidOrderId, 0, 0};
      }
      const OrderId id = next_id(index);
      const Quantity remaining = match(side, limit_level, quantity, id, on_trade);
      if (remaining == 0) {
        pool_.deallocate(index);  // fully filled: nothing to rest
      } else {
        rest(index, id, side, price, limit_level, remaining);
      }
      return {id, quantity - remaining, remaining};
    }

    // An order that never rests needs no slot. Its ID carries kNullIndex where
    // the slot would be, so it can never be mistaken for a live order.
    const OrderId id = next_id(kNullIndex);
    if (type == OrderType::FOK && !can_fill(side, limit_level, quantity)) {
      return {id, 0, 0};
    }
    const Quantity remaining = match(side, limit_level, quantity, id, on_trade);
    return {id, quantity - remaining, 0};
  }

  // A Limit order: the same as submit() above with OrderType::Limit.
  template <std::invocable<const Trade&> OnTrade>
  [[nodiscard]] SubmitResult submit(Side side, Price price, Quantity quantity,
                                    OnTrade&& on_trade) noexcept {
    return submit(side, price, quantity, OrderType::Limit, std::forward<OnTrade>(on_trade));
  }

  // Rests a new order at the back of the queue for its price WITHOUT matching
  // it, for building a book from a feed that has already done the matching.
  // Orders that should trade go through submit(). Returns the order's ID, or
  // kInvalidOrderId if the price is outside the book's range, the quantity is
  // zero, or the book is full.
  [[nodiscard]] OrderId add(Side side, Price price, Quantity quantity) noexcept {
    const std::size_t level = level_of(price);
    if (level >= num_levels_ || quantity == 0) [[unlikely]] {
      return kInvalidOrderId;
    }
    const OrderIndex index = pool_.allocate();
    if (index == kNullIndex) [[unlikely]] {
      return kInvalidOrderId;
    }
    const OrderId id = next_id(index);
    rest(index, id, side, price, level, quantity);
    return id;
  }

  // Removes a resting order. Returns false if the ID is not a live order.
  bool cancel(OrderId id) noexcept {
    Order* order = lookup(id);
    if (order == nullptr) [[unlikely]] {
      return false;
    }
    remove(*order);
    return true;
  }

  // Fills up to `quantity` of a resting order, removing it once fully filled.
  // Returns the quantity actually filled; 0 if the ID is not a live order.
  Quantity execute(OrderId id, Quantity quantity) noexcept {
    Order* order = lookup(id);
    if (order == nullptr) [[unlikely]] {
      return 0;
    }
    if (quantity >= order->quantity) {
      const Quantity filled = order->quantity;
      remove(*order);
      return filled;
    }
    sides_[side_index(order->side)].levels[level_of(order->price)].reduce(*order, quantity);
    return quantity;
  }

  [[nodiscard]] std::optional<Price> best_bid() const noexcept {
    return price_of(best_level(Side::Buy));
  }

  [[nodiscard]] std::optional<Price> best_ask() const noexcept {
    return price_of(best_level(Side::Sell));
  }

  // Total resting quantity on one side at one price.
  [[nodiscard]] std::uint64_t quantity_at(Side side, Price price) const noexcept {
    const std::size_t level = level_of(price);
    return level < num_levels_ ? sides_[side_index(side)].levels[level].total_quantity : 0;
  }

  // The live order with this ID, or nullptr.
  [[nodiscard]] const Order* find(OrderId id) const noexcept {
    const OrderIndex index = slot_of(id);
    if (index >= pool_.capacity()) {
      return nullptr;
    }
    const Order& order = pool_[index];
    return (order.id == id && order.quantity != 0) ? &order : nullptr;
  }

  // Visits the orders resting at a price, first-to-trade first. For snapshots
  // and tests; not part of the hot path.
  template <typename Visitor>
  void for_each_order(Side side, Price price, Visitor&& visit) const {
    const std::size_t level = level_of(price);
    if (level >= num_levels_) {
      return;
    }
    for (OrderIndex index = sides_[side_index(side)].levels[level].head; index != kNullIndex;
         index = pool_[index].next) {
      visit(pool_[index]);
    }
  }

  [[nodiscard]] std::string_view symbol() const noexcept {
    const char* end = std::find(std::begin(symbol_), std::end(symbol_), '\0');
    return {symbol_, static_cast<std::size_t>(end - symbol_)};
  }

  [[nodiscard]] std::size_t size() const noexcept { return pool_.size(); }
  [[nodiscard]] bool empty() const noexcept { return pool_.empty(); }

 private:
  struct BookSide {
    explicit BookSide(std::size_t num_levels)
        : levels(std::make_unique<PriceLevel[]>(num_levels)), occupied(num_levels) {}

    std::unique_ptr<PriceLevel[]> levels;  // indexed by price - min_price
    LevelBitmap occupied;                  // which levels are non-empty
  };

  static constexpr std::size_t side_index(Side side) noexcept {
    return static_cast<std::size_t>(side);
  }

  static constexpr OrderIndex slot_of(OrderId id) noexcept {
    return static_cast<OrderIndex>(id);
  }

  [[nodiscard]] OrderId next_id(OrderIndex index) noexcept {
    return (next_sequence_++ << 32) | index;
  }

  // Subtracting as unsigned makes a price below min_price wrap to a huge
  // value, so one `< num_levels_` comparison checks both ends of the range.
  [[nodiscard]] std::size_t level_of(Price price) const noexcept {
    return static_cast<std::size_t>(static_cast<std::uint64_t>(price) -
                                    static_cast<std::uint64_t>(min_price_));
  }

  [[nodiscard]] std::optional<Price> price_of(std::size_t level) const noexcept {
    if (level == LevelBitmap::npos) {
      return std::nullopt;
    }
    return min_price_ + static_cast<Price>(level);
  }

  // The level that trades first on a side: the highest bid or the lowest ask.
  // LevelBitmap::npos if the side is empty.
  [[nodiscard]] std::size_t best_level(Side side) const noexcept {
    const LevelBitmap& occupied = sides_[side_index(side)].occupied;
    return side == Side::Buy ? occupied.find_last() : occupied.find_first();
  }

  // Whether an incoming order limited to `limit_level` may trade at `level`.
  static constexpr bool crosses(Side taker_side, std::size_t level,
                                std::size_t limit_level) noexcept {
    return taker_side == Side::Buy ? level <= limit_level : level >= limit_level;
  }

  // Trades an incoming order against the opposite side, best price first and
  // oldest order first, until it is filled or nothing is left at
  // `limit_level` or better. Returns the quantity that did not trade.
  template <typename OnTrade>
  Quantity match(Side side, std::size_t limit_level, Quantity quantity, OrderId taker_id,
                 OnTrade& on_trade) noexcept {
    const Side maker_side = opposite(side);
    BookSide& makers = sides_[side_index(maker_side)];
    Quantity remaining = quantity;

    while (remaining != 0) {
      const std::size_t level = best_level(maker_side);
      if (level == LevelBitmap::npos || !crosses(side, level, limit_level)) {
        break;
      }

      PriceLevel& price_level = makers.levels[level];
      const Price trade_price = min_price_ + static_cast<Price>(level);
      do {
        Order& maker = pool_[price_level.head];
        const OrderId maker_id = maker.id;
        const Quantity fill = std::min(remaining, maker.quantity);
        if (fill == maker.quantity) {
          remove(maker);
        } else {
          price_level.reduce(maker, fill);
        }
        remaining -= fill;
        on_trade(make_trade(maker_id, taker_id, trade_price, fill, side));
      } while (remaining != 0 && !price_level.empty());
    }
    return remaining;
  }

  // True if at least `quantity` rests on the opposite side at `limit_level`
  // or better. It adds up each level's running total and hops between
  // occupied levels with the bitmap, so it never walks a queue of orders.
  [[nodiscard]] bool can_fill(Side side, std::size_t limit_level,
                              Quantity quantity) const noexcept {
    const Side maker_side = opposite(side);
    const BookSide& makers = sides_[side_index(maker_side)];
    std::uint64_t available = 0;
    std::size_t level = best_level(maker_side);
    while (level != LevelBitmap::npos && crosses(side, level, limit_level)) {
      available += makers.levels[level].total_quantity;
      if (available >= quantity) {
        return true;
      }
      level = maker_side == Side::Sell ? makers.occupied.find_next(level)
                                       : makers.occupied.find_prev(level);
    }
    return false;
  }

  [[nodiscard]] Order* lookup(OrderId id) noexcept {
    return const_cast<Order*>(find(id));
  }

  // Writes the order into its reserved slot and queues it at its price.
  void rest(OrderIndex index, OrderId id, Side side, Price price, std::size_t level,
            Quantity quantity) noexcept {
    Order& order = pool_[index];
    order.id = id;
    order.price = price;
    std::copy_n(symbol_, sizeof(symbol_), order.symbol);
    order.quantity = quantity;
    order.side = side;

    BookSide& book_side = sides_[side_index(side)];
    book_side.levels[level].push_back(pool_, index);
    book_side.occupied.set(level);
  }

  // The order carries everything needed to unlink it: its slot (in its ID),
  // its side and price (which level) and its neighbours (prev / next).
  void remove(Order& order) noexcept {
    const OrderIndex index = slot_of(order.id);
    const std::size_t level = level_of(order.price);
    BookSide& book_side = sides_[side_index(order.side)];
    PriceLevel& price_level = book_side.levels[level];

    price_level.erase(pool_, index);
    if (price_level.empty()) {
      book_side.occupied.reset(level);
    }
    order.quantity = 0;  // marks the slot dead, so a repeated cancel is rejected
    pool_.deallocate(index);
  }

  [[nodiscard]] Trade make_trade(OrderId maker_id, OrderId taker_id, Price price,
                                 Quantity quantity, Side taker_side) const noexcept {
    Trade trade{};  // zeroed, so padding bytes are deterministic if the event is copied out raw
    trade.maker_id = maker_id;
    trade.taker_id = taker_id;
    trade.price = price;
    std::copy_n(symbol_, sizeof(symbol_), trade.symbol);
    trade.quantity = quantity;
    trade.taker_side = taker_side;
    return trade;
  }

  OrderPool pool_;
  std::array<BookSide, 2> sides_;  // indexed by Side
  Price min_price_;
  std::size_t num_levels_;
  std::uint64_t next_sequence_ = 1;  // starts at 1 so that no ID equals kInvalidOrderId
  char symbol_[8] = {};
};

}  // namespace lob

#pragma once

#include <type_traits>

#include "lob/types.hpp"

namespace lob {

// One fill between an incoming (taker) order and a resting (maker) order.
struct Trade {
  OrderId maker_id;   // the resting order
  OrderId taker_id;   // the incoming order
  Price price;        // always the resting order's price
  char symbol[8];
  Quantity quantity;
  Side taker_side;
};

static_assert(sizeof(Trade) == 40);
static_assert(std::is_trivial_v<Trade> && std::is_standard_layout_v<Trade>);

}  // namespace lob

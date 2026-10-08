#pragma once

#include <type_traits>

#include "lob/compiler.hpp"
#include "lob/types.hpp"

namespace lob {

// One resting order. Exactly one cache line, so touching an order costs at
// most one cache miss and no order ever straddles two lines.
//
// Fields are declared widest-first so there is no interior padding. No default
// member initialisers: they would make the type non-trivial.
struct alignas(kCacheLineSize) Order {
  OrderId id;
  Price price;
  char symbol[8];     // fixed width, not NUL-terminated; compares as one 64-bit load
  Quantity quantity;  // remaining (unfilled) quantity
  OrderIndex next;    // intrusive FIFO link; doubles as the pool's free-list link
  OrderIndex prev;
  Side side;
};

static_assert(sizeof(Order) == kCacheLineSize);
static_assert(alignof(Order) == kCacheLineSize);
static_assert(std::is_trivial_v<Order> && std::is_standard_layout_v<Order>);

}  // namespace lob

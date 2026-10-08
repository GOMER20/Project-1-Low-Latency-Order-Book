#pragma once

#include <cassert>
#include <cstdint>
#include <type_traits>

#include "lob/order_pool.hpp"

namespace lob {

// The FIFO queue of resting orders at one price. The list is intrusive: the
// links are Order::next / Order::prev, so the level itself is just two indices
// and a running total. 16 bytes, i.e. four levels per cache line.
//
// The level never allocates or frees orders; it only links slots that the
// caller obtained from the OrderPool.
struct PriceLevel {
  OrderIndex head = kNullIndex;  // oldest order: first to trade
  OrderIndex tail = kNullIndex;  // newest order: last to trade
  std::uint64_t total_quantity = 0;

  [[nodiscard]] bool empty() const noexcept { return head == kNullIndex; }

  // Appends at the tail, giving the order the lowest time priority.
  void push_back(OrderPool& pool, OrderIndex index) noexcept {
    Order& order = pool[index];
    order.next = kNullIndex;
    order.prev = tail;
    if (tail != kNullIndex) {
      pool[tail].next = index;
    } else {
      head = index;
    }
    tail = index;
    total_quantity += order.quantity;
  }

  // Unlinks an order from anywhere in the queue without walking it. The slot
  // is not returned to the pool; that is the caller's job.
  void erase(OrderPool& pool, OrderIndex index) noexcept {
    assert(!empty());
    const Order& order = pool[index];
    if (order.prev != kNullIndex) {
      pool[order.prev].next = order.next;
    } else {
      head = order.next;
    }
    if (order.next != kNullIndex) {
      pool[order.next].prev = order.prev;
    } else {
      tail = order.prev;
    }
    total_quantity -= order.quantity;
  }

  // Partial fill: shrinks an order in place, keeping its queue position.
  void reduce(Order& order, Quantity amount) noexcept {
    assert(amount <= order.quantity);
    order.quantity -= amount;
    total_quantity -= amount;
  }
};

static_assert(sizeof(PriceLevel) == 16);
static_assert(std::is_trivially_copyable_v<PriceLevel> &&
              std::is_standard_layout_v<PriceLevel>);

}  // namespace lob

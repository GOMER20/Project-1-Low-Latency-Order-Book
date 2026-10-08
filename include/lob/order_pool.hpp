#pragma once

#include <cassert>
#include <cstddef>
#include <memory>

#include "lob/order.hpp"

namespace lob {

// Fixed-capacity pool of Orders. It allocates once, at construction, and never
// again; allocate() and deallocate() are a handful of instructions each.
//
// Free slots form a LIFO stack threaded through Order::next, so the pool needs
// no bookkeeping memory of its own, and the slot handed out next is the one
// freed most recently - the one most likely to still be in L1.
//
// Single-threaded by design: no locks, no atomics.
class OrderPool {
 public:
  explicit OrderPool(std::size_t capacity)
      : orders_(std::make_unique<Order[]>(capacity)),
        capacity_(static_cast<OrderIndex>(capacity)) {
    assert(capacity < kNullIndex);
    // Slots start zeroed, so a never-used slot reads as a dead order
    // (quantity 0) rather than as garbage.
    //
    // Build the list back to front so slots are first handed out in ascending
    // address order. Writing every slot also pre-faults its page, moving the
    // page-fault cost from the first trade to startup.
    for (OrderIndex i = capacity_; i-- > 0;) {
      orders_[i].next = free_head_;
      free_head_ = i;
    }
  }

  OrderPool(const OrderPool&) = delete;
  OrderPool& operator=(const OrderPool&) = delete;

  // Returns kNullIndex when the pool is exhausted.
  [[nodiscard]] OrderIndex allocate() noexcept {
    const OrderIndex index = free_head_;
    if (index == kNullIndex) [[unlikely]] {
      return kNullIndex;
    }
    free_head_ = orders_[index].next;
    ++size_;
    return index;
  }

  void deallocate(OrderIndex index) noexcept {
    assert(index < capacity_ && size_ > 0);
    orders_[index].next = free_head_;
    free_head_ = index;
    --size_;
  }

  [[nodiscard]] Order& operator[](OrderIndex index) noexcept {
    assert(index < capacity_);
    return orders_[index];
  }

  [[nodiscard]] const Order& operator[](OrderIndex index) const noexcept {
    assert(index < capacity_);
    return orders_[index];
  }

  [[nodiscard]] OrderIndex size() const noexcept { return size_; }
  [[nodiscard]] OrderIndex capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] bool full() const noexcept { return size_ == capacity_; }

 private:
  std::unique_ptr<Order[]> orders_;
  OrderIndex capacity_;
  OrderIndex free_head_ = kNullIndex;
  OrderIndex size_ = 0;
};

}  // namespace lob

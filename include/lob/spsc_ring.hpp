#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <memory>
#include <type_traits>

#include "lob/compiler.hpp"

namespace lob {

// A bounded queue for exactly one producer thread and one consumer thread.
//
// There are no locks, no compare-and-swap loops and no retries: every call
// finishes in a fixed number of steps (wait-free). The only shared state is
// two counters. The producer alone writes `tail_`, the consumer alone writes
// `head_`, and each lives on its own cache line so that the two threads never
// invalidate each other's line just by doing their own work.
//
// Each side also keeps a private copy of the other side's counter and only
// re-reads the real one when the ring looks full (producer) or empty
// (consumer). In steady state a push or a pop therefore touches no cache line
// that the other thread is writing.
//
// The counters only ever increase and are masked on use, so the capacity is a
// power of two and every slot is usable.
template <typename T>
  requires std::is_trivially_copyable_v<T> && std::default_initializable<T>
class SpscRing {
 public:
  // The capacity is `min_capacity` rounded up to a power of two. The single
  // allocation happens here.
  explicit SpscRing(std::size_t min_capacity)
      : mask_(std::bit_ceil(min_capacity) - 1), slots_(std::make_unique<T[]>(mask_ + 1)) {}

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer thread only. Returns false if the ring is full.
  [[nodiscard]] bool try_push(const T& value) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_cache_ == capacity()) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (tail - head_cache_ == capacity()) {
        return false;
      }
    }
    slots_[tail & mask_] = value;
    // Release: the consumer must see the slot's contents before the new tail.
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only. Returns false if the ring is empty.
  [[nodiscard]] bool try_pop(T& out) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) {
        return false;
      }
    }
    out = slots_[head & mask_];
    // Release: the producer may reuse the slot only after this read is done.
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only. Passes every item currently in the ring to `visit`,
  // oldest first, then frees all of them with a single store. Returns how
  // many were consumed. `visit` must not throw.
  template <std::invocable<const T&> Visitor>
  std::size_t drain(Visitor&& visit) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    for (std::size_t index = head; index != tail; ++index) {
      visit(slots_[index & mask_]);
    }
    tail_cache_ = tail;
    if (tail != head) {
      head_.store(tail, std::memory_order_release);
    }
    return tail - head;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

  // Exact when neither thread is mid-call; otherwise a snapshot that may
  // already be out of date. Safe to call from any thread.
  [[nodiscard]] std::size_t size() const noexcept {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return std::min(tail - head, capacity());
  }

  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

 private:
  static_assert(std::atomic<std::size_t>::is_always_lock_free);

  // Never written after construction, so both threads can share this line.
  const std::size_t mask_;
  const std::unique_ptr<T[]> slots_;

  // The producer's line.
  alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
  std::size_t head_cache_ = 0;

  // The consumer's line.
  alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
  std::size_t tail_cache_ = 0;
};

}  // namespace lob

#pragma once

#include <cstdint>
#include <limits>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;        // integer ticks; never floating point
using Quantity = std::uint32_t;
using OrderIndex = std::uint32_t;  // slot number inside the OrderPool

inline constexpr OrderIndex kNullIndex = std::numeric_limits<OrderIndex>::max();
inline constexpr OrderId kInvalidOrderId = 0;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
  Limit,   // trade what it can, then rest the remainder until filled or cancelled
  Market,  // trade at any price; never rests
  IOC,     // immediate-or-cancel: trade at the limit price or better; never rests
  FOK,     // fill-or-kill: trade in full immediately or not at all; never rests
};

constexpr Side opposite(Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

}  // namespace lob

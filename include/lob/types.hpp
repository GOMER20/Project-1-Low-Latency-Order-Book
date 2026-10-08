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

constexpr Side opposite(Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

}  // namespace lob

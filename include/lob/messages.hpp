#pragma once

#include <cstdint>
#include <type_traits>

#include "lob/types.hpp"

namespace lob {

enum class CommandType : std::uint8_t { Submit, Cancel };

// A request sent to the engine thread.
struct Command {
  std::uint64_t client_tag;  // chosen by the sender; echoed in every event the command causes
  OrderId order_id;          // Cancel: the order to cancel
  Price price;               // Submit: limit price
  Quantity quantity;         // Submit: order quantity
  Side side;                 // Submit
  CommandType type;
  OrderType order_type;      // Submit: Limit, Market, IOC or FOK
};

static_assert(sizeof(Command) == 32);
static_assert(std::is_trivial_v<Command> && std::is_standard_layout_v<Command>);

enum class EventType : std::uint8_t {
  Accepted,        // a Submit was taken; order_id is the ID the book gave it
  Rejected,        // a Submit was refused: price out of range, zero quantity or book full
  Trade,           // one fill between the command's order and a resting order
  Cancelled,       // a Cancel removed its order
  CancelRejected,  // a Cancel named an order that is not live
};

// Something the engine thread did in response to a command.
//
// Every command produces exactly one of Accepted, Rejected, Cancelled or
// CancelRejected. A Submit that trades produces its Trade events first and its
// Accepted last, so the Accepted event also marks the end of the command.
//
// Market, IOC and FOK orders never rest, so their Accepted event always has
// `resting` equal to zero: whatever they did not fill was discarded.
//
// Fields that do not apply to an event's type are zero.
struct Event {
  std::uint64_t client_tag;  // tag of the command that caused this event
  OrderId order_id;          // the command's order; for a Trade, the incoming order
  OrderId maker_id;          // Trade: the resting order that was hit
  Price price;               // Trade: the fill price. Accepted / Rejected: the limit price
  Quantity quantity;         // Trade: size of this fill. Accepted: total filled on arrival
  Quantity resting;          // Accepted: quantity left resting in the book
  Side side;                 // Submit events: the side of the command's order
  EventType type;
};

static_assert(sizeof(Event) == 48);
static_assert(std::is_trivial_v<Event> && std::is_standard_layout_v<Event>);

}  // namespace lob

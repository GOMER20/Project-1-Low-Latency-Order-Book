#pragma once

#include <cstdint>
#include <type_traits>

#include "lob/types.hpp"

namespace lob {

enum class CommandType : std::uint8_t {
  Submit,
  Cancel,
  // The next two are sent by ShardedEngine to move a symbol from one engine to
  // another; see MatchingEngine::detach(). `quantity` carries the move's number.
  Detach,  // stop running this symbol and say so
  Attach,  // start running this symbol, once it has been let go
  // Asks for a fresh picture of one symbol on the market data feed.
  Snapshot,
  // Never sent to an engine. It appears only in a recording of a session, to
  // say that the symbol was moved to another shard at this point; `quantity`
  // carries the shard it went to. See recording.hpp.
  Move,
};

// A request sent to the engine thread.
//
// Exactly 32 bytes, so two commands fill one cache line and none straddles
// two. To fit, a Submit's price and a Cancel's order ID share the same eight
// bytes; a command only ever uses the one that belongs to its type. Measured
// against a 40-byte layout with separate fields, this is a few percent faster.
struct Command {
  std::uint64_t client_tag;  // chosen by the sender; echoed in every event the command causes
  union {
    Price price;             // Submit: limit price
    OrderId order_id;        // Cancel: the order to cancel
  };
  Quantity quantity;         // Submit: order quantity
  SymbolId symbol;           // which book the command is for
  Side side;                 // Submit
  CommandType type;
  OrderType order_type;      // Submit: Limit, Market, IOC or FOK
};

static_assert(sizeof(Command) == 32);
static_assert(std::is_trivial_v<Command> && std::is_standard_layout_v<Command>);

enum class EventType : std::uint8_t {
  Accepted,        // a Submit was taken; order_id is the ID the book gave it
  Rejected,        // a Submit was refused: unknown symbol, price out of range, zero
                   // quantity or book full
  Trade,           // one fill between the command's order and a resting order
  Cancelled,       // a Cancel removed its order
  CancelRejected,  // a Cancel named an order that is not live, or an unknown symbol
  // Internal. An engine's last word on a symbol it has stopped running, with
  // the move's number in `quantity`. ShardedEngine reads it and never passes
  // it on to its caller.
  Handoff,
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
  SymbolId symbol;           // the book the command was for
  Side side;                 // Submit events: the side of the command's order
  EventType type;
};

static_assert(sizeof(Event) == 48);
static_assert(std::is_trivial_v<Event> && std::is_standard_layout_v<Event>);

// The public market data feed: what anyone may see of a symbol, as opposed to
// the Events above, which tell the sender of an order what became of it.
enum class MarketDataType : std::uint8_t {
  BestPrices,  // the best bid and ask and the quantity at each, after either changed
  Level,       // the total quantity now resting at one price on one side; 0 means none
  Trade,       // a trade: its price, its size and the side of the incoming order
  Clear,       // forget what you know of this symbol's depth: a fresh picture follows
};

// One message on the feed. Exactly one cache line.
//
// Quantities are absolute, never changes: a Level message gives the total at a
// price, not how much was added or removed. A reader that misses a message is
// therefore wrong only about that price, and only until it next changes.
//
// `sequence` counts a symbol's messages from 1 with no gaps, so a jump means
// some were dropped because the reader fell behind. Fields that do not belong
// to a message's type are zero.
struct MarketData {
  std::uint64_t sequence;
  Price price;                 // Level, Trade
  std::uint64_t quantity;      // Level: total resting at `price`. Trade: size of the trade
  Price bid_price;             // BestPrices; meaningful only if bid_quantity is not 0
  std::uint64_t bid_quantity;  // BestPrices: resting at the best bid; 0 if there are no bids
  Price ask_price;             // BestPrices; meaningful only if ask_quantity is not 0
  std::uint64_t ask_quantity;  // BestPrices: resting at the best ask; 0 if there are no asks
  SymbolId symbol;
  Side side;                   // Level: which side. Trade: the incoming order's side
  MarketDataType type;
};

static_assert(sizeof(MarketData) == 64);
static_assert(std::is_trivial_v<MarketData> && std::is_standard_layout_v<MarketData>);

}  // namespace lob

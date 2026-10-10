#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

// The messages HFT exchanges with the program that uses it.
//
// Every message is an 8-byte Header followed by a payload of Header::size
// bytes. Numbers are little-endian. Every struct here is laid out by hand with
// no padding, so the bytes on the wire are exactly the bytes of the struct, on
// any compiler, and a client in another language can pack and unpack them
// field by field (hft/node/hft-client.mjs does).
//
// Prices are whole numbers of ticks; the program using HFT decides what a tick
// is worth. Gomer Trading uses one cent.

namespace hft {

static_assert(std::endian::native == std::endian::little,
              "the wire format is little-endian and is written straight from memory");

inline constexpr std::uint32_t kProtocolVersion = 1;

// A payload larger than this is not a message; the connection is closed.
inline constexpr std::uint32_t kMaxPayload = 1u << 20;

// The most levels a side that one Quote or one Depth can carry.
inline constexpr std::size_t kMaxLevels = 32;

enum class Kind : std::uint16_t {
  // --- To HFT ---
  Hello = 1,         // first message: which books to run
  Order = 2,         // a new order
  Cancel = 3,        // cancel a resting order
  Quote = 4,         // what the house should be bidding and offering in one symbol
  DepthRequest = 5,  // ask for the top of one symbol's book
  StatsRequest = 6,  // ask for the statistics
  Ping = 7,
  Shutdown = 8,      // finish the recording, write the statistics and exit

  // --- From HFT ---
  Welcome = 101,         // answer to Hello
  Accepted = 102,        // an Order was taken
  Rejected = 103,        // an Order was refused
  Fill = 104,            // one of the client's orders traded
  Cancelled = 105,       // a Cancel removed its order
  CancelRejected = 106,  // a Cancel named an order that is not resting
  Depth = 107,           // answer to DepthRequest
  Stats = 108,           // answer to StatsRequest: JSON text
  Pong = 109,
  Error = 110,           // the last message could not be understood: text, then the connection closes
};

struct Header {
  std::uint16_t kind;
  std::uint16_t reserved;  // zero
  std::uint32_t size;      // bytes of payload that follow
};

// --- To HFT --------------------------------------------------------------------

// Hello is this struct followed by `book_count` HelloBooks. A symbol's number,
// used in every other message, is its position in that list.
struct Hello {
  std::uint32_t version;     // kProtocolVersion
  std::uint32_t book_count;
  std::uint32_t flags;       // kHelloFresh, or zero
  std::uint32_t reserved;
};

// Start again with empty books even if HFT is already running these ones.
// Without it, a client that reconnects finds the books as it left them.
inline constexpr std::uint32_t kHelloFresh = 1;

struct HelloBook {
  char symbol[8];            // not NUL-terminated if all eight are used
  std::int64_t min_price;    // lowest price the book takes, in ticks
  std::uint32_t num_levels;  // prices from min_price up to min_price + num_levels - 1
  std::uint32_t max_orders;  // most orders that can rest at once
};

struct Order {
  std::uint64_t tag;       // chosen by the client; comes back in every answer about this order
  std::int64_t price;      // limit price in ticks; ignored for a market order
  std::uint32_t quantity;
  std::uint16_t symbol;
  std::uint8_t side;       // 0 buy, 1 sell
  std::uint8_t type;       // 0 limit, 1 market, 2 immediate-or-cancel, 3 fill-or-kill
};

struct Cancel {
  std::uint64_t tag;       // comes back in the answer
  std::uint64_t order_id;  // from the order's Accepted
  std::uint16_t symbol;
  std::uint8_t reserved[6];
};

// Quote is this struct followed by `bid_levels` QuoteLevels, best first, then
// `ask_levels` more. It says what the house should have resting in the book:
// HFT cancels and places house orders until that is what is there. A side
// with no levels means the house quotes nothing on that side.
//
// Each side's prices must run strictly away from its best, and the best bid
// must be below the best offer. A Quote that breaks either rule, or names a
// symbol that does not exist, is ignored and counted; it is not an error.
struct Quote {
  std::uint16_t symbol;
  std::uint8_t bid_levels;
  std::uint8_t ask_levels;
  std::uint32_t reserved;
};

struct QuoteLevel {
  std::int64_t price;
  std::uint32_t quantity;
  std::uint32_t reserved;
};

struct DepthRequest {
  std::uint16_t symbol;
  std::uint16_t levels;  // how many a side, at most kMaxLevels
  std::uint32_t reserved;
};

struct Ping {
  std::uint64_t value;  // comes back in the Pong
};

// --- From HFT ------------------------------------------------------------------

struct Welcome {
  std::uint32_t version;
  std::uint32_t book_count;
  std::uint64_t session;   // counts up each time HFT starts again with empty books
  std::uint8_t resumed;    // 1 if the books were kept from before this connection
  std::uint8_t reserved[7];
};

struct Accepted {
  std::uint64_t tag;
  std::uint64_t order_id;  // needed to cancel it; unique within its symbol
  std::uint32_t filled;    // traded on arrival; the Fills for it came just before this
  std::uint32_t resting;   // left in the book
  std::uint16_t symbol;
  std::uint8_t reserved[6];
};

struct Rejected {
  std::uint64_t tag;
  std::uint16_t symbol;
  std::uint8_t reserved[6];
};

struct Fill {
  std::uint64_t tag;        // the tag of the client's order that traded
  std::uint64_t order_id;
  std::int64_t price;
  std::uint32_t quantity;   // this fill
  std::uint32_t remaining;  // of the order, after this fill
  std::uint16_t symbol;
  std::uint8_t side;        // the side of the client's order
  std::uint8_t maker;       // 1 if the order was resting, 0 if it was the one arriving
  std::uint8_t with_house;  // 1 if the other side was the house, 0 if another client order
  std::uint8_t reserved[3];
};

// Cancelled and CancelRejected.
struct CancelAnswer {
  std::uint64_t tag;       // the Cancel's tag
  std::uint64_t order_id;
  std::uint16_t symbol;
  std::uint8_t reserved[6];
};

// Depth is this struct followed by `bid_levels` DepthLevels, best first, then
// `ask_levels` more.
struct Depth {
  std::uint16_t symbol;
  std::uint8_t bid_levels;
  std::uint8_t ask_levels;
  std::uint32_t reserved;
};

struct DepthLevel {
  std::int64_t price;
  std::uint64_t quantity;  // everything resting at this price, the house's and the clients'
};

struct Pong {
  std::uint64_t value;
};

template <typename T>
concept WireStruct = std::is_trivially_copyable_v<T> && std::has_unique_object_representations_v<T>;

static_assert(WireStruct<Header> && sizeof(Header) == 8);
static_assert(WireStruct<Hello> && sizeof(Hello) == 16);
static_assert(WireStruct<HelloBook> && sizeof(HelloBook) == 24);
static_assert(WireStruct<Order> && sizeof(Order) == 24);
static_assert(WireStruct<Cancel> && sizeof(Cancel) == 24);
static_assert(WireStruct<Quote> && sizeof(Quote) == 8);
static_assert(WireStruct<QuoteLevel> && sizeof(QuoteLevel) == 16);
static_assert(WireStruct<DepthRequest> && sizeof(DepthRequest) == 8);
static_assert(WireStruct<Ping> && sizeof(Ping) == 8);
static_assert(WireStruct<Welcome> && sizeof(Welcome) == 24);
static_assert(WireStruct<Accepted> && sizeof(Accepted) == 32);
static_assert(WireStruct<Rejected> && sizeof(Rejected) == 16);
static_assert(WireStruct<Fill> && sizeof(Fill) == 40);
static_assert(WireStruct<CancelAnswer> && sizeof(CancelAnswer) == 24);
static_assert(WireStruct<Depth> && sizeof(Depth) == 8);
static_assert(WireStruct<DepthLevel> && sizeof(DepthLevel) == 16);
static_assert(WireStruct<Pong> && sizeof(Pong) == 8);

// --- Writing messages ----------------------------------------------------------

// Bytes waiting to be sent. It grows to the largest burst it has had to hold
// and then stays that size, so in steady state appending allocates nothing.
class Outbox {
 public:
  Outbox() { bytes_.reserve(std::size_t{1} << 16); }

  // Starts a message; follow with put() for each part of the payload.
  void begin(Kind kind) {
    start_ = bytes_.size();
    Header header{};
    header.kind = static_cast<std::uint16_t>(kind);
    put(header);
  }

  template <WireStruct T>
  void put(const T& part) {
    const auto* const from = reinterpret_cast<const std::byte*>(&part);
    bytes_.insert(bytes_.end(), from, from + sizeof(T));
  }

  void put(std::string_view text) {
    const auto* const from = reinterpret_cast<const std::byte*>(text.data());
    bytes_.insert(bytes_.end(), from, from + text.size());
  }

  // Finishes the message begun last by filling in its size.
  void end() {
    const auto size = static_cast<std::uint32_t>(bytes_.size() - start_ - sizeof(Header));
    std::memcpy(bytes_.data() + start_ + offsetof(Header, size), &size, sizeof(size));
  }

  // A whole message with a fixed payload.
  template <WireStruct T>
  void send(Kind kind, const T& payload) {
    begin(kind);
    put(payload);
    end();
  }

  void send(Kind kind, std::string_view text) {
    begin(kind);
    put(text);
    end();
  }

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
  [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }
  void clear() noexcept { bytes_.clear(); }

  // Drops the first `count` bytes, which have been sent.
  void consume(std::size_t count) {
    bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<std::ptrdiff_t>(count));
  }

 private:
  std::vector<std::byte> bytes_;
  std::size_t start_ = 0;
};

// --- Reading messages ----------------------------------------------------------

// Reads a fixed struct from the front of `bytes` and moves `bytes` past it.
// Returns false, leaving `bytes` alone, if there are not enough.
template <WireStruct T>
[[nodiscard]] bool take(std::span<const std::byte>& bytes, T& out) noexcept {
  if (bytes.size() < sizeof(T)) {
    return false;
  }
  std::memcpy(&out, bytes.data(), sizeof(T));
  bytes = bytes.subspan(sizeof(T));
  return true;
}

}  // namespace hft

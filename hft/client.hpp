#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "hft/core.hpp"
#include "hft/wire.hpp"

// The client's side of HFT, in C++: building the messages a client sends,
// reading the ones that come back, and two ways of reaching an HFT: over its
// socket, or straight into a Core in the same program. The simulator and the
// tests use these; a client in another language does the same things with its
// own tools (see hft/node/hft-client.mjs).

namespace hft {

// --- What a client says ---------------------------------------------------------

struct BookSpec {
  std::string_view symbol;
  std::int64_t min_price;
  std::uint32_t num_levels;
  std::uint32_t max_orders;
};

inline void say_hello(Outbox& out, std::span<const BookSpec> books, std::uint32_t flags = 0) {
  Hello hello{};
  hello.version = kProtocolVersion;
  hello.book_count = static_cast<std::uint32_t>(books.size());
  hello.flags = flags;
  out.begin(Kind::Hello);
  out.put(hello);
  for (const BookSpec& book : books) {
    HelloBook wire{};
    std::memcpy(wire.symbol, book.symbol.data(), std::min(book.symbol.size(), sizeof(wire.symbol)));
    wire.min_price = book.min_price;
    wire.num_levels = book.num_levels;
    wire.max_orders = book.max_orders;
    out.put(wire);
  }
  out.end();
}

inline void say_order(Outbox& out, std::uint64_t tag, std::uint16_t symbol, lob::Side side,
                      std::int64_t price, std::uint32_t quantity,
                      lob::OrderType type = lob::OrderType::Limit) {
  Order order{};
  order.tag = tag;
  order.price = price;
  order.quantity = quantity;
  order.symbol = symbol;
  order.side = static_cast<std::uint8_t>(side);
  order.type = static_cast<std::uint8_t>(type);
  out.send(Kind::Order, order);
}

inline void say_cancel(Outbox& out, std::uint64_t tag, std::uint16_t symbol,
                       std::uint64_t order_id) {
  Cancel cancel{};
  cancel.tag = tag;
  cancel.order_id = order_id;
  cancel.symbol = symbol;
  out.send(Kind::Cancel, cancel);
}

struct HouseLevel {
  std::int64_t price;
  std::uint32_t quantity;
};

// What the house should be bidding and offering in one symbol, best first.
inline void say_quote(Outbox& out, std::uint16_t symbol, std::span<const HouseLevel> bids,
                      std::span<const HouseLevel> asks) {
  Quote quote{};
  quote.symbol = symbol;
  quote.bid_levels = static_cast<std::uint8_t>(bids.size());
  quote.ask_levels = static_cast<std::uint8_t>(asks.size());
  out.begin(Kind::Quote);
  out.put(quote);
  for (const std::span<const HouseLevel> side : {bids, asks}) {
    for (const HouseLevel& level : side) {
      QuoteLevel wire{};
      wire.price = level.price;
      wire.quantity = level.quantity;
      out.put(wire);
    }
  }
  out.end();
}

inline void say_depth_request(Outbox& out, std::uint16_t symbol, std::uint16_t levels) {
  DepthRequest request{};
  request.symbol = symbol;
  request.levels = levels;
  out.send(Kind::DepthRequest, request);
}

inline void say_ping(Outbox& out, std::uint64_t value) { out.send(Kind::Ping, Ping{value}); }

inline void say_stats_request(Outbox& out) { out.send(Kind::StatsRequest, std::string_view{}); }

inline void say_shutdown(Outbox& out) { out.send(Kind::Shutdown, std::string_view{}); }

// --- Reading what comes back ----------------------------------------------------

// Passes each whole message at the front of `bytes` to `visit(kind, payload)`
// and returns how many bytes those messages took up. A message that has only
// partly arrived is left for next time.
template <typename Visitor>
std::size_t for_each_message(std::span<const std::byte> bytes, Visitor&& visit) {
  std::size_t at = 0;
  while (bytes.size() - at >= sizeof(Header)) {
    Header header{};
    std::memcpy(&header, bytes.data() + at, sizeof(header));
    if (bytes.size() - at < sizeof(Header) + header.size) {
      break;
    }
    visit(static_cast<Kind>(header.kind), bytes.subspan(at + sizeof(Header), header.size));
    at += sizeof(Header) + header.size;
  }
  return at;
}

// Reads a fixed struct from a payload; zeroes if the payload is too short.
template <WireStruct T>
[[nodiscard]] T read_as(std::span<const std::byte> payload) noexcept {
  T value{};
  static_cast<void>(take(payload, value));
  return value;
}

[[nodiscard]] inline std::string_view text_of(std::span<const std::byte> payload) noexcept {
  return {reinterpret_cast<const char*>(payload.data()), payload.size()};
}

// --- Reaching an HFT ------------------------------------------------------------

// Straight into a Core in this program: no socket, no copying between
// processes. What the simulator uses to measure HFT itself.
class DirectLink {
 public:
  explicit DirectLink(Core& core) : core_(core) {}

  // Hands every message in `requests` to the Core, then passes each reply to
  // `visit(kind, payload)`. Empties `requests`. Returns false if the Core
  // could not understand a message.
  template <typename Visitor>
  bool exchange(Outbox& requests, Visitor&& visit) {
    bool understood = true;
    for_each_message(requests.bytes(), [&](Kind kind, std::span<const std::byte> payload) {
      understood = understood && core_.handle(kind, payload, replies_);
    });
    requests.clear();
    for_each_message(replies_.bytes(), visit);
    replies_.clear();
    return understood;
  }

 private:
  Core& core_;
  Outbox replies_;
};

// Over HFT's socket, the way a separate program reaches it.
class SocketLink {
 public:
  SocketLink() = default;
  ~SocketLink() { close(); }

  SocketLink(const SocketLink&) = delete;
  SocketLink& operator=(const SocketLink&) = delete;

  [[nodiscard]] bool connect(const std::string& path) {
    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path)) {
      return false;
    }
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      close();
      return false;
    }
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    // Never block in a send: see send_bytes().
    ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
    return true;
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    pending_.clear();
  }

  [[nodiscard]] bool connected() const noexcept { return fd_ >= 0; }

  // Sends raw bytes, as they are.
  //
  // HFT answers as it reads, so while a large batch is still going out its
  // answers are already coming back. A client that only sent until it was
  // done, and only then read, would fill the socket in one direction while
  // HFT filled it in the other, and both would wait for ever. So whenever the
  // socket will not take more, this reads what has arrived and keeps it for
  // exchange() or receive().
  [[nodiscard]] bool send_bytes(std::span<const std::byte> bytes, int patience_ms = 10'000) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    while (!bytes.empty()) {
      const ssize_t sent = ::send(fd_, bytes.data(), bytes.size(), flags);
      if (sent > 0) {
        bytes = bytes.subspan(static_cast<std::size_t>(sent));
        continue;
      }
      if (sent < 0 && errno == EINTR) {
        continue;
      }
      if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        return false;
      }
      pollfd waiting{fd_, POLLIN | POLLOUT, 0};
      const int ready = ::poll(&waiting, 1, patience_ms);
      if (ready == 0 || (ready < 0 && errno != EINTR)) {
        return false;
      }
      if (ready > 0 && (waiting.revents & POLLIN) != 0 && !read_available()) {
        return false;
      }
    }
    return true;
  }

  // Sends every message in `requests`, then waits until HFT has answered all
  // of them, passing each reply to `visit(kind, payload)`. Empties `requests`.
  //
  // HFT answers in order, so a Ping sent last comes back last: its Pong marks
  // the end. Returns false if the connection was lost, or HFT said nothing
  // for `patience_ms`.
  template <typename Visitor>
  bool exchange(Outbox& requests, Visitor&& visit, int patience_ms = 10'000) {
    const std::uint64_t marker = ++pings_;
    say_ping(requests, marker);
    const bool sent = send_bytes(requests.bytes());
    requests.clear();
    if (!sent) {
      return false;
    }
    bool done = false;
    bool first = true;
    while (!done) {
      // Some of the answers may have arrived while the requests were going out.
      if (!(first && !pending_.empty()) && !read_more(patience_ms)) {
        return false;
      }
      first = false;
      const std::size_t used =
          for_each_message(pending_, [&](Kind kind, std::span<const std::byte> payload) {
            if (kind == Kind::Pong && read_as<Pong>(payload).value == marker) {
              done = true;
            } else {
              visit(kind, payload);
            }
          });
      pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used));
    }
    return true;
  }

  // Waits for more bytes and passes on any whole messages. For reading what
  // HFT says without having asked: an Error before it hangs up, for one.
  // Returns false if the connection closed or nothing came in time.
  template <typename Visitor>
  bool receive(Visitor&& visit, int patience_ms = 10'000) {
    if (!read_more(patience_ms)) {
      return false;
    }
    const std::size_t used = for_each_message(pending_, visit);
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used));
    return true;
  }

 private:
  // Waits for bytes to arrive and takes them. False if none came in time, or
  // the connection is closed.
  [[nodiscard]] bool read_more(int patience_ms) {
    pollfd waiting{fd_, POLLIN, 0};
    int ready = 0;
    do {
      ready = ::poll(&waiting, 1, patience_ms);
    } while (ready < 0 && errno == EINTR);
    return ready > 0 && read_available();
  }

  // Takes whatever has arrived. False if the connection is closed.
  [[nodiscard]] bool read_available() {
    std::byte buffer[1 << 16];
    const ssize_t got = ::recv(fd_, buffer, sizeof(buffer), 0);
    if (got > 0) {
      pending_.insert(pending_.end(), buffer, buffer + got);
      return true;
    }
    return got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
  }

  int fd_ = -1;
  std::uint64_t pings_ = 0;
  std::vector<std::byte> pending_;
};

}  // namespace hft

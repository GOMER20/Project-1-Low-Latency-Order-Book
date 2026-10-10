#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

// What HFT measures about itself while it runs, and how it writes that down:
// as JSON for a program to read, and as a page of text for a person.

namespace hft {

// How long things took, kept as counts in buckets rather than as a list of
// samples, so that recording one costs a few instructions and no memory.
//
// Each power of two is split into eight buckets, which makes any figure read
// back from it right to within an eighth: 12.5%. Samples are in nanoseconds;
// the buckets cover everything up to several minutes.
class Histogram {
 public:
  void record(std::uint64_t nanoseconds) noexcept {
    ++counts_[bucket_of(nanoseconds)];
    ++count_;
    sum_ += nanoseconds;
    max_ = std::max(max_, nanoseconds);
    min_ = std::min(min_, nanoseconds);
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t max() const noexcept { return count_ != 0 ? max_ : 0; }
  [[nodiscard]] std::uint64_t min() const noexcept { return count_ != 0 ? min_ : 0; }
  [[nodiscard]] std::uint64_t mean() const noexcept { return count_ != 0 ? sum_ / count_ : 0; }

  // The value that this fraction of the samples were at or below: 0.5 is the
  // median, 0.99 the 99th percentile. It is the top of the bucket the answer
  // falls in, so it errs on the side of saying things were slower.
  [[nodiscard]] std::uint64_t percentile(double fraction) const noexcept {
    if (count_ == 0) {
      return 0;
    }
    const auto wanted = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(fraction * static_cast<double>(count_) + 0.999999));
    std::uint64_t seen = 0;
    // The last bucket also holds everything too large for the others, so its
    // top says nothing; the largest sample does.
    for (std::size_t bucket = 0; bucket + 1 < counts_.size(); ++bucket) {
      seen += counts_[bucket];
      if (seen >= wanted) {
        return std::min(top_of(bucket), max_);
      }
    }
    return max_;
  }

  void reset() noexcept { *this = Histogram{}; }

 private:
  static constexpr std::size_t kSubBuckets = 8;
  static constexpr std::size_t kBuckets = 40 * kSubBuckets;

  // Values below 8 get a bucket each. Above that, the top three bits after the
  // leading one choose among the eight buckets of the value's power of two.
  [[nodiscard]] static std::size_t bucket_of(std::uint64_t value) noexcept {
    if (value < kSubBuckets) {
      return static_cast<std::size_t>(value);
    }
    const int top = std::bit_width(value) - 1;  // position of the leading one, 3 or more
    const auto sub = static_cast<std::size_t>((value >> (top - 3)) & 7);
    const std::size_t bucket = (static_cast<std::size_t>(top) - 2) * kSubBuckets + sub;
    return std::min(bucket, kBuckets - 1);
  }

  // The largest value that falls in a bucket.
  [[nodiscard]] static std::uint64_t top_of(std::size_t bucket) noexcept {
    if (bucket < kSubBuckets) {
      return bucket;
    }
    const std::size_t top = bucket / kSubBuckets + 2;
    const std::uint64_t sub = bucket % kSubBuckets;
    const std::uint64_t base = (std::uint64_t{8} + sub) << (top - 3);
    return base + ((std::uint64_t{1} << (top - 3)) - 1);
  }

  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t count_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t max_ = 0;
  std::uint64_t min_ = ~std::uint64_t{0};
};

// What has happened in one symbol.
struct SymbolStats {
  std::string name;
  std::uint64_t orders = 0;         // client orders received
  std::uint64_t trades = 0;
  std::uint64_t shares = 0;         // traded
  std::int64_t last_price = 0;      // of the latest trade; meaningful once trades is not 0
  std::uint64_t quotes = 0;         // house quote updates
};

// Everything HFT counts. One of these lasts for one session: from the books
// being created empty to their being thrown away.
struct Stats {
  // --- When ---
  std::uint64_t session = 0;
  std::chrono::system_clock::time_point started_wall{};
  std::chrono::steady_clock::time_point started{};

  // --- What came in ---
  std::uint64_t orders = 0;            // client orders
  std::uint64_t cancels = 0;           // client cancels
  std::uint64_t quotes = 0;            // house quote updates
  std::uint64_t quotes_refused = 0;    // house quotes that made no sense and were ignored
  std::uint64_t depth_requests = 0;

  // --- What became of the client's orders ---
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t cancel_rejected = 0;
  std::uint64_t fills = 0;             // Fill messages sent: one for each client order in a trade

  // --- Trading ---
  std::uint64_t trades = 0;
  std::uint64_t trades_between_clients = 0;  // neither side was the house
  std::uint64_t shares = 0;
  std::int64_t notional = 0;           // sum of price x quantity, in ticks

  // --- The house ---
  std::uint64_t house_orders = 0;      // placed
  std::uint64_t house_cancels = 0;
  std::uint64_t house_rejected = 0;    // refused by the book: full, or outside its prices

  // --- The engine underneath ---
  std::uint64_t engine_commands = 0;   // everything sent to the matching engine
  std::uint64_t engine_events = 0;
  std::uint64_t events_dropped = 0;    // must stay 0
  std::uint64_t recorder_waits = 0;    // times a command waited for the recording to catch up
  std::uint64_t digest = 0;            // fingerprint of every event so far
  std::uint64_t commands_recorded = 0;
  bool recording = false;
  std::string recording_file;
  std::uint64_t recording_limit = 0;   // most commands the recording will hold; 0 for no limit
  bool recording_stopped = false;      // the limit was reached; the session went on unrecorded
  std::uint64_t recorded_digest = 0;   // fingerprint of the recorded part: what a replay must give

  // --- Speed ---
  Histogram order_time;                // receiving a client order or cancel to having answered it
  Histogram quote_time;                // the same for a house quote update
  std::uint64_t busiest_second = 0;    // most messages handled in one second
  std::uint64_t this_second = 0;
  std::int64_t this_second_started = -1;

  // --- Now ---
  std::uint64_t resting_client_orders = 0;
  std::uint64_t resting_house_orders = 0;

  std::vector<SymbolStats> symbols;

  [[nodiscard]] double uptime_seconds() const noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  }

  // Counts one handled message towards the busiest second.
  void count_message(std::chrono::steady_clock::time_point now) noexcept {
    const std::int64_t second =
        std::chrono::duration_cast<std::chrono::seconds>(now - started).count();
    if (second != this_second_started) {
      this_second_started = second;
      this_second = 0;
    }
    busiest_second = std::max(busiest_second, ++this_second);
  }
};

namespace detail {

inline void append(std::string& out, std::string_view text) { out.append(text); }

inline void append_number(std::string& out, std::uint64_t value) { out.append(std::to_string(value)); }
inline void append_number(std::string& out, std::int64_t value) { out.append(std::to_string(value)); }

inline void append_number(std::string& out, double value) {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.3f", value);
  out.append(buffer);
}

// Names here are tickers and file paths. Quotes, backslashes and control
// characters are the only things that could break the JSON.
inline void append_string(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
      out.append(buffer);
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
}

template <typename T>
void field(std::string& out, std::string_view name, const T& value, bool last = false) {
  append_string(out, name);
  out.push_back(':');
  append_number(out, value);
  if (!last) {
    out.push_back(',');
  }
}

inline void field_text(std::string& out, std::string_view name, std::string_view value) {
  append_string(out, name);
  out.push_back(':');
  append_string(out, value);
  out.push_back(',');
}

inline void field_bool(std::string& out, std::string_view name, bool value) {
  append_string(out, name);
  out.append(value ? ":true," : ":false,");
}

inline void histogram(std::string& out, std::string_view name, const Histogram& h) {
  append_string(out, name);
  out.append(":{");
  field(out, "count", h.count());
  field(out, "min_ns", h.min());
  field(out, "mean_ns", h.mean());
  field(out, "p50_ns", h.percentile(0.50));
  field(out, "p90_ns", h.percentile(0.90));
  field(out, "p99_ns", h.percentile(0.99));
  field(out, "p999_ns", h.percentile(0.999));
  field(out, "max_ns", h.max(), /*last=*/true);
  out.append("},");
}

[[nodiscard]] inline std::string iso_time(std::chrono::system_clock::time_point when) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

[[nodiscard]] inline std::string hex(std::uint64_t value) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
  return buffer;
}

// 1234567 -> "1,234,567".
[[nodiscard]] inline std::string grouped(std::uint64_t value) {
  std::string digits = std::to_string(value);
  for (std::ptrdiff_t at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) {
    digits.insert(static_cast<std::size_t>(at), ",");
  }
  return digits;
}

// An amount of money to two places: 1234567.891 -> "1,234,567.89".
[[nodiscard]] inline std::string money(double amount) {
  const auto hundredths = static_cast<std::uint64_t>(amount < 0 ? 0 : amount * 100.0 + 0.5);
  char cents[8];
  std::snprintf(cents, sizeof(cents), ".%02u", static_cast<unsigned>(hundredths % 100));
  return grouped(hundredths / 100) + cents;
}

// A time in whichever unit reads best: "740 ns", "12.4 us", "3.1 ms".
[[nodiscard]] inline std::string duration(std::uint64_t nanoseconds) {
  char buffer[32];
  if (nanoseconds < 1'000) {
    std::snprintf(buffer, sizeof(buffer), "%llu ns", static_cast<unsigned long long>(nanoseconds));
  } else if (nanoseconds < 1'000'000) {
    std::snprintf(buffer, sizeof(buffer), "%.1f us", static_cast<double>(nanoseconds) / 1e3);
  } else if (nanoseconds < 1'000'000'000) {
    std::snprintf(buffer, sizeof(buffer), "%.1f ms", static_cast<double>(nanoseconds) / 1e6);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.2f s", static_cast<double>(nanoseconds) / 1e9);
  }
  return buffer;
}

}  // namespace detail

// The statistics as JSON, for the program using HFT.
[[nodiscard]] inline std::string to_json(const Stats& stats) {
  using namespace detail;
  std::string out;
  out.reserve(4'096 + stats.symbols.size() * 160);
  out.push_back('{');
  field_text(out, "engine", "HFT");
  field(out, "session", stats.session);
  field_text(out, "started", iso_time(stats.started_wall));
  field(out, "uptime_seconds", stats.uptime_seconds());

  field(out, "orders", stats.orders);
  field(out, "cancels", stats.cancels);
  field(out, "quotes", stats.quotes);
  field(out, "quotes_refused", stats.quotes_refused);
  field(out, "depth_requests", stats.depth_requests);

  field(out, "accepted", stats.accepted);
  field(out, "rejected", stats.rejected);
  field(out, "cancelled", stats.cancelled);
  field(out, "cancel_rejected", stats.cancel_rejected);
  field(out, "fills", stats.fills);

  field(out, "trades", stats.trades);
  field(out, "trades_between_clients", stats.trades_between_clients);
  field(out, "shares", stats.shares);
  field(out, "notional_ticks", stats.notional);

  field(out, "house_orders", stats.house_orders);
  field(out, "house_cancels", stats.house_cancels);
  field(out, "house_rejected", stats.house_rejected);

  field(out, "engine_commands", stats.engine_commands);
  field(out, "engine_events", stats.engine_events);
  field(out, "events_dropped", stats.events_dropped);
  field(out, "recorder_waits", stats.recorder_waits);
  field_text(out, "digest", hex(stats.digest));
  field_bool(out, "recording", stats.recording);
  field_text(out, "recording_file", stats.recording_file);
  field(out, "commands_recorded", stats.commands_recorded);
  field(out, "recording_limit", stats.recording_limit);
  field_bool(out, "recording_stopped", stats.recording_stopped);
  field_text(out, "recorded_digest", hex(stats.recorded_digest));

  histogram(out, "order_time", stats.order_time);
  histogram(out, "quote_time", stats.quote_time);
  field(out, "busiest_second", stats.busiest_second);

  field(out, "resting_client_orders", stats.resting_client_orders);
  field(out, "resting_house_orders", stats.resting_house_orders);

  append_string(out, "symbols");
  out.append(":[");
  for (std::size_t index = 0; index < stats.symbols.size(); ++index) {
    const SymbolStats& symbol = stats.symbols[index];
    out.push_back('{');
    field_text(out, "symbol", symbol.name);
    field(out, "orders", symbol.orders);
    field(out, "quotes", symbol.quotes);
    field(out, "trades", symbol.trades);
    field(out, "shares", symbol.shares);
    field(out, "last_price", symbol.last_price, /*last=*/true);
    out.push_back('}');
    if (index + 1 != stats.symbols.size()) {
      out.push_back(',');
    }
  }
  out.append("]}");
  return out;
}

// The same as a page of text. `tick` is what one price tick is worth, for
// showing the value traded in money: 0.01 when a tick is a cent.
[[nodiscard]] inline std::string to_text(const Stats& stats, double tick = 0.01) {
  using namespace detail;
  std::string out;
  const auto line = [&](std::string_view label, const std::string& value) {
    out.append("  ");
    out.append(label);
    if (label.size() < 34) {
      out.append(34 - label.size(), ' ');
    }
    out.append(value);
    out.push_back('\n');
  };
  const auto speed = [&](std::string_view title, const Histogram& h) {
    out.append(title);
    out.push_back('\n');
    if (h.count() == 0) {
      out.append("  nothing to measure yet\n");
      return;
    }
    line("typical (half were faster)", duration(h.percentile(0.50)));
    line("9 in 10 faster than", duration(h.percentile(0.90)));
    line("99 in 100 faster than", duration(h.percentile(0.99)));
    line("999 in 1,000 faster than", duration(h.percentile(0.999)));
    line("slowest", duration(h.max()));
    line("average", duration(h.mean()));
  };

  const double uptime = stats.uptime_seconds();
  out.append("HFT session ");
  out.append(std::to_string(stats.session));
  out.append("\n");
  line("started (UTC)", iso_time(stats.started_wall));
  {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.1f minutes", uptime / 60.0);
    line("ran for", buffer);
  }

  out.append("\nWhat it was asked to do\n");
  line("orders", grouped(stats.orders));
  line("cancels", grouped(stats.cancels));
  line("house quote updates", grouped(stats.quotes));
  line("busiest second", grouped(stats.busiest_second) + " messages");

  out.append("\nWhat became of the orders\n");
  line("accepted", grouped(stats.accepted));
  line("refused", grouped(stats.rejected));
  line("cancelled", grouped(stats.cancelled));
  line("cancels refused", grouped(stats.cancel_rejected));

  out.append("\nTrading\n");
  line("trades", grouped(stats.trades));
  line("  of which between two clients", grouped(stats.trades_between_clients));
  line("shares traded", grouped(stats.shares));
  line("value traded", money(static_cast<double>(stats.notional) * tick));
  line("resting now: client orders", grouped(stats.resting_client_orders));
  line("resting now: house orders", grouped(stats.resting_house_orders));

  out.push_back('\n');
  speed("Time to handle an order or a cancel", stats.order_time);
  out.push_back('\n');
  speed("Time to handle a house quote update", stats.quote_time);

  out.append("\nUnderneath\n");
  line("commands to the matching engine", grouped(stats.engine_commands));
  line("events from it", grouped(stats.engine_events));
  line("events lost", grouped(stats.events_dropped));
  line("house quotes ignored", grouped(stats.quotes_refused));
  line("house orders refused by a book", grouped(stats.house_rejected));
  line("recording", stats.recording ? stats.recording_file : std::string("off"));
  if (stats.recording) {
    line("commands recorded", grouped(stats.commands_recorded));
    if (stats.recording_stopped) {
      line("", "the recording reached its limit of " + grouped(stats.recording_limit) +
                   " commands and stopped there");
      line("fingerprint of the recorded part", hex(stats.recorded_digest));
    }
  }
  line("fingerprint of the session", hex(stats.digest));

  std::vector<const SymbolStats*> busiest;
  for (const SymbolStats& symbol : stats.symbols) {
    if (symbol.orders != 0 || symbol.trades != 0) {
      busiest.push_back(&symbol);
    }
  }
  std::sort(busiest.begin(), busiest.end(), [](const SymbolStats* a, const SymbolStats* b) {
    return a->shares != b->shares ? a->shares > b->shares : a->name < b->name;
  });
  if (!busiest.empty()) {
    out.append("\nBusiest symbols        orders      trades        shares\n");
    for (std::size_t index = 0; index < std::min<std::size_t>(busiest.size(), 10); ++index) {
      const SymbolStats& symbol = *busiest[index];
      char buffer[96];
      std::snprintf(buffer, sizeof(buffer), "  %-14s %12s %11s %13s\n", symbol.name.c_str(),
                    grouped(symbol.orders).c_str(), grouped(symbol.trades).c_str(),
                    grouped(symbol.shares).c_str());
      out.append(buffer);
    }
  }
  return out;
}

}  // namespace hft

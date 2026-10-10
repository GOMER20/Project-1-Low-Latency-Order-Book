#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "lob/compiler.hpp"
#include "lob/messages.hpp"
#include "lob/order_book.hpp"
#include "lob/spsc_ring.hpp"

// Recording a session, to replay it later.
//
// An order book is a deterministic machine: what it does depends on nothing
// but the commands it is given and the order they arrive in. Order IDs, fills,
// rejections and market data all follow from that. So a session is recorded by
// writing down its commands, and reproduced by feeding the same commands to a
// fresh engine; see replay.hpp.
//
// The file is a small header, one entry per book, then one 32-byte entry per
// command, in the order the commands were sent:
//
//   RecordingHeader | RecordedBook x books | RecordedCommand ... to the end
//
// There is no count and no end marker, so a recording cut short by a crash is
// still good as far as it goes.

namespace lob {

// --- The file's layout ---------------------------------------------------------
// Each struct is laid out by hand with no padding, so a file's bytes do not
// depend on the compiler, and are the same every time the same session is
// recorded. Numbers are stored the way the recording machine holds them; the
// header says which way that is, and a file from a machine of the other kind
// is turned down rather than misread.

inline constexpr char kRecordingMagic[8] = {'L', 'O', 'B', '-', 'R', 'E', 'C', '\n'};
inline constexpr std::uint32_t kRecordingByteOrder = 0x01020304;
inline constexpr std::uint32_t kRecordingVersion = 1;

struct RecordingHeader {
  char magic[8];
  std::uint32_t byte_order;    // kRecordingByteOrder, as the recording machine wrote it
  std::uint32_t version;
  std::uint32_t book_count;
  std::uint32_t command_size;  // sizeof(RecordedCommand)
};

struct RecordedBook {
  char symbol[8];  // not NUL-terminated if all eight are used
  std::int64_t min_price;
  std::uint64_t num_levels;
  std::uint64_t max_orders;
};

struct RecordedCommand {
  std::uint64_t client_tag;
  std::uint64_t price_or_order_id;  // Cancel: the order ID. Otherwise the price
  std::uint32_t quantity;           // Submit: the quantity. Move: the shard moved to
  std::uint16_t symbol;
  std::uint8_t side;
  std::uint8_t type;                // a CommandType: Submit, Cancel, Snapshot or Move
  std::uint8_t order_type;
  std::uint8_t unused[7];           // always zero
};

static_assert(sizeof(RecordingHeader) == 24 &&
              std::has_unique_object_representations_v<RecordingHeader>);
static_assert(sizeof(RecordedBook) == 32 &&
              std::has_unique_object_representations_v<RecordedBook>);
static_assert(sizeof(RecordedCommand) == 32 &&
              std::has_unique_object_representations_v<RecordedCommand>);

[[nodiscard]] inline RecordedCommand to_recorded(const Command& command) noexcept {
  RecordedCommand recorded{};
  recorded.client_tag = command.client_tag;
  recorded.price_or_order_id = command.type == CommandType::Cancel
                                   ? command.order_id
                                   : static_cast<std::uint64_t>(command.price);
  recorded.quantity = command.quantity;
  recorded.symbol = command.symbol;
  recorded.side = static_cast<std::uint8_t>(command.side);
  recorded.type = static_cast<std::uint8_t>(command.type);
  recorded.order_type = static_cast<std::uint8_t>(command.order_type);
  return recorded;
}

[[nodiscard]] inline Command from_recorded(const RecordedCommand& recorded) noexcept {
  Command command{};
  command.client_tag = recorded.client_tag;
  command.type = static_cast<CommandType>(recorded.type);
  if (command.type == CommandType::Cancel) {
    command.order_id = recorded.price_or_order_id;
  } else {
    command.price = static_cast<Price>(recorded.price_or_order_id);
  }
  command.quantity = recorded.quantity;
  command.symbol = recorded.symbol;
  command.side = static_cast<Side>(recorded.side);
  command.order_type = static_cast<OrderType>(recorded.order_type);
  return command;
}

// Whether these 32 bytes are something a recorder could have written. The
// symbol is not looked at: an engine may be sent a command for a symbol it
// does not have, which it answers as refused, and that is part of the session
// like any other command.
[[nodiscard]] inline bool is_valid(const RecordedCommand& recorded) noexcept {
  const auto type = static_cast<CommandType>(recorded.type);
  const bool known_type = type == CommandType::Submit || type == CommandType::Cancel ||
                          type == CommandType::Snapshot || type == CommandType::Move;
  return known_type && recorded.side <= static_cast<std::uint8_t>(Side::Sell) &&
         recorded.order_type <= static_cast<std::uint8_t>(OrderType::FOK) &&
         std::all_of(std::begin(recorded.unused), std::end(recorded.unused),
                     [](std::uint8_t byte) { return byte == 0; });
}

// --- Writing --------------------------------------------------------------------

// Writes a session's commands to a file as they are sent.
//
//   gateway thread --record()--> [ ring ] --> writer thread --> file
//
// The thread sending commands never touches the file. All it does is copy the
// command into a ring, which costs a few nanoseconds; a thread of the
// recorder's own takes the commands from the ring and writes them out. A slow
// disk therefore delays the writer, not the sender, for as long as the ring
// has room.
//
// When the ring does fill, the recorder does not drop anything: a recording
// with a hole in it could not reproduce the session. can_record() says there
// is no room, and the engine then refuses the command, exactly as it does
// when its own command ring is full, and the sender tries again.
//
// MatchingEngine and ShardedEngine create one of these when their config names
// a file to record to; there is normally no need to use it directly.
class SessionRecorder {
 public:
  // Opens the file and writes the header and the books. If that fails the
  // recorder still accepts commands, and throws them away; check ok().
  SessionRecorder(const std::string& path, std::span<const BookConfig> books,
                  std::size_t capacity)
      : ring_(capacity), buffer_(kFileBufferSize), file_(std::fopen(path.c_str(), "wb")) {
    bool good = file_ != nullptr;
    if (good) {
      // A buffer of our own, so that the size of each write to the operating
      // system does not depend on the C library's default.
      std::setvbuf(file_, buffer_.data(), _IOFBF, buffer_.size());

      RecordingHeader header{};
      std::copy(std::begin(kRecordingMagic), std::end(kRecordingMagic), header.magic);
      header.byte_order = kRecordingByteOrder;
      header.version = kRecordingVersion;
      header.book_count = static_cast<std::uint32_t>(books.size());
      header.command_size = sizeof(RecordedCommand);
      good = std::fwrite(&header, sizeof(header), 1, file_) == 1;

      for (const BookConfig& book : books) {
        RecordedBook recorded{};
        std::copy_n(book.symbol.begin(), std::min(book.symbol.size(), sizeof(recorded.symbol)),
                    recorded.symbol);
        recorded.min_price = book.min_price;
        recorded.num_levels = book.num_levels;
        recorded.max_orders = book.max_orders;
        good = good && std::fwrite(&recorded, sizeof(recorded), 1, file_) == 1;
      }
      good = good && std::fflush(file_) == 0;
    }
    ok_.store(good, std::memory_order_release);
    writer_ = std::thread([this] { run(); });
  }

  // Writes out whatever is still in the ring, then closes the file. Nothing
  // may be recorded while this runs.
  ~SessionRecorder() {
    stop_.store(true, std::memory_order_release);
    writer_.join();
    if (file_ != nullptr) {
      std::fclose(file_);
    }
  }

  SessionRecorder(const SessionRecorder&) = delete;
  SessionRecorder& operator=(const SessionRecorder&) = delete;

  // --- The thread sending commands ---------------------------------------------

  // Whether there is room to record one more command. If there is, the next
  // record() is certain to fit.
  [[nodiscard]] bool can_record() noexcept { return ring_.can_push(); }

  // Call only after can_record() has returned true.
  void record(const Command& command) noexcept {
    const bool recorded = ring_.try_push(command);
    assert(recorded);
    static_cast<void>(recorded);
  }

  // --- Any thread ----------------------------------------------------------------

  // Returns once every command recorded before the call has been handed to
  // the operating system, so that another program, or this one, can read it
  // from the file. It does not force the data onto the disk itself. It can
  // take some milliseconds if the recorder had nothing to do when asked.
  void flush() noexcept {
    const std::uint64_t mine = flush_requested_.fetch_add(1, std::memory_order_acq_rel) + 1;
    while (flush_served_.load(std::memory_order_acquire) < mine) {
      std::this_thread::yield();
    }
  }

  // False if the file could not be opened, or a write to it has failed. From
  // then on nothing more is written, though commands are still accepted so
  // that the engine carries on.
  [[nodiscard]] bool ok() const noexcept { return ok_.load(std::memory_order_acquire); }

  // Commands written so far. It trails what has been recorded until flush()
  // returns.
  [[nodiscard]] std::uint64_t written() const noexcept {
    return written_.load(std::memory_order_acquire);
  }

 private:
  static constexpr std::size_t kFileBufferSize = std::size_t{1} << 18;
  static constexpr std::size_t kBlock = 1'024;  // commands handed to the file at a time
  static constexpr std::chrono::microseconds kShortNap{100};
  static constexpr std::chrono::microseconds kLongNap{12'800};

  void run() noexcept {
    bool unflushed = false;
    std::chrono::microseconds nap = kShortNap;
    for (;;) {
      // These two are read before the ring is. Whatever was recorded before a
      // flush was asked for, or before the recorder was told to stop, is then
      // certain to be in the drain that follows.
      const bool stopping = stop_.load(std::memory_order_acquire);
      const std::uint64_t requested = flush_requested_.load(std::memory_order_acquire);

      // Commands are put into the file's layout a block at a time and handed
      // over a block at a time: one call per command would cost more than
      // everything else this thread does.
      std::uint64_t wrote = 0;
      std::size_t waiting = 0;
      const auto write_block = [&] {
        if (waiting != 0 && ok_.load(std::memory_order_relaxed)) {
          const std::size_t done = std::fwrite(block_.data(), sizeof(RecordedCommand), waiting, file_);
          wrote += done;
          if (done != waiting) {
            ok_.store(false, std::memory_order_release);
          }
        }
        waiting = 0;
      };
      const std::size_t taken = ring_.drain([&](const Command& command) {
        block_[waiting++] = to_recorded(command);
        if (waiting == block_.size()) {
          write_block();
        }
      });
      write_block();
      if (wrote != 0) {
        written_.store(written_.load(std::memory_order_relaxed) + wrote,
                       std::memory_order_release);
        unflushed = true;
      }

      // Hand what has been written to the operating system when someone is
      // waiting for it, and otherwise whenever there is a lull.
      const bool asked = requested != flush_served_.load(std::memory_order_relaxed);
      if (unflushed && (asked || stopping || taken == 0)) {
        if (std::fflush(file_) != 0) {
          ok_.store(false, std::memory_order_release);
        }
        unflushed = false;
      }
      if (asked) {
        flush_served_.store(requested, std::memory_order_release);
      }
      if (stopping) {
        return;
      }

      // Sleep between rounds rather than hover over the ring: every look at it
      // takes the cache line the sender is writing to. Only when the ring is
      // filling up, or someone is waiting on a flush, is there any hurry.
      //
      // While nothing at all is arriving the naps grow longer, so that a
      // recorder with nothing to do wakes a few dozen times a second, not ten
      // thousand. The first command to arrive brings them back down.
      if (asked || taken != 0) {
        nap = kShortNap;
      }
      if (!asked && (taken == 0 || taken < ring_.capacity() / 2)) {
        std::this_thread::sleep_for(nap);
        if (taken == 0) {
          nap = std::min(nap * 2, kLongNap);
        }
      }
    }
  }

  SpscRing<Command> ring_;
  std::vector<char> buffer_;  // the file's buffer; must outlive the file being open
  std::FILE* file_;           // after construction, used only by the writer thread
  std::array<RecordedCommand, kBlock> block_{};  // used only by the writer thread

  alignas(kCacheLineSize) std::atomic<bool> ok_{false};
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> written_{0};
  std::atomic<std::uint64_t> flush_requested_{0};  // how many flushes have been asked for
  std::atomic<std::uint64_t> flush_served_{0};     // the latest of them to be carried out

  std::thread writer_;  // last: everything above exists before it starts
};

// --- Reading --------------------------------------------------------------------

// A recorded session, read into memory: the books it was run with and every
// command that was sent, in order.
class Recording {
 public:
  // Returns nothing if the file cannot be opened or is not a recording this
  // build can read: wrong format, wrong version, or written on a machine with
  // the other byte order.
  [[nodiscard]] static std::optional<Recording> load(const std::string& path) {
    std::FILE* const file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
      return std::nullopt;
    }
    Recording recording;
    const bool readable = recording.read(file);
    std::fclose(file);
    if (!readable) {
      return std::nullopt;
    }
    return recording;
  }

  Recording(Recording&&) noexcept = default;
  Recording& operator=(Recording&&) noexcept = default;
  // Not copyable: the books' names point into this object's own storage.
  Recording(const Recording&) = delete;
  Recording& operator=(const Recording&) = delete;

  // The books of the recorded session, ready to build an engine with:
  //   MatchingEngine engine({.books = recording.books()});
  // An engine keeps its own copy of each name, so it does not matter if the
  // recording is gone before the engine is.
  [[nodiscard]] const std::vector<BookConfig>& books() const noexcept { return books_; }

  // Every command, in the order it was sent. Besides Submit, Cancel and
  // Snapshot, a command may be a Move: see CommandType.
  [[nodiscard]] std::span<const Command> commands() const noexcept { return commands_; }

  // True if the file ends part-way through a command, as it will if the
  // recording program was killed, or holds something that is not a command.
  // commands() is then everything that came before that point.
  [[nodiscard]] bool damaged() const noexcept { return damaged_; }

 private:
  Recording() = default;

  bool read(std::FILE* file) {
    RecordingHeader header{};
    if (std::fread(&header, sizeof(header), 1, file) != 1 ||
        !std::equal(std::begin(kRecordingMagic), std::end(kRecordingMagic), header.magic) ||
        header.byte_order != kRecordingByteOrder || header.version != kRecordingVersion ||
        header.command_size != sizeof(RecordedCommand) || header.book_count == 0 ||
        header.book_count > std::numeric_limits<SymbolId>::max()) {
      return false;
    }

    recorded_books_.resize(header.book_count);
    if (std::fread(recorded_books_.data(), sizeof(RecordedBook), recorded_books_.size(), file) !=
        recorded_books_.size()) {
      return false;
    }
    books_.reserve(recorded_books_.size());
    for (const RecordedBook& recorded : recorded_books_) {
      const char* const end =
          std::find(std::begin(recorded.symbol), std::end(recorded.symbol), '\0');
      books_.push_back(BookConfig{
          .symbol = std::string_view(recorded.symbol,
                                     static_cast<std::size_t>(end - recorded.symbol)),
          .min_price = recorded.min_price,
          .num_levels = static_cast<std::size_t>(recorded.num_levels),
          .max_orders = static_cast<std::size_t>(recorded.max_orders),
      });
    }

    // The commands, a block at a time. A short read is the end of the file;
    // bytes left over that do not make a whole command mean it was cut off.
    std::vector<RecordedCommand> block(kBlock);
    for (;;) {
      const std::size_t bytes = std::fread(block.data(), 1, kBlock * sizeof(RecordedCommand), file);
      const std::size_t whole = bytes / sizeof(RecordedCommand);
      for (std::size_t index = 0; index < whole; ++index) {
        if (!is_valid(block[index])) {
          damaged_ = true;
          return true;
        }
        commands_.push_back(from_recorded(block[index]));
      }
      if (bytes % sizeof(RecordedCommand) != 0) {
        damaged_ = true;
      }
      if (bytes != kBlock * sizeof(RecordedCommand)) {
        return true;
      }
    }
  }

  static constexpr std::size_t kBlock = 4'096;  // commands read at a time

  std::vector<RecordedBook> recorded_books_;  // owns the names that books_ points into
  std::vector<BookConfig> books_;
  std::vector<Command> commands_;
  bool damaged_ = false;
};

}  // namespace lob

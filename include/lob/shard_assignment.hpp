#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

namespace lob {

// Decides which shard each symbol goes to, given how busy each symbol is
// expected to be. `loads[i]` is symbol i's expected load in any unit, such as
// orders per day. The result has one entry per symbol: the shard it gets.
//
// The method is "largest first": take the symbols from busiest to quietest and
// give each one to whichever shard is carrying the least so far. This is the
// longest-processing-time rule from job scheduling. It does not always find
// the best split, but the busiest shard never carries more than 4/3 of what
// it would under the best possible split.
//
// Ties go to the shard holding fewer symbols, then to the lower-numbered
// shard, and equally busy symbols are taken in ID order. Two things follow:
//
//   - Equal loads are simply dealt in turn: symbol 0 to shard 0, symbol 1 to
//     shard 1, and so on round the shards.
//   - As long as there are at least as many symbols as shards, no shard is
//     left empty, even if every load is zero.
//
// The work is done once, at start-up: it is not on the hot path.
[[nodiscard]] inline std::vector<std::uint16_t> assign_shards(
    const std::vector<std::uint64_t>& loads, std::size_t shards) {
  std::vector<std::size_t> busiest_first(loads.size());
  std::iota(busiest_first.begin(), busiest_first.end(), std::size_t{0});
  std::stable_sort(busiest_first.begin(), busiest_first.end(),
                   [&](std::size_t a, std::size_t b) { return loads[a] > loads[b]; });

  std::vector<std::uint64_t> carried(shards, 0);  // load given to each shard so far
  std::vector<std::size_t> held(shards, 0);       // symbols given to each shard so far
  std::vector<std::uint16_t> shard_of(loads.size(), 0);

  for (const std::size_t symbol : busiest_first) {
    std::size_t lightest = 0;
    for (std::size_t shard = 1; shard < shards; ++shard) {
      if (carried[shard] < carried[lightest] ||
          (carried[shard] == carried[lightest] && held[shard] < held[lightest])) {
        lightest = shard;
      }
    }
    shard_of[symbol] = static_cast<std::uint16_t>(lightest);
    carried[lightest] += loads[symbol];
    ++held[lightest];
  }
  return shard_of;
}

// One symbol to move from one shard to another.
struct RebalancingMove {
  std::size_t symbol;
  std::size_t from;
  std::size_t to;
};

// Decides whether moving one symbol would even out the shards, given what each
// symbol has been carrying lately (`loads`) and where each is now (`shard_of`).
// `carried` is working space with one slot per shard; on return it holds the
// load on each shard as things stand.
//
// Nothing is moved while the busiest shard carries no more than its fair share
// plus `tolerance_percent`. A fair share is the total divided by the number of
// shards. The tolerance is what stops small, passing differences from causing
// moves, each of which briefly pauses a shard.
//
// Otherwise the move is from the busiest shard to the quietest. Moving a
// symbol of load w changes the gap between those two from g to |g - 2w|, so
// the symbol chosen is the one whose load is nearest half the gap.
//
// A move is only worth its pause if it at least halves the gap, which means
// g/4 <= w <= 3g/4. That rules out symbols too quiet to matter, which would
// otherwise be shuffled about to trim small differences, and symbols so busy
// that moving them would only move the problem. If no symbol qualifies, the
// answer is no move.
//
// A move chosen this way never raises the busiest shard's load, and repeated
// one at a time the moves always come to a stop.
[[nodiscard]] inline std::optional<RebalancingMove> choose_rebalancing_move(
    std::span<const std::uint64_t> loads, std::span<const std::uint16_t> shard_of,
    std::span<std::uint64_t> carried, std::uint32_t tolerance_percent) noexcept {
  const std::size_t shards = carried.size();
  if (shards < 2) {
    return std::nullopt;
  }
  std::fill(carried.begin(), carried.end(), std::uint64_t{0});
  std::uint64_t total = 0;
  for (std::size_t symbol = 0; symbol < loads.size(); ++symbol) {
    carried[shard_of[symbol]] += loads[symbol];
    total += loads[symbol];
  }

  std::size_t busiest = 0;
  std::size_t quietest = 0;
  for (std::size_t shard = 1; shard < shards; ++shard) {
    if (carried[shard] > carried[busiest]) {
      busiest = shard;
    }
    if (carried[shard] < carried[quietest]) {
      quietest = shard;
    }
  }

  // busiest <= (total / shards) * (1 + tolerance), without dividing.
  if (carried[busiest] * shards * 100 <= total * (100 + tolerance_percent)) {
    return std::nullopt;
  }

  const std::uint64_t gap = carried[busiest] - carried[quietest];
  std::optional<RebalancingMove> best;
  std::uint64_t best_gap_after = gap;
  for (std::size_t symbol = 0; symbol < loads.size(); ++symbol) {
    const std::uint64_t load = loads[symbol];
    if (shard_of[symbol] != busiest || load == 0) {
      continue;
    }
    const std::uint64_t gap_after = 2 * load > gap ? 2 * load - gap : gap - 2 * load;
    if (2 * gap_after > gap) {
      continue;  // would not even halve the gap
    }
    if (gap_after < best_gap_after) {
      best_gap_after = gap_after;
      best = RebalancingMove{symbol, busiest, quietest};
    }
  }
  return best;
}

}  // namespace lob

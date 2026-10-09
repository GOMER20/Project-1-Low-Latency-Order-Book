#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
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

}  // namespace lob

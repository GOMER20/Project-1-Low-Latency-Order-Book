#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lob {

// One bit per price level, set while that level holds resting orders.
//
// The bits are arranged in three tiers: each word in a higher tier summarises
// 64 words of the tier below. The lowest or highest occupied level among up to
// 262,144 is therefore found with exactly three count-zero instructions, no
// matter how many empty levels lie in between. Nothing is ever scanned.
class LevelBitmap {
 public:
  static constexpr std::size_t kMaxBits = 64 * 64 * 64;
  static constexpr std::size_t npos = ~std::size_t{0};

  explicit LevelBitmap(std::size_t bits)
      : leaf_(std::make_unique<std::uint64_t[]>((bits + 63) / 64)) {
    assert(bits <= kMaxBits);
  }

  void set(std::size_t bit) noexcept {
    leaf_[bit >> 6] |= mask(bit);
    mid_[bit >> 12] |= mask(bit >> 6);
    top_ |= mask(bit >> 12);
  }

  // A summary bit is cleared only once the whole word beneath it is empty.
  void reset(std::size_t bit) noexcept {
    if ((leaf_[bit >> 6] &= ~mask(bit)) != 0) {
      return;
    }
    if ((mid_[bit >> 12] &= ~mask(bit >> 6)) != 0) {
      return;
    }
    top_ &= ~mask(bit >> 12);
  }

  [[nodiscard]] bool test(std::size_t bit) const noexcept {
    return (leaf_[bit >> 6] & mask(bit)) != 0;
  }

  [[nodiscard]] bool none() const noexcept { return top_ == 0; }

  // Lowest set bit, or npos if there is none.
  [[nodiscard]] std::size_t find_first() const noexcept {
    if (top_ == 0) {
      return npos;
    }
    const std::size_t mid_word = lowest(top_);
    const std::size_t leaf_word = (mid_word << 6) | lowest(mid_[mid_word]);
    return (leaf_word << 6) | lowest(leaf_[leaf_word]);
  }

  // Highest set bit, or npos if there is none.
  [[nodiscard]] std::size_t find_last() const noexcept {
    if (top_ == 0) {
      return npos;
    }
    const std::size_t mid_word = highest(top_);
    const std::size_t leaf_word = (mid_word << 6) | highest(mid_[mid_word]);
    return (leaf_word << 6) | highest(leaf_[leaf_word]);
  }

  // Lowest set bit strictly above `bit`, or npos. Like find_first, it never
  // scans: at most one word is examined in each tier on the way up, and one
  // in each tier on the way back down.
  [[nodiscard]] std::size_t find_next(std::size_t bit) const noexcept {
    std::size_t leaf_word = bit >> 6;
    std::uint64_t word = leaf_[leaf_word] & above(bit);
    if (word != 0) {
      return (leaf_word << 6) | lowest(word);
    }
    std::size_t mid_word = leaf_word >> 6;
    word = mid_[mid_word] & above(leaf_word);
    if (word == 0) {
      word = top_ & above(mid_word);
      if (word == 0) {
        return npos;
      }
      mid_word = lowest(word);
      word = mid_[mid_word];
    }
    leaf_word = (mid_word << 6) | lowest(word);
    return (leaf_word << 6) | lowest(leaf_[leaf_word]);
  }

  // Highest set bit strictly below `bit`, or npos.
  [[nodiscard]] std::size_t find_prev(std::size_t bit) const noexcept {
    std::size_t leaf_word = bit >> 6;
    std::uint64_t word = leaf_[leaf_word] & below(bit);
    if (word != 0) {
      return (leaf_word << 6) | highest(word);
    }
    std::size_t mid_word = leaf_word >> 6;
    word = mid_[mid_word] & below(leaf_word);
    if (word == 0) {
      word = top_ & below(mid_word);
      if (word == 0) {
        return npos;
      }
      mid_word = highest(word);
      word = mid_[mid_word];
    }
    leaf_word = (mid_word << 6) | highest(word);
    return (leaf_word << 6) | highest(leaf_[leaf_word]);
  }

 private:
  static constexpr std::uint64_t mask(std::size_t bit) noexcept {
    return std::uint64_t{1} << (bit & 63);
  }

  // The bits of a word above / below position (index & 63), exclusive. The
  // shift is split in two so that position 63 does not shift by 64.
  static constexpr std::uint64_t above(std::size_t index) noexcept {
    return (~std::uint64_t{0} << (index & 63)) << 1;
  }

  static constexpr std::uint64_t below(std::size_t index) noexcept {
    return mask(index) - 1;
  }

  // std::countr_zero / std::countl_zero are the C++20 spellings of
  // __builtin_ctzll / __builtin_clzll and compile to the same TZCNT / LZCNT.
  static constexpr std::size_t lowest(std::uint64_t word) noexcept {
    return static_cast<std::size_t>(std::countr_zero(word));
  }

  static constexpr std::size_t highest(std::uint64_t word) noexcept {
    return static_cast<std::size_t>(63 - std::countl_zero(word));
  }

  std::uint64_t top_ = 0;
  std::array<std::uint64_t, 64> mid_{};
  std::unique_ptr<std::uint64_t[]> leaf_;
};

}  // namespace lob

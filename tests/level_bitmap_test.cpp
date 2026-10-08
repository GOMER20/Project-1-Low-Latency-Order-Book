#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "lob/level_bitmap.hpp"

namespace {

using lob::LevelBitmap;

TEST(LevelBitmap, EmptyBitmapFindsNothing) {
  LevelBitmap bitmap(1024);
  EXPECT_TRUE(bitmap.none());
  EXPECT_EQ(bitmap.find_first(), LevelBitmap::npos);
  EXPECT_EQ(bitmap.find_last(), LevelBitmap::npos);
}

TEST(LevelBitmap, SingleBitIsBothFirstAndLast) {
  LevelBitmap bitmap(1024);
  bitmap.set(137);
  EXPECT_TRUE(bitmap.test(137));
  EXPECT_FALSE(bitmap.test(136));
  EXPECT_FALSE(bitmap.none());
  EXPECT_EQ(bitmap.find_first(), 137u);
  EXPECT_EQ(bitmap.find_last(), 137u);
}

// Bits chosen to fall in different leaf words and different mid-tier words.
TEST(LevelBitmap, FindsExtremesAcrossAllTiers) {
  LevelBitmap bitmap(LevelBitmap::kMaxBits);
  bitmap.set(5);
  bitmap.set(70);
  bitmap.set(5'000);
  bitmap.set(200'000);

  EXPECT_EQ(bitmap.find_first(), 5u);
  EXPECT_EQ(bitmap.find_last(), 200'000u);

  bitmap.reset(5);
  EXPECT_EQ(bitmap.find_first(), 70u);

  bitmap.reset(200'000);
  EXPECT_EQ(bitmap.find_last(), 5'000u);

  bitmap.reset(70);
  EXPECT_EQ(bitmap.find_first(), 5'000u);
  EXPECT_EQ(bitmap.find_last(), 5'000u);

  bitmap.reset(5'000);
  EXPECT_TRUE(bitmap.none());
}

TEST(LevelBitmap, SummaryStaysSetWhileWordHasOtherBits) {
  LevelBitmap bitmap(1024);
  bitmap.set(64);
  bitmap.set(65);

  bitmap.reset(64);
  EXPECT_EQ(bitmap.find_first(), 65u);
  EXPECT_EQ(bitmap.find_last(), 65u);

  bitmap.reset(65);
  EXPECT_TRUE(bitmap.none());
}

TEST(LevelBitmap, HandlesFirstAndLastRepresentableBits) {
  LevelBitmap bitmap(LevelBitmap::kMaxBits);
  bitmap.set(0);
  bitmap.set(LevelBitmap::kMaxBits - 1);
  EXPECT_EQ(bitmap.find_first(), 0u);
  EXPECT_EQ(bitmap.find_last(), LevelBitmap::kMaxBits - 1);
}

TEST(LevelBitmap, SettingASetBitAndClearingAClearBitAreHarmless) {
  LevelBitmap bitmap(1024);
  bitmap.set(9);
  bitmap.set(9);
  bitmap.reset(500);
  EXPECT_EQ(bitmap.find_first(), 9u);
  EXPECT_EQ(bitmap.find_last(), 9u);
}

// Bits chosen to sit on both sides of every word boundary in every tier.
TEST(LevelBitmap, FindNextAndFindPrevStepFromOneSetBitToTheNext) {
  const std::vector<std::size_t> bits{5, 6, 63, 64, 4'095, 4'096, 200'000};
  LevelBitmap bitmap(LevelBitmap::kMaxBits);
  for (const std::size_t bit : bits) {
    bitmap.set(bit);
  }

  for (std::size_t i = 0; i + 1 < bits.size(); ++i) {
    EXPECT_EQ(bitmap.find_next(bits[i]), bits[i + 1]);
    EXPECT_EQ(bitmap.find_prev(bits[i + 1]), bits[i]);
  }
  EXPECT_EQ(bitmap.find_next(bits.back()), LevelBitmap::npos);
  EXPECT_EQ(bitmap.find_prev(bits.front()), LevelBitmap::npos);
}

// The starting position does not have to be a set bit.
TEST(LevelBitmap, FindNextAndFindPrevWorkFromUnsetPositions) {
  LevelBitmap bitmap(LevelBitmap::kMaxBits);
  bitmap.set(100);
  bitmap.set(100'000);

  EXPECT_EQ(bitmap.find_next(0), 100u);
  EXPECT_EQ(bitmap.find_next(99), 100u);
  EXPECT_EQ(bitmap.find_next(101), 100'000u);
  EXPECT_EQ(bitmap.find_prev(LevelBitmap::kMaxBits - 1), 100'000u);
  EXPECT_EQ(bitmap.find_prev(99'999), 100u);
  EXPECT_EQ(bitmap.find_prev(100), LevelBitmap::npos);
  EXPECT_EQ(bitmap.find_next(100'000), LevelBitmap::npos);
}

TEST(LevelBitmap, FindNextAndFindPrevOnAnEmptyBitmapFindNothing) {
  LevelBitmap bitmap(LevelBitmap::kMaxBits);
  EXPECT_EQ(bitmap.find_next(0), LevelBitmap::npos);
  EXPECT_EQ(bitmap.find_next(70'000), LevelBitmap::npos);
  EXPECT_EQ(bitmap.find_prev(70'000), LevelBitmap::npos);
  EXPECT_EQ(bitmap.find_prev(LevelBitmap::kMaxBits - 1), LevelBitmap::npos);
}

TEST(LevelBitmap, FindNextAndFindPrevReachTheFirstAndLastBits) {
  LevelBitmap bitmap(LevelBitmap::kMaxBits);
  bitmap.set(0);
  bitmap.set(LevelBitmap::kMaxBits - 1);

  EXPECT_EQ(bitmap.find_next(0), LevelBitmap::kMaxBits - 1);
  EXPECT_EQ(bitmap.find_prev(LevelBitmap::kMaxBits - 1), 0u);
  EXPECT_EQ(bitmap.find_next(LevelBitmap::kMaxBits - 1), LevelBitmap::npos);
  EXPECT_EQ(bitmap.find_prev(0), LevelBitmap::npos);
}

// Random set / reset sequence checked against a plain vector<bool>.
TEST(LevelBitmap, MatchesReferenceUnderRandomUpdates) {
  constexpr std::size_t kBits = 5'000;  // spans two mid-tier words
  LevelBitmap bitmap(kBits);
  std::vector<bool> reference(kBits, false);
  std::uint64_t state = 0x9E3779B97F4A7C15ULL;

  for (int step = 0; step < 5'000; ++step) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::size_t bit = static_cast<std::size_t>(state >> 33) % kBits;
    // Set one time in four, so the map stays sparse and summaries get cleared.
    if (((state >> 20) & 3) == 0) {
      bitmap.set(bit);
      reference[bit] = true;
    } else {
      bitmap.reset(bit);
      reference[bit] = false;
    }

    std::size_t expected_first = LevelBitmap::npos;
    std::size_t expected_last = LevelBitmap::npos;
    for (std::size_t i = 0; i < kBits; ++i) {
      if (reference[i]) {
        if (expected_first == LevelBitmap::npos) {
          expected_first = i;
        }
        expected_last = i;
      }
    }
    ASSERT_EQ(bitmap.find_first(), expected_first);
    ASSERT_EQ(bitmap.find_last(), expected_last);

    // Neighbours of the bit just touched, whether it ended up set or clear.
    std::size_t expected_next = LevelBitmap::npos;
    for (std::size_t i = bit + 1; i < kBits && expected_next == LevelBitmap::npos; ++i) {
      if (reference[i]) {
        expected_next = i;
      }
    }
    std::size_t expected_prev = LevelBitmap::npos;
    for (std::size_t i = bit; i-- > 0 && expected_prev == LevelBitmap::npos;) {
      if (reference[i]) {
        expected_prev = i;
      }
    }
    ASSERT_EQ(bitmap.find_next(bit), expected_next);
    ASSERT_EQ(bitmap.find_prev(bit), expected_prev);
  }
}

}  // namespace

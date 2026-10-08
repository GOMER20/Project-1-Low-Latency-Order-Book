// Sanity checks on the toolchain the engine relies on.
#include <bit>
#include <cstdint>

#include <gtest/gtest.h>

#include "lob/compiler.hpp"

TEST(Toolchain, CompilesAsCxx20) {
  EXPECT_GE(__cplusplus, 202002L);
}

TEST(Toolchain, CacheLineConstantIsVisible) {
  EXPECT_EQ(lob::kCacheLineSize, 64u);
}

// LevelBitmap depends on the count-zero instructions.
TEST(Toolchain, CountTrailingZerosIntrinsic) {
  const std::uint64_t mask = 0b1000;
  EXPECT_EQ(__builtin_ctzll(mask), 3);
  EXPECT_EQ(std::countr_zero(mask), 3);
}

#pragma once

#include <cstddef>

namespace lob {

// Hard-coded instead of std::hardware_destructive_interference_size: that
// constant can change with -march/-mtune, which would silently alter struct
// layouts between builds. 64 bytes is correct for x86-64 and most AArch64.
inline constexpr std::size_t kCacheLineSize = 64;

}  // namespace lob

#define LOB_ALWAYS_INLINE [[gnu::always_inline]] inline
#define LOB_NOINLINE [[gnu::noinline]]

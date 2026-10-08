#pragma once

#include <cstddef>

namespace lob {

// Hard-coded instead of std::hardware_destructive_interference_size: that
// constant can change with -march/-mtune, which would silently alter struct
// layouts between builds. 64 bytes is correct for x86-64 and most AArch64.
inline constexpr std::size_t kCacheLineSize = 64;

// Tells the CPU this is a spin-wait loop. It saves power, frees the core's
// other hyper-thread and avoids a costly pipeline flush when the wait ends.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#endif
}

}  // namespace lob

#define LOB_ALWAYS_INLINE [[gnu::always_inline]] inline
#define LOB_NOINLINE [[gnu::noinline]]

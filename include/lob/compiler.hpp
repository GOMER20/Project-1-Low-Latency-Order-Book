#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

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

// A cheap, steadily increasing counter for timing short stretches of work on
// one thread. The unit is whatever the CPU's counter ticks in, so the values
// are only good for comparing with each other, not for turning into seconds.
//
// Reading it costs a few nanoseconds: the CPU's own counter is read directly,
// with no call into the operating system.
[[nodiscard]] inline std::uint64_t cycle_ticks() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
  std::uint64_t ticks;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(ticks));
  return ticks;
#else
  return static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

}  // namespace lob

#define LOB_ALWAYS_INLINE [[gnu::always_inline]] inline
#define LOB_NOINLINE [[gnu::noinline]]

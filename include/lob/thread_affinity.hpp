#pragma once

#include <cstddef>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace lob {

// Whether this platform can bind a thread to one CPU. Linux can. macOS cannot:
// it only accepts scheduling hints, and not at all on Apple silicon.
#if defined(__linux__)
inline constexpr bool kCanPinThreads = true;
#else
inline constexpr bool kCanPinThreads = false;
#endif

// Binds the calling thread to a single CPU, so the scheduler can no longer
// move it between cores. A migration costs far more than the move itself: the
// thread arrives at a core whose caches hold none of its data.
//
// Returns false, changing nothing, if the CPU does not exist, is not
// available to this process, or the platform cannot pin threads.
//
// Pinning only stops this thread from leaving the core. Keeping other work
// off that core is a system setting (for example isolcpus or cpusets).
[[nodiscard]] inline bool pin_current_thread_to_cpu(int cpu) noexcept {
#if defined(__linux__)
  if (cpu < 0 || cpu >= CPU_SETSIZE) {
    return false;
  }
  const auto index = static_cast<std::size_t>(cpu);
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  CPU_SET(index, &allowed);
  return pthread_setaffinity_np(pthread_self(), sizeof(allowed), &allowed) == 0;
#else
  static_cast<void>(cpu);
  return false;
#endif
}

}  // namespace lob

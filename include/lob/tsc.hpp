// Fine-grained timing for the latency benchmark.
//
// steady_clock on Windows is QueryPerformanceCounter at 100ns granularity —
// coarser than a single book operation, so percentiles quantise to the
// clock, not the code. The x86 time-stamp counter ticks at ~0.3-0.5ns and
// modern CPUs (constant_tsc/invariant TSC) tick it uniformly regardless of
// frequency scaling. We read TSC around each operation and convert ticks to
// nanoseconds with a wall-clock calibration.
//
// Honesty notes carried into the README: the rdtsc pair itself costs
// ~5-10ns and is INCLUDED in per-op numbers; rdtsc is not a serialising
// instruction, so individual samples can be skewed a few ns by out-of-order
// execution — fine for percentiles over a million samples, not for citing
// any single reading.

#pragma once

#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#define LOB_HAVE_TSC 1
#endif

namespace lob {

inline std::uint64_t tsc_now() {
#if defined(LOB_HAVE_TSC)
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Nanoseconds per TSC tick, measured against steady_clock over ~200ms.
inline double tsc_ns_per_tick() {
#if defined(LOB_HAVE_TSC)
    const auto wall_start = std::chrono::steady_clock::now();
    const std::uint64_t tsc_start = tsc_now();
    while (std::chrono::steady_clock::now() - wall_start < std::chrono::milliseconds(200)) {
    }
    const std::uint64_t tsc_stop = tsc_now();
    const auto wall_stop = std::chrono::steady_clock::now();
    const double ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall_stop - wall_start).count();
    return ns / static_cast<double>(tsc_stop - tsc_start);
#else
    return 1.0;  // fallback clock already reports nanoseconds
#endif
}

}  // namespace lob

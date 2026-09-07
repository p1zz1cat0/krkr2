#pragma once

#include <cstdint>
#include <ctime>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace yoghourt_telemetry {

// Session-wide join key shared by runtime frames, host process samples, and
// events. Apple: mach_continuous_time converted to nanoseconds, so the base
// keeps counting across sleep and matches the host-side PerformanceMonitor.
// Other platforms: CLOCK_MONOTONIC. Wall-clock time is never used as a join
// key; it only appears in display/diagnostic contexts on the host.
inline uint64_t MonotonicNs() {
#if defined(__APPLE__)
    static const mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t info{};
        (void)mach_timebase_info(&info);
        return info;
    }();
    return static_cast<uint64_t>(mach_continuous_time()) * base.numer / base.denom;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

} // namespace yoghourt_telemetry

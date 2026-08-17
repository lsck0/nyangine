#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nyangine/nyangine.h"

u64 nya_clock_get_timestamp_s(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    // Convert FILETIME to 64-bit
    u64 time = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    // FILETIME is in 100-nanosecond intervals since January 1, 1601 (UTC)
    // Convert to seconds and adjust to Unix epoch (January 1, 1970)
    const u64 EPOCH_DIFFERENCE_S = 11644473600ULL;
    return (time / 10'000'000) - EPOCH_DIFFERENCE_S;
}

u64 nya_clock_get_timestamp_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    // Convert FILETIME to 64-bit
    u64 time = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    // FILETIME is in 100-nanosecond intervals since January 1, 1601 (UTC)
    // Convert to milliseconds and adjust to Unix epoch (January 1, 1970)
    const u64 EPOCH_DIFFERENCE_MS = 11644473600000ULL;
    return (time / 10'000) - EPOCH_DIFFERENCE_MS;
}

u64 nya_clock_get_timestamp_µs(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    // Convert FILETIME to 64-bit
    u64 time = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    // FILETIME is in 100-nanosecond intervals since January 1, 1601 (UTC)
    // Convert to microseconds and adjust to Unix epoch (January 1, 1970)
    const u64 EPOCH_DIFFERENCE_µS = 11644473600000000ULL;
    return (time / 10) - EPOCH_DIFFERENCE_µS;
}

u64 nya_clock_get_timestamp_ns(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    // Convert FILETIME to 64-bit
    u64 time = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    // FILETIME is in 100-nanosecond intervals since January 1, 1601 (UTC)
    /*
     * Convert to nanoseconds and adjust to Unix epoch (January 1, 1970).
     *
     * The offset is 11644473600 seconds expressed in the unit being returned, which for nanoseconds
     * is nineteen digits. It was written with seventeen — the microsecond offset above, reused — so
     * this returned roughly 1.33e19 where it should return 1.77e18, about three hundred and sixty
     * six years into the future.
     *
     * Latent rather than visible because every caller in the tree subtracts two of these and the
     * offset cancels: frame timing, nya_app_uptime_ns and the profiler all measure durations. It is
     * public API, though, and it disagreed with both the POSIX implementation and the other three
     * functions here.
     */
    const u64 EPOCH_DIFFERENCE_NS = 11644473600000000000ULL;
    return (time * 100) - EPOCH_DIFFERENCE_NS;
}

/*
 * ─────────────────────────────────────────────────────────
 * MONOTONIC
 * ─────────────────────────────────────────────────────────
 */

/*
 * QueryPerformanceCounter, which is the Windows monotonic clock. GetTickCount64 is monotonic too but
 * only has millisecond resolution and a ~15ms update period, which is coarser than a frame.
 *
 * The frequency is fixed for the lifetime of the process, so it is read once. Ticks are converted by
 * splitting into whole seconds and a remainder before scaling: `ticks * 1'000'000'000` overflows a
 * u64 after about 5.8 seconds at a 10MHz counter, which is the obvious way to write this and is
 * wrong within the first ten seconds of running.
 */

NYA_INTERNAL s64 _nya_clock_frequency(void) {
    static s64 frequency = 0;
    if (frequency == 0) {
        LARGE_INTEGER value;
        // Cannot fail on anything since Windows XP, but a zero here would divide by zero below.
        if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0) return 1;
        frequency = value.QuadPart;
    }
    return frequency;
}

u64 nya_clock_get_monotonic_ns(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    s64 frequency = _nya_clock_frequency();
    u64 ticks     = (u64)counter.QuadPart;

    u64 seconds   = ticks / (u64)frequency;
    u64 remainder = ticks % (u64)frequency;

    return (seconds * 1'000'000'000ULL) + ((remainder * 1'000'000'000ULL) / (u64)frequency);
}

u64 nya_clock_get_monotonic_µs(void) {
    return nya_clock_get_monotonic_ns() / 1'000ULL;
}

u64 nya_clock_get_monotonic_ms(void) {
    return nya_clock_get_monotonic_ns() / 1'000'000ULL;
}

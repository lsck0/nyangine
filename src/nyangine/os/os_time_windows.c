#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nyangine/os/os_time.h"

/** FILETIME counts 100 ns intervals from 1601-01-01, which is this many of them before the Unix epoch. */
#define _NYA_OS_TIME_EPOCH_INTERVALS 116'444'736'000'000'000ULL

u64 nya_os_time_wall_ns(void) {
    // precise, so a file written a moment ago never reads as newer than now; the plain call ticks every ~15 ms.
    FILETIME stamp;
    GetSystemTimePreciseAsFileTime(&stamp);

    u64 intervals = ((u64)stamp.dwHighDateTime << 32) | stamp.dwLowDateTime;

    return (intervals - _NYA_OS_TIME_EPOCH_INTERVALS) * 100ULL;
}

/*
 * QueryPerformanceCounter, which is the Windows monotonic clock. GetTickCount64 is monotonic too but
 * only has millisecond resolution and a ~15 ms update period, which is coarser than a frame.
 */

/** The counter's ticks per second, asked once: it is fixed while the system runs. */
NYA_INTERNAL s64 _nya_os_time_frequency(void) {
    static s64 frequency = 0;

    if (frequency == 0) {
        LARGE_INTEGER value;

        // cannot fail on anything since Windows XP, but a zero here would divide by zero below.
        if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0) return 1;

        frequency = value.QuadPart;
    }

    return frequency;
}

u64 nya_os_time_monotonic_ns(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    u64 frequency = (u64)_nya_os_time_frequency();
    u64 ticks     = (u64)counter.QuadPart;

    // split, because ticks * 1e9 overflows a u64 after about 18 seconds at a 1 GHz counter.
    return ((ticks / frequency) * 1'000'000'000ULL) + (((ticks % frequency) * 1'000'000'000ULL) / frequency);
}

// Sleep rounds up to the scheduler's tick, which is ~15 ms unless something raised the timer resolution.
void nya_os_time_sleep_ms(u32 milliseconds) {
    Sleep(milliseconds);
}

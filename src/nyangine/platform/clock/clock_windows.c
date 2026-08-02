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
    // Convert to nanoseconds and adjust to Unix epoch (January 1, 1970)
    const u64 EPOCH_DIFFERENCE_NS = 116444736000000000ULL;
    return (time * 100) - EPOCH_DIFFERENCE_NS;
}

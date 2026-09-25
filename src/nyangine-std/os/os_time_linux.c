#include "nyangine-std/os/os_time.h"

// After the engine's own header: base_basic.h asks for POSIX 2008, and time.h declares clock_gettime only once it has seen that.
#include <time.h>

u64 nya_os_time_wall_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    return (u64)now.tv_sec * 1'000'000'000ULL + (u64)now.tv_nsec;
}

// CLOCK_MONOTONIC not CLOCK_BOOTTIME: suspended time is not run time, or a frame timer would report one frame of nine hours after an overnight suspend.
u64 nya_os_time_monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return (u64)now.tv_sec * 1'000'000'000ULL + (u64)now.tv_nsec;
}

void nya_os_time_sleep_ms(u32 milliseconds) {
    struct timespec request   = { .tv_sec = milliseconds / 1'000U, .tv_nsec = (long)(milliseconds % 1'000U) * 1'000'000L };
    struct timespec remaining = request;

    // A signal cuts the sleep short and nanosleep hands back what was left, so waiting again with that is the only way "at least this long" holds.
    while (nanosleep(&request, &remaining) != 0 && errno == EINTR) request = remaining;
}

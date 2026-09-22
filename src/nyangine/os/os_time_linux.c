#include "nyangine/os/os_time.h"

// after the engine's own header: base_basic.h asks for POSIX 2008, and time.h only declares
// clock_gettime when it has seen that.
#include <time.h>

u64 nya_os_time_wall_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    return (u64)now.tv_sec * 1'000'000'000ULL + (u64)now.tv_nsec;
}

/*
 * CLOCK_MONOTONIC rather than CLOCK_BOOTTIME: time spent suspended is not time the program ran, and a
 * frame timer that counted a laptop being closed overnight would report one frame of nine hours.
 */
u64 nya_os_time_monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return (u64)now.tv_sec * 1'000'000'000ULL + (u64)now.tv_nsec;
}

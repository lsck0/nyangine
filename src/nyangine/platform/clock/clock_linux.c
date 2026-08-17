#include "nyangine/nyangine.h"

u64 nya_clock_get_timestamp_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)(ts.tv_sec);
}

u64 nya_clock_get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)(ts.tv_sec) * 1'000 + (u64)(ts.tv_nsec) / 1'000'000;
}

NYA_API u64 nya_clock_get_timestamp_µs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)(ts.tv_sec) * 1'000'000 + (u64)(ts.tv_nsec) / 1'000;
}

u64 nya_clock_get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)(ts.tv_sec) * 1'000'000'000 + (u64)(ts.tv_nsec);
}

/*
 * ─────────────────────────────────────────────────────────
 * MONOTONIC
 * ─────────────────────────────────────────────────────────
 */

/*
 * CLOCK_MONOTONIC rather than CLOCK_BOOTTIME: time spent suspended is not time the program ran, and
 * a frame timer that counted a laptop being closed overnight would report one frame of nine hours.
 */

u64 nya_clock_get_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)(ts.tv_sec) * 1'000'000'000 + (u64)(ts.tv_nsec);
}

u64 nya_clock_get_monotonic_µs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)(ts.tv_sec) * 1'000'000 + (u64)(ts.tv_nsec) / 1'000;
}

u64 nya_clock_get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)(ts.tv_sec) * 1'000 + (u64)(ts.tv_nsec) / 1'000'000;
}

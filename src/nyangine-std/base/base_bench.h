/**
 * @file base_bench.h
 *
 * ```c
 * s32 main(void) {
 *     nya_bench_begin("render3d sort");
 *
 *     nya_bench("qsort",  4096, { memcpy(work, base, bytes); qsort(work, n, sizeof(Key), compare); });
 *     nya_bench("radix",  4096, { memcpy(work, base, bytes); radix(work, scratch, n); });
 *
 *     return nya_bench_end();
 * }
 * ```
 *
 * Built at -O2 without sanitizers, unlike tests. Sanitizers distort unevenly, hitting memory heavy
 * code hardest: the reverb measured 5.52% of frame time under ASAN and 0.22% of a core without it.
 * */
#pragma once

#include "nyangine-std/base/base_types.h"

// CONSTANTS

/** How many untimed rounds run first, to fault in pages and settle the branch predictors and caches. */
#ifndef NYA_BENCH_WARMUP
#define NYA_BENCH_WARMUP 3
#endif

/**
 * How many timed rounds each case runs. The best is reported and the median printed beside it.
 * */
#ifndef NYA_BENCH_ROUNDS
#define NYA_BENCH_ROUNDS 9
#endif

/**
 * The shortest a timed sample may be, in nanoseconds. The batch size is calibrated to reach it.
 * */
#ifndef NYA_BENCH_MIN_SAMPLE_NS
#define NYA_BENCH_MIN_SAMPLE_NS 100'000
#endif

/** The most a calibration will batch, so a pathologically fast body cannot spin forever. */
#ifndef NYA_BENCH_MAX_BATCH
#define NYA_BENCH_MAX_BATCH 1'000'000
#endif

// FUNCTIONS AND MACROS

/** Starts a named group and prints its header. */
NYA_API void nya_bench_begin(NYA_ConstCString group);

/** Prints the group's summary and returns a process exit code: zero unless a case failed. */
NYA_API s32 nya_bench_end(void);

/**
 * Records one case's result. Called by the nya_bench macro; rarely useful directly.
 *
 * `samples` are per-iteration nanosecond figures, one per round, and are sorted in place.
 * */
NYA_API void nya_bench_report(NYA_ConstCString name, f64* samples, u32 sample_count, u64 batch, u64 items);

/**
 * Times `body`, reporting nanoseconds per iteration and, when `items` is non-zero, per item.
 * */
#define nya_bench(name_, items_, ...)                                                                                                              \
    do {                                                                                                                                             \
        /* Untimed, to fault in pages and settle caches and branch predictors. */                                                                     \
        for (u32 _nya_bench_warm = 0; _nya_bench_warm < NYA_BENCH_WARMUP; _nya_bench_warm++) { __VA_ARGS__; }                                               \
                                                                                                                                                     \
        /* Calibrate a batch size so one timed sample outlasts the clock call; doubling rather than dividing an estimate needs no assumption that the body is linear in the batch. */ \
        u64 _nya_bench_batch = 1;                                                                                                                     \
        while (_nya_bench_batch < NYA_BENCH_MAX_BATCH) {                                                                                              \
            u64 _nya_bench_c0 = nya_clock_get_monotonic_ns();                                                                                         \
            for (u64 _nya_bench_i = 0; _nya_bench_i < _nya_bench_batch; _nya_bench_i++) { __VA_ARGS__; }                                                    \
            if ((nya_clock_get_monotonic_ns() - _nya_bench_c0) >= (u64)NYA_BENCH_MIN_SAMPLE_NS) break;                                                \
            _nya_bench_batch *= 2;                                                                                                                    \
        }                                                                                                                                             \
                                                                                                                                                      \
        f64 _nya_bench_samples[NYA_BENCH_ROUNDS];                                                                                                      \
        for (u32 _nya_bench_round = 0; _nya_bench_round < NYA_BENCH_ROUNDS; _nya_bench_round++) {                                                      \
            u64 _nya_bench_t0 = nya_clock_get_monotonic_ns();                                                                                          \
            for (u64 _nya_bench_i = 0; _nya_bench_i < _nya_bench_batch; _nya_bench_i++) { __VA_ARGS__; }                                                     \
            u64 _nya_bench_elapsed = nya_clock_get_monotonic_ns() - _nya_bench_t0;                                                                     \
                                                                                                                                                      \
            _nya_bench_samples[_nya_bench_round] = (f64)_nya_bench_elapsed / (f64)_nya_bench_batch;                                                    \
        }                                                                                                                                             \
                                                                                                                                                      \
        nya_bench_report(name_, _nya_bench_samples, NYA_BENCH_ROUNDS, _nya_bench_batch, (u64)(items_));                                                \
    } while (0)

/**
 * Makes a value observable, so the optimiser cannot delete the work that produced it.
 * */
#if defined(__clang__) || defined(__GNUC__)
// Copied into a local first, then constrained as memory.
#define nya_bench_keep(value_)                                                                                                                       \
    do {                                                                                                                                             \
        __auto_type _nya_bench_kept = (value_);                                                                                                      \
        __asm__ __volatile__("" : : "m"(_nya_bench_kept) : "memory");                                                                                \
    } while (0)
#else
#define nya_bench_keep(value_)                                                                                                                       \
    do {                                                                                                                                             \
        volatile __auto_type _nya_bench_sink = (value_);                                                                                             \
        nya_unused(_nya_bench_sink);                                                                                                                 \
    } while (0)
#endif

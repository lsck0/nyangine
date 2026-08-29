/**
 * @file base_bench.h
 *
 * Microbenchmarks: measuring whether a change helped, as opposed to finding what is slow.
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
 * **This is the other half of `perf`, not a replacement for it.** A profile answers "where does the
 * time go" over a whole frame; it cannot answer "is this version faster than that one", because the
 * difference is usually smaller than the noise between two runs of a game. This answers the second
 * question and is useless for the first.
 *
 * ⚠ **Built without sanitizers and at -O2, unlike a test.** That is the entire point. A sanitizer build
 * distorts unevenly — it lands hardest on code with a high ratio of memory accesses to arithmetic — and
 * a benchmark run under one measures the sanitizer. The engine's own reverb was profiled at 5.52% of
 * frame time under ASAN and measured at 0.22% of a core without it, a factor of twenty-five.
 *
 * **Each case is run many times and the *best* is reported, not the mean.** A benchmark competes with
 * every other process on the machine, so slow samples are contamination and fast ones are not: the
 * minimum is the closest thing to the number the code would produce alone. The spread is printed
 * alongside so a suspiciously wide one is visible rather than hidden in an average.
 * */
#pragma once

#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How many untimed rounds run first, to fault in pages and settle the branch predictors and caches. */
#ifndef NYA_BENCH_WARMUP
#define NYA_BENCH_WARMUP 3
#endif

/**
 * How many timed rounds each case runs. The best is reported and the median printed beside it.
 *
 * More than one because a single round can be lucky or unlucky; reporting both is what makes a case
 * where they disagree — the machine was busy — visible instead of silently believed.
 * */
#ifndef NYA_BENCH_ROUNDS
#define NYA_BENCH_ROUNDS 9
#endif

/**
 * The shortest a timed sample may be, in nanoseconds. The batch size is calibrated to reach it.
 *
 * `nya_clock_get_monotonic_ns` costs tens of nanoseconds to call, so timing a single execution of
 * something that takes twenty measures the clock. Running the body enough times that the sample lasts
 * a hundred microseconds pushes that overhead below a thousandth of the result.
 * */
#ifndef NYA_BENCH_MIN_SAMPLE_NS
#define NYA_BENCH_MIN_SAMPLE_NS 100'000
#endif

/** The most a calibration will batch, so a pathologically fast body cannot spin forever. */
#ifndef NYA_BENCH_MAX_BATCH
#define NYA_BENCH_MAX_BATCH 1'000'000
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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
 *
 * `items` is what one iteration processes — triangles, samples, entities — so two cases over different
 * input sizes stay comparable. Pass 0 when there is no natural unit.
 *
 * The body is variadic rather than a single parameter, so it may contain commas — a braced initialiser
 * or a multi-argument call inside it would otherwise be split into separate macro arguments.
 *
 * The body runs inside a loop the compiler cannot hoist it out of, because `nya_bench_keep` forces the
 * result to be observable. Without that, an optimiser deletes a pure computation whose value is unused
 * and the benchmark measures an empty loop — which reads as an enormous and entirely fictional speedup.
 * */
#define nya_bench(name_, items_, ...)                                                                                                              \
    do {                                                                                                                                             \
        /* Untimed, to fault in pages and settle caches and branch predictors. */                                                                     \
        for (u32 _nya_bench_warm = 0; _nya_bench_warm < NYA_BENCH_WARMUP; _nya_bench_warm++) { __VA_ARGS__; }                                               \
                                                                                                                                                     \
        /*                                                                                                                                           \
         * Calibrate a batch size, so one timed sample is long enough that the clock call does not                                                    \
         * dominate it. Doubling rather than dividing an estimate: the body may not be linear in the                                                  \
         * batch, and doubling until it is long enough needs no assumption that it is.                                                                \
         */                                                                                                                                          \
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
 *
 * An empty asm block that claims to read the value and to clobber memory. The compiler has to
 * materialise it and cannot assume anything survived across the barrier, which is exactly enough to
 * stop a benchmark measuring nothing.
 * */
#if defined(__clang__) || defined(__GNUC__)
/*
 * Copied into a local first, then constrained as memory.
 *
 * The value is often a vector-extension element — `v[0].x` — which cannot satisfy a register
 * constraint directly, and a plain "m" on the original would demand it be addressable. A local is
 * both, and the copy costs nothing the barrier was not already going to cost.
 */
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

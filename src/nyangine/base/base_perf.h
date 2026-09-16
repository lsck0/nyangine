/**
 * @file base_perf.h
 *
 * Example:
 * ```c
 * nya_perf_timer_start("My Timer");
 * ....
 * NYA_PerfMeasurement* timer = nya_perf_timer_get("My Timer");
 * ```
 *
 * Example:
 * ```c
 * {
 *  nya_perf_time_this_scope("My Scoped Timer");
 *  ...
 * }
 *
 * NYA_PerfMeasurement* timer = nya_perf_timer_get("My Scoped Timer");
 * ```
 *
 * Example:
 * ```c
 * void my_function() {
 *   nya_perf_time_this_function();
 *   ...
 * }
 *
 * NYA_PerfMeasurement* timer = nya_perf_timer_get("my_function");
 * ```
 * */
#pragma once

#include "nyangine/base/base_string.h"

/**
 * Whether the timers are compiled in.
 * */
#if (NYA_DEVELOPMENT_BUILD || defined(NYA_PERF_FORCE_DEBUG)) && !defined(NYA_PERF_FORCE_NODEBUG)
#define NYA_PERF_ENABLED 1
#else
#define NYA_PERF_ENABLED 0
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPE DEFINITIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_PERF_MEASUREMENT_SAMPLES 64

typedef struct NYA_PerfMeasurement NYA_PerfMeasurement;
typedef struct NYA_PerfSpan        NYA_PerfSpan;
typedef struct NYA_PerfStats       NYA_PerfStats;
nya_derive_array(NYA_PerfMeasurement);
nya_derive_array(NYA_PerfSpan);

/**
 * NYA_PerfMeasurement
 */
struct NYA_PerfMeasurement {
    /** A copy owned by the perf arena, since callers pass strings from a game DLL that a reload unmaps. */
    NYA_ConstCString name;

    /** The pointer last passed in, compared as a fast path and never read. */
    NYA_ConstCString key;

    b8               is_running;

    u64 started_ns[NYA_PERF_MEASUREMENT_SAMPLES];
    u64 ended_ns[NYA_PERF_MEASUREMENT_SAMPLES];
    u64 elapsed_ns[NYA_PERF_MEASUREMENT_SAMPLES];
    u64 last_elapsed_ns;
    u64 last_elapsed_ms;
    u64 last_elapsed_s;

    u64 started_cycles[NYA_PERF_MEASUREMENT_SAMPLES];
    u64 ended_cycles[NYA_PERF_MEASUREMENT_SAMPLES];
    u64 elapsed_cycles[NYA_PERF_MEASUREMENT_SAMPLES];
    u64 last_elapsed_cycles;

    /**
     * Which frame each sample belongs to, as counted by nya_perf_frame_begin.
     * */
    u64 frame[NYA_PERF_MEASUREMENT_SAMPLES];

    /**
     * How many timers were already running when this sample started.
     * */
    u32 depth[NYA_PERF_MEASUREMENT_SAMPLES];

    /** Slots written so far, saturating at NYA_PERF_MEASUREMENT_SAMPLES. Which of the ring's entries mean anything. */
    u64 sample_count;

    /** Every completed run since the timer was first seen, unbounded by the ring. */
    u64 total_runs;

    u64 current;
};

/**
 * One completed scope, as a frame breakdown sees it.
 * */
struct NYA_PerfSpan {
    NYA_ConstCString name;

    u64 frame;
    u32 depth;

    /** Both relative to the perf epoch, so spans from different timers are directly comparable. */
    u64 started_ns;
    u64 ended_ns;

    u64 elapsed_ns;
    u64 elapsed_cycles;
};

/**
 * Aggregates over a measurement's ring, so nobody hand writes the loop this header used to document.
 * */
struct NYA_PerfStats {
    u64 sample_count;

    u64 min_ns;
    u64 max_ns;
    u64 total_ns;
    f64 average_ns;

    u64 min_cycles;
    u64 max_cycles;
    u64 total_cycles;
    f64 average_cycles;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * With the module compiled out, the readers return nothing rather than panicking.
 */
// clang-format off
#if NYA_PERF_ENABLED
#define nya_perf_timer_get(name)      _nya_perf_timer_get(name)
#define nya_perf_timer_start(name)    _nya_perf_timer_start(name)
#define nya_perf_timer_stop(name)     _nya_perf_timer_stop(name)
#define nya_perf_timer_reset(name)    _nya_perf_timer_reset(name)
#define nya_perf_timer_get_all()      _nya_perf_timer_get_all()
#define nya_perf_frame_begin()        _nya_perf_frame_begin()
#define nya_perf_frame_current()      _nya_perf_frame_current()
#define nya_perf_frame_spans(f, a)    _nya_perf_frame_spans(f, a)
#define nya_perf_stats(measurement)   _nya_perf_stats(measurement)
#define nya_perf_report()             _nya_perf_report()
#define nya_perf_frame_report(frame)  _nya_perf_frame_report(frame)
#else
#define nya_perf_timer_get(name)      ({ nya_unused(name); (NYA_PerfMeasurement*)nullptr; })
#define nya_perf_timer_start(name)    nya_unused(name)
#define nya_perf_timer_stop(name)     nya_unused(name)
#define nya_perf_timer_reset(name)    nya_unused(name)
#define nya_perf_timer_get_all()      ((NYA_ArrayᐸNYA_PerfMeasurementᐳ*)nullptr)
#define nya_perf_frame_begin()        ((void)0)
#define nya_perf_frame_current()      ((u64)0)
#define nya_perf_frame_spans(f, a)    ({ nya_unused(f); nya_unused(a); (u32)0; })
#define nya_perf_stats(measurement)   ({ nya_unused(measurement); (NYA_PerfStats){ 0 }; })
#define nya_perf_report()             ((void)0)
#define nya_perf_frame_report(frame)  ({ nya_unused(frame); (void)0; })
#endif // NYA_PERF_ENABLED

#define nya_perf_time_this_scope(name) __attr_cleanup(_nya_perf_cleanup) NYA_CString CONCAT(_nya_perf_scope_timer_, __LINE__) = (nya_perf_timer_start(name), (NYA_CString)(name))
#define nya_perf_time_this_function()  nya_perf_time_this_scope(__FUNCTION__)
// clang-format on

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_PERF_ENABLED
NYA_API NYA_PerfMeasurement*            _nya_perf_timer_get(NYA_ConstCString name);
NYA_API void                            _nya_perf_timer_start(NYA_ConstCString name);
NYA_API void                            _nya_perf_timer_stop(NYA_ConstCString name);
NYA_API void                            _nya_perf_timer_reset(NYA_ConstCString name);
NYA_API NYA_ArrayᐸNYA_PerfMeasurementᐳ* _nya_perf_timer_get_all(void);

/**
 * Starts a new frame for the purposes of timing, and returns nothing.
 * */
NYA_API void _nya_perf_frame_begin(void);

/** The frame number nya_perf_frame_begin is currently on. */
NYA_API u64 _nya_perf_frame_current(void) __attr_no_discard;

/**
 * Collects every completed span belonging to `frame` into `out_spans`, ordered by start time.
 * */
NYA_API u32 _nya_perf_frame_spans(u64 frame, NYA_ArrayᐸNYA_PerfSpanᐳ* out_spans);

/** Aggregates over the measurement's ring. A null measurement gives a zeroed result rather than a fault. */
NYA_API NYA_PerfStats _nya_perf_stats(const NYA_PerfMeasurement* measurement) __attr_no_discard;

/** Logs every timer as a table: name, runs, last, min, average, max. */
NYA_API void _nya_perf_report(void);

/** Logs one frame's spans as an indented tree, which is how the frame was actually put together. */
NYA_API void _nya_perf_frame_report(u64 frame);
#endif // NYA_PERF_ENABLED

NYA_API inline void _nya_perf_cleanup(NYA_CString* name_ptr) {
    if (name_ptr && *name_ptr) nya_perf_timer_stop(*name_ptr);
}

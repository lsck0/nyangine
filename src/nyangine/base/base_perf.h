/**
 * @file base_perf.h
 *
 * Debug performance measuring.
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPE DEFINITIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_PERF_MEASUREMENT_SAMPLES 64

typedef struct NYA_PerfMeasurement NYA_PerfMeasurement;
nya_derive_array(NYA_PerfMeasurement);

/**
 * NYA_PerfMeasurement
 *
 * The measurements are stored in a ring buffer, so to print in order:
 * for (u64 i = measurement->current + 1; i != measurement->current; i = (i + 1) % NYA_PERF_MEASUREMENT_SAMPLES) {}
 *
 * Alternatively, just access the last_elapsed_* members.
 */
struct NYA_PerfMeasurement {
    NYA_ConstCString name;
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

    u64 current;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// clang-format off
#if NYA_DEBUG
#define nya_perf_timer_get(name)   _nya_perf_timer_get(name)
#define nya_perf_timer_start(name) _nya_perf_timer_start(name)
#define nya_perf_timer_stop(name)  _nya_perf_timer_stop(name)
#define nya_perf_timer_get_all()   _nya_perf_timer_get_all()
#else
#define nya_perf_timer_get(name)   ({ nya_unused(name); nya_panic("Perf module is disabled."); })
#define nya_perf_timer_start(name) nya_unused(name)
#define nya_perf_timer_stop(name)  nya_unused(name)
#define nya_perf_timer_get_all()   nya_panic("Perf module is disabled.")
#endif // NYA_DEBUG

#define nya_perf_time_this_scope(name) __attr_cleanup(_nya_perf_cleanup) NYA_CString CONCAT(_nya_perf_scope_timer_, __LINE__) = (nya_perf_timer_start(name), (NYA_CString)(name))
#define nya_perf_time_this_function()  nya_perf_time_this_scope(__FUNCTION__)
// clang-format on

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_DEBUG
NYA_API NYA_PerfMeasurement*            _nya_perf_timer_get(NYA_ConstCString name);
NYA_API void                            _nya_perf_timer_start(NYA_ConstCString name);
NYA_API void                            _nya_perf_timer_stop(NYA_ConstCString name);
NYA_API void                            _nya_perf_timer_reset(NYA_ConstCString name);
NYA_API NYA_ArrayᐸNYA_PerfMeasurementᐳ* _nya_perf_timer_get_all(void);
#endif // NYA_DEBUG

NYA_API inline void _nya_perf_cleanup(NYA_CString* name_ptr) {
    if (name_ptr && *name_ptr) nya_perf_timer_stop(*name_ptr);
}

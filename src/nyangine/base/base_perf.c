#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

__attr_allow_unused NYA_INTERNAL NYA_Arena*                      _nya_perf_arena         = nullptr;
__attr_allow_unused NYA_INTERNAL NYA_ArrayᐸNYA_PerfMeasurementᐳ* _nya_perf_measurements  = nullptr;
__attr_allow_unused NYA_INTERNAL u64                             _nya_perf_start_time_ns = 0;
__attr_allow_unused NYA_INTERNAL u64                             _nya_perf_start_cycles  = 0;

__attr_allow_unused NYA_INTERNAL void _nya_perf_init(void);
__attr_allow_unused NYA_INTERNAL void _nya_perf_shutdown(void);
__attr_allow_unused NYA_INTERNAL u64  _nya_perf_time_since_start_ns(void);
__attr_allow_unused NYA_INTERNAL u64  _nya_perf_cycles_since_start(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_PerfMeasurement* _nya_perf_timer_get(NYA_ConstCString name) {
    nya_assert(name);

    nya_array_foreach (_nya_perf_measurements, measurement) {
        if (nya_string_equals(measurement->name, name)) return measurement;
    }

    return nullptr;
}

void _nya_perf_timer_start(NYA_ConstCString name) {
    nya_assert(name);

    NYA_PerfMeasurement* measurement = _nya_perf_timer_get(name);
    if (measurement != nullptr) {
        u64 index                          = (measurement->current + 1) % NYA_PERF_MEASUREMENT_SAMPLES;
        measurement->is_running            = true;
        measurement->started_ns[index]     = _nya_perf_time_since_start_ns();
        measurement->ended_ns[index]       = 0;
        measurement->elapsed_ns[index]     = 0;
        measurement->started_cycles[index] = _nya_perf_cycles_since_start();
        measurement->ended_cycles[index]   = 0;
        measurement->elapsed_cycles[index] = 0;
        measurement->current               = index;
        return;
    }

    NYA_PerfMeasurement new_measurement = {
        .name           = name,
        .is_running     = true,
        .started_ns     = { _nya_perf_time_since_start_ns() },
        .ended_ns       = { 0 },
        .elapsed_ns     = { 0 },
        .started_cycles = { _nya_perf_cycles_since_start() },
        .ended_cycles   = { 0 },
        .elapsed_cycles = { 0 },
        .current        = 0,
    };
    nya_array_push_back(_nya_perf_measurements, new_measurement);
}

void _nya_perf_timer_stop(NYA_ConstCString name) {
    nya_assert(name);

    NYA_PerfMeasurement* measurement = _nya_perf_timer_get(name);
    nya_assert(measurement != nullptr, "Timer '%s' was not started.", name);

    u64 index                          = measurement->current;
    measurement->is_running            = false;
    measurement->ended_ns[index]       = _nya_perf_time_since_start_ns();
    measurement->elapsed_ns[index]     = measurement->ended_ns[index] - measurement->started_ns[index];
    measurement->last_elapsed_ns       = measurement->elapsed_ns[index];
    measurement->last_elapsed_ms       = measurement->last_elapsed_ns / 1000000;
    measurement->last_elapsed_s        = measurement->last_elapsed_ns / 1000000000;
    measurement->ended_cycles[index]   = _nya_perf_cycles_since_start();
    measurement->elapsed_cycles[index] = measurement->ended_cycles[index] - measurement->started_cycles[index];
    measurement->last_elapsed_cycles   = measurement->elapsed_cycles[index];
}

NYA_ArrayᐸNYA_PerfMeasurementᐳ* _nya_perf_timer_get_all(void) {
    return _nya_perf_measurements;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_DEBUG
__attr_constructor NYA_INTERNAL void _nya_perf_init(void) {
    nya_assert(_nya_perf_measurements == nullptr);

    _nya_perf_arena        = nya_arena_create(.name = "Perf Arena");
    _nya_perf_measurements = nya_array_create(_nya_perf_arena, NYA_PerfMeasurement);

    _nya_perf_start_time_ns = nya_clock_get_timestamp_ns();
    _nya_perf_start_cycles  = __rdtsc();
}

__attr_destructor NYA_INTERNAL void _nya_perf_shutdown(void) {
    nya_arena_destroy(_nya_perf_arena);
}
#endif // NYA_DEBUG

NYA_INTERNAL u64 _nya_perf_time_since_start_ns(void) {
    return nya_clock_get_timestamp_ns() - _nya_perf_start_time_ns;
}

NYA_INTERNAL u64 _nya_perf_cycles_since_start(void) {
    return __rdtsc() - _nya_perf_start_cycles;
}

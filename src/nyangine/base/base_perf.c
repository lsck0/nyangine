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

/** Which frame nya_perf_frame_begin is on. Everything measured is tagged with this. */
__attr_allow_unused NYA_INTERNAL u64 _nya_perf_frame = 0;

/**
 * Timers currently running, which is the nesting depth the next sample is recorded at.
 *
 * A plain counter rather than a stack of names: what a span needs to know is how deep it sits, and
 * a timer started inside another is by definition one level below it. Incremented on start and
 * decremented on stop, so a scope timer brackets it exactly.
 * */
__attr_allow_unused NYA_INTERNAL u32 _nya_perf_depth = 0;

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

    /*
     * Pointer first, contents only as a fallback. The same trick _nya_arena_callsite_for uses.
     *
     * This runs on every timer start *and* every stop, so a scope timer pays for it twice, and it
     * was a strcmp against every registered timer both times — the profiler's own lookup being the
     * most expensive thing in the profiler. Nearly every name reaching here is __FUNCTION__ or a
     * string literal, and the compiler pools those, so the pointer matches on the first comparison
     * for a timer that has run before.
     *
     * The contents comparison stays because it has to: a name assembled at runtime, or two
     * translation units that did not get a pooled literal, are both still the same timer by name.
     * Pointer equality only ever short circuits a match the strcmp would also have found, so this
     * changes speed and nothing else.
     */
    nya_array_foreach (_nya_perf_measurements, measurement) {
        if (measurement->name == name) return measurement;
    }

    nya_array_foreach (_nya_perf_measurements, measurement) {
        if (nya_string_equals(measurement->name, name)) return measurement;
    }

    return nullptr;
}

void _nya_perf_timer_start(NYA_ConstCString name) {
    nya_assert(name);

    // Read before the depth is bumped: a top level scope sits at depth 0, and the timer it opens
    // inside itself at depth 1.
    u32 depth = _nya_perf_depth++;

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
        measurement->frame[index]          = _nya_perf_frame;
        measurement->depth[index]          = depth;
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
        .frame          = { _nya_perf_frame },
        .depth          = { depth },
        .current        = 0,
    };
    nya_array_push_back(_nya_perf_measurements, new_measurement);
}

void _nya_perf_timer_stop(NYA_ConstCString name) {
    nya_assert(name);

    NYA_PerfMeasurement* measurement = _nya_perf_timer_get(name);
    nya_assert(measurement != nullptr, "Timer '%s' was not started.", name);

    if (_nya_perf_depth > 0) _nya_perf_depth--;

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

    // Counted on stop rather than start, so both only ever describe finished work. A timer that is
    // still running is not a sample anything should average over.
    if (measurement->sample_count < NYA_PERF_MEASUREMENT_SAMPLES) measurement->sample_count++;
    measurement->total_runs++;
}

void _nya_perf_timer_reset(NYA_ConstCString name) {
    nya_assert(name);

    NYA_PerfMeasurement* measurement = _nya_perf_timer_get(name);
    if (measurement == nullptr) return;

    // The name is kept and the samples are dropped, so a caller measuring one level load after
    // another compares like with like rather than averaging across both. Was declared here and
    // never defined, which meant there was no way to do that at all.
    NYA_ConstCString name_copy = measurement->name;
    *measurement               = (NYA_PerfMeasurement){ .name = name_copy };
}

NYA_ArrayᐸNYA_PerfMeasurementᐳ* _nya_perf_timer_get_all(void) {
    return _nya_perf_measurements;
}

void _nya_perf_frame_begin(void) {
    _nya_perf_frame++;

    // Anything still running across a frame boundary would otherwise leave the depth counter
    // permanently raised, and every later span would be recorded one level too deep. A frame is the
    // natural place to notice, since nothing is expected to be open at the top of one.
    if (_nya_perf_depth != 0) {
        nya_log_warn("Perf: " FMTu32 " timer(s) still running at the start of frame " FMTu64 ", nesting depth reset.", _nya_perf_depth, _nya_perf_frame);
        _nya_perf_depth = 0;
    }
}

u64 _nya_perf_frame_current(void) {
    return _nya_perf_frame;
}

NYA_PerfStats _nya_perf_stats(const NYA_PerfMeasurement* measurement) {
    NYA_PerfStats stats = { 0 };
    if (measurement == nullptr || measurement->sample_count == 0) return stats;

    stats.min_ns     = UINT64_MAX;
    stats.min_cycles = UINT64_MAX;

    /*
     * Walked backwards from the newest sample, `sample_count` of them.
     *
     * The ring is written at `current` and wraps, so the valid entries are the last sample_count
     * slots ending at current — not the first sample_count slots of the array. Reading it forwards
     * would average whatever a wrapped ring left in the gap.
     */
    for (u64 i = 0; i < measurement->sample_count; i++) {
        u64 index = (measurement->current + NYA_PERF_MEASUREMENT_SAMPLES - i) % NYA_PERF_MEASUREMENT_SAMPLES;

        // Still running, so its elapsed is not meaningful yet.
        if (measurement->ended_ns[index] == 0 && measurement->started_ns[index] != 0) continue;

        u64 elapsed_ns     = measurement->elapsed_ns[index];
        u64 elapsed_cycles = measurement->elapsed_cycles[index];

        if (elapsed_ns < stats.min_ns) stats.min_ns = elapsed_ns;
        if (elapsed_ns > stats.max_ns) stats.max_ns = elapsed_ns;
        stats.total_ns += elapsed_ns;

        if (elapsed_cycles < stats.min_cycles) stats.min_cycles = elapsed_cycles;
        if (elapsed_cycles > stats.max_cycles) stats.max_cycles = elapsed_cycles;
        stats.total_cycles += elapsed_cycles;

        stats.sample_count++;
    }

    if (stats.sample_count == 0) return (NYA_PerfStats){ 0 };

    stats.average_ns     = (f64)stats.total_ns / (f64)stats.sample_count;
    stats.average_cycles = (f64)stats.total_cycles / (f64)stats.sample_count;

    return stats;
}

u32 _nya_perf_frame_spans(u64 frame, NYA_ArrayᐸNYA_PerfSpanᐳ* out_spans) {
    nya_assert(out_spans != nullptr);

    u32 appended = 0;

    nya_array_foreach (_nya_perf_measurements, measurement) {
        for (u64 i = 0; i < measurement->sample_count; i++) {
            u64 index = (measurement->current + NYA_PERF_MEASUREMENT_SAMPLES - i) % NYA_PERF_MEASUREMENT_SAMPLES;

            if (measurement->frame[index] != frame) continue;
            if (measurement->ended_ns[index] == 0) continue; // never finished, so it has no duration

            nya_array_push_back(out_spans, ((NYA_PerfSpan){
                .name           = measurement->name,
                .frame          = frame,
                .depth          = measurement->depth[index],
                .started_ns     = measurement->started_ns[index],
                .ended_ns       = measurement->ended_ns[index],
                .elapsed_ns     = measurement->elapsed_ns[index],
                .elapsed_cycles = measurement->elapsed_cycles[index],
            }));
            appended++;
        }
    }

    /*
     * Sorted by start, which is what makes the result a timeline rather than a bag.
     *
     * Insertion sort because a frame holds a handful of spans and the array is nearly ordered
     * already — measurements are walked in creation order, which for scope timers is roughly the
     * order they first ran.
     */
    for (u64 i = 1; i < out_spans->length; i++) {
        NYA_PerfSpan key = out_spans->items[i];
        u64          j   = i;
        while (j > 0 && out_spans->items[j - 1].started_ns > key.started_ns) {
            out_spans->items[j] = out_spans->items[j - 1];
            j--;
        }
        out_spans->items[j] = key;
    }

    return appended;
}

void _nya_perf_report(void) {
    if (_nya_perf_measurements == nullptr || _nya_perf_measurements->length == 0) {
        nya_log_info("Perf: no timers recorded.");
        return;
    }

    nya_log_info("Perf: %-32s %8s %10s %10s %10s %10s", "timer", "runs", "last ms", "min ms", "avg ms", "max ms");

    nya_array_foreach (_nya_perf_measurements, measurement) {
        NYA_PerfStats stats = _nya_perf_stats(measurement);
        if (stats.sample_count == 0) continue;

        nya_log_info(
            "Perf: %-32s %8" PRIu64 " %10.3f %10.3f %10.3f %10.3f",
            measurement->name,
            measurement->total_runs,
            (f64)measurement->last_elapsed_ns / 1'000'000.0,
            (f64)stats.min_ns / 1'000'000.0,
            stats.average_ns / 1'000'000.0,
            (f64)stats.max_ns / 1'000'000.0
        );
    }
}

void _nya_perf_frame_report(u64 frame) {
    NYA_Arena                arena = nya_arena_create_on_stack(.name = "perf_frame_report");
    defer                    nya_arena_destroy_on_stack(&arena);
    NYA_ArrayᐸNYA_PerfSpanᐳ* spans = nya_array_create(&arena, NYA_PerfSpan);

    u32 count = _nya_perf_frame_spans(frame, spans);
    if (count == 0) {
        nya_log_info("Perf: frame " FMTu64 " has no recorded spans.", frame);
        return;
    }

    // The frame's own span, if there is one, is the total everything else is a fraction of.
    u64 frame_total_ns = 0;
    nya_array_foreach (spans, span) {
        if (span->depth == 0 && span->elapsed_ns > frame_total_ns) frame_total_ns = span->elapsed_ns;
    }

    nya_log_info("Perf: frame " FMTu64 ", " FMTu32 " spans", frame, count);

    nya_array_foreach (spans, span) {
        // Indented by nesting depth, which is what makes the output the shape of the frame rather
        // than a list of names that happen to be in time order.
        char indent[32];
        u64  requested = (u64)span->depth * 2;
        u64  width     = requested < sizeof(indent) - 1 ? requested : sizeof(indent) - 1;
        nya_memset(indent, ' ', width);
        indent[width] = '\0';

        f64 share = frame_total_ns > 0 ? (f64)span->elapsed_ns * 100.0 / (f64)frame_total_ns : 0.0;

        nya_log_info("Perf:   %s%-*s %8.3f ms  %5.1f%%", indent, 30 - (s32)width, span->name, (f64)span->elapsed_ns / 1'000'000.0, share);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_PERF_ENABLED
__attr_constructor NYA_INTERNAL void _nya_perf_init(void) {
    nya_assert(_nya_perf_measurements == nullptr);

    _nya_perf_arena        = nya_arena_create(.name = "Perf Arena");
    _nya_perf_measurements = nya_array_create(_nya_perf_arena, NYA_PerfMeasurement);

    _nya_perf_start_time_ns = nya_clock_get_monotonic_ns();
    _nya_perf_start_cycles  = __rdtsc();
}

__attr_destructor NYA_INTERNAL void _nya_perf_shutdown(void) {
    nya_arena_destroy(_nya_perf_arena);
}
#endif // NYA_PERF_ENABLED

NYA_INTERNAL u64 _nya_perf_time_since_start_ns(void) {
    return nya_clock_get_monotonic_ns() - _nya_perf_start_time_ns;
}

NYA_INTERNAL u64 _nya_perf_cycles_since_start(void) {
    return __rdtsc() - _nya_perf_start_cycles;
}

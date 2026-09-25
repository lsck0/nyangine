/**
 * @file debug_trace.c
 * */
#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_TRACE_ENABLED

typedef struct {
    char name[NYA_TRACE_NAME_MAX];

    /** Arenas whose name starts with this are the feature's RAM. Empty for none. */
    char arena_prefix[NYA_TRACE_NAME_MAX];
} _NYA_TraceFeatureEntry;

typedef struct {
    _NYA_TraceFeatureEntry features[NYA_TRACE_FEATURE_MAX];

    /** u32 for the ceiling registry. */
    u32 feature_count;

    /* The open frame. CPU time, calls and draws come from any thread; GPU time from the renderer's. */
    atomic u64 cpu_ns[NYA_TRACE_FEATURE_MAX];
    atomic u32 calls[NYA_TRACE_FEATURE_MAX];
    atomic u32 draws[NYA_TRACE_FEATURE_MAX];
    u64        gpu_ns[NYA_TRACE_FEATURE_MAX];

    /* Closed frames, a ring per feature. Nanoseconds saturate at four seconds. */
    u32 history_cpu_ns[NYA_TRACE_FEATURE_MAX][NYA_TRACE_HISTORY];
    u32 history_gpu_ns[NYA_TRACE_FEATURE_MAX][NYA_TRACE_HISTORY];
    u32 history_calls[NYA_TRACE_FEATURE_MAX][NYA_TRACE_HISTORY];
    u32 history_draws[NYA_TRACE_FEATURE_MAX][NYA_TRACE_HISTORY];
    u32 history_cursor;
    u32 history_count;

    /** Read by every scope on every thread. */
    atomic b8 active;

    u64 frame;
    u64 requested_frame;

    /**
     * The recording capture's buffer, null when none records. Cleared a frame before the writer takes the buffer, so a
     * scope on another thread that loaded it just before has finished its write.
     * */
    NYA_TraceEvent* _Atomic capture_events;
    atomic u32              capture_count;

    /** capture_count clamped, for the ceiling registry. */
    u32 capture_live;

    u32 capture_frames_left;

    /** The buffer waiting out its grace frame before the writer takes it. */
    NYA_TraceEvent* capture_pending;

    atomic b8 capture_writing;
    u64       capture_epoch_ns;
    char      capture_path[256];

    b8 registered;
} _NYA_Trace;

/** Engine features are in the table from the start, so their ids are constants. */
NYA_INTERNAL _NYA_Trace _nya_trace = {
    .features = {
        [NYA_TRACE_OTHER]             = { "other", "" },
        [NYA_TRACE_SHADOWS]           = { "shadows", "" },
        [NYA_TRACE_SCENE]             = { "scene", "render_system" },
        [NYA_TRACE_TRANSPARENT]       = { "transparent", "" },
        [NYA_TRACE_PARTICLES]         = { "particles", "" },
        [NYA_TRACE_FLUID]             = { "fluid", "" },
        [NYA_TRACE_DECALS]            = { "decals", "" },
        [NYA_TRACE_SKINNING]          = { "skinning", "" },
        [NYA_TRACE_TARGETS]           = { "targets", "" },
        [NYA_TRACE_OCCLUSION]         = { "occlusion", "" },
        [NYA_TRACE_INK]               = { "ink", "" },
        [NYA_TRACE_DEPTH_OF_FIELD]    = { "depth of field", "" },
        [NYA_TRACE_ANTIALIAS]         = { "antialiasing", "" },
        [NYA_TRACE_GRADE]             = { "grade", "" },
        [NYA_TRACE_BLOOM]             = { "bloom", "" },
        [NYA_TRACE_SPEED_LINES]       = { "speed lines", "" },
        [NYA_TRACE_POST]              = { "post", "" },
        [NYA_TRACE_HDR_OUTPUT]        = { "hdr output", "" },
        [NYA_TRACE_BATCH2D]           = { "2d batch", "" },
        [NYA_TRACE_UI]                = { "ui", "" },
        [NYA_TRACE_TEXT]              = { "text", "i18n" },
        [NYA_TRACE_DEBUG_DRAW]        = { "debug draw", "" },
        [NYA_TRACE_PHYSICS2D]         = { "physics 2d", "" },
        [NYA_TRACE_PHYSICS3D]         = { "physics 3d", "" },
        [NYA_TRACE_AUDIO_PROPAGATION] = { "audio rays", "" },
        [NYA_TRACE_AUDIO_EFFECTS]     = { "audio effects", "audio" },
        [NYA_TRACE_NET_ENCODE]        = { "net encode", "net_" },
        [NYA_TRACE_NET_DECODE]        = { "net decode", "" },
        [NYA_TRACE_ASSETS]            = { "assets", "asset" },
        [NYA_TRACE_HOT_RELOAD]        = { "hot reload", "" },
    },
    .feature_count = NYA_TRACE_ENGINE_FEATURES,
};

NYA_INTERNAL thread_local NYA_TraceFeature _nya_trace_current     = NYA_TRACE_OTHER;
NYA_INTERNAL thread_local u64              _nya_trace_children_ns = 0;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Appends an event to the recording capture, if there is one. Any thread; a GPU group goes under NYA_TRACE_GPU_THREAD. */
NYA_INTERNAL void _nya_trace_capture_append(NYA_TraceFeature feature, u64 started_ns, u64 ended_ns, b8 gpu);

/** Moves the capture along a frame: counts down, clears the buffer when done, and hands it to the writer a frame later. */
NYA_INTERNAL void _nya_trace_capture_frame_end(void);

/** Formats and writes a capture, then frees it. Runs on a detached thread. */
NYA_INTERNAL s32 SDLCALL _nya_trace_capture_write(void* events);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_TraceFeature nya_trace_feature_register(NYA_ConstCString name, NYA_ConstCString arena_prefix) {
    nya_assert(name != nullptr && name[0] != '\0', "a trace feature needs a name");

    for (u32 i = 0; i < _nya_trace.feature_count; i++) {
        if (strncmp(_nya_trace.features[i].name, name, NYA_TRACE_NAME_MAX - 1) == 0) return (NYA_TraceFeature)i;
    }

    if (_nya_trace.feature_count >= NYA_TRACE_FEATURE_MAX) {
        nya_log_warn("The trace feature table is full at %d; '%s' is traced as other. Raise NYA_TRACE_FEATURE_MAX.", NYA_TRACE_FEATURE_MAX, name);
        return NYA_TRACE_OTHER;
    }

    // copied: the name may live in a game DLL that a reload unmaps.
    _NYA_TraceFeatureEntry* entry = &_nya_trace.features[_nya_trace.feature_count];
    (void)snprintf(entry->name, sizeof(entry->name), "%s", name);
    (void)snprintf(entry->arena_prefix, sizeof(entry->arena_prefix), "%s", arena_prefix != nullptr ? arena_prefix : "");

    return (NYA_TraceFeature)_nya_trace.feature_count++;
}

u32 nya_trace_feature_count(void) {
    return _nya_trace.feature_count;
}

NYA_ConstCString nya_trace_feature_name(NYA_TraceFeature feature) {
    nya_assert(feature < _nya_trace.feature_count);

    return _nya_trace.features[feature].name;
}

NYA_TraceFeature nya_trace_feature_current(void) {
    return _nya_trace_current;
}

void nya_trace_request(void) {
    _nya_trace.requested_frame = _nya_trace.frame;
}

b8 nya_trace_active(void) {
    return atomic_load_explicit(&_nya_trace.active, memory_order_relaxed);
}

void nya_trace_frame_end(void) {
    if (!_nya_trace.registered) {
        nya_ceiling_register("trace_features", NYA_TRACE_FEATURE_MAX, &_nya_trace.feature_count);
        nya_ceiling_register("trace_events", NYA_TRACE_CAPTURE_EVENTS_MAX, &_nya_trace.capture_live);
        _nya_trace.registered = true;
    }

    b8 was_active = nya_trace_active();

    if (was_active) {
        u32 cursor = _nya_trace.history_cursor;

        for (u32 f = 0; f < _nya_trace.feature_count; f++) {
            u64 cpu_ns = atomic_exchange_explicit(&_nya_trace.cpu_ns[f], 0, memory_order_relaxed);

            _nya_trace.history_cpu_ns[f][cursor] = (u32)nya_min(cpu_ns, (u64)UINT32_MAX);
            _nya_trace.history_gpu_ns[f][cursor] = (u32)nya_min(_nya_trace.gpu_ns[f], (u64)UINT32_MAX);
            _nya_trace.history_calls[f][cursor]  = atomic_exchange_explicit(&_nya_trace.calls[f], 0, memory_order_relaxed);
            _nya_trace.history_draws[f][cursor]  = atomic_exchange_explicit(&_nya_trace.draws[f], 0, memory_order_relaxed);
            _nya_trace.gpu_ns[f]                 = 0;
        }

        _nya_trace.history_cursor = (cursor + 1) % NYA_TRACE_HISTORY;
        _nya_trace.history_count  = nya_min(_nya_trace.history_count + 1, (u32)NYA_TRACE_HISTORY);
    }

    _nya_trace_capture_frame_end();

    _nya_trace.frame++;

    b8 capturing = atomic_load(&_nya_trace.capture_events) != nullptr;
    b8 active    = capturing || _nya_trace.requested_frame + 1 >= _nya_trace.frame;

    // a readout opened long after the last one should not average in what it saw then.
    if (active && !was_active) {
        _nya_trace.history_count  = 0;
        _nya_trace.history_cursor = 0;

        for (u32 f = 0; f < NYA_TRACE_FEATURE_MAX; f++) {
            atomic_store(&_nya_trace.cpu_ns[f], 0);
            atomic_store(&_nya_trace.calls[f], 0);
            atomic_store(&_nya_trace.draws[f], 0);
            _nya_trace.gpu_ns[f] = 0;
        }
    }

    atomic_store_explicit(&_nya_trace.active, active, memory_order_relaxed);
}

void nya_trace_draws(u32 count) {
    if (!nya_trace_active()) return;

    atomic_fetch_add_explicit(&_nya_trace.draws[_nya_trace_current], count, memory_order_relaxed);
}

void nya_trace_gpu(NYA_TraceFeature feature, u64 started_ns, u64 ended_ns) {
    nya_assert(feature < NYA_TRACE_FEATURE_MAX);
    nya_assert(ended_ns >= started_ns);

    _nya_trace.gpu_ns[feature] += ended_ns - started_ns;

    _nya_trace_capture_append(feature, started_ns, ended_ns, true);
}

u32 nya_trace_stats(OUT NYA_TraceStats* out, u32 capacity, NYA_TraceSort sort) {
    nya_assert(out != nullptr || capacity == 0);
    nya_assert(sort < NYA_TRACE_SORT_COUNT);

    u64 ram_bytes[NYA_TRACE_FEATURE_MAX] = { 0 };

    // each named arena to the first feature whose prefix it starts with.
    u32 arenas = nya_arena_registry_count();

    for (u32 i = 0; i < arenas; i++) {
        NYA_Arena* arena = nya_arena_registry_at(i);
        if (arena == nullptr) continue;

        NYA_ArenaStats stats = nya_arena_stats(arena);
        if (stats.name == nullptr) continue;

        for (u32 f = 0; f < _nya_trace.feature_count; f++) {
            const char* prefix = _nya_trace.features[f].arena_prefix;
            if (prefix[0] == '\0' || strncmp(stats.name, prefix, strlen(prefix)) != 0) continue;

            ram_bytes[f] += stats.used_bytes;
            break;
        }
    }

    u32 count   = 0;
    u32 history = _nya_trace.history_count;

    for (u32 f = 0; f < _nya_trace.feature_count && count < capacity; f++) {
        f64 cpu_average = 0.0, gpu_average = 0.0, calls = 0.0, draws = 0.0;
        u32 cpu_max = 0, gpu_max = 0, unused = 0;

        nya_trace_window(_nya_trace.history_cpu_ns[f], history, &cpu_average, &cpu_max);
        nya_trace_window(_nya_trace.history_gpu_ns[f], history, &gpu_average, &gpu_max);
        nya_trace_window(_nya_trace.history_calls[f], history, &calls, &unused);
        nya_trace_window(_nya_trace.history_draws[f], history, &draws, &unused);

        NYA_TraceStats row = {
            .name       = _nya_trace.features[f].name,
            .feature    = (NYA_TraceFeature)f,
            .cpu_ms     = cpu_average / 1e6,
            .cpu_max_ms = (f64)cpu_max / 1e6,
            .gpu_ms     = gpu_average / 1e6,
            .gpu_max_ms = (f64)gpu_max / 1e6,
            .has_gpu    = gpu_max > 0,
            .vram_bytes = nya_gpu_memory_feature_bytes((NYA_TraceFeature)f),
            .ram_bytes  = ram_bytes[f],
            .calls      = (f32)calls,
            .draws      = (f32)draws,
        };

        if (cpu_max == 0 && gpu_max == 0 && row.vram_bytes == 0 && row.ram_bytes == 0 && draws == 0.0) continue;

        out[count++] = row;
    }

    nya_trace_sort(out, count, sort);

    return count;
}

void nya_trace_report(void) {
    NYA_TraceStats rows[NYA_TRACE_FEATURE_MAX];
    u32            count = nya_trace_stats(rows, NYA_TRACE_FEATURE_MAX, NYA_TRACE_SORT_CPU);

    nya_log_info("Trace over %u frames: %-16s %9s %9s %9s %9s %10s %10s %7s %7s", _nya_trace.history_count, "feature", "cpu ms", "cpu max", "gpu ms",
                 "gpu max", "vram", "ram", "calls", "draws");

    for (u32 i = 0; i < count; i++) {
        const NYA_TraceStats* row = &rows[i];

        char gpu[16]     = "n/a";
        char gpu_max[16] = "n/a";

        if (row->has_gpu) {
            (void)snprintf(gpu, sizeof(gpu), "%.3f", row->gpu_ms);
            (void)snprintf(gpu_max, sizeof(gpu_max), "%.3f", row->gpu_max_ms);
        }

        nya_log_info("Trace: %-16s %9.3f %9.3f %9s %9s %10llu %10llu %7.1f %7.1f", row->name, row->cpu_ms, row->cpu_max_ms, gpu, gpu_max,
                     (unsigned long long)row->vram_bytes, (unsigned long long)row->ram_bytes, (f64)row->calls, (f64)row->draws);
    }
}

b8 nya_trace_capture_begin(u32 frames, NYA_ConstCString path) {
    nya_assert(frames > 0);
    nya_assert(path != nullptr);

    if (nya_trace_capturing()) return false;

    NYA_TraceEvent* events = nya_malloc(sizeof(NYA_TraceEvent) * NYA_TRACE_CAPTURE_EVENTS_MAX);
    if (events == nullptr) return false;

    (void)snprintf(_nya_trace.capture_path, sizeof(_nya_trace.capture_path), "%s", path);

    _nya_trace.capture_frames_left = frames;
    _nya_trace.capture_epoch_ns    = nya_clock_get_monotonic_ns();
    atomic_store(&_nya_trace.capture_count, 0);

    // on from this frame, not the next: the first captured frame is this one.
    atomic_store(&_nya_trace.capture_events, events);
    atomic_store_explicit(&_nya_trace.active, true, memory_order_relaxed);

    nya_log_info("Capturing %u frames of trace to %s.", frames, path);

    return true;
}

b8 nya_trace_capturing(void) {
    return atomic_load(&_nya_trace.capture_events) != nullptr || _nya_trace.capture_pending != nullptr || atomic_load(&_nya_trace.capture_writing);
}

NYA_TraceScope _nya_trace_scope_begin(NYA_TraceFeature feature) {
    nya_assert(feature < NYA_TRACE_FEATURE_MAX);

    NYA_TraceScope scope = { .feature = feature, .previous = _nya_trace_current };
    _nya_trace_current   = feature;

    if (!nya_trace_active()) return scope;

    // children count from zero inside this scope; the outer scope's tally comes back when it closes.
    scope.outer_children_ns = _nya_trace_children_ns;
    _nya_trace_children_ns  = 0;
    scope.started_ns        = nya_clock_get_monotonic_ns();

    return scope;
}

void _nya_trace_scope_end(NYA_TraceScope* scope) {
    _nya_trace_current = scope->previous;

    if (scope->started_ns == 0) return;

    u64 ended_ns   = nya_clock_get_monotonic_ns();
    u64 elapsed_ns = ended_ns - scope->started_ns;
    u64 own_ns     = elapsed_ns > _nya_trace_children_ns ? elapsed_ns - _nya_trace_children_ns : 0;

    _nya_trace_children_ns = scope->outer_children_ns + elapsed_ns;

    atomic_fetch_add_explicit(&_nya_trace.cpu_ns[scope->feature], own_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&_nya_trace.calls[scope->feature], 1, memory_order_relaxed);

    _nya_trace_capture_append(scope->feature, scope->started_ns, ended_ns, false);
}

#endif // NYA_TRACE_ENABLED

u64 nya_trace_gpu_elapsed(u64 submitted_ns, u64 previous_done_ns, u64 done_ns) {
    // the queue runs one group at a time, so a group submitted while the last one ran starts when that one finished.
    u64 started_ns = nya_max(submitted_ns, previous_done_ns);

    return done_ns > started_ns ? done_ns - started_ns : 0;
}

void nya_trace_window(const u32* samples, u32 count, OUT f64* out_average, OUT u32* out_max) {
    nya_assert(samples != nullptr || count == 0);
    nya_assert(out_average != nullptr && out_max != nullptr);

    u64 total   = 0;
    u32 maximum = 0;

    for (u32 i = 0; i < count; i++) {
        total   += samples[i];
        maximum  = nya_max(maximum, samples[i]);
    }

    *out_average = count > 0 ? (f64)total / (f64)count : 0.0;
    *out_max     = maximum;
}

void nya_trace_sort(NYA_TraceStats* stats, u32 count, NYA_TraceSort sort) {
    nya_assert(stats != nullptr || count == 0);
    nya_assert(sort < NYA_TRACE_SORT_COUNT);

    // insertion, since the table holds a few dozen rows and has to stay stable.
    for (u32 i = 1; i < count; i++) {
        NYA_TraceStats row = stats[i];
        u32            j   = i;

        while (j > 0) {
            const NYA_TraceStats* before = &stats[j - 1];

            b8 moves = false;

            switch (sort) {
                case NYA_TRACE_SORT_CPU:  moves = row.cpu_ms > before->cpu_ms; break;
                case NYA_TRACE_SORT_GPU:  moves = (row.has_gpu ? row.gpu_ms : -1.0) > (before->has_gpu ? before->gpu_ms : -1.0); break;
                case NYA_TRACE_SORT_VRAM: moves = row.vram_bytes > before->vram_bytes; break;
                case NYA_TRACE_SORT_RAM:  moves = row.ram_bytes > before->ram_bytes; break;
                case NYA_TRACE_SORT_NAME: moves = strcmp(row.name, before->name) < 0; break;
                case NYA_TRACE_SORT_COUNT:
                default:                  break;
            }

            if (!moves) break;

            stats[j] = *before;
            j--;
        }

        stats[j] = row;
    }
}

u64 nya_trace_capture_format(const NYA_TraceEvent* events, u32 count, const NYA_ConstCString* names, u32 name_count, u64 epoch_ns, OUT char* out,
                             u64 capacity) {
    nya_assert(events != nullptr || count == 0);
    nya_assert(names != nullptr);
    nya_assert(out != nullptr || capacity == 0);

    u64 used = 0;

    // snprintf semantics throughout: past the capacity the bytes are only counted.
    #define _NYA_TRACE_APPEND(...)                                                                                                                     \
        do {                                                                                                                                         \
            s32 written = snprintf(used < capacity ? out + used : nullptr, used < capacity ? capacity - used : 0, __VA_ARGS__);                      \
            used += written > 0 ? (u64)written : 0;                                                                                                  \
        } while (0)

    _NYA_TRACE_APPEND("{\"traceEvents\":[\n");

    for (u32 i = 0; i < count; i++) {
        const NYA_TraceEvent* event = &events[i];

        NYA_ConstCString name     = event->feature < name_count ? names[event->feature] : "unknown";
        u64              start_ns = event->started_ns > epoch_ns ? event->started_ns - epoch_ns : 0;
        u64              span_ns  = event->ended_ns > event->started_ns ? event->ended_ns - event->started_ns : 0;

        _NYA_TRACE_APPEND("{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"X\",\"pid\":1,\"tid\":%u,\"ts\":%.3f,\"dur\":%.3f}%s\n", name,
                          event->thread == NYA_TRACE_GPU_THREAD ? "gpu" : "cpu", event->thread, (f64)start_ns / 1000.0, (f64)span_ns / 1000.0,
                          i + 1 < count ? "," : "");
    }

    _NYA_TRACE_APPEND("]}\n");

    #undef _NYA_TRACE_APPEND

    return used;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_TRACE_ENABLED

void _nya_trace_capture_append(NYA_TraceFeature feature, u64 started_ns, u64 ended_ns, b8 gpu) {
    NYA_TraceEvent* events = atomic_load(&_nya_trace.capture_events);
    if (events == nullptr) return;

    // claimed before written, so two threads never share a slot. past the end the count keeps growing and nothing is kept.
    u32 index = atomic_fetch_add(&_nya_trace.capture_count, 1);
    if (index >= NYA_TRACE_CAPTURE_EVENTS_MAX) return;

    // the low half of the id, which is what differs between threads of one process.
    u32 thread = gpu ? NYA_TRACE_GPU_THREAD : (u32)SDL_GetCurrentThreadID();

    events[index] = (NYA_TraceEvent){ .started_ns = started_ns, .ended_ns = ended_ns, .thread = thread, .feature = feature };
}

void _nya_trace_capture_frame_end(void) {
    if (_nya_trace.capture_pending != nullptr) {
        NYA_TraceEvent* events      = _nya_trace.capture_pending;
        _nya_trace.capture_pending = nullptr;

        atomic_store(&_nya_trace.capture_writing, true);

        SDL_Thread* writer = SDL_CreateThread(_nya_trace_capture_write, "Trace Writer", events);

        if (writer == nullptr) {
            nya_log_warn("Could not start the trace writer: %s", SDL_GetError());
            nya_free(events);
            atomic_store(&_nya_trace.capture_writing, false);
        } else {
            SDL_DetachThread(writer);
        }
    }

    NYA_TraceEvent* events = atomic_load(&_nya_trace.capture_events);

    _nya_trace.capture_live = events != nullptr ? nya_min(atomic_load(&_nya_trace.capture_count), (u32)NYA_TRACE_CAPTURE_EVENTS_MAX) : 0;

    if (events == nullptr) return;

    _nya_trace.capture_frames_left--;
    if (_nya_trace.capture_frames_left > 0) return;

    atomic_store(&_nya_trace.capture_events, (NYA_TraceEvent*)nullptr);
    _nya_trace.capture_pending = events;
}

s32 _nya_trace_capture_write(void* events) {
    u32 recorded = atomic_load(&_nya_trace.capture_count);
    u32 count    = nya_min(recorded, (u32)NYA_TRACE_CAPTURE_EVENTS_MAX);

    // registered features are only ever appended, so every name below the count is stable while this runs.
    NYA_ConstCString names[NYA_TRACE_FEATURE_MAX];
    for (u32 f = 0; f < _nya_trace.feature_count; f++) names[f] = _nya_trace.features[f].name;

    u64   size = nya_trace_capture_format(events, count, names, _nya_trace.feature_count, _nya_trace.capture_epoch_ns, nullptr, 0) + 1;
    char* text = nya_malloc(size);

    if (text != nullptr) {
        (void)nya_trace_capture_format(events, count, names, _nya_trace.feature_count, _nya_trace.capture_epoch_ns, text, size);

        NYA_Error written = nya_file_write_atomic(_nya_trace.capture_path, (NYA_ConstCString)text);

        if (written.ok) {
            nya_log_info("Wrote %u trace events to %s%s.", count, _nya_trace.capture_path, recorded > count ? " (the rest were dropped)" : "");
        } else {
            nya_log_warn("Could not write the trace to %s: %s", _nya_trace.capture_path, (NYA_ConstCString)written.message);
        }

        nya_free(text);
    }

    nya_free(events);
    atomic_store(&_nya_trace.capture_writing, false);

    return 0;
}

#endif // NYA_TRACE_ENABLED

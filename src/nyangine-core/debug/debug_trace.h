/**
 * @file debug_trace.h
 *
 * ```c
 * void nya_render3d_shadow_pass(NYA_Window* window) {
 *     nya_trace_scope(NYA_TRACE_SHADOWS);
 *     ...
 * }
 *
 * // a game's own feature, once:
 * NYA_TraceFeature crowds = nya_trace_feature_register("crowds", "crowd");
 *
 * // while a readout is open, every frame. tracing stops a frame after the last request.
 * nya_trace_request();
 * NYA_TraceStats rows[NYA_TRACE_FEATURE_MAX];
 * u32 count = nya_trace_stats(rows, NYA_TRACE_FEATURE_MAX, NYA_TRACE_SORT_CPU);
 *
 * nya_trace_capture_begin(120, "./logs/trace.json");
 * ```
 *
 * Time and memory by feature. A scope charges the time it runs, minus the scopes nested in it, to its feature, and
 * everything created inside it (GPU textures and buffers) is counted against that feature until released. Time is
 * only measured while something asked for it; a scope otherwise swaps one thread local byte on the way in and out.
 * Compiled out of shipping builds.
 *
 * GPU time is measured per group of command buffers. While tracing, the renderer submits what it encoded whenever
 * the innermost feature changes at a point with no render pass open, with a fence, and a thread stamps each fence as
 * it completes. A group's time runs from when the GPU was free to start it (its submission, or the previous group's
 * completion) to its own completion, so it includes driver scheduling, and it is attributed a frame or two late.
 * Features sharing a pass with another, such as particles drawn in the camera pass, report no GPU time of their own.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if (NYA_DEVELOPMENT_BUILD || NYA_TEST || defined(NYA_TRACE_FORCE_ENABLED)) && !defined(NYA_TRACE_FORCE_DISABLED)
#define NYA_TRACE_ENABLED 1
#else
#define NYA_TRACE_ENABLED 0
#endif

/** Features the table holds, engine and game together. Registered as the `trace_features` ceiling. */
#define NYA_TRACE_FEATURE_MAX 48

/** Frames the averages and maxima run over. About a second at 60 Hz. */
#ifndef NYA_TRACE_HISTORY
#define NYA_TRACE_HISTORY 64
#endif

/** Longest feature name, terminator included. */
#define NYA_TRACE_NAME_MAX 24

/**
 * Scopes a capture records before it drops the rest, 24 bytes each. Allocated when a capture starts and freed once
 * it is written. Registered as the `trace_events` ceiling.
 * */
#ifndef NYA_TRACE_CAPTURE_EVENTS_MAX
#define NYA_TRACE_CAPTURE_EVENTS_MAX 65536
#endif

/** The thread id GPU groups are written under in a capture, since they run on no CPU thread. */
#define NYA_TRACE_GPU_THREAD 0

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef u8                    NYA_TraceFeature;
typedef enum NYA_TraceSort    NYA_TraceSort;
typedef struct NYA_TraceScope NYA_TraceScope;
typedef struct NYA_TraceStats NYA_TraceStats;
typedef struct NYA_TraceEvent NYA_TraceEvent;

/** The engine's features, in table order. A game's start at NYA_TRACE_ENGINE_FEATURES. */
enum {
    /** Outside every scope. What an allocation made there is counted as. */
    NYA_TRACE_OTHER,

    NYA_TRACE_SHADOWS,

    /** The 3D camera pass, its uploads and the mesh batch. */
    NYA_TRACE_SCENE,

    /** Sorting translucent geometry, and the glass capture. */
    NYA_TRACE_TRANSPARENT,
    NYA_TRACE_PARTICLES,

    /** The Navier-Stokes step and the splats or quads it draws as. */
    NYA_TRACE_FLUID,

    NYA_TRACE_DECALS,
    NYA_TRACE_SKINNING,

    /** The window's colour and depth buffers and the post chain's scene target. */
    NYA_TRACE_TARGETS,

    NYA_TRACE_OCCLUSION,
    NYA_TRACE_INK,
    NYA_TRACE_DEPTH_OF_FIELD,
    NYA_TRACE_ANTIALIAS,
    NYA_TRACE_GRADE,
    NYA_TRACE_BLOOM,
    NYA_TRACE_SPEED_LINES,

    /** A caller's post pass without a feature of its own, the debug view and the chain's second image. */
    NYA_TRACE_POST,
    NYA_TRACE_HDR_OUTPUT,

    NYA_TRACE_BATCH2D,
    NYA_TRACE_UI,

    /** Shaping and glyph atlases. */
    NYA_TRACE_TEXT,
    NYA_TRACE_DEBUG_DRAW,

    NYA_TRACE_PHYSICS2D,
    NYA_TRACE_PHYSICS3D,
    NYA_TRACE_AUDIO_PROPAGATION,

    /** The bus effect chains, on the mixer's thread. */
    NYA_TRACE_AUDIO_EFFECTS,
    NYA_TRACE_NET_ENCODE,
    NYA_TRACE_NET_DECODE,
    NYA_TRACE_ASSETS,
    NYA_TRACE_HOT_RELOAD,

    NYA_TRACE_ENGINE_FEATURES,
};

/** What nya_trace_stats orders by, largest first, except by name. */
enum NYA_TraceSort {
    NYA_TRACE_SORT_CPU,
    NYA_TRACE_SORT_GPU,
    NYA_TRACE_SORT_VRAM,
    NYA_TRACE_SORT_RAM,
    NYA_TRACE_SORT_NAME,

    NYA_TRACE_SORT_COUNT,
};

/** Held by nya_trace_scope for the scope's lifetime. */
struct NYA_TraceScope {
    /** Zero while tracing was off when the scope opened, so it measures nothing on the way out. */
    u64 started_ns;

    /** The enclosing scope's children so far, set aside while this scope counts its own. */
    u64 outer_children_ns;

    NYA_TraceFeature feature;
    NYA_TraceFeature previous;
};

/** One feature over the last NYA_TRACE_HISTORY frames. */
struct NYA_TraceStats {
    NYA_ConstCString name;
    NYA_TraceFeature feature;

    /** Time in the feature's own scopes per frame, children excluded. */
    f64 cpu_ms;
    f64 cpu_max_ms;

    /** Only meaningful with `has_gpu`: the feature had a GPU group of its own in the window. */
    f64 gpu_ms;
    f64 gpu_max_ms;
    b8  has_gpu;

    /** GPU textures and buffers created inside the feature's scopes and not yet released. */
    u64 vram_bytes;

    /** Used bytes of the arenas whose name starts with the feature's prefix. */
    u64 ram_bytes;

    /** Scopes entered and draw calls made per frame, on average. */
    f32 calls;
    f32 draws;
};

/** One completed scope in a capture. */
struct NYA_TraceEvent {
    u64              started_ns;
    u64              ended_ns;
    u32              thread;
    NYA_TraceFeature feature;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * With tracing compiled out, a scope is nothing and the readers return nothing.
 */
// clang-format off
#if NYA_TRACE_ENABLED
#define nya_trace_scope(feature) __attr_cleanup(_nya_trace_scope_end) NYA_TraceScope CONCAT(_nya_trace_scope_, __LINE__) = _nya_trace_scope_begin(feature)

/** A scope for `feature` only outside any other, so a shared helper such as a batch flush is charged to its caller. */
#define nya_trace_scope_fallback(feature) nya_trace_scope(nya_trace_feature_current() == NYA_TRACE_OTHER ? (feature) : nya_trace_feature_current())

/** A scope that spans a begin and end pair of calls. Ended in the reverse order of opening, like any other. */
#define nya_trace_begin(feature) _nya_trace_scope_begin(feature)
#define nya_trace_end(scope)     _nya_trace_scope_end(scope)
#else
#define nya_trace_scope(feature)          nya_unused(feature)
#define nya_trace_scope_fallback(feature) nya_unused(feature)
#define nya_trace_begin(feature)          ({ nya_unused(feature); (NYA_TraceScope){ 0 }; })
#define nya_trace_end(scope)              nya_unused(scope)
#define nya_trace_feature_register(name, arena_prefix) ({ nya_unused(name, arena_prefix); (NYA_TraceFeature)NYA_TRACE_OTHER; })
#define nya_trace_feature_count()          ((u32)0)
#define nya_trace_feature_name(feature)    ({ nya_unused(feature); (NYA_ConstCString)""; })
#define nya_trace_feature_current()        ((NYA_TraceFeature)NYA_TRACE_OTHER)
#define nya_trace_request()                ((void)0)
#define nya_trace_active()                 ((b8)false)
#define nya_trace_frame_end()              ((void)0)
#define nya_trace_draws(count)             nya_unused(count)
#define nya_trace_gpu(feature, started_ns, ended_ns) nya_unused(feature, started_ns, ended_ns)
#define nya_trace_stats(out, capacity, sort) ({ nya_unused(out, capacity, sort); (u32)0; })
#define nya_trace_report()                 ((void)0)
#define nya_trace_capture_begin(frames, path) ({ nya_unused(frames, path); (b8)false; })
#define nya_trace_capturing()              ((b8)false)
#endif // NYA_TRACE_ENABLED
// clang-format on

#if NYA_TRACE_ENABLED
/**
 * Adds a feature to the table and returns its id. `arena_prefix` names the arenas its RAM is read from; null for
 * none. A name already registered returns the id it has. NYA_TRACE_OTHER when the table is full.
 * */
NYA_API NYA_TraceFeature nya_trace_feature_register(NYA_ConstCString name, NYA_ConstCString arena_prefix);

NYA_API u32              nya_trace_feature_count(void) __attr_no_discard;
NYA_API NYA_ConstCString nya_trace_feature_name(NYA_TraceFeature feature) __attr_no_discard;

/** The innermost open scope's feature on this thread. */
NYA_API NYA_TraceFeature nya_trace_feature_current(void) __attr_no_discard;

/** Keeps time measured through the next frame. A readout calls it every frame it is shown. */
NYA_API void nya_trace_request(void);

/** Whether scopes measure time this frame. */
NYA_API b8 nya_trace_active(void) __attr_no_discard;

/** Closes the frame's figures into the history. Called by the app loop once a frame. */
NYA_API void nya_trace_frame_end(void);

/** Counts draw calls against the current feature. */
NYA_API void nya_trace_draws(u32 count);

/** Charges a completed GPU group to `feature`. Called by the renderer as fences complete. */
NYA_API void nya_trace_gpu(NYA_TraceFeature feature, u64 started_ns, u64 ended_ns);

/**
 * Fills `out` with every feature that has time, draws or memory, sorted by `sort`, and returns how many. Memory is
 * read as it is now; RAM walks the arena registry, so this is for a readout's refresh rather than every frame.
 * */
NYA_API u32 nya_trace_stats(OUT NYA_TraceStats* out, u32 capacity, NYA_TraceSort sort);

/** Logs the table, sorted by CPU time. */
NYA_API void nya_trace_report(void);

/**
 * Records every scope and GPU group for `frames` frames, then writes them to `path` as Chrome trace JSON (open in
 * chrome://tracing or ui.perfetto.dev) from a thread of its own. False while a capture is recording or writing.
 * */
NYA_API b8 nya_trace_capture_begin(u32 frames, NYA_ConstCString path);

/** Whether a capture is recording or still being written. */
NYA_API b8 nya_trace_capturing(void) __attr_no_discard;

NYA_API NYA_TraceScope _nya_trace_scope_begin(NYA_TraceFeature feature);
NYA_API void           _nya_trace_scope_end(NYA_TraceScope* scope);
#endif // NYA_TRACE_ENABLED

/*
 * The arithmetic the tables are made of, compiled in every build so it can be tested.
 */

/** Time a GPU group took: from when the queue was free to start it to its completion. Zero when out of order. */
NYA_API u64 nya_trace_gpu_elapsed(u64 submitted_ns, u64 previous_done_ns, u64 done_ns) __attr_no_discard;

/** Average and maximum of the first `count` samples of a ring. */
NYA_API void nya_trace_window(const u32* samples, u32 count, OUT f64* out_average, OUT u32* out_max);

/** Orders `stats` by `sort` in place, largest first, names alphabetically. Stable, so equal rows keep table order. */
NYA_API void nya_trace_sort(NYA_TraceStats* stats, u32 count, NYA_TraceSort sort);

/**
 * Writes `count` events as Chrome trace JSON into `out`, names looked up in `names`. Returns the bytes needed, which
 * is more than `capacity` when the buffer was too small. Timestamps become microseconds since `epoch_ns`.
 * */
NYA_API u64 nya_trace_capture_format(const NYA_TraceEvent* events, u32 count, const NYA_ConstCString* names, u32 name_count, u64 epoch_ns,
                                     OUT char* out, u64 capacity);

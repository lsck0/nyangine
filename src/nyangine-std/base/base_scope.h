/**
 * @file base_scope.h
 *
 * One marker for "this block is a unit of work": its time, the arena memory it allocated, and its place on the
 * stack a crash report prints.
 *
 * ```c
 * void nya_tilemap_draw(NYA_Tilemap* map) {
 *     nya_scope_function();
 *     ...
 *     {
 *         nya_scope("tilemap_chunks");
 *         ...
 *     }
 * }
 *
 * // a function whose locals a crash report should print as well; see base_watch.h
 * // @watch
 * void gny_world_load(u32 level) {
 *     nya_scope_watch(gny_world_load);
 *     ...
 * }
 *
 * // once a frame, from the app loop; then the table, from a readout
 * nya_scope_frame_end();
 * NYA_ScopeStats rows[NYA_SCOPE_SITE_MAX];
 * u32 count = nya_scope_stats(rows, NYA_SCOPE_SITE_MAX, NYA_SCOPE_SORT_SELF);
 * ```
 *
 * Every scope site is a static NYA_ScopeSite, so entering one is a push of a pointer onto a per thread stack and
 * leaving it is a pop, closed by a `defer`. Development and test builds measure: each frame holds when it started,
 * how long its children took (so a site's self time is its own), the thread's allocated bytes at entry and the
 * highest live byte count seen under it. The arena calls the two hooks below on every allocation and free, so
 * attribution is per thread and needs no lock. A frame's figures land in a per site table, copied names and all, so
 * a site from a game DLL that a reload unmapped still reads.
 *
 * Release builds keep the stack and nothing else, because the names of the scopes a crash happened in are worth
 * more than the two stores they cost; see bench/bench_scope.c. NYA_SCOPE_FORCE_DISABLED compiles every scope out.
 *
 * debug_trace.h's features are tags on scopes: nya_scope_tagged charges the scope, and every untagged scope under
 * it, to a feature through the sink debug_trace registers. That is why this module knows a tag but not a feature.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_types.h"

/*
 * ---------------------------------------------------------------------------------------------------------------------
 * CONSTANTS
 * ---------------------------------------------------------------------------------------------------------------------
 */

#if defined(NYA_SCOPE_FORCE_DISABLED)
#define NYA_SCOPE_ENABLED  0
#define NYA_SCOPE_MEASURED 0
#elif (NYA_DEVELOPMENT_BUILD || NYA_TEST || defined(NYA_SCOPE_FORCE_MEASURED) || defined(NYA_TRACE_FORCE_ENABLED)) && !defined(NYA_SCOPE_FORCE_NAMES)
#define NYA_SCOPE_ENABLED  1
#define NYA_SCOPE_MEASURED 1
#else
#define NYA_SCOPE_ENABLED  1
#define NYA_SCOPE_MEASURED 0
#endif

/**
 * Scopes one thread can have open at once. Marker scopes sit on units of work, not on every call, and the deepest
 * chain gnyame opens is under ten; 64 leaves recursion through a scoped function room before the assert. 48 bytes a
 * frame, so about 3 KiB per thread. Its deepest across every thread is the `scope_depth` ceiling.
 * */
#ifndef NYA_SCOPE_DEPTH_MAX
#define NYA_SCOPE_DEPTH_MAX 64
#endif

/**
 * Distinct sites the table holds. The engine and gnyame have about a hundred; past the bound a site is counted as
 * the first row, "untracked", and a warning names the constant. The `scope_sites` ceiling.
 * */
#ifndef NYA_SCOPE_SITE_MAX
#define NYA_SCOPE_SITE_MAX 256
#endif

/** Frames the averages and maxima run over, as debug_trace does. About a second at 60 Hz. */
#ifndef NYA_SCOPE_HISTORY
#define NYA_SCOPE_HISTORY 64
#endif

/** Longest copied name and function, terminator included. */
#define NYA_SCOPE_NAME_MAX 48

/** Longest copied file path, terminator included. A longer path keeps its tail, which is the part that names it. */
#define NYA_SCOPE_FILE_MAX 64

/** What an untagged scope carries. A tag's meaning belongs to whoever set the sink. */
#define NYA_SCOPE_TAG_NONE 0

/*
 * ---------------------------------------------------------------------------------------------------------------------
 * TYPES
 * ---------------------------------------------------------------------------------------------------------------------
 */

typedef u8                    NYA_ScopeTag;
typedef enum NYA_ScopeSort    NYA_ScopeSort;
typedef struct NYA_ScopeSite  NYA_ScopeSite;
typedef struct NYA_ScopeMark  NYA_ScopeMark;
typedef struct NYA_ScopeView  NYA_ScopeView;
typedef struct NYA_ScopeStats NYA_ScopeStats;

/**
 * Called as a measured scope with a tag other than NYA_SCOPE_TAG_NONE closes. `self_ns` excludes nested scopes;
 * `tagged` is false when the tag was inherited from an enclosing scope rather than given at this one.
 * */
typedef void (*NYA_ScopeTagSinkFn)(NYA_ScopeTag tag, u64 started_ns, u64 ended_ns, u64 self_ns, b8 tagged);

/** What nya_scope_stats orders by, largest first, except by name. */
enum NYA_ScopeSort {
    NYA_SCOPE_SORT_SELF,
    NYA_SCOPE_SORT_TOTAL,
    NYA_SCOPE_SORT_ALLOCATED,
    NYA_SCOPE_SORT_PEAK,
    NYA_SCOPE_SORT_NAME,

    NYA_SCOPE_SORT_COUNT,
};

/** One place in the source that opens a scope. Static at the site; the strings are read only while it is open. */
struct NYA_ScopeSite {
    NYA_ConstCString name;
    NYA_ConstCString function;
    NYA_ConstCString file;
    u32              line;

    /** The site's row in the table plus one, zero until its first entry. */
    atomic u32 slot;
};

/** What nya_scope_begin hands back for nya_scope_end to check it closes the scope it opened. */
struct NYA_ScopeMark {
    u32 depth;
};

/** One open scope on the calling thread, as nya_scope_at reads it. The strings are the table's copies. */
struct NYA_ScopeView {
    NYA_ConstCString name;
    NYA_ConstCString function;
    NYA_ConstCString file;
    u32              line;
    NYA_ScopeTag     tag;

    /** Zero in a build that does not measure. */
    u64 elapsed_ns;
    u64 allocated_bytes;

    /** The most bytes live on this thread above what was live at entry, so far. */
    u64 peak_bytes;
};

/** One site over the last NYA_SCOPE_HISTORY frames, averaged per frame unless named a maximum. */
struct NYA_ScopeStats {
    NYA_ConstCString name;
    NYA_ConstCString function;
    NYA_ConstCString file;
    u32              line;

    f32 calls;
    f64 total_ms;
    f64 self_ms;
    f64 self_max_ms;

    /** Arena bytes allocated inside the site's scopes, nested scopes included and excluded. */
    u64 allocated_bytes;
    u64 allocated_self_bytes;

    /** The highest a scope of this site raised its thread's live bytes, in the window and ever. */
    u64 peak_bytes;
    u64 high_water_bytes;
};

/*
 * ---------------------------------------------------------------------------------------------------------------------
 * FUNCTIONS AND MACROS
 * ---------------------------------------------------------------------------------------------------------------------
 */

// clang-format off
#if NYA_SCOPE_ENABLED
/** Opens a scope named by the literal `name` until the end of the enclosing block. */
#define nya_scope(name) _nya_scope_open(name, NYA_SCOPE_TAG_NONE, false, CONCAT(_nya_scope_site_, __LINE__))

/** A scope named after the enclosing function. */
#define nya_scope_function() nya_scope(__func__)

/** A scope charged, with every untagged scope inside it, to `tag`; see debug_trace.h. `tag` may be computed. */
#define nya_scope_tagged(name, tag) _nya_scope_open(name, tag, true, CONCAT(_nya_scope_site_, __LINE__))

/**
 * A scope whose name is only known at run time, such as a layer's id. The name is looked up in the table on every
 * entry, so this costs a hash and a scan the static forms do not; keep it off anything hot.
 * */
#define nya_scope_named(label)                                                                                                                       \
    NYA_ScopeSite CONCAT(_nya_scope_site_, __LINE__) = { .name = (label), .function = __func__, .file = __FILE__, .line = __LINE__ };               \
    _nya_scope_push(&CONCAT(_nya_scope_site_, __LINE__), NYA_SCOPE_TAG_NONE, false);                                                             \
    defer _nya_scope_pop(&CONCAT(_nya_scope_site_, __LINE__))

/**
 * A scope that opens in one call and closes in another, ended in the reverse order of opening like any other. Only
 * for a bracket the language cannot see, such as a frame's begin and end; anything with a block uses nya_scope.
 * */
#define nya_scope_begin(label, tag)                                                                                                                  \
    ({                                                                                                                                               \
        static NYA_ScopeSite _nya_scope_site_begin = { .name = (label), .function = __func__, .file = __FILE__, .line = __LINE__ };                  \
        const NYA_ScopeTag   _nya_scope_tag_begin  = (tag);                                                                                         \
        _nya_scope_push(&_nya_scope_site_begin, _nya_scope_tag_begin, _nya_scope_tag_begin != NYA_SCOPE_TAG_NONE);                                \
    })
#define nya_scope_end(mark) _nya_scope_end(mark)
#else
#define nya_scope(name)             nya_unused(name)
#define nya_scope_function()        ((void)0)
#define nya_scope_tagged(name, tag) nya_unused(name, tag)
#define nya_scope_named(name)       nya_unused(name)
#define nya_scope_begin(name, tag)  ({ nya_unused(name, tag); (NYA_ScopeMark){ 0 }; })
#define nya_scope_end(mark)         nya_unused(mark)
#endif // NYA_SCOPE_ENABLED

/**
 * A scope named after the function, plus the function's watched locals: `function` must be the enclosing function
 * and carry the `// @watch` annotation. See base_watch.h.
 * */
#define nya_scope_watch(function) nya_scope_function(); nya_watch(function)
// clang-format on

/**
 * Closes the frame's figures into the history. Called by the app loop once a frame, before its first scope opens,
 * so every scope of a frame lands in that frame. Main thread.
 * */
NYA_API void nya_scope_frame_end(void);

/** Scopes open on the calling thread. */
NYA_API u32 nya_scope_depth(void) __attr_no_discard;

/** The calling thread's open scope at `index`, 0 the outermost. Allocates nothing and takes no lock: a crash reads it. */
NYA_API NYA_ScopeView nya_scope_at(u32 index) __attr_no_discard;

/** The innermost open scope's tag on the calling thread, inherited from an enclosing tagged scope if it has none. */
NYA_API NYA_ScopeTag nya_scope_tag_current(void) __attr_no_discard;

/** Sets who hears about tagged scopes closing; null for nobody. debug_trace.h sets it before the first frame. */
NYA_API void nya_scope_tag_sink_set(NYA_ScopeTagSinkFn sink);

/** Sites the table holds, the "untracked" row included. */
NYA_API u32 nya_scope_site_count(void) __attr_no_discard;

/**
 * Fills `out` with every site that ran in the window or holds a high water mark, sorted by `sort`, and returns how
 * many. Zero in a build that does not measure.
 * */
NYA_API u32 nya_scope_stats(OUT NYA_ScopeStats* out, u32 capacity, NYA_ScopeSort sort);

/** Orders `stats` by `sort` in place, largest first, names alphabetically. Stable. */
NYA_API void nya_scope_sort(NYA_ScopeStats* stats, u32 count, NYA_ScopeSort sort);

/** Logs the table, sorted by self time. */
NYA_API void nya_scope_report(void);

// INTERNALS

// clang-format off
#define _nya_scope_open(label, tag, tagged, site)                                                                                                    \
    static NYA_ScopeSite site = { .name = (label), .function = __func__, .file = __FILE__, .line = __LINE__ };                                       \
    _nya_scope_push(&site, (tag), (tagged));                                                                                                         \
    defer _nya_scope_pop(&site)
// clang-format on

NYA_API NYA_ScopeMark _nya_scope_push(NYA_ScopeSite* site, NYA_ScopeTag tag, b8 tagged);
NYA_API void          _nya_scope_pop(NYA_ScopeSite* site);
NYA_API void          _nya_scope_end(NYA_ScopeMark* mark);

/** Called by the arena on every allocation and free, with the padded size it really took or gave back. */
NYA_API void _nya_scope_arena_alloc(u64 size_bytes);
NYA_API void _nya_scope_arena_free(u64 size_bytes);

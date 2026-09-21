/**
 * @file http_metrics.h
 *
 * The first resource: this program, over HTTP. Frame time, the ceilings, the arenas, and the system
 * registry's per-owner accounting.
 *
 * ```
 * nya_http_metrics_router    the router. Merge it and the numbers are online
 * ```
 *
 * ```
 * GET  /api/metrics              frame time, the server's own counters
 * GET  /api/metrics/ceilings     every fixed capacity array and how full it is
 * GET  /api/metrics/arenas       every live arena: used, reserved, fragmentation
 * GET  /api/metrics/systems      per owner: how many systems, what they cost, what they hold
 * POST /api/metrics/accounting   turns the registry's per system timing on and off
 * ```
 *
 * ```c
 * NYA_EXPECT(nya_http_server_merge(nya_http_metrics_router()));
 * ```
 *
 * ── a view, not new instrumentation ──
 *
 * Every number here is already queryable: nya_app_get, nya_ceiling_*, nya_arena_stats and
 * nya_system_owner_stats_at. Nothing is counted for this resource's benefit and nothing runs while it
 * is not being asked, which is why the handlers are a copy loop and nothing else.
 *
 * The one exception is the `POST`, which turns on the registry's per system timing: that costs a clock
 * read per system per phase and is off by default, so it is a thing to ask for rather than a thing to
 * report. It is also the only route here behind the extractor, and it needs NYA_HTTP_SCOPE_WRITE.
 *
 * ── one path per shape ──
 *
 * Four GETs rather than one with a `view` parameter, because they answer four different shapes. A
 * query parameter that picks between shapes is a tagged union the schema has to describe as one of
 * several; a query parameter that picks between *instances* of one shape is what this codebase's REST
 * pattern uses it for, and there is no such choice here.
 *
 * ── the lists are bounded ──
 *
 * A reflection describes a C array of a fixed length, so each list is a fixed array plus a count and
 * the count is what a reader walks. A program with more arenas than NYA_HTTP_METRICS_MAX_ROWS reports
 * the first of them and says so in `truncated`, which is a number being missing rather than a request
 * failing.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_system.h"
#include "nyangine/http/http_router.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_HTTP_METRICS_PATH            "/api/metrics"
#define NYA_HTTP_METRICS_CEILINGS_PATH   "/api/metrics/ceilings"
#define NYA_HTTP_METRICS_ARENAS_PATH     "/api/metrics/arenas"
#define NYA_HTTP_METRICS_SYSTEMS_PATH    "/api/metrics/systems"
#define NYA_HTTP_METRICS_ACCOUNTING_PATH "/api/metrics/accounting"

/**
 * Rows one list answer carries.
 *
 * The engine registers about twenty ceilings and runs with a few dozen arenas, so this shows all of
 * them today with room to grow. Past it the answer is truncated rather than large: a metrics body is
 * read by a page that refreshes, and an unbounded one is an unbounded response buffer.
 * */
#define NYA_HTTP_METRICS_MAX_ROWS 48

/** Longest name in a row, terminator included. Matches what the ceiling and arena registries hold. */
#define NYA_HTTP_METRICS_MAX_NAME 64

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_HttpMetricsDto    NYA_HttpMetricsDto;
typedef struct NYA_HttpCeilingDto    NYA_HttpCeilingDto;
typedef struct NYA_HttpCeilingsDto   NYA_HttpCeilingsDto;
typedef struct NYA_HttpArenaDto      NYA_HttpArenaDto;
typedef struct NYA_HttpArenasDto     NYA_HttpArenasDto;
typedef struct NYA_HttpOwnerDto      NYA_HttpOwnerDto;
typedef struct NYA_HttpSystemsDto    NYA_HttpSystemsDto;
typedef struct NYA_HttpAccountingDto NYA_HttpAccountingDto;

// @reflect
/** What GET /api/metrics answers: the frame, and what the server itself has done. */
struct NYA_HttpMetricsDto {
    /** Seconds since the epoch when this was measured, so a page can say how stale it is. */
    u64 measured_at_s;

    /** Nanoseconds since nya_app_init. */
    u64 uptime_ns;

    f32 fps;
    f32 delta_time_s;

    /** Time spent in the frame before the limiter slept. The number to read for "is this slow". */
    u64 work_ns;

    /** What the limiter slept to hold the frame rate. Zero when the frame ran over budget. */
    u64 sleep_ns;

    /** Start of the last frame to start of this one, sleep included. */
    u64 elapsed_ns;

    /** The frame rate limit, as the shortest frame the loop will allow. */
    u64 min_frame_time_ns;

    /** Connections this server is holding, and how many requests it has answered since it started. */
    u32 connection_count;
    u64 request_count;

    /** Whether the registry is timing each system. Off means every `time_ns` below reads zero. */
    b8 accounting_enabled;
};

// @reflect
/** One fixed capacity array and how full it is. */
struct NYA_HttpCeilingDto {
    char name[NYA_HTTP_METRICS_MAX_NAME];

    u32 capacity;
    u32 live;

    /** `live` over `capacity`, 0 to 1, so a reader need not divide to sort. */
    f32 fullness;
};

// @reflect
/** What GET /api/metrics/ceilings answers. */
struct NYA_HttpCeilingsDto {
    /** How many of `rows` mean anything. Never more than NYA_HTTP_METRICS_MAX_ROWS. */
    u32 count;

    /** How many were registered and did not fit. Zero in every ordinary run. */
    u32 truncated;

    NYA_HttpCeilingDto rows[NYA_HTTP_METRICS_MAX_ROWS];
};

// @reflect
/** One live arena. */
struct NYA_HttpArenaDto {
    char name[NYA_HTTP_METRICS_MAX_NAME];

    u64 region_count;

    /** Bytes handed out, freed blocks not yet reused included. */
    u64 used_bytes;

    /** Total capacity of every region: what this arena has taken from the system. */
    u64 reserved_bytes;

    /** Bytes in free lists, available again. */
    u64 free_list_bytes;

    /** How broken up the free space is, 0 to 1. */
    f32 fragmentation;
};

// @reflect
/** What GET /api/metrics/arenas answers. */
struct NYA_HttpArenasDto {
    u32 count;
    u32 truncated;

    NYA_HttpArenaDto rows[NYA_HTTP_METRICS_MAX_ROWS];
};

// @reflect
/** One owner's systems: the engine's, the game's, or a plugin's. */
struct NYA_HttpOwnerDto {
    char name[NYA_HTTP_METRICS_MAX_NAME];

    u32 system_count;
    u32 enabled_count;

    /** What they cost over the last frame. Zero while accounting is off. */
    u64 time_ns;

    /** What they report holding, summed from each system's own answer. */
    u64 memory_bytes;
};

// @reflect
/** What GET /api/metrics/systems answers. */
struct NYA_HttpSystemsDto {
    u32 count;
    u32 truncated;

    /** Repeated here so a page fetching only this one still knows the times are meaningful. */
    b8 accounting_enabled;

    NYA_HttpOwnerDto rows[NYA_SYSTEM_OWNER_MAX];
};

// @reflect
/**
 * The body of POST /api/metrics/accounting, and what it answers with: the same shape both ways, so
 * the answer is the state that was actually reached rather than an echo of the request.
 * */
struct NYA_HttpAccountingDto {
    b8 enabled;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Static storage, so it outlives any mount and needs no lifetime from the caller. */
NYA_API const NYA_HttpRouter* nya_http_metrics_router(void) __attr_no_discard;

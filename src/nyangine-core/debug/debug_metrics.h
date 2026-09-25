/**
 * @file debug_metrics.h
 *
 * The first resource: this program, over HTTP. Frame time, the ceilings, the arenas, and the system
 * registry's per-owner accounting.
 *
 * ```
 * nya_http_metrics_router    the router. Merge it and the numbers are online
 * ```
 *
 * ```
 * QUERY /api/metrics              frame time, the server's own counters
 * QUERY /api/metrics/ceilings     every fixed capacity array and how full it is
 * QUERY /api/metrics/arenas       every live arena: used, reserved, fragmentation
 * QUERY /api/metrics/systems      per owner: how many systems, what they cost, what they hold
 * PUT   /api/metrics/accounting   turns the registry's per system timing on and off
 * GET   /metrics                  the same numbers in the Prometheus text exposition format
 * ```
 *
 * ── the scrape endpoint ──
 *
 * The routes above answer a page this program draws; `GET /metrics` answers a Prometheus server that
 * scrapes. It renders the same ceilings, the same gauge registry and the same frame counters as the
 * text exposition format — `# HELP`, `# TYPE gauge`, one `name{label="value"} number` sample a line —
 * which every Prometheus-compatible collector already reads. A GET with no parameters, because that is
 * the only request a scraper makes, and off the `/api` prefix because `/metrics` is the path the whole
 * ecosystem defaults to.
 *
 * It is bounded like the rest of this file, and it does not trust a name. A ceiling or gauge name is a
 * registrant's string, so it is carried as an escaped label value and never spliced into a metric
 * name; the fixed metric and label names are held to Prometheus's `[a-zA-Z_:][a-zA-Z0-9_:]*`. A name
 * that arrived with a quote, a newline or a brace in it cannot end a line early, and cannot forge a
 * second sample — nya_http_metrics_prometheus is where both of those are made true.
 *
 * ```sh
 * curl -X QUERY http://127.0.0.1:7777/api/metrics
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
 * The one exception is the `PUT`, which turns on the registry's per system timing: that costs a clock
 * read per system per phase and is off by default, so it is a thing to ask for rather than a thing to
 * report. It is also the only route here behind the extractor, and it needs NYA_HTTP_SCOPE_WRITE.
 *
 * ── the verbs ──
 *
 * The reads are QUERY, which is what a read is written as here: safe and idempotent like a GET, and
 * able to carry a request DTO, which is how a reader would later ask for a subset without the schema
 * having to describe a query string. None of these four takes a document yet, and a QUERY with no body
 * is the whole answer; adding a filter DTO is a `request_type` and nothing else.
 *
 * The write is a PUT rather than a POST because it sets a flag to a value: sending it twice reaches
 * the same state, it creates nothing, and the answer is the state that was reached. POST is for
 * creating, and nothing here creates.
 *
 * ── one path per shape ──
 *
 * Four reads rather than one with a `view` parameter, because they answer four different shapes. A
 * parameter that picks between shapes is a tagged union the schema has to describe as one of several;
 * a parameter that picks between *instances* of one shape is what this codebase's REST pattern uses
 * one for, and there is no such choice here.
 *
 * ── the lists are bounded ──
 *
 * A reflection describes a C array of a fixed length, so each list is a fixed array plus a count and
 * the count is what a reader walks. A program with more arenas than NYA_HTTP_METRICS_MAX_ROWS reports
 * the first of them and says so in `truncated`, which is a number being missing rather than a request
 * failing.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/core/core_system.h"
#include "nyangine-core/http/http_router.h"

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

/** The scrape endpoint. Off `/api` on purpose: `/metrics` is the path a Prometheus job defaults to. */
#define NYA_HTTP_METRICS_PROMETHEUS_PATH "/metrics"

/** The Content-Type a Prometheus text body carries. The version is the format's, not this program's. */
#define NYA_HTTP_METRICS_PROMETHEUS_CONTENT_TYPE "text/plain; version=0.0.4"

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

/**
 * Bytes the Prometheus render may produce, terminator included.
 *
 * The worst case is every ceiling and every gauge with a name that is all escapable characters, plus
 * the frame counters and every family's two header lines. Well inside the response buffer; a render
 * that would pass it is truncated at a sample boundary rather than growing, because a scrape body is
 * something a bounded server answers on one buffer and not a stream.
 * */
#define NYA_HTTP_METRICS_PROMETHEUS_MAX_BYTES 32768

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
/** What QUERY /api/metrics answers: the frame, and what the server itself has done. */
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
/** What QUERY /api/metrics/ceilings answers. */
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
/** What QUERY /api/metrics/arenas answers. */
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
/** What QUERY /api/metrics/systems answers. */
struct NYA_HttpSystemsDto {
    u32 count;
    u32 truncated;

    /** Repeated here so a page fetching only this one still knows the times are meaningful. */
    b8 accounting_enabled;

    NYA_HttpOwnerDto rows[NYA_SYSTEM_OWNER_MAX];
};

// @reflect
/**
 * The body of PUT /api/metrics/accounting, and what it answers with: the same shape both ways, so
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

/**
 * Renders the ceiling registry, the gauge registry and the frame counters into `out` as the Prometheus
 * text exposition format, null terminated, and returns how many bytes were written before the
 * terminator.
 *
 * The whole render is one read of numbers the program already keeps: it counts nothing and allocates
 * nothing, exactly like the QUERY handlers. It is bounded by `capacity` and by the registries' own
 * fixed sizes both, so a full registry renders a full body and never a growing one; a render that
 * would overrun `capacity` stops on a sample boundary, leaving valid text rather than a half-written
 * line. Metric and label names go out held to `[a-zA-Z_:][a-zA-Z0-9_:]*`, and a registrant's name
 * goes out as an escaped label value, so no name a subsystem chose can spell a line a collector would
 * reject. Split out from the handler so a test can render without a socket or an exchange.
 * */
NYA_API u64 nya_http_metrics_prometheus(OUT char* out, u64 capacity);

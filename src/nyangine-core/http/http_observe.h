/**
 * @file http_observe.h
 *
 * The two things a request leaves behind for a machine: a latency histogram a Prometheus job scrapes,
 * and a span an OpenTelemetry collector receives. Fed from the one duration http_log.c already measures,
 * so neither adds a clock read to the hot path and both cost nothing while switched off.
 *
 * ```
 * nya_http_observe_request        one request's method, status and duration into the RED histogram
 * nya_http_observe_metrics_set_enabled  turn the histogram on or off (on by default; it is fixed storage)
 * nya_http_trace_config_set       turn OTLP span recording on or off (off by default)
 * nya_http_trace_record           one request into the span ring, W3C traceparent parsed if present
 * nya_http_trace_export_json      the buffered spans as an OTLP/JSON ExportTraceServiceRequest
 * nya_http_trace_flush            serialize, hand to an exporter, and clear the ring
 * nya_http_traceparent_parse      a W3C `traceparent` header into a trace id, span id and flags
 * ```
 *
 * ── RED, and why the cardinality is bounded ──
 *
 * `http_request_duration_seconds` is a Prometheus histogram with fixed bucket boundaries, labelled by
 * method and by status class ("2xx", "5xx") and by nothing else. A full path or a user id in a label is
 * a new time series per distinct value, which is the cardinality bomb that takes a Prometheus server
 * down; a method is one of nine and a status class one of five, so the whole table is a fixed
 * NYA_HTTP_METHOD_COUNT x 6 array of counters that never grows. The Rate of the RED triad is the
 * histogram's own `_count`; the Errors are `http_request_errors_total`, a counter keyed by method that
 * counts the 5xx answers; the Duration is the buckets. The route pattern is deliberately not a label —
 * it belongs on a span, where each is one event rather than a multiplier on every series.
 *
 * The counters are process-static and lock-free: recording is a handful of relaxed atomic adds, so it is
 * near-free, and switching the histogram off makes it a single flag read. Nothing here ever allocates.
 *
 * ── spans, bounded and off by default ──
 *
 * A span per request carries a trace id, a span id, the parent span id when the caller sent one, the
 * route pattern as the span name (never the raw path — that can carry ids a path parameter put there),
 * the start and end in Unix nanoseconds, the method and the status. They live in a fixed ring of
 * NYA_HTTP_TRACE_SPAN_MAX, drop-oldest, so a burst of traffic with no collector draining it costs a
 * bounded amount of memory and never grows. Recording is off by default and, while off, returns before
 * it touches the ring or reads a clock.
 *
 * The engine ships no outbound OTLP dependency: nya_http_trace_export_json renders the ring as the
 * OTLP/JSON `ExportTraceServiceRequest` a collector's `/v1/traces` accepts, and nya_http_trace_flush
 * hands that text to an exporter callback a program wires to nya_http_client (or, in a test, to a
 * buffer). The seam is the exporter; what it does with the bytes is the program's.
 *
 * ── privacy ──
 *
 * No label and no span field is the caller's: a method, a status class, a status code and a route
 * pattern are the program's own vocabulary. The raw path, the query, the address, the identity and every
 * header stay out, for the same reason http_log.c redacts them.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/http/http_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Finite histogram bucket boundaries, in seconds: 5ms, 10ms, 25ms, 50ms, 100ms, 250ms, 500ms, 1s, 2.5s,
 * 5s, 10s. The Prometheus client libraries' default set, so an existing dashboard's quantile query reads
 * this histogram unchanged. A twelfth, implicit `+Inf` bucket holds everything slower, which is the
 * histogram's total count.
 * */
#define NYA_HTTP_OBSERVE_BUCKET_COUNT 11

/** Status classes the histogram keys on: index 0 is anything outside 1xx–5xx, 1–5 are 1xx–5xx. */
#define NYA_HTTP_OBSERVE_CLASS_COUNT 6

/**
 * Spans the ring holds before it drops the oldest. Each is about a hundred bytes, so the ring is a few
 * tens of kilobytes of fixed storage — a burst a collector has not drained yet, bounded and never grown.
 * */
#define NYA_HTTP_TRACE_SPAN_MAX 256

/** Longest span name, terminator included. A route pattern, which is short and bounded already. */
#define NYA_HTTP_TRACE_NAME_MAX 64

/** Bytes of a trace id (128 bits) and a span id (64 bits), as W3C `traceparent` and OTLP both define them. */
#define NYA_HTTP_TRACE_ID_BYTES  16
#define NYA_HTTP_SPAN_ID_BYTES   8

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_HttpTraceConfig NYA_HttpTraceConfig;
typedef struct NYA_HttpSpan        NYA_HttpSpan;

/** What turns span recording on. Zero is the default: off, so a program opts into tracing on purpose. */
struct NYA_HttpTraceConfig {
    b8 enabled;
};

/** One recorded request. Fixed size, so the ring is a plain array and nothing here is a pointer to free. */
struct NYA_HttpSpan {
    u8 trace_id[NYA_HTTP_TRACE_ID_BYTES];
    u8 span_id[NYA_HTTP_SPAN_ID_BYTES];

    /** The caller's span, when a valid `traceparent` named one; otherwise this is a root span. */
    u8 parent_span_id[NYA_HTTP_SPAN_ID_BYTES];
    b8 has_parent;

    /** The route pattern, never the raw path. Empty when no route matched. */
    char name[NYA_HTTP_TRACE_NAME_MAX];

    u64 start_unix_ns;
    u64 end_unix_ns;

    NYA_HttpMethod method;
    u32            status;
};

/**
 * The exporter seam. Handed the serialized OTLP/JSON and its length; returns whether it took them, which
 * is what decides if nya_http_trace_flush clears the ring. What it does with the bytes — POST them to a
 * collector over nya_http_client, write them to a file, count them in a test — is the program's.
 * */
typedef NYA_Error (*NYA_HttpTraceExportFn)(const char* json, u64 size, void* userdata);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/* ── RED metrics ── */

/**
 * Records one finished request. `duration_ns` is the whole exchange, as http_log.c already measured it;
 * it lands in the bucket for `method` and the class of `status`, and a 5xx also bumps the error counter.
 *
 * Lock-free relaxed atomics into fixed storage, so it is safe from any worker thread and near-free. A
 * single flag read and a return while the histogram is switched off.
 * */
NYA_API void nya_http_observe_request(NYA_HttpMethod method, NYA_HttpStatus status, u64 duration_ns);

/** Turns the histogram on or off. On by default: it is fixed storage and recording is a few atomic adds. */
NYA_API void nya_http_observe_metrics_set_enabled(b8 enabled);

/** Whether the histogram is recording. The Prometheus renderer reads this to decide whether to emit it. */
NYA_API b8 nya_http_observe_metrics_enabled(void) __attr_no_discard;

/** Zeroes every histogram and error counter. For a test that wants a known table; not for the hot path. */
NYA_API void nya_http_observe_reset(void);

/** The number of finite buckets, NYA_HTTP_OBSERVE_BUCKET_COUNT. The `+Inf` bucket is the total count. */
NYA_API u32 nya_http_observe_bucket_count(void) __attr_no_discard;

/** The `le` boundary of finite bucket `index`, in seconds. */
NYA_API f64 nya_http_observe_bucket_bound_s(u32 index) __attr_no_discard;

/**
 * The cumulative count for `method` and `status_class` at or below finite bucket `index`: the value a
 * Prometheus `_bucket{le=...}` line carries. `status_class` is 1–5 for 1xx–5xx, 0 for anything else.
 * */
NYA_API u64 nya_http_observe_bucket_cumulative(NYA_HttpMethod method, u32 status_class, u32 index) __attr_no_discard;

/** How many requests `method` and `status_class` have seen: the histogram's `_count` and `+Inf` bucket. */
NYA_API u64 nya_http_observe_count(NYA_HttpMethod method, u32 status_class) __attr_no_discard;

/** The sum of durations for `method` and `status_class`, in nanoseconds. The renderer divides for `_sum`. */
NYA_API u64 nya_http_observe_sum_ns(NYA_HttpMethod method, u32 status_class) __attr_no_discard;

/** How many 5xx answers `method` has produced: the `http_request_errors_total` counter. */
NYA_API u64 nya_http_observe_errors(NYA_HttpMethod method) __attr_no_discard;

/* ── OTLP tracing ── */

/** Installs the tracing config. Copied, so the caller's struct need not outlive the call. */
NYA_API void nya_http_trace_config_set(NYA_HttpTraceConfig config);

/** What is in force. */
NYA_API NYA_HttpTraceConfig nya_http_trace_config_get(void) __attr_no_discard;

/**
 * Records one span, when tracing is on. `route` is the route pattern and becomes the span name; `parent`
 * is the raw `traceparent` header or null. A valid parent supplies the trace id and this span's parent
 * id; otherwise a fresh 128-bit trace id is drawn and this is a root span. The span id is always fresh.
 *
 * Written into the ring under a short spinlock, drop-oldest when the ring is full. Returns before it
 * reads a clock or touches the ring while tracing is off, which is what makes the disabled path free.
 * */
NYA_API void nya_http_trace_record(
    NYA_HttpMethod   method,
    NYA_HttpStatus   status,
    NYA_ConstCString route,
    u64              start_unix_ns,
    u64              duration_ns,
    NYA_ConstCString parent
);

/** How many spans the ring holds right now. */
NYA_API u32 nya_http_trace_span_count(void) __attr_no_discard;

/** Copies the span at `index` (oldest first) into `out`. False when `index` is past what the ring holds. */
NYA_API b8 nya_http_trace_span_at(u32 index, OUT NYA_HttpSpan* out) __attr_no_discard;

/** Empties the ring. */
NYA_API void nya_http_trace_reset(void);

/**
 * Renders the buffered spans into `out` as an OTLP/JSON `ExportTraceServiceRequest`, null terminated, and
 * returns the bytes written before the terminator. Bounded by `capacity` and by the ring's fixed size
 * both; a render that would overrun stops on a whole span rather than a half-written one. Does not clear
 * the ring — nya_http_trace_flush does that once an exporter has taken the bytes.
 * */
NYA_API u64 nya_http_trace_export_json(OUT char* out, u64 capacity);

/**
 * Serializes the buffered spans, hands them to `exporter`, and clears the ring when it took them. `arena`
 * is scratch for the serialized text and is not kept. A null exporter, or one that fails, leaves the ring
 * as it was so the spans are tried again. Returns what the exporter returned, or OK when the ring is empty.
 * */
NYA_API NYA_Error nya_http_trace_flush(NYA_HttpTraceExportFn exporter, void* userdata, NYA_Arena* arena) __attr_no_discard;

/**
 * Parses a W3C `traceparent` value — `00-<32 hex>-<16 hex>-<2 hex>` — into its trace id, span id and
 * flags. True only when every field is present, hex, and the ids are not all-zero, which the spec
 * forbids. A header this rejects is treated as absent, and the request begins a fresh trace.
 * */
NYA_API b8 nya_http_traceparent_parse(
    NYA_ConstCString header,
    u64              size,
    OUT u8           trace_id[NYA_HTTP_TRACE_ID_BYTES],
    OUT u8           span_id[NYA_HTTP_SPAN_ID_BYTES],
    OUT u8*          flags
) __attr_no_discard;

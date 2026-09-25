#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-core/http/http_observe.h"
#include "nyangine-std/os/os_random.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The finite bucket boundaries in nanoseconds. Compared with `<=`, as a Prometheus `le` bucket is. */
NYA_INTERNAL const u64 _NYA_OBSERVE_BOUNDS_NS[NYA_HTTP_OBSERVE_BUCKET_COUNT] = {
    5000000ULL,     /* 5ms   */
    10000000ULL,    /* 10ms  */
    25000000ULL,    /* 25ms  */
    50000000ULL,    /* 50ms  */
    100000000ULL,   /* 100ms */
    250000000ULL,   /* 250ms */
    500000000ULL,   /* 500ms */
    1000000000ULL,  /* 1s    */
    2500000000ULL,  /* 2.5s  */
    5000000000ULL,  /* 5s    */
    10000000000ULL, /* 10s   */
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The RED histogram: a fixed table keyed by method and status class and nothing else, so it cannot grow
 * a new time series at runtime. `buckets` carries the finite boundaries plus a trailing `+Inf` slot, so
 * every observation lands in exactly one slot and the total is their sum. Relaxed atomics: a counter a
 * scrape reads a little stale is right for a histogram, and it keeps recording off any lock.
 * */
NYA_INTERNAL struct {
    atomic u64 buckets[NYA_HTTP_METHOD_COUNT][NYA_HTTP_OBSERVE_CLASS_COUNT][NYA_HTTP_OBSERVE_BUCKET_COUNT + 1];
    atomic u64 count[NYA_HTTP_METHOD_COUNT][NYA_HTTP_OBSERVE_CLASS_COUNT];
    atomic u64 sum_ns[NYA_HTTP_METHOD_COUNT][NYA_HTTP_OBSERVE_CLASS_COUNT];
    atomic u64 errors[NYA_HTTP_METHOD_COUNT];
} _NYA_OBSERVE = { 0 };

/** On by default: the histogram is fixed storage and recording is a few atomic adds. */
NYA_INTERNAL atomic b8 _NYA_OBSERVE_METRICS_ENABLED = true;

/** Off by default: a program opts into span recording on purpose. */
NYA_INTERNAL NYA_HttpTraceConfig _NYA_OBSERVE_TRACE_CONFIG = { 0 };

/**
 * The span ring, drop-oldest, guarded by a spinlock rather than a mutex: this module has no init hook to
 * create one, the critical section is a single struct copy, and the contention is one span per request
 * across at most a handful of workers. `head` is the oldest; a full ring overwrites it and advances.
 * */
NYA_INTERNAL NYA_HttpSpan _NYA_OBSERVE_RING[NYA_HTTP_TRACE_SPAN_MAX];
NYA_INTERNAL u32          _NYA_OBSERVE_HEAD       = 0;
NYA_INTERNAL u32          _NYA_OBSERVE_SPAN_COUNT = 0;
NYA_INTERNAL atomic b8    _NYA_OBSERVE_LOCK       = false;

/** A monotonic fallback for id bytes when the CSPRNG is unavailable: an id must be unique, not secret. */
NYA_INTERNAL atomic u64 _NYA_OBSERVE_SEQUENCE = 0;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Clamps `method` to a table index, and `status` to a class 0–5 (0 is anything outside 1xx–5xx). */
NYA_INTERNAL u32 _nya_observe_method_index(NYA_HttpMethod method) __attr_no_discard;
NYA_INTERNAL u32 _nya_observe_status_class(NYA_HttpStatus status) __attr_no_discard;

/** The finite bucket a duration falls in, or NYA_HTTP_OBSERVE_BUCKET_COUNT for the `+Inf` slot. */
NYA_INTERNAL u32 _nya_observe_bucket_of(u64 duration_ns) __attr_no_discard;

NYA_INTERNAL void _nya_observe_lock(void);
NYA_INTERNAL void _nya_observe_unlock(void);

/** Fills `out` with `size` unpredictable bytes, or with a sequence-and-address fallback when it cannot. */
NYA_INTERNAL void _nya_observe_id_bytes(OUT u8* out, u64 size);

/** A hex nibble, or -1 for a byte that is not one. */
NYA_INTERNAL s32 _nya_observe_hex_nibble(char c) __attr_no_discard;

/** `size` bytes at `hex` (twice `size` characters) into `out`. False when any character is not hex. */
NYA_INTERNAL b8 _nya_observe_hex_decode(const char* hex, u8* out, u64 size) __attr_no_discard;

/** Whether every one of `size` bytes is zero, which a trace or span id may not be. */
NYA_INTERNAL b8 _nya_observe_bytes_all_zero(const u8* bytes, u64 size) __attr_no_discard;

/*
 * ── the OTLP/JSON appender ──
 *
 * The same all-or-nothing appender the Prometheus render uses, over a fixed buffer: a put that would pass
 * the limit writes nothing and raises `overflow`, so a caller that snapshots `size` before a span and
 * restores it on overflow always leaves a whole, closeable document rather than a half-written one.
 */
typedef struct {
    char* data;
    u64   capacity; /**< the terminator's byte included */
    u64   size;     /**< bytes written, terminator not counted; `data[size]` is always '\0' */
    b8    overflow;
} _NYA_ObserveJson;

NYA_INTERNAL void _nya_observe_json_put(_NYA_ObserveJson* buffer, NYA_ConstCString text);
NYA_INTERNAL void _nya_observe_json_put_u64(_NYA_ObserveJson* buffer, u64 value);
NYA_INTERNAL void _nya_observe_json_put_hex(_NYA_ObserveJson* buffer, const u8* bytes, u64 size);

/** Appends `text` as the inside of a JSON string: a quote, a backslash and the control bytes escaped. */
NYA_INTERNAL void _nya_observe_json_put_escaped(_NYA_ObserveJson* buffer, NYA_ConstCString text);

/** One span object into the buffer, without a leading or trailing comma. */
NYA_INTERNAL void _nya_observe_json_put_span(_NYA_ObserveJson* buffer, const NYA_HttpSpan* span);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION — RED METRICS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_http_observe_request(NYA_HttpMethod method, NYA_HttpStatus status, u64 duration_ns) {
    if (!atomic_load_explicit(&_NYA_OBSERVE_METRICS_ENABLED, memory_order_relaxed)) return;

    u32 m      = _nya_observe_method_index(method);
    u32 cls    = _nya_observe_status_class(status);
    u32 bucket = _nya_observe_bucket_of(duration_ns);

    (void)atomic_fetch_add_explicit(&_NYA_OBSERVE.buckets[m][cls][bucket], 1, memory_order_relaxed);
    (void)atomic_fetch_add_explicit(&_NYA_OBSERVE.count[m][cls], 1, memory_order_relaxed);
    (void)atomic_fetch_add_explicit(&_NYA_OBSERVE.sum_ns[m][cls], duration_ns, memory_order_relaxed);

    if (cls == 5) (void)atomic_fetch_add_explicit(&_NYA_OBSERVE.errors[m], 1, memory_order_relaxed);
}

void nya_http_observe_metrics_set_enabled(b8 enabled) {
    atomic_store_explicit(&_NYA_OBSERVE_METRICS_ENABLED, enabled, memory_order_relaxed);
}

b8 nya_http_observe_metrics_enabled(void) {
    return atomic_load_explicit(&_NYA_OBSERVE_METRICS_ENABLED, memory_order_relaxed);
}

void nya_http_observe_reset(void) {
    for (u32 m = 0; m < NYA_HTTP_METHOD_COUNT; m++) {
        for (u32 cls = 0; cls < NYA_HTTP_OBSERVE_CLASS_COUNT; cls++) {
            for (u32 b = 0; b <= NYA_HTTP_OBSERVE_BUCKET_COUNT; b++) {
                atomic_store_explicit(&_NYA_OBSERVE.buckets[m][cls][b], 0, memory_order_relaxed);
            }
            atomic_store_explicit(&_NYA_OBSERVE.count[m][cls], 0, memory_order_relaxed);
            atomic_store_explicit(&_NYA_OBSERVE.sum_ns[m][cls], 0, memory_order_relaxed);
        }
        atomic_store_explicit(&_NYA_OBSERVE.errors[m], 0, memory_order_relaxed);
    }
}

u32 nya_http_observe_bucket_count(void) {
    return NYA_HTTP_OBSERVE_BUCKET_COUNT;
}

f64 nya_http_observe_bucket_bound_s(u32 index) {
    if (index >= NYA_HTTP_OBSERVE_BUCKET_COUNT) return 0.0;

    return (f64)_NYA_OBSERVE_BOUNDS_NS[index] / 1.0e9;
}

u64 nya_http_observe_bucket_cumulative(NYA_HttpMethod method, u32 status_class, u32 index) {
    u32 m = _nya_observe_method_index(method);
    if (status_class >= NYA_HTTP_OBSERVE_CLASS_COUNT || index >= NYA_HTTP_OBSERVE_BUCKET_COUNT) return 0;

    u64 cumulative = 0;
    for (u32 b = 0; b <= index; b++) cumulative += atomic_load_explicit(&_NYA_OBSERVE.buckets[m][status_class][b], memory_order_relaxed);

    return cumulative;
}

u64 nya_http_observe_count(NYA_HttpMethod method, u32 status_class) {
    u32 m = _nya_observe_method_index(method);
    if (status_class >= NYA_HTTP_OBSERVE_CLASS_COUNT) return 0;

    return atomic_load_explicit(&_NYA_OBSERVE.count[m][status_class], memory_order_relaxed);
}

u64 nya_http_observe_sum_ns(NYA_HttpMethod method, u32 status_class) {
    u32 m = _nya_observe_method_index(method);
    if (status_class >= NYA_HTTP_OBSERVE_CLASS_COUNT) return 0;

    return atomic_load_explicit(&_NYA_OBSERVE.sum_ns[m][status_class], memory_order_relaxed);
}

u64 nya_http_observe_errors(NYA_HttpMethod method) {
    return atomic_load_explicit(&_NYA_OBSERVE.errors[_nya_observe_method_index(method)], memory_order_relaxed);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION — OTLP TRACING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_http_trace_config_set(NYA_HttpTraceConfig config) {
    _NYA_OBSERVE_TRACE_CONFIG = config;
}

NYA_HttpTraceConfig nya_http_trace_config_get(void) {
    return _NYA_OBSERVE_TRACE_CONFIG;
}

void nya_http_trace_record(
    NYA_HttpMethod   method,
    NYA_HttpStatus   status,
    NYA_ConstCString route,
    u64              start_unix_ns,
    u64              duration_ns,
    NYA_ConstCString parent
) {
    // The whole point of the disabled path: no clock, no id draw, no ring, no lock.
    if (!_NYA_OBSERVE_TRACE_CONFIG.enabled) return;

    NYA_HttpSpan span = {
        .method        = method,
        .status        = (u32)status,
        .start_unix_ns = start_unix_ns,
        .end_unix_ns   = start_unix_ns + duration_ns,
    };

    (void)snprintf(span.name, sizeof(span.name), "%s", route != nullptr ? route : "");

    // A valid parent supplies the trace id and this span's parent; otherwise a fresh trace begins here.
    u8 parent_span[NYA_HTTP_SPAN_ID_BYTES] = { 0 };
    u8 flags                               = 0;

    if (parent != nullptr && nya_http_traceparent_parse(parent, strlen(parent), span.trace_id, parent_span, &flags)) {
        nya_memcpy(span.parent_span_id, parent_span, sizeof(span.parent_span_id));
        span.has_parent = true;
    } else {
        _nya_observe_id_bytes(span.trace_id, sizeof(span.trace_id));
    }

    // A fresh span id in either case: this server is the span, not the caller.
    _nya_observe_id_bytes(span.span_id, sizeof(span.span_id));

    _nya_observe_lock();
    defer _nya_observe_unlock();

    if (_NYA_OBSERVE_SPAN_COUNT < NYA_HTTP_TRACE_SPAN_MAX) {
        _NYA_OBSERVE_RING[(_NYA_OBSERVE_HEAD + _NYA_OBSERVE_SPAN_COUNT) % NYA_HTTP_TRACE_SPAN_MAX] = span;
        _NYA_OBSERVE_SPAN_COUNT++;
    } else {
        // Full: overwrite the oldest and carry the window forward.
        _NYA_OBSERVE_RING[_NYA_OBSERVE_HEAD] = span;
        _NYA_OBSERVE_HEAD                  = (_NYA_OBSERVE_HEAD + 1) % NYA_HTTP_TRACE_SPAN_MAX;
    }
}

u32 nya_http_trace_span_count(void) {
    _nya_observe_lock();
    defer _nya_observe_unlock();

    return _NYA_OBSERVE_SPAN_COUNT;
}

b8 nya_http_trace_span_at(u32 index, OUT NYA_HttpSpan* out) {
    nya_assert(out != nullptr);

    _nya_observe_lock();
    defer _nya_observe_unlock();

    if (index >= _NYA_OBSERVE_SPAN_COUNT) return false;

    *out = _NYA_OBSERVE_RING[(_NYA_OBSERVE_HEAD + index) % NYA_HTTP_TRACE_SPAN_MAX];

    return true;
}

void nya_http_trace_reset(void) {
    _nya_observe_lock();
    defer _nya_observe_unlock();

    _NYA_OBSERVE_HEAD  = 0;
    _NYA_OBSERVE_SPAN_COUNT = 0;
}

u64 nya_http_trace_export_json(char* out, u64 capacity) {
    nya_assert(out != nullptr);
    nya_assert(capacity > 0);

    _NYA_ObserveJson buffer = { .data = out, .capacity = capacity, .size = 0, .overflow = false };
    out[0]                 = '\0';

    // The envelope a collector's OTLP/JSON receiver expects, with the service and scope named once.
    _nya_observe_json_put(&buffer,
                  "{\"resourceSpans\":[{\"resource\":{\"attributes\":[{\"key\":\"service.name\",\"value\":{\"stringValue\":\"nyangine\"}}]},"
                  "\"scopeSpans\":[{\"scope\":{\"name\":\"nyangine.http\"},\"spans\":[");

    NYA_ConstCString suffix        = "]}]}]}";
    u64              suffix_length = strlen(suffix);

    // Reserve the closing bytes so a span that does not fit can be rolled back and the document still closed.
    u64 full_capacity = buffer.capacity;
    buffer.capacity   = buffer.capacity > suffix_length ? buffer.capacity - suffix_length : 1;

    _nya_observe_lock();

    for (u32 index = 0; index < _NYA_OBSERVE_SPAN_COUNT; index++) {
        u64 mark = buffer.size;

        if (index > 0) _nya_observe_json_put(&buffer, ",");

        NYA_HttpSpan span = _NYA_OBSERVE_RING[(_NYA_OBSERVE_HEAD + index) % NYA_HTTP_TRACE_SPAN_MAX];
        _nya_observe_json_put_span(&buffer, &span);

        if (buffer.overflow) {
            // Roll the whole span, and its comma, back to a boundary and stop: what is written stays valid.
            buffer.size       = mark;
            buffer.data[mark] = '\0';
            buffer.overflow   = false;
            break;
        }
    }

    _nya_observe_unlock();

    buffer.capacity = full_capacity;
    _nya_observe_json_put(&buffer, suffix);

    return buffer.size;
}

NYA_Error nya_http_trace_flush(NYA_HttpTraceExportFn exporter, void* userdata, NYA_Arena* arena) {
    if (exporter == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no exporter to flush to");
    if (arena == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no arena to serialize into");

    if (nya_http_trace_span_count() == 0) return NYA_OK;

    // Bounded by the ring's fixed size: every span is well under a kilobyte and there are at most
    // NYA_HTTP_TRACE_SPAN_MAX of them, so a fixed envelope holds the worst case without a growing buffer.
    u64   capacity = (u64)NYA_HTTP_TRACE_SPAN_MAX * 1024ULL + 4096ULL;
    char* json     = nya_arena_alloc(arena, capacity);
    if (json == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room to serialize spans");

    u64 size = nya_http_trace_export_json(json, capacity);

    NYA_Error exported = exporter(json, size, userdata);

    // Cleared only once the exporter took the bytes, so a failed export tries the same spans again.
    if (exported.ok) nya_http_trace_reset();

    return exported;
}

b8 nya_http_traceparent_parse(
    NYA_ConstCString header,
    u64              size,
    OUT u8           trace_id[NYA_HTTP_TRACE_ID_BYTES],
    OUT u8           span_id[NYA_HTTP_SPAN_ID_BYTES],
    OUT u8*          flags
) {
    nya_assert(trace_id != nullptr && span_id != nullptr && flags != nullptr);

    // version(2) '-' trace(32) '-' span(16) '-' flags(2) — 55 characters. A future version may append
    // more, so this reads the fixed head and ignores a trailing "-…"; a shorter header is malformed.
    if (header == nullptr || size < 55) return false;

    if (header[2] != '-' || header[35] != '-' || header[52] != '-') return false;

    u8 version = 0;
    if (!_nya_observe_hex_decode(header, &version, 1)) return false;

    // 0xff is reserved and forbidden by the spec; anything else is read as version 00's layout.
    if (version == 0xff) return false;

    if (!_nya_observe_hex_decode(header + 3, trace_id, NYA_HTTP_TRACE_ID_BYTES)) return false;
    if (!_nya_observe_hex_decode(header + 36, span_id, NYA_HTTP_SPAN_ID_BYTES)) return false;
    if (!_nya_observe_hex_decode(header + 53, flags, 1)) return false;

    // An all-zero trace or span id is forbidden: it is what an implementation writes when it has none.
    if (_nya_observe_bytes_all_zero(trace_id, NYA_HTTP_TRACE_ID_BYTES) || _nya_observe_bytes_all_zero(span_id, NYA_HTTP_SPAN_ID_BYTES)) return false;

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 _nya_observe_method_index(NYA_HttpMethod method) {
    return (u32)method < (u32)NYA_HTTP_METHOD_COUNT ? (u32)method : (u32)NYA_HTTP_METHOD_NONE;
}

u32 _nya_observe_status_class(NYA_HttpStatus status) {
    u32 cls = (u32)status / 100;

    return cls >= 1 && cls <= 5 ? cls : 0;
}

u32 _nya_observe_bucket_of(u64 duration_ns) {
    for (u32 index = 0; index < NYA_HTTP_OBSERVE_BUCKET_COUNT; index++) {
        if (duration_ns <= _NYA_OBSERVE_BOUNDS_NS[index]) return index;
    }

    return NYA_HTTP_OBSERVE_BUCKET_COUNT; // the `+Inf` slot
}

void _nya_observe_lock(void) {
    // A test-and-set spin: the critical section is a struct copy, so a waiter is a handful of cycles.
    while (atomic_exchange_explicit(&_NYA_OBSERVE_LOCK, true, memory_order_acquire)) {
        // spin
    }
}

void _nya_observe_unlock(void) {
    atomic_store_explicit(&_NYA_OBSERVE_LOCK, false, memory_order_release);
}

void _nya_observe_id_bytes(OUT u8* out, u64 size) {
    if (nya_os_random_bytes(out, size)) return;

    // The CSPRNG is unavailable — rare, and an id only has to be unique, not secret. A per-call sequence
    // run through a bit mixer is distinct for every call within the process, which is all a trace id needs.
    u64 sequence = atomic_fetch_add_explicit(&_NYA_OBSERVE_SEQUENCE, 1, memory_order_relaxed) + 1;
    u64 mixed    = sequence * 0x9E3779B97F4A7C15ULL;

    for (u64 index = 0; index < size; index++) out[index] = (u8)(mixed >> ((index % 8) * 8)) ^ (u8)(index * 31 + 1);
}

s32 _nya_observe_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;

    return -1;
}

b8 _nya_observe_hex_decode(const char* hex, u8* out, u64 size) {
    for (u64 index = 0; index < size; index++) {
        s32 high = _nya_observe_hex_nibble(hex[index * 2]);
        s32 low  = _nya_observe_hex_nibble(hex[index * 2 + 1]);

        if (high < 0 || low < 0) return false;

        out[index] = (u8)((high << 4) | low);
    }

    return true;
}

b8 _nya_observe_bytes_all_zero(const u8* bytes, u64 size) {
    for (u64 index = 0; index < size; index++) {
        if (bytes[index] != 0) return false;
    }

    return true;
}

void _nya_observe_json_put(_NYA_ObserveJson* buffer, NYA_ConstCString text) {
    nya_assert(buffer != nullptr && text != nullptr);

    if (buffer->overflow) return;

    u64 length = strlen(text);

    if (buffer->size + length + 1 > buffer->capacity) {
        buffer->overflow = true;
        return;
    }

    nya_memcpy(buffer->data + buffer->size, text, length);
    buffer->size              += length;
    buffer->data[buffer->size] = '\0';
}

void _nya_observe_json_put_u64(_NYA_ObserveJson* buffer, u64 value) {
    char digits[24];
    (void)snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);
    _nya_observe_json_put(buffer, digits);
}

void _nya_observe_json_put_hex(_NYA_ObserveJson* buffer, const u8* bytes, u64 size) {
    for (u64 index = 0; index < size; index++) {
        char pair[3];
        (void)snprintf(pair, sizeof(pair), "%02x", bytes[index]);
        _nya_observe_json_put(buffer, pair);
    }
}

void _nya_observe_json_put_escaped(_NYA_ObserveJson* buffer, NYA_ConstCString text) {
    if (text == nullptr) return;

    for (u64 index = 0; text[index] != '\0'; index++) {
        char c = text[index];

        switch (c) {
            case '"' : _nya_observe_json_put(buffer, "\\\""); break;
            case '\\': _nya_observe_json_put(buffer, "\\\\"); break;
            case '\n': _nya_observe_json_put(buffer, "\\n"); break;
            case '\r': _nya_observe_json_put(buffer, "\\r"); break;
            case '\t': _nya_observe_json_put(buffer, "\\t"); break;
            default  : {
                if ((unsigned char)c < 0x20) {
                    // A control byte becomes its \u escape, so no byte can break the string.
                    char escaped[8];
                    (void)snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)(unsigned char)c);
                    _nya_observe_json_put(buffer, escaped);
                } else {
                    char one[2] = { c, '\0' };
                    _nya_observe_json_put(buffer, one);
                }
            } break;
        }
    }
}

void _nya_observe_json_put_span(_NYA_ObserveJson* buffer, const NYA_HttpSpan* span) {
    _nya_observe_json_put(buffer, "{\"traceId\":\"");
    _nya_observe_json_put_hex(buffer, span->trace_id, sizeof(span->trace_id));
    _nya_observe_json_put(buffer, "\",\"spanId\":\"");
    _nya_observe_json_put_hex(buffer, span->span_id, sizeof(span->span_id));
    _nya_observe_json_put(buffer, "\",");

    if (span->has_parent) {
        _nya_observe_json_put(buffer, "\"parentSpanId\":\"");
        _nya_observe_json_put_hex(buffer, span->parent_span_id, sizeof(span->parent_span_id));
        _nya_observe_json_put(buffer, "\",");
    }

    // kind 2 is SPAN_KIND_SERVER; the timestamps are OTLP uint64 nanoseconds, carried as strings.
    _nya_observe_json_put(buffer, "\"name\":\"");
    _nya_observe_json_put_escaped(buffer, span->name);
    _nya_observe_json_put(buffer, "\",\"kind\":2,\"startTimeUnixNano\":\"");
    _nya_observe_json_put_u64(buffer, span->start_unix_ns);
    _nya_observe_json_put(buffer, "\",\"endTimeUnixNano\":\"");
    _nya_observe_json_put_u64(buffer, span->end_unix_ns);
    _nya_observe_json_put(buffer, "\",\"attributes\":[{\"key\":\"http.request.method\",\"value\":{\"stringValue\":\"");
    _nya_observe_json_put_escaped(buffer, nya_http_method_text(span->method));
    _nya_observe_json_put(buffer, "\"}},{\"key\":\"http.response.status_code\",\"value\":{\"intValue\":\"");
    _nya_observe_json_put_u64(buffer, span->status);
    _nya_observe_json_put(buffer, "\"}},{\"key\":\"http.route\",\"value\":{\"stringValue\":\"");
    _nya_observe_json_put_escaped(buffer, span->name);
    _nya_observe_json_put(buffer, "\"}}]");

    // A 5xx is an ERROR span (status code 2); everything else is left UNSET, as an OTLP server span is.
    if (span->status >= 500) _nya_observe_json_put(buffer, ",\"status\":{\"code\":2}");

    _nya_observe_json_put(buffer, "}");
}

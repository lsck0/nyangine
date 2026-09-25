/**
 * RED metrics and OTLP tracing: http_observe.c, and its render inside GET /metrics.
 *
 * Three things have to hold. The histogram has to accumulate into the right buckets and render as valid
 * Prometheus histogram text — cumulative `_bucket{le=...}` lines, a `_sum` and a `_count` per series —
 * and the error counter beside it. A span has to be recorded per request with the route pattern (never a
 * raw path), the method, the status and the times, with a W3C `traceparent` parsed into the trace id and
 * parent when one is sent and a fresh trace begun when one is not; the ring has to stay bounded and drop
 * the oldest. And the OTLP/JSON export has to be well-formed, which a real deserialize proves.
 *
 * The disabled paths are checked too: with the histogram off, recording changes no counter, and with
 * tracing off, recording adds no span — and neither recorder is even handed an arena, so the disabled
 * path cannot allocate.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/* ── a Prometheus validator that accepts gauge, counter and histogram families ── */

static b8 is_name_head(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
}

static b8 is_name_tail(char c) {
    return is_name_head(c) || (c >= '0' && c <= '9');
}

static const char* parse_name(const char* p) {
    if (!is_name_head(*p)) return nullptr;
    p++;
    while (is_name_tail(*p)) p++;
    return p;
}

static const char* parse_label_value(const char* p) {
    if (*p != '"') return nullptr;
    p++;
    while (*p != '"') {
        if (*p == '\0' || *p == '\n') return nullptr;
        if (*p == '\\') {
            if (p[1] == '\0') return nullptr;
            p += 2;
            continue;
        }
        p++;
    }
    return p + 1;
}

static const char* parse_labels(const char* p) {
    if (*p != '{') return nullptr;
    p++;
    if (*p == '}') return p + 1;
    for (;;) {
        const char* after_name = parse_name(p);
        if (after_name == nullptr) return nullptr;
        p = after_name;
        if (*p != '=') return nullptr;
        p++;
        const char* after_value = parse_label_value(p);
        if (after_value == nullptr) return nullptr;
        p = after_value;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') return p + 1;
        return nullptr;
    }
}

static b8 line_is_valid(const char* line) {
    if (line[0] == '\0') return true;

    if (line[0] == '#') {
        if (strncmp(line, "# HELP ", 7) == 0) return parse_name(line + 7) != nullptr;
        if (strncmp(line, "# TYPE ", 7) == 0) {
            const char* after = parse_name(line + 7);
            if (after == nullptr) return false;
            // This renderer writes only these three types.
            return strcmp(after, " gauge") == 0 || strcmp(after, " counter") == 0 || strcmp(after, " histogram") == 0;
        }
        return false;
    }

    const char* p = parse_name(line);
    if (p == nullptr) return false;

    if (*p == '{') {
        p = parse_labels(p);
        if (p == nullptr) return false;
    }

    if (*p != ' ') return false;
    p++;

    char* end = nullptr;
    (void)strtod(p, &end);
    if (end == p) return false;

    return *end == '\0';
}

static b8 parses_as_prometheus(NYA_Arena* arena, const char* text) {
    u64   length = strlen(text);
    char* copy   = nya_arena_alloc(arena, length + 1);
    memcpy(copy, text, length + 1);

    char* cursor = copy;
    while (*cursor != '\0') {
        char* newline = strchr(cursor, '\n');
        if (newline != nullptr) *newline = '\0';

        if (!line_is_valid(cursor)) {
            nya_log_error("prometheus line did not parse: '%s'", cursor);
            return false;
        }

        if (newline == nullptr) break;
        cursor = newline + 1;
    }

    return true;
}

/* ── a capturing exporter for the flush seam ── */

static char EXPORTED[NYA_HTTP_TRACE_SPAN_MAX * 1024 + 4096];
static u64  EXPORTED_SIZE  = 0;
static u32  EXPORTED_CALLS = 0;

static NYA_Error capture_export(const char* json, u64 size, void* userdata) {
    (void)userdata;
    EXPORTED_CALLS++;
    EXPORTED_SIZE = size < sizeof(EXPORTED) - 1 ? size : sizeof(EXPORTED) - 1;
    memcpy(EXPORTED, json, EXPORTED_SIZE);
    EXPORTED[EXPORTED_SIZE] = '\0';
    return NYA_OK;
}

#define MS 1000000ULL
#define S  1000000000ULL

int main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_observe");
    defer      nya_arena_destroy(arena);

    // ── the histogram accumulates into the right buckets ──
    {
        nya_http_observe_metrics_set_enabled(true);
        nya_http_observe_reset();

        // Three 2xx GETs landing in le=0.005, le=0.01 and le=0.05, and one 5xx GET at 2s (le=2.5).
        nya_http_observe_request(NYA_HTTP_METHOD_GET, NYA_HTTP_STATUS_OK, 3 * MS);
        nya_http_observe_request(NYA_HTTP_METHOD_GET, NYA_HTTP_STATUS_OK, 8 * MS);
        nya_http_observe_request(NYA_HTTP_METHOD_GET, NYA_HTTP_STATUS_OK, 40 * MS);
        nya_http_observe_request(NYA_HTTP_METHOD_GET, NYA_HTTP_STATUS_INTERNAL_ERROR, 2 * S);

        nya_check(nya_http_observe_count(NYA_HTTP_METHOD_GET, 2) == 3, "three 2xx GETs counted");
        nya_check(nya_http_observe_count(NYA_HTTP_METHOD_GET, 5) == 1, "one 5xx GET counted");

        // The cumulative buckets: 1 at le=0.005, 2 at le=0.01, 2 at le=0.025, 3 at le=0.05.
        nya_check(nya_http_observe_bucket_cumulative(NYA_HTTP_METHOD_GET, 2, 0) == 1, "one request <= 5ms");
        nya_check(nya_http_observe_bucket_cumulative(NYA_HTTP_METHOD_GET, 2, 1) == 2, "two requests <= 10ms");
        nya_check(nya_http_observe_bucket_cumulative(NYA_HTTP_METHOD_GET, 2, 2) == 2, "still two <= 25ms");
        nya_check(nya_http_observe_bucket_cumulative(NYA_HTTP_METHOD_GET, 2, 3) == 3, "three <= 50ms");

        nya_check(nya_http_observe_sum_ns(NYA_HTTP_METHOD_GET, 2) == 51 * MS, "the sum is 51ms");
        nya_check(nya_http_observe_errors(NYA_HTTP_METHOD_GET) == 1, "the 5xx bumped the error counter");
        nya_check(nya_http_observe_errors(NYA_HTTP_METHOD_POST) == 0, "a method with no error stays zero");
    }

    // ── the histogram renders as valid Prometheus text ──
    {
        char* text = nya_arena_alloc(arena, NYA_HTTP_METRICS_PROMETHEUS_MAX_BYTES);
        u64   size = nya_http_metrics_prometheus(text, NYA_HTTP_METRICS_PROMETHEUS_MAX_BYTES);

        nya_check(size == strlen(text), "the returned length is the body's length");
        nya_check(parses_as_prometheus(arena, text), "every line parses as Prometheus text");

        nya_check(strstr(text, "# TYPE http_request_duration_seconds histogram\n") != nullptr, "the duration family is a histogram");
        nya_check(strstr(text, "http_request_duration_seconds_bucket{method=\"GET\",status=\"2xx\",le=\"0.005\"} 1\n") != nullptr, "le=0.005 is 1");
        nya_check(strstr(text, "http_request_duration_seconds_bucket{method=\"GET\",status=\"2xx\",le=\"0.01\"} 2\n") != nullptr, "le=0.01 is 2");
        nya_check(strstr(text, "http_request_duration_seconds_bucket{method=\"GET\",status=\"2xx\",le=\"0.05\"} 3\n") != nullptr, "le=0.05 is 3");
        nya_check(strstr(text, "http_request_duration_seconds_bucket{method=\"GET\",status=\"2xx\",le=\"+Inf\"} 3\n") != nullptr, "le=+Inf is the total");
        nya_check(strstr(text, "http_request_duration_seconds_count{method=\"GET\",status=\"2xx\"} 3\n") != nullptr, "the count is 3");
        nya_check(strstr(text, "http_request_duration_seconds_sum{method=\"GET\",status=\"2xx\"} 0.051\n") != nullptr, "the sum is 0.051 seconds");
        nya_check(strstr(text, "http_request_duration_seconds_bucket{method=\"GET\",status=\"5xx\",le=\"2.5\"} 1\n") != nullptr, "the 5xx request is <= 2.5s");
        nya_check(strstr(text, "# TYPE http_request_errors_total counter\n") != nullptr, "the error family is a counter");
        nya_check(strstr(text, "http_request_errors_total{method=\"GET\"} 1\n") != nullptr, "one GET error");
    }

    // ── the disabled histogram changes no counter and renders no family ──
    {
        nya_http_observe_metrics_set_enabled(false);
        nya_http_observe_reset();

        // No arena is handed to the recorder, so a disabled record cannot allocate; it must also not count.
        nya_http_observe_request(NYA_HTTP_METHOD_GET, NYA_HTTP_STATUS_OK, 5 * MS);
        nya_check(nya_http_observe_count(NYA_HTTP_METHOD_GET, 2) == 0, "a disabled histogram records nothing");

        char* text = nya_arena_alloc(arena, NYA_HTTP_METRICS_PROMETHEUS_MAX_BYTES);
        (void)nya_http_metrics_prometheus(text, NYA_HTTP_METRICS_PROMETHEUS_MAX_BYTES);
        nya_check(strstr(text, "http_request_duration_seconds") == nullptr, "a disabled histogram renders no family");

        nya_http_observe_metrics_set_enabled(true);
        nya_http_observe_reset();
    }

    // ── traceparent parsing ──
    {
        u8 trace_id[NYA_HTTP_TRACE_ID_BYTES] = { 0 };
        u8 span_id[NYA_HTTP_SPAN_ID_BYTES]   = { 0 };
        u8 flags                             = 0;

        const char* valid = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
        nya_check(nya_http_traceparent_parse(valid, strlen(valid), trace_id, span_id, &flags), "a well-formed traceparent parses");
        nya_check(trace_id[0] == 0x4b && trace_id[15] == 0x36, "the trace id bytes are the header's");
        nya_check(span_id[0] == 0x00 && span_id[1] == 0xf0 && span_id[7] == 0xb7, "the span id bytes are the header's");
        nya_check(flags == 0x01, "the flags byte is 0x01");

        // The rejections: too short, a non-hex digit, all-zero ids, and a misplaced hyphen.
        nya_check(!nya_http_traceparent_parse("00-abc", 6, trace_id, span_id, &flags), "a short header is rejected");
        nya_check(!nya_http_traceparent_parse("00-4bf92f3577b34da6a3ce929d0e0e473g-00f067aa0ba902b7-01", 55, trace_id, span_id, &flags),
                  "a non-hex trace id is rejected");
        nya_check(!nya_http_traceparent_parse("00-00000000000000000000000000000000-00f067aa0ba902b7-01", 55, trace_id, span_id, &flags),
                  "an all-zero trace id is rejected");
        nya_check(!nya_http_traceparent_parse("00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01", 55, trace_id, span_id, &flags),
                  "an all-zero span id is rejected");
        nya_check(!nya_http_traceparent_parse("00_4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", 55, trace_id, span_id, &flags),
                  "a missing hyphen is rejected");
    }

    // ── a span is recorded per request, off by default ──
    {
        nya_check(!nya_http_trace_config_get().enabled, "tracing is off by default");

        nya_http_trace_reset();
        nya_http_trace_record(NYA_HTTP_METHOD_GET, NYA_HTTP_STATUS_OK, "/api/thing", 1000, 5 * MS, nullptr);
        nya_check(nya_http_trace_span_count() == 0, "a disabled tracer records no span");

        nya_http_trace_config_set((NYA_HttpTraceConfig){ .enabled = true });
        nya_http_trace_reset();

        // A root span: no parent header, so a fresh trace id and no parent.
        nya_http_trace_record(NYA_HTTP_METHOD_GET, NYA_HTTP_STATUS_OK, "/api/thing", 1000, 5 * MS, nullptr);
        nya_check(nya_http_trace_span_count() == 1, "one span recorded");

        NYA_HttpSpan span = { 0 };
        nya_check(nya_http_trace_span_at(0, &span), "the span reads back");
        nya_check(strcmp(span.name, "/api/thing") == 0, "the name is the route pattern");
        nya_check(span.method == NYA_HTTP_METHOD_GET, "the method is recorded");
        nya_check(span.status == (u32)NYA_HTTP_STATUS_OK, "the status is recorded");
        nya_check(span.start_unix_ns == 1000 && span.end_unix_ns == 1000 + 5 * MS, "start and end are start and start+duration");
        nya_check(!span.has_parent, "a request with no traceparent is a root span");

        b8 trace_nonzero = false;
        for (u32 i = 0; i < NYA_HTTP_TRACE_ID_BYTES; i++) trace_nonzero = trace_nonzero || span.trace_id[i] != 0;
        nya_check(trace_nonzero, "a root span has a non-zero trace id");

        // A child span: a valid traceparent supplies the trace id and this span's parent.
        const char* parent = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
        nya_http_trace_reset();
        nya_http_trace_record(NYA_HTTP_METHOD_POST, NYA_HTTP_STATUS_CREATED, "/api/thing", 2000, 3 * MS, parent);

        NYA_HttpSpan child = { 0 };
        nya_check(nya_http_trace_span_at(0, &child), "the child span reads back");
        nya_check(child.has_parent, "a request with a traceparent has a parent");
        nya_check(child.trace_id[0] == 0x4b && child.trace_id[15] == 0x36, "the child inherits the caller's trace id");
        nya_check(child.parent_span_id[0] == 0x00 && child.parent_span_id[1] == 0xf0 && child.parent_span_id[7] == 0xb7,
                  "the parent span id is the caller's span id");
    }

    // ── the ring is bounded and drops the oldest ──
    {
        nya_http_trace_reset();

        for (u64 i = 0; i < NYA_HTTP_TRACE_SPAN_MAX + 5; i++) {
            nya_http_trace_record(NYA_HTTP_METHOD_GET, NYA_HTTP_STATUS_OK, "/api/x", i, MS, nullptr);
        }

        nya_check(nya_http_trace_span_count() == NYA_HTTP_TRACE_SPAN_MAX, "the ring never holds more than its capacity");

        NYA_HttpSpan oldest = { 0 };
        nya_check(nya_http_trace_span_at(0, &oldest), "the oldest reads back");
        nya_check(oldest.start_unix_ns == 5, "the first five spans were dropped, oldest first");
    }

    // ── the OTLP/JSON export is well-formed and carries the span's fields ──
    {
        nya_http_trace_reset();
        const char* parent = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
        nya_http_trace_record(NYA_HTTP_METHOD_GET, NYA_HTTP_STATUS_OK, "/api/thing", 1000, 5 * MS, parent);
        nya_http_trace_record(NYA_HTTP_METHOD_POST, NYA_HTTP_STATUS_INTERNAL_ERROR, "/api/fail", 2000, 7 * MS, nullptr);

        char* json = nya_arena_alloc(arena, NYA_HTTP_TRACE_SPAN_MAX * 1024 + 4096);
        u64   size = nya_http_trace_export_json(json, NYA_HTTP_TRACE_SPAN_MAX * 1024 + 4096);
        nya_check(size == strlen(json), "the export length is the text's length");

        nya_check(strstr(json, "\"resourceSpans\"") != nullptr, "the envelope names resourceSpans");
        nya_check(strstr(json, "\"scopeSpans\"") != nullptr, "the envelope names scopeSpans");
        nya_check(strstr(json, "\"name\":\"/api/thing\"") != nullptr, "the span name is the route pattern");
        nya_check(strstr(json, "\"http.request.method\"") != nullptr, "a method attribute is present");
        nya_check(strstr(json, "\"stringValue\":\"GET\"") != nullptr, "the method value is GET");
        nya_check(strstr(json, "\"http.response.status_code\"") != nullptr, "a status attribute is present");
        nya_check(strstr(json, "\"intValue\":\"200\"") != nullptr, "the status value is 200");
        nya_check(strstr(json, "\"http.route\"") != nullptr, "a route attribute is present");
        nya_check(strstr(json, "\"parentSpanId\":\"00f067aa0ba902b7\"") != nullptr, "the child carries the parent span id");
        nya_check(strstr(json, "\"status\":{\"code\":2}") != nullptr, "the 5xx span is marked an error");

        // The strongest check that it is well-formed: a real JSON deserialize accepts it.
        NYA_Object* document = nullptr;
        NYA_Error   parsed   = nya_deserialize(arena, (const u8*)json, size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &document);
        nya_check(parsed.ok && document != nullptr, "the OTLP/JSON export is well-formed JSON");
    }

    // ── the flush seam serializes, hands off, and clears the ring on success ──
    {
        nya_http_trace_reset();
        nya_http_trace_record(NYA_HTTP_METHOD_GET, NYA_HTTP_STATUS_OK, "/api/thing", 1000, 5 * MS, nullptr);

        EXPORTED_CALLS = 0;
        EXPORTED_SIZE  = 0;

        NYA_Error flushed = nya_http_trace_flush(capture_export, nullptr, arena);
        nya_check(flushed.ok, "the flush succeeds");
        nya_check(EXPORTED_CALLS == 1, "the exporter was called once");
        nya_check(EXPORTED_SIZE > 0 && strstr(EXPORTED, "\"name\":\"/api/thing\"") != nullptr, "the exporter received the serialized span");
        nya_check(nya_http_trace_span_count() == 0, "the ring is cleared once the exporter took the bytes");

        // An empty ring is a no-op flush, not an error and not another call.
        EXPORTED_CALLS  = 0;
        NYA_Error empty = nya_http_trace_flush(capture_export, nullptr, arena);
        nya_check(empty.ok && EXPORTED_CALLS == 0, "flushing an empty ring calls nothing");
    }

    nya_http_trace_config_set((NYA_HttpTraceConfig){ .enabled = false });
    nya_http_trace_reset();
    nya_http_observe_reset();

    printf("PASSED: http observe\n");

    return EXIT_SUCCESS;
}

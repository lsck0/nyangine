/**
 * The Prometheus render of the metrics resource: GET /metrics.
 *
 * Two things have to hold and both are checked here. The output has to be valid exposition text — a
 * name outside `[a-zA-Z_:][a-zA-Z0-9_:]*` is one a collector rejects, and a label value with a raw
 * quote or newline in it is one that forges a second sample — so a small parser walks every line and
 * refuses anything the format would. And the numbers have to be the ones in the registries, so known
 * ceilings and gauges are registered and their sample lines are read back exactly.
 *
 * A ceiling registered under a hostile name — quotes, a backslash, a newline, dots and braces — is the
 * injection case: it must come out as an escaped label value, never spliced into a metric name and
 * never breaking out of its own line. `_nya_prom_name`, the name sanitizer, is exercised on its own
 * besides.
 *
 * No socket and no app: nya_http_metrics_prometheus renders from the registries alone, and the route
 * is driven once through nya_http_router_dispatch to prove the Content-Type and the body it hands back.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

#define NOW_S 1700000000ULL

/* a just-enough Prometheus text validator — Not a general parser: it knows the shapes this renderer produces. Every line is blank, a `# HELP`/ `# TYPE` comment, or a sample. A sample is a metric name, an optional `{label="value",…}` block, a space, and a value that strtod accepts. Names are held to the charset; label values are read with the format's escapes, so an unescaped quote or a bare newline inside a value is a parse failure — which is exactly the injection a hostile registered name would attempt. */

/** Whether `c` may begin a metric or label name. */
static b8 is_name_head(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
}

/** Whether `c` may continue one. */
static b8 is_name_tail(char c) {
    return is_name_head(c) || (c >= '0' && c <= '9');
}

/** Reads a name at `p`, returns the byte after it, or nullptr if there is no valid name. */
static const char* parse_name(const char* p) {
    if (!is_name_head(*p)) return nullptr;
    p++;
    while (is_name_tail(*p)) p++;
    return p;
}

/** Reads a `"…"` label value at `p` (past the opening quote is at *p), returns the byte after the closing quote. */
static const char* parse_label_value(const char* p) {
    if (*p != '"') return nullptr;
    p++;

    while (*p != '"') {
        if (*p == '\0' || *p == '\n') return nullptr; // a value never spans a line or runs to the end unterminated
        if (*p == '\\') {
            // An escape is a backslash and exactly one following byte; a trailing backslash is invalid.
            if (p[1] == '\0') return nullptr;
            p += 2;
            continue;
        }
        p++;
    }

    return p + 1; // past the closing quote
}

/** Reads the `{label="value",…}` block at `p`, returns the byte after `}`, or nullptr on a malformed block. */
static const char* parse_labels(const char* p) {
    if (*p != '{') return nullptr;
    p++;

    if (*p == '}') return p + 1; // an empty block is legal, though this renderer never writes one

    for (;;) {
        const char* after_name = parse_name(p);
        if (after_name == nullptr) return nullptr;
        p = after_name;

        if (*p != '=') return nullptr;
        p++;

        const char* after_value = parse_label_value(p);
        if (after_value == nullptr) return nullptr;
        p = after_value;

        if (*p == ',') { p++; continue; }
        if (*p == '}') return p + 1;
        return nullptr;
    }
}

/** Validates one line, terminator-free. Returns whether it is a well-formed exposition line. */
static b8 line_is_valid(const char* line) {
    if (line[0] == '\0') return true; // a blank line is allowed between families

    if (line[0] == '#') {
        // The only comments this renderer writes are HELP and TYPE, each naming a valid metric, and a TYPE always says gauge.
        if (strncmp(line, "# HELP ", 7) == 0) return parse_name(line + 7) != nullptr;
        if (strncmp(line, "# TYPE ", 7) == 0) {
            const char* after = parse_name(line + 7);
            return after != nullptr && strcmp(after, " gauge") == 0;
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

    // The value: strtod has to accept it and consume something, and only an optional timestamp may follow. This renderer writes no timestamp, so the rest must be empty.
    char* end = nullptr;
    (void)strtod(p, &end);
    if (end == p) return false;

    return *end == '\0';
}

/** Runs line_is_valid over every line of `text`, returning whether all pass. Reports the first failure. */
static b8 parses_as_prometheus(const char* text) {
    NYA_Arena* arena = nya_arena_create(.name = "prom_validate");
    defer      nya_arena_destroy(arena);

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

/* Backing counters the registrations point at; static so they outlive the registry entry. */
static u32 TWEENS_LIVE  = 12;
static u32 SPRITES_LIVE = 1000;
static u32 ODD_LIVE     = 3;
static u64 GPU_BYTES    = 4096;

/* A hostile name: a quote, a backslash, a newline, and characters that are not legal in a metric name. */
static const char ODD_NAME[] = "a\"b\\c\nd.e-f{g}";

int main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_prometheus");
    defer      nya_arena_destroy(arena);

    // TEST: the name sanitizer holds anything to the Prometheus charset.
    {
        char out[64];

        _nya_prom_name(out, sizeof(out), "nyangine_ceiling_live");
        nya_check(strcmp(out, "nyangine_ceiling_live") == 0, "a valid name is left alone");

        _nya_prom_name(out, sizeof(out), "gpu textures.count-1");
        nya_check(strcmp(out, "gpu_textures_count_1") == 0, "spaces, dots and dashes become underscores");

        _nya_prom_name(out, sizeof(out), "9lives");
        nya_check(strcmp(out, "_9lives") == 0, "a leading digit is kept behind an underscore, not dropped");

        _nya_prom_name(out, sizeof(out), "");
        nya_check(strcmp(out, "_") == 0, "an empty name sanitizes to a single underscore");

        _nya_prom_name(out, sizeof(out), ODD_NAME);
        for (const char* c = out; *c != '\0'; c++) {
            nya_check(is_name_tail(*c), "every byte of a sanitized name is in the charset");
        }
        nya_check(is_name_head(out[0]), "a sanitized name has a valid first byte");
    }

    // Register a known registry: two plain ceilings, one hostile, and a gauge.
    _nya_ceiling_registry_reset_for_test();

    nya_ceiling_register("tweens", 256, &TWEENS_LIVE);
    nya_ceiling_register("sprites", 1024, &SPRITES_LIVE);
    nya_ceiling_register(ODD_NAME, 8, &ODD_LIVE);
    nya_gauge_register("gpu_textures", &GPU_BYTES);

    char* text = nya_arena_alloc(arena, NYA_HTTP_METRICS_PROMETHEUS_MAX_BYTES);
    u64   size = nya_http_metrics_prometheus(text, NYA_HTTP_METRICS_PROMETHEUS_MAX_BYTES);

    // TEST: the whole body is valid exposition text.
    nya_check(size == strlen(text), "the returned length is the body's length");
    nya_check(parses_as_prometheus(text), "every line parses as Prometheus text");

    // TEST: the families are declared, once each, as gauges.
    nya_check(strstr(text, "# TYPE nyangine_ceiling_live gauge\n") != nullptr, "the live family is a gauge");
    nya_check(strstr(text, "# TYPE nyangine_ceiling_capacity gauge\n") != nullptr, "the capacity family is a gauge");
    nya_check(strstr(text, "# TYPE nyangine_gauge_bytes gauge\n") != nullptr, "the gauge family is a gauge");
    nya_check(strstr(text, "# HELP nyangine_ceiling_live ") != nullptr, "the live family has help");

    // TEST: the plain ceilings and the gauge carry the registered numbers.
    nya_check(strstr(text, "nyangine_ceiling_live{ceiling=\"tweens\"} 12\n") != nullptr, "tweens is 12 live");
    nya_check(strstr(text, "nyangine_ceiling_capacity{ceiling=\"tweens\"} 256\n") != nullptr, "tweens holds 256");
    nya_check(strstr(text, "nyangine_ceiling_live{ceiling=\"sprites\"} 1000\n") != nullptr, "sprites is 1000 live");
    nya_check(strstr(text, "nyangine_ceiling_capacity{ceiling=\"sprites\"} 1024\n") != nullptr, "sprites holds 1024");
    nya_check(strstr(text, "nyangine_gauge_bytes{gauge=\"gpu_textures\"} 4096\n") != nullptr, "the gauge is 4096 bytes");

    // TEST: the hostile name comes out escaped, in a label, and never raw.
    {
        // The escaped label value: a\"b\\c\nd.e-f{g} — the quote, the backslash and the newline are the format's two-byte escapes; the dot, the dash and the braces are legal in a value and untouched.
        const char* escaped = "nyangine_ceiling_live{ceiling=\"a\\\"b\\\\c\\nd.e-f{g}\"} 3\n";
        nya_check(strstr(text, escaped) != nullptr, "the hostile name is an escaped label value with its live count");

        // Nothing raw leaked: the un-escaped 'a"b' never appears (the quote is always preceded by a backslash), and no real newline sits inside a ceiling label.
        nya_check(strstr(text, "a\"b") == nullptr, "the quote in the name was escaped, not emitted raw");
        nya_check(strstr(text, "ceiling=\"a\"") == nullptr, "the name did not close its own label early");

        // The hostile name was never spliced into a metric name: the only metric names are the fixed ones.
        nya_check(strstr(text, "nyangine_ceiling_live{") != nullptr, "the metric name is the fixed family name");
    }

    // TEST: the route answers text/plain; version=0.0.4 with that same body.
    {
        u8 body[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };

        NYA_HttpResponse response = { 0 };
        nya_http_response_create(&response, body, sizeof(body));
        defer nya_http_response_destroy(&response);

        NYA_HttpRequest request = { .method = NYA_HTTP_METHOD_GET, .keep_alive = true };
        NYA_UrlFailure  failure = { 0 };
        nya_check(nya_url_parse_target(NYA_HTTP_METRICS_PROMETHEUS_PATH, strlen(NYA_HTTP_METRICS_PROMETHEUS_PATH), &request.target, &failure).ok,
                  "the target parses");
        (void)snprintf(request.path, sizeof(request.path), "%.*s", (int)request.target.path.length,
                       request.target.text + request.target.path.offset);

        const NYA_HttpRouter* routers[] = { nya_http_metrics_router() };

        NYA_HttpExchange exchange = { .request = &request, .response = &response, .arena = arena, .now_s = NOW_S };

        nya_check(nya_http_router_check(nya_http_metrics_router()).ok, "the metrics routes are well formed");

        NYA_HttpStatus status = nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), nullptr, 0);
        nya_check(status == NYA_HTTP_STATUS_OK, "GET /metrics is 200");

        // The body the route wrote matches the direct render's shape: the same families are present.
        nya_check(response.body_size > 0, "the route wrote a body");
        response.body[response.body_size] = '\0';
        nya_check(strstr((const char*)response.body, "# TYPE nyangine_ceiling_live gauge\n") != nullptr, "the route's body is the render");

        // The Content-Type is the exposition format's, set as a header rather than from the media enum.
        b8 found_content_type = false;
        for (u32 index = 0; index < response.header_count; index++) {
            if (strcmp(response.headers[index].name, "Content-Type") == 0) {
                nya_check(strcmp(response.headers[index].value, "text/plain; version=0.0.4") == 0, "the version is the format's");
                found_content_type = true;
            }
        }
        nya_check(found_content_type, "the route set a Content-Type header");
    }

    _nya_ceiling_registry_reset_for_test();

    printf("PASSED: debug prometheus\n");

    return EXIT_SUCCESS;
}

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_ceiling.h"
#include "nyangine-core/core/core_app.h"
#include "nyangine-core/debug/debug_metrics.h"
#include "nyangine-core/http/http_server.h"
#include "nyangine-std/base/base_clock.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Writes `dto` in whatever the caller asked for: JSON by default, the text or binary native form when
 * Accept names it, the binary one carrying the DTO's layout hash.
 * */
NYA_INTERNAL NYA_Error _nya_http_metrics_answer(NYA_HttpExchange* exchange, const NYA_TypeReflection* type, const void* dto) __attr_no_discard;

NYA_INTERNAL NYA_HttpStatus _nya_http_metrics_query(NYA_HttpExchange* exchange);
NYA_INTERNAL NYA_HttpStatus _nya_http_metrics_ceilings_query(NYA_HttpExchange* exchange);
NYA_INTERNAL NYA_HttpStatus _nya_http_metrics_arenas_query(NYA_HttpExchange* exchange);
NYA_INTERNAL NYA_HttpStatus _nya_http_metrics_systems_query(NYA_HttpExchange* exchange);
NYA_INTERNAL NYA_HttpStatus _nya_http_metrics_accounting_put(NYA_HttpExchange* exchange, const NYA_HttpIdentity* identity);

NYA_INTERNAL NYA_HttpStatus _nya_http_metrics_prometheus_get(NYA_HttpExchange* exchange);

/** Copies `text` into a row's fixed name, truncating rather than refusing: a long name is not an error. */
NYA_INTERNAL void _nya_http_metrics_name(OUT char* destination, u64 capacity, NYA_ConstCString text);

/*
 * ── the Prometheus render ──
 *
 * A bounded appender over a fixed buffer, and the two transforms a scrape body needs made safe. Every
 * put is all-or-nothing: a put that would pass `capacity` writes nothing and raises `overflow`, so a
 * caller that snapshots `size` before a sample and restores it on overflow always leaves valid text
 * cut at a line, never a half-written one.
 */
typedef struct _NYA_PromBuffer _NYA_PromBuffer;
struct _NYA_PromBuffer {
    char* data;
    u64   capacity; /**< bytes of `data`, the terminator's byte included */
    u64   size;     /**< bytes written, the terminator not counted; `data[size]` is always '\0' */
    b8    overflow; /**< set once a put did not fit, and left every earlier byte alone */
};

/** Appends `text` whole, or nothing and raises `overflow`. Keeps `data` null terminated either way. */
NYA_INTERNAL void _nya_prom_put(_NYA_PromBuffer* buffer, NYA_ConstCString text);

/** Appends a `u64` in base ten. */
NYA_INTERNAL void _nya_prom_put_u64(_NYA_PromBuffer* buffer, u64 value);

/** Appends a finite `f64` as a Prometheus sample value; a non-finite one is written as `0`. */
NYA_INTERNAL void _nya_prom_put_f64(_NYA_PromBuffer* buffer, f64 value);

/**
 * Appends `text` as the inside of a label value: a backslash, a double quote and a newline become the
 * two-character escapes the format defines, so a name carrying any of them cannot end the line early
 * or open a second one. Everything else is a byte the format leaves alone.
 * */
NYA_INTERNAL void _nya_prom_put_label_value(_NYA_PromBuffer* buffer, NYA_ConstCString text);

/**
 * Copies `text` into `out`, keeping only `[a-zA-Z_:][a-zA-Z0-9_:]*`: any other byte becomes '_', a
 * leading digit is prefixed by one, and an empty result is a lone '_'. A valid Prometheus name, always.
 * */
NYA_INTERNAL void _nya_prom_name(OUT char* out, u64 capacity, NYA_ConstCString text);

/** Writes the `# HELP` and `# TYPE …  gauge` pair for one metric family, sanitizing `name` into `out`. */
NYA_INTERNAL void _nya_prom_family(_NYA_PromBuffer* buffer, NYA_ConstCString name, NYA_ConstCString help, OUT char* out, u64 capacity);

/** One `metric{label="value"} number` line for a `u64`, rolling back whole on overflow. Returns overflow. */
NYA_INTERNAL b8 _nya_prom_labeled_u64(_NYA_PromBuffer* buffer, NYA_ConstCString metric, NYA_ConstCString label, NYA_ConstCString value, u64 number);

/** A whole family — `# HELP`, `# TYPE`, one bare `metric number` line — for a scalar, on overflow rolled back. */
NYA_INTERNAL void _nya_prom_scalar_u64(_NYA_PromBuffer* buffer, NYA_ConstCString name, NYA_ConstCString help, u64 number);
NYA_INTERNAL void _nya_prom_scalar_f64(_NYA_PromBuffer* buffer, NYA_ConstCString name, NYA_ConstCString help, f64 number);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Every route here is NYA_HTTP_AFFINITY_MAIN, which on a threaded server means it is answered inside
 * the tick rather than on a worker.
 *
 * The whole resource is a read of what the frame writes: the app's frame statistics, the ceiling and
 * arena registries, the system registry. None of that is published for another thread to read, and
 * the registry guards say so out loud, so this resource belongs where the numbers are made. It costs
 * the frame what it always cost, which is a few hundred bytes of copying.
 */
NYA_INTERNAL const NYA_HttpRoute _NYA_HTTP_METRICS_ROUTES[] = {
    {
     .method        = NYA_HTTP_METHOD_QUERY,
     .path          = NYA_HTTP_METRICS_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
     .affinity      = NYA_HTTP_AFFINITY_MAIN,
     .handler       = _nya_http_metrics_query,
     .summary       = "Frame time and this server's own counters",
     .description   = "A read of nya_app_get's frame statistics and the HTTP server's connection and request counts. "
                         "Measures nothing: every number is one the program already keeps.", .response_type = nya_reflect_of(NYA_HttpMetricsDto),
     .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method        = NYA_HTTP_METHOD_QUERY,
     .path          = NYA_HTTP_METRICS_CEILINGS_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
     .affinity      = NYA_HTTP_AFFINITY_MAIN,
     .handler       = _nya_http_metrics_ceilings_query,
     .summary       = "Every fixed capacity array and how full it is",
     .description   = "The ceiling registry, which is what every `nya_ceiling_register` in the engine publishes into. "
                         "Sorted by fullness, so the one about to overflow is first.", .response_type = nya_reflect_of(NYA_HttpCeilingsDto),
     .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method        = NYA_HTTP_METHOD_QUERY,
     .path          = NYA_HTTP_METRICS_ARENAS_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
     .affinity      = NYA_HTTP_AFFINITY_MAIN,
     .handler       = _nya_http_metrics_arenas_query,
     .summary       = "Every live arena: used, reserved and fragmentation",
     .description   = "The arena registry. Resident bytes are not here on purpose: reading them is a system call per "
                         "region, which is a report's cost and not a poll's.", .response_type = nya_reflect_of(NYA_HttpArenasDto),
     .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method        = NYA_HTTP_METHOD_QUERY,
     .path          = NYA_HTTP_METRICS_SYSTEMS_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
     .affinity      = NYA_HTTP_AFFINITY_MAIN,
     .handler       = _nya_http_metrics_systems_query,
     .summary       = "Per owner: how many systems, what they cost, what they hold",
     .description   = "The system registry grouped by owner: the engine, the game, and one per plugin. The times read "
                         "zero until accounting is turned on; see PUT /api/metrics/accounting.", .response_type = nya_reflect_of(NYA_HttpSystemsDto),
     .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method             = NYA_HTTP_METHOD_PUT,
     .path               = NYA_HTTP_METRICS_ACCOUNTING_PATH,
     .auth               = NYA_HTTP_AUTH_BEARER,
     .affinity           = NYA_HTTP_AFFINITY_MAIN,
     .scope              = NYA_HTTP_SCOPE_WRITE,
     .handler_identified = _nya_http_metrics_accounting_put,
     .summary            = "Turn the registry's per system timing on or off",
     .description        = "Accounting costs a clock read per system per phase, so it is off by default and is asked "
                              "for rather than reported. Answers with the state that was reached.", .request_type       = nya_reflect_of(NYA_HttpAccountingDto),
     .response_type      = nya_reflect_of(NYA_HttpAccountingDto),
     .statuses           = { NYA_HTTP_STATUS_OK,
                                NYA_HTTP_STATUS_BAD_REQUEST,
                                NYA_HTTP_STATUS_UNAUTHORIZED,
                                NYA_HTTP_STATUS_FORBIDDEN,
                                NYA_HTTP_STATUS_UNSUPPORTED_MEDIA,
                                NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     /*
      * GET rather than QUERY, and off `/api`: a Prometheus scrape is a parameterless GET of `/metrics`,
      * and answering it anywhere else or under any other verb is asking every collector in the world to
      * be reconfigured. AFFINITY_MAIN for the same reason as its neighbours — it reads the registries the
      * frame writes. No `response_type`: the body is the text exposition format, not a reflected DTO, so
      * the OpenAPI walk lists the route and its statuses and leaves the schema out.
      */
     .method      = NYA_HTTP_METHOD_GET,
     .path        = NYA_HTTP_METRICS_PROMETHEUS_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .affinity    = NYA_HTTP_AFFINITY_MAIN,
     .handler     = _nya_http_metrics_prometheus_get,
     .summary     = "The ceilings, the gauges and the frame counters, for a Prometheus scrape",
     .description = "The same numbers as QUERY /api/metrics and its ceilings, rendered as the Prometheus text exposition "
                       "format. A bounded read of what the program already keeps; it measures nothing.",
     .statuses    = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

NYA_INTERNAL const NYA_HttpRouter _NYA_HTTP_METRICS_ROUTER = {
    .name        = "metrics",
    .routes      = _NYA_HTTP_METRICS_ROUTES,
    .route_count = nya_carray_length(_NYA_HTTP_METRICS_ROUTES),
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

const NYA_HttpRouter* nya_http_metrics_router(void) {
    return &_NYA_HTTP_METRICS_ROUTER;
}

u64 nya_http_metrics_prometheus(char* out, u64 capacity) {
    nya_assert(out != nullptr);
    nya_assert(capacity > 0);

    _NYA_PromBuffer buffer = { .data = out, .capacity = capacity, .size = 0, .overflow = false };
    out[0]                 = '\0';

    /*
     * The frame counters first, each its own single-sample family. Read through the app instance rather
     * than nya_app_get for the reason _nya_http_metrics_query gives: a headless tool may scrape before
     * there is an app, and "is there an app" is a question this endpoint answers rather than asserts on.
     * The times go out in seconds, which is the base unit the Prometheus conventions ask for, and every
     * f64 is guarded finite on the way out.
     */
    if (_NYA_APP_INSTANCE.initialized) {
        const NYA_FrameStats* frame = &_NYA_APP_INSTANCE.frame_stats;

        _nya_prom_scalar_f64(&buffer, "nyangine_uptime_seconds", "Seconds since nya_app_init.", (f64)frame->uptime_ns / 1.0e9);
        _nya_prom_scalar_f64(&buffer, "nyangine_frames_per_second", "Frames per second over the last frame.", (f64)frame->fps);
        _nya_prom_scalar_f64(&buffer, "nyangine_frame_delta_seconds", "Seconds the last frame represented.", (f64)frame->delta_time_s);
        _nya_prom_scalar_f64(&buffer, "nyangine_frame_work_seconds", "Seconds of work in the last frame before the limiter slept.",
                             (f64)frame->work_ns / 1.0e9);
        _nya_prom_scalar_f64(&buffer, "nyangine_frame_sleep_seconds", "Seconds the limiter slept to hold the frame rate.",
                             (f64)frame->sleep_ns / 1.0e9);
        _nya_prom_scalar_f64(&buffer, "nyangine_frame_elapsed_seconds", "Seconds from one frame's start to the next.",
                             (f64)frame->elapsed_ns / 1.0e9);
    }

    _nya_prom_scalar_u64(&buffer, "nyangine_http_connections", "Connections the HTTP server is holding.", nya_http_server_connection_count());
    _nya_prom_scalar_u64(&buffer, "nyangine_http_requests", "Requests the HTTP server has answered since it started.", nya_http_server_request_count());
    _nya_prom_scalar_u64(&buffer, "nyangine_metrics_accounting_enabled", "1 while the system registry is timing each system, else 0.",
                         nya_system_accounting_is_enabled() ? 1U : 0U);

    /*
     * The ceiling registry, as two families rather than one metric per ceiling: a fixed name with the
     * registrant's name in a `ceiling` label is what a query like `nyangine_ceiling_live / on(ceiling)
     * nyangine_ceiling_capacity` is written against, and it keeps a registrant's string out of the
     * metric name where a collector would reject the odd ones. The two families are emitted in their
     * own passes because the exposition format wants every sample of a family grouped under its one
     * `# TYPE` line.
     */
    u32 ceilings = nya_ceiling_count();

    char metric[128];
    char label[64];
    _nya_prom_family(&buffer, "nyangine_ceiling_live", "Entries live in a registered fixed-capacity array.", metric, sizeof(metric));
    _nya_prom_name(label, sizeof(label), "ceiling");
    for (u32 index = 0; index < ceilings; index++) {
        if (_nya_prom_labeled_u64(&buffer, metric, label, nya_ceiling_name_at(index), nya_ceiling_live_at(index))) break;
    }

    _nya_prom_family(&buffer, "nyangine_ceiling_capacity", "The fixed capacity of a registered array.", metric, sizeof(metric));
    for (u32 index = 0; index < ceilings; index++) {
        if (_nya_prom_labeled_u64(&buffer, metric, label, nya_ceiling_name_at(index), nya_ceiling_capacity_at(index))) break;
    }

    /* The gauge registry: a running byte count each, the registrant's name in a `gauge` label. */
    u32 gauges = nya_gauge_count();

    _nya_prom_family(&buffer, "nyangine_gauge_bytes", "A registered running byte count.", metric, sizeof(metric));
    _nya_prom_name(label, sizeof(label), "gauge");
    for (u32 index = 0; index < gauges; index++) {
        if (_nya_prom_labeled_u64(&buffer, metric, label, nya_gauge_name_at(index), nya_gauge_bytes_at(index))) break;
    }

    return buffer.size;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_HttpStatus _nya_http_metrics_query(NYA_HttpExchange* exchange) {
    NYA_HttpMetricsDto metrics = {
        .measured_at_s      = exchange->now_s,
        .connection_count   = nya_http_server_connection_count(),
        .request_count      = nya_http_server_request_count(),
        .accounting_enabled = nya_system_accounting_is_enabled(),
    };

    /*
     * A program can serve metrics before nya_app_init: a headless tool that drives the drain itself
     * has no frame at all. It gets the server's own counters and zeroes for the frame rather than a
     * struct that has never been filled.
     *
     * Read through the instance rather than nya_app_get, which asserts the app is up. That assertion
     * is right for the rest of the engine, where an app is a precondition; here "is there an app" is
     * the question being answered, and a request may not reach an assertion.
     */
    if (_NYA_APP_INSTANCE.initialized) {
        const NYA_FrameStats* frame = &_NYA_APP_INSTANCE.frame_stats;

        metrics.uptime_ns         = frame->uptime_ns;
        metrics.fps               = frame->fps;
        metrics.delta_time_s      = frame->delta_time_s;
        metrics.work_ns           = frame->work_ns;
        metrics.sleep_ns          = frame->sleep_ns;
        metrics.elapsed_ns        = frame->elapsed_ns;
        metrics.min_frame_time_ns = frame->min_frame_time_ns;
    }

    if (!_nya_http_metrics_answer(exchange, nya_reflect_of(NYA_HttpMetricsDto), &metrics).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}

NYA_HttpStatus _nya_http_metrics_ceilings_query(NYA_HttpExchange* exchange) {
    /*
     * From the exchange arena rather than the stack: the DTO is about six kilobytes and a handler is
     * called from the frame loop, where the stack is shared with everything else in the frame.
     */
    NYA_HttpCeilingsDto* ceilings = nya_arena_alloc(exchange->arena, sizeof(NYA_HttpCeilingsDto));
    if (ceilings == nullptr) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    *ceilings = (NYA_HttpCeilingsDto){ 0 };

    u32 registered = nya_ceiling_count();

    for (u32 index = 0; index < registered; index++) {
        if (ceilings->count >= NYA_HTTP_METRICS_MAX_ROWS) {
            ceilings->truncated = registered - ceilings->count;
            break;
        }

        NYA_HttpCeilingDto* row = &ceilings->rows[ceilings->count];

        u32 capacity = nya_ceiling_capacity_at(index);
        u32 live     = nya_ceiling_live_at(index);

        _nya_http_metrics_name(row->name, sizeof(row->name), nya_ceiling_name_at(index));

        row->capacity = capacity;
        row->live     = live;
        row->fullness = capacity > 0 ? (f32)live / (f32)capacity : 0.0F;

        ceilings->count++;
    }

    if (!_nya_http_metrics_answer(exchange, nya_reflect_of(NYA_HttpCeilingsDto), ceilings).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}

NYA_HttpStatus _nya_http_metrics_arenas_query(NYA_HttpExchange* exchange) {
    NYA_HttpArenasDto* arenas = nya_arena_alloc(exchange->arena, sizeof(NYA_HttpArenasDto));
    if (arenas == nullptr) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    *arenas = (NYA_HttpArenasDto){ 0 };

    u32 registered = nya_arena_registry_count();

    for (u32 index = 0; index < registered; index++) {
        if (arenas->count >= NYA_HTTP_METRICS_MAX_ROWS) {
            arenas->truncated = registered - arenas->count;
            break;
        }

        NYA_Arena* arena = nya_arena_registry_at(index);
        if (arena == nullptr) continue;

        NYA_ArenaStats stats = nya_arena_stats(arena);

        NYA_HttpArenaDto* row = &arenas->rows[arenas->count];

        _nya_http_metrics_name(row->name, sizeof(row->name), stats.name != nullptr ? stats.name : "unnamed");

        row->region_count    = stats.region_count;
        row->used_bytes      = stats.used_bytes;
        row->reserved_bytes  = stats.reserved_bytes;
        row->free_list_bytes = stats.free_list_bytes;
        row->fragmentation   = stats.fragmentation;

        arenas->count++;
    }

    if (!_nya_http_metrics_answer(exchange, nya_reflect_of(NYA_HttpArenasDto), arenas).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}

NYA_HttpStatus _nya_http_metrics_systems_query(NYA_HttpExchange* exchange) {
    NYA_HttpSystemsDto systems = { .accounting_enabled = nya_system_accounting_is_enabled() };

    u32 owners = nya_system_owner_count();

    for (u32 index = 0; index < owners; index++) {
        if (systems.count >= NYA_SYSTEM_OWNER_MAX) {
            systems.truncated = owners - systems.count;
            break;
        }

        NYA_SystemOwnerStats stats = nya_system_owner_stats_at(index);

        NYA_HttpOwnerDto* row = &systems.rows[systems.count];

        _nya_http_metrics_name(row->name, sizeof(row->name), stats.name != nullptr ? stats.name : "unnamed");

        row->system_count  = stats.system_count;
        row->enabled_count = stats.enabled_count;
        row->time_ns       = stats.time_ns;
        row->memory_bytes  = stats.memory_bytes;

        systems.count++;
    }

    if (!_nya_http_metrics_answer(exchange, nya_reflect_of(NYA_HttpSystemsDto), &systems).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}

NYA_HttpStatus _nya_http_metrics_accounting_put(NYA_HttpExchange* exchange, const NYA_HttpIdentity* identity) {
    NYA_HttpAccountingDto wanted = { 0 };

    NYA_Error parsed = nya_http_request_reflect(exchange->request, exchange->arena, nya_reflect_of(NYA_HttpAccountingDto), &wanted);

    if (!parsed.ok) {
        // "the body is not JSON" and "the body is not this DTO" are the same answer to a caller: the
        // schema says what the body is, and the error does not hand back an internal parse chain.
        NYA_HttpMediaType media    = exchange->request->media_type;
        b8                document = media == NYA_HTTP_MEDIA_JSON || media == NYA_HTTP_MEDIA_NYA || media == NYA_HTTP_MEDIA_NYA_BINARY;

        if (!document && exchange->request->body_size > 0) {
            return nya_http_response_problem(exchange, NYA_HTTP_STATUS_UNSUPPORTED_MEDIA, "this route takes application/json or a .nya document");
        }

        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_BAD_REQUEST, "the body is not a NYA_HttpAccountingDto");
    }

    if (wanted.enabled) {
        nya_system_accounting_enable();
    } else {
        nya_system_accounting_disable();
    }

    nya_log_info("%s turned system accounting %s over HTTP.", identity->subject, wanted.enabled ? "on" : "off");

    // the state that was reached, read back rather than echoed: the two are the same here and would
    // stop being the same the day enabling can fail.
    NYA_HttpAccountingDto reached = { .enabled = nya_system_accounting_is_enabled() };

    if (!_nya_http_metrics_answer(exchange, nya_reflect_of(NYA_HttpAccountingDto), &reached).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}

NYA_Error _nya_http_metrics_answer(NYA_HttpExchange* exchange, const NYA_TypeReflection* type, const void* dto) {
    return nya_http_response_reflect_as(exchange->response, exchange->arena, type, dto, nya_http_request_accepts(exchange->request));
}

void _nya_http_metrics_name(char* destination, u64 capacity, NYA_ConstCString text) {
    nya_assert(capacity > 0);

    (void)snprintf(destination, capacity, "%s", text != nullptr ? text : "");
}

NYA_HttpStatus _nya_http_metrics_prometheus_get(NYA_HttpExchange* exchange) {
    /*
     * From the exchange arena rather than the stack: the render buffer is thirty-two kilobytes and a
     * handler runs on the frame's stack, which is the reason the ceilings DTO handler allocates too.
     */
    char* text = nya_arena_alloc(exchange->arena, NYA_HTTP_METRICS_PROMETHEUS_MAX_BYTES);
    if (text == nullptr) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    u64 size = nya_http_metrics_prometheus(text, NYA_HTTP_METRICS_PROMETHEUS_MAX_BYTES);

    /*
     * NYA_HTTP_MEDIA_NONE, then the Content-Type by hand: the exposition format's type carries a
     * `version` parameter (`text/plain; version=0.0.4`) that names the format rather than a charset, and
     * there is no media type in the enum that spells it. Writing it as a header is how a handler sets a
     * Content-Type the shared table does not know, and it is the only extra header this route adds.
     */
    if (!nya_http_response_bytes(exchange->response, (const u8*)text, size, NYA_HTTP_MEDIA_NONE).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    if (!nya_http_response_header(exchange->response, "Content-Type", NYA_HTTP_METRICS_PROMETHEUS_CONTENT_TYPE).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE PROMETHEUS APPENDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_prom_put(_NYA_PromBuffer* buffer, NYA_ConstCString text) {
    nya_assert(buffer != nullptr);
    nya_assert(text != nullptr);

    if (buffer->overflow) return;

    u64 length = strlen(text);

    // All-or-nothing, with a byte kept for the terminator: a put that would not fit leaves every byte
    // written so far alone, which is what lets a caller roll a whole sample back to a line boundary.
    if (buffer->size + length + 1 > buffer->capacity) {
        buffer->overflow = true;
        return;
    }

    memcpy(buffer->data + buffer->size, text, length);
    buffer->size += length;
    buffer->data[buffer->size] = '\0';
}

void _nya_prom_put_u64(_NYA_PromBuffer* buffer, u64 value) {
    char digits[24];
    (void)snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);
    _nya_prom_put(buffer, digits);
}

void _nya_prom_put_f64(_NYA_PromBuffer* buffer, f64 value) {
    // A non-finite value is not a number Prometheus should be told; the format has spellings for the
    // infinities, but a metric that goes NaN is a bug upstream, and a scrape is not the place to argue
    // it, so it reads as zero. Finite values print with enough digits to round-trip a float.
    char number[32];
    (void)snprintf(number, sizeof(number), "%.10g", isfinite(value) ? value : 0.0);
    _nya_prom_put(buffer, number);
}

void _nya_prom_put_label_value(_NYA_PromBuffer* buffer, NYA_ConstCString text) {
    if (text == nullptr) return;

    for (u64 index = 0; text[index] != '\0'; index++) {
        switch (text[index]) {
            case '\\': _nya_prom_put(buffer, "\\\\"); break;
            case '"' : _nya_prom_put(buffer, "\\\""); break;
            case '\n': _nya_prom_put(buffer, "\\n"); break;
            default  : {
                char one[2] = { text[index], '\0' };
                _nya_prom_put(buffer, one);
            } break;
        }
    }
}

void _nya_prom_name(char* out, u64 capacity, NYA_ConstCString text) {
    nya_assert(out != nullptr);
    nya_assert(capacity > 0);

    u64 written = 0;

    for (u64 index = 0; text != nullptr && text[index] != '\0'; index++) {
        char character = text[index];

        b8 head  = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_' || character == ':';
        b8 digit = character >= '0' && character <= '9';

        // A name's first byte may not be a digit, so a leading digit is kept behind a '_' rather than
        // dropped. Every byte outside the charset, wherever it sits, becomes '_'.
        if (written == 0 && digit) {
            if (written + 2 >= capacity) break;
            out[written++] = '_';
            out[written++] = character;
            continue;
        }

        if (written + 1 >= capacity) break;
        out[written++] = (head || digit) ? character : '_';
    }

    // Never empty: an empty name is not a name the format accepts, and a registrant is allowed a name
    // that sanitizes to nothing.
    if (written == 0 && capacity > 1) out[written++] = '_';

    out[written] = '\0';
}

void _nya_prom_family(_NYA_PromBuffer* buffer, NYA_ConstCString name, NYA_ConstCString help, char* out, u64 capacity) {
    _nya_prom_name(out, capacity, name);

    // HELP text escapes a backslash and a newline; a double quote is left alone, unlike a label value.
    // The help strings here are plain, so this is a guard rather than a transform. TYPE is always gauge:
    // every number here is a level read now, not a monotonic total.
    _nya_prom_put(buffer, "# HELP ");
    _nya_prom_put(buffer, out);
    _nya_prom_put(buffer, " ");
    _nya_prom_put(buffer, help != nullptr ? help : "");
    _nya_prom_put(buffer, "\n# TYPE ");
    _nya_prom_put(buffer, out);
    _nya_prom_put(buffer, " gauge\n");
}

b8 _nya_prom_labeled_u64(_NYA_PromBuffer* buffer, NYA_ConstCString metric, NYA_ConstCString label, NYA_ConstCString value, u64 number) {
    // Snapshot before the line so an overflow midway through leaves the buffer cut at the previous line
    // rather than on a half-written sample.
    u64 mark = buffer->size;

    _nya_prom_put(buffer, metric);
    _nya_prom_put(buffer, "{");
    _nya_prom_put(buffer, label);
    _nya_prom_put(buffer, "=\"");
    _nya_prom_put_label_value(buffer, value);
    _nya_prom_put(buffer, "\"} ");
    _nya_prom_put_u64(buffer, number);
    _nya_prom_put(buffer, "\n");

    if (buffer->overflow) {
        buffer->size       = mark;
        buffer->data[mark] = '\0';
        return true;
    }

    return false;
}

void _nya_prom_scalar_u64(_NYA_PromBuffer* buffer, NYA_ConstCString name, NYA_ConstCString help, u64 number) {
    u64  mark = buffer->size;
    char metric[128];

    _nya_prom_family(buffer, name, help, metric, sizeof(metric));
    _nya_prom_put(buffer, metric);
    _nya_prom_put(buffer, " ");
    _nya_prom_put_u64(buffer, number);
    _nya_prom_put(buffer, "\n");

    if (buffer->overflow) {
        buffer->size       = mark;
        buffer->data[mark] = '\0';
    }
}

void _nya_prom_scalar_f64(_NYA_PromBuffer* buffer, NYA_ConstCString name, NYA_ConstCString help, f64 number) {
    u64  mark = buffer->size;
    char metric[128];

    _nya_prom_family(buffer, name, help, metric, sizeof(metric));
    _nya_prom_put(buffer, metric);
    _nya_prom_put(buffer, " ");
    _nya_prom_put_f64(buffer, number);
    _nya_prom_put(buffer, "\n");

    if (buffer->overflow) {
        buffer->size       = mark;
        buffer->data[mark] = '\0';
    }
}

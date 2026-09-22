#include <stdio.h>

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_ceiling.h"
#include "nyangine/core/core_app.h"
#include "nyangine/debug/debug_metrics.h"
#include "nyangine/http/http_server.h"
#include "nyangine/base/base_clock.h"

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

/** Copies `text` into a row's fixed name, truncating rather than refusing: a long name is not an error. */
NYA_INTERNAL void _nya_http_metrics_name(OUT char* destination, u64 capacity, NYA_ConstCString text);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL const NYA_HttpRoute _NYA_HTTP_METRICS_ROUTES[] = {
    {
     .method        = NYA_HTTP_METHOD_QUERY,
     .path          = NYA_HTTP_METRICS_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
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

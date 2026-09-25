#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-core/http/http_health.h"
#include "nyangine-core/http/http_message.h"

// PRIVATE TYPES

/** One registered readiness check: what it is called, and the function and state that decide it. */
typedef struct {
    char            name[NYA_HTTP_HEALTH_MAX_NAME];
    NYA_HttpReadyFn check;
    void*           user;
} _NYA_HttpHealthCheck;

// PRIVATE API DECLARATION

NYA_INTERNAL NYA_HttpStatus _nya_http_healthz(NYA_HttpExchange* exchange);
NYA_INTERNAL NYA_HttpStatus _nya_http_readyz(NYA_HttpExchange* exchange);

// STATE

// The registry, filled at startup and read by the /readyz route after (see the file note on when registering is safe); a fixed table since everything here is bounded and readiness has a handful of dependencies at most.
static _NYA_HttpHealthCheck _CHECKS[NYA_HTTP_HEALTH_MAX_CHECKS];
static u32                  _CHECK_COUNT = 0;

// ROUTES

// Liveness is NYA_HTTP_AFFINITY_WORKER: it reads nothing, so any worker answers it without waiting on the frame — a 200 as long as the process can answer at all. Readiness is NYA_HTTP_AFFINITY_MAIN: its checks read the program's own state (a database handle, a breaker), answered inside nya_system_http_tick at up to one frame of latency.
NYA_INTERNAL const NYA_HttpRoute _NYA_HTTP_HEALTH_ROUTES[] = {
    {
     .method        = NYA_HTTP_METHOD_GET,
     .path          = NYA_HTTP_HEALTHZ_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
     .handler       = _nya_http_healthz,
     .summary       = "Liveness: the process is up",
     .description   = "Depends on nothing and is always 200 when it can answer at all. A probe that gets no answer, not a "
                        "503, is what says this process should be restarted; readiness is the route that goes 503.",
     .response_type = nya_reflect_of(NYA_HttpHealthDto),
     .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method        = NYA_HTTP_METHOD_GET,
     .path          = NYA_HTTP_READYZ_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
     .affinity      = NYA_HTTP_AFFINITY_MAIN,
     .handler       = _nya_http_readyz,
     .summary       = "Readiness: every declared dependency is ready",
     .description   = "Runs the registered readiness checks. 200 when all pass, 503 when any fail; the body names each check "
                        "and its verdict both ways, so a 503 says which dependency is down. See nya_http_health_check_register.",
     .response_type = nya_reflect_of(NYA_HttpReadinessDto),
     .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

NYA_INTERNAL const NYA_HttpRouter _NYA_HTTP_HEALTH_ROUTER = {
    .name        = "health",
    .routes      = _NYA_HTTP_HEALTH_ROUTES,
    .route_count = nya_carray_length(_NYA_HTTP_HEALTH_ROUTES),
};

// PUBLIC API IMPLEMENTATION

NYA_Error nya_http_health_check_register(NYA_ConstCString name, NYA_HttpReadyFn check, void* user) {
    if (name == nullptr || name[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a readiness check needs a name");
    if (check == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a readiness check needs a function");

    if (strlen(name) >= NYA_HTTP_HEALTH_MAX_NAME) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the check name '%s' is longer than %d bytes", name, NYA_HTTP_HEALTH_MAX_NAME - 1);
    }

    if (_CHECK_COUNT >= NYA_HTTP_HEALTH_MAX_CHECKS) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for more than %d readiness checks", NYA_HTTP_HEALTH_MAX_CHECKS);
    }

    _NYA_HttpHealthCheck* slot = &_CHECKS[_CHECK_COUNT];

    (void)snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->check = check;
    slot->user  = user;

    _CHECK_COUNT += 1;

    return NYA_OK;
}

void nya_http_health_checks_clear(void) {
    nya_memset(_CHECKS, 0, sizeof(_CHECKS));
    _CHECK_COUNT = 0;
}

u32 nya_http_health_check_count(void) {
    return _CHECK_COUNT;
}

b8 nya_http_health_circuit_ready(void* user) {
    const NYA_HttpHealthCircuit* circuit = user;

    // No breaker is not-ready rather than a crash: a misconfigured check should take the route to 503, not the process down.
    if (circuit == nullptr || circuit->breaker == nullptr) return false;

    // OPEN is the breaker failing calls fast because the dependency is down — "not ready to serve"; CLOSED and HALF_OPEN both let calls through, so both read as ready.
    return nya_circuit_state(circuit->breaker, circuit->key) != NYA_CIRCUIT_OPEN;
}

const NYA_HttpRouter* nya_http_health_router(void) {
    return &_NYA_HTTP_HEALTH_ROUTER;
}

// PRIVATE API IMPLEMENTATION

NYA_HttpStatus _nya_http_healthz(NYA_HttpExchange* exchange) {
    NYA_HttpHealthDto health = { 0 };
    (void)snprintf(health.status, sizeof(health.status), "ok");

    NYA_Error written = nya_http_response_reflect_as(
        exchange->response, exchange->arena, nya_reflect_of(NYA_HttpHealthDto), &health, nya_http_request_accepts(exchange->request)
    );

    if (!written.ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_OK;
}

NYA_HttpStatus _nya_http_readyz(NYA_HttpExchange* exchange) {
    NYA_HttpReadinessDto readiness = { .ready = true };

    // The count is bounded at register, so this can only be the table's length; the clamp is a belt over those braces so the fixed array is never overrun.
    u32 count = _CHECK_COUNT < NYA_HTTP_HEALTH_MAX_CHECKS ? _CHECK_COUNT : (u32)NYA_HTTP_HEALTH_MAX_CHECKS;

    readiness.count = count;

    for (u32 i = 0; i < count; i++) {
        b8 ready = _CHECKS[i].check != nullptr && _CHECKS[i].check(_CHECKS[i].user);

        (void)snprintf(readiness.checks[i].name, sizeof(readiness.checks[i].name), "%s", _CHECKS[i].name);
        readiness.checks[i].ready = ready;

        if (!ready) {
            readiness.ready = false;
            readiness.failed += 1;
        }
    }

    NYA_Error written = nya_http_response_reflect_as(
        exchange->response, exchange->arena, nya_reflect_of(NYA_HttpReadinessDto), &readiness, nya_http_request_accepts(exchange->request)
    );

    // The body couldn't be written: a server fault, not a readiness verdict, so a 500 rather than a 503 that would read as "not ready" when the truth is "couldn't say".
    if (!written.ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // The status carries the same verdict as the body: 200 when everything passed, 503 when anything didn't, which takes this instance out of rotation without killing it.
    return readiness.ready ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_SERVICE_UNAVAILABLE;
}

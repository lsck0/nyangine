/**
 * @file http_health.h
 *
 * Two routes an orchestrator polls: is this process up, and is it ready to serve.
 *
 * ```
 * nya_http_health_router          the router. Merge it and /healthz and /readyz are online
 * nya_http_health_check_register  add a named readiness check the /readyz route runs
 * nya_http_health_checks_clear    forget every registered check
 * nya_http_health_check_count     how many are registered, for a test or a ceiling audit
 * nya_http_health_circuit_ready   a ready-check that reads a base_circuit breaker: OPEN is not ready
 * ```
 *
 * ```
 * GET /healthz    liveness: the process is up. Always 200 when it can answer at all
 * GET /readyz     readiness: every declared dependency is ready. 200 when all pass, 503 when any fail
 * ```
 *
 * ```c
 * NYA_EXPECT(nya_http_health_check_register("db", db_ready, my_db));
 * NYA_EXPECT(nya_http_server_merge(nya_http_health_router()));
 * ```
 *
 * ── why two routes and not one ──
 *
 * They answer different questions and a caller does different things with the answers. Liveness is
 * "should this process be restarted": a 200 means the loop is running and can answer, and the only
 * honest failure is no answer at all, so /healthz depends on nothing and is always 200 when reached.
 * Readiness is "should traffic be sent here now": a process that is up but whose database is still
 * opening is alive and not ready, and answering 200 to readiness there would send it work it drops. So
 * /readyz runs the checks and answers 503 while any of them is not ready, which is what takes a pod out
 * of a load balancer's rotation without killing it.
 *
 * ── the checks are a small registry, composed by the program ──
 *
 * A readiness check is a function and a name the program registers: "db", "keyring", an upstream. The
 * route runs all of them and answers 200 only if every one passes; a 503 body names which failed, so
 * the answer says what is wrong rather than only that something is. The engine ships none of them,
 * because what "ready" means is the program's — the same reason the metrics resource measures nothing
 * of its own. A breaker composes straight in: nya_http_health_circuit_ready reads a base_circuit
 * breaker and reports OPEN as not-ready, so a dependency the program already fails fast on shows up in
 * readiness without a second source of truth.
 *
 * ── thread safety ──
 *
 * The registry is filled at startup, before the server serves, and read by the route after. Registering
 * a check while the server is running is not safe, which is the same contract nya_http_server_merge has
 * and for the same reason: the route table is set up once and read many times. The /readyz route is
 * NYA_HTTP_AFFINITY_MAIN so the check functions run where a program's own state lives; see http_router.h.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_circuit.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/http/http_router.h"

// CONSTANTS

#define NYA_HTTP_HEALTHZ_PATH "/healthz"
#define NYA_HTTP_READYZ_PATH  "/readyz"

/**
 * Readiness checks one server declares. A program depends on a handful of things — a database, a
 * keyring, one or two upstreams — and this is well past that. Past it a register is refused rather than
 * silently dropped.
 * */
#define NYA_HTTP_HEALTH_MAX_CHECKS 16

/** Longest check name, terminator included. A dependency's name — "db", "keyring" — fits easily. */
#define NYA_HTTP_HEALTH_MAX_NAME 32

// TYPES

typedef struct NYA_HttpHealthDto      NYA_HttpHealthDto;
typedef struct NYA_HttpReadyCheckDto  NYA_HttpReadyCheckDto;
typedef struct NYA_HttpReadinessDto   NYA_HttpReadinessDto;
typedef struct NYA_HttpHealthCircuit  NYA_HttpHealthCircuit;

/**
 * A readiness check: true when the dependency it stands for is ready to serve. Given the `user` pointer
 * it was registered with, so one function can back several checks over different state.
 *
 * It runs inside the /readyz request, on the main thread, so it does what a NYA_HTTP_AFFINITY_MAIN
 * handler may: read the program's own state, cheaply. It must not block — a check that waits on a
 * network round trip turns a readiness poll into a request that can hang.
 * */
typedef b8 (*NYA_HttpReadyFn)(void* user);

// @reflect
/** What GET /healthz answers: liveness, which is always the same word when the process can answer. */
struct NYA_HttpHealthDto {
    /** "ok". A field rather than an empty body so a caller parses one shape here and at /readyz. */
    char status[16];
};

// @reflect
/** One readiness check's verdict, in the /readyz answer. */
struct NYA_HttpReadyCheckDto {
    char name[NYA_HTTP_HEALTH_MAX_NAME];
    b8   ready;
};

// @reflect
/**
 * What GET /readyz answers, whether it is a 200 or a 503: the same body both ways, so a caller reads
 * which dependency is down rather than only that readiness failed.
 * */
struct NYA_HttpReadinessDto {
    /** True only when every check passed, which is the same thing the 200-versus-503 status says. */
    b8 ready;

    /** How many checks ran, which is how many of `checks` mean anything. */
    u32 count;

    /** How many of them were not ready. Zero exactly when `ready`. */
    u32 failed;

    NYA_HttpReadyCheckDto checks[NYA_HTTP_HEALTH_MAX_CHECKS];
};

/**
 * The user data nya_http_health_circuit_ready reads: a breaker and the key to ask it about. Held by the
 * program for as long as the check is registered, since the check keeps the pointer.
 * */
struct NYA_HttpHealthCircuit {
    NYA_CircuitBreaker* breaker;
    NYA_ConstCString    key;
};

// FUNCTIONS

/**
 * Adds a readiness check the /readyz route runs. `name` is copied; `check` is called with `user` on
 * every readiness request.
 *
 * NYA_ERROR_INVALID_ARGUMENT for a null or empty name, a name that does not fit, or a null function;
 * NYA_ERROR_OUT_OF_MEMORY once NYA_HTTP_HEALTH_MAX_CHECKS are registered. Register before the server
 * serves; see the file note on when this is safe.
 * */
NYA_API NYA_Error nya_http_health_check_register(NYA_ConstCString name, NYA_HttpReadyFn check, void* user);

/** Forgets every registered check, so /readyz passes on nothing. Mostly for a test between cases. */
NYA_API void nya_http_health_checks_clear(void);

/** How many checks are registered. */
NYA_API u32 nya_http_health_check_count(void) __attr_no_discard;

/**
 * A readiness check over a base_circuit breaker: not ready exactly when the breaker for its key is OPEN,
 * which is the breaker saying the dependency is down and calls are being failed fast. `user` is a
 * NYA_HttpHealthCircuit the program owns.
 * */
NYA_API b8 nya_http_health_circuit_ready(void* user);

/** Static storage, so it outlives any mount and needs no lifetime from the caller. */
NYA_API const NYA_HttpRouter* nya_http_health_router(void) __attr_no_discard;

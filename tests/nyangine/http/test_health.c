/**
 * The liveness and readiness routes: /healthz is always 200, /readyz is 200 only while every registered
 * check passes and 503 the moment one does not.
 *
 * Driven through nya_http_router_dispatch, like test_router.c: a route table and an exchange are data
 * and need no socket. A check whose verdict a static bool controls is what flips readiness back and
 * forth, and a base_circuit breaker is registered too, to show the tie-in — an OPEN breaker reads as
 * not-ready.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#define NOW_S 1700000000ULL

/* The controllable check: /readyz is ready exactly when this is true. */
static b8 CHECK_OK = true;

static b8 controllable_ready(void* user) {
    nya_unused(user);
    return CHECK_OK;
}

static b8 always_ready(void* user) {
    nya_unused(user);
    return true;
}

/** Builds a bare GET request for `path`, as the parser would leave it. From test_router.c. */
static void make_get(OUT NYA_HttpRequest* request, NYA_ConstCString path) {
    *request = (NYA_HttpRequest){ .method = NYA_HTTP_METHOD_GET, .keep_alive = true };

    NYA_UrlFailure failure = { 0 };
    NYA_EXPECT(nya_url_parse_target(path, strlen(path), &request->target, &failure), "while building a request");

    (void)snprintf(request->path, sizeof(request->path), "%.*s", (int)request->target.path.length, request->target.text + request->target.path.offset);
}

/** One dispatch through the health router. */
static NYA_HttpStatus dispatch(NYA_Arena* arena, const NYA_HttpRequest* request, NYA_HttpResponse* response) {
    const NYA_HttpRouter* routers[] = { nya_http_health_router() };

    NYA_HttpExchange exchange = {
        .request  = request,
        .response = response,
        .arena    = arena,
        .now_s    = NOW_S,
    };

    nya_http_response_reset(response);
    nya_memset(response->body, 0, response->body_capacity);

    return nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), nullptr, 0);
}

/** Whether the readiness body parses and its `ready` field is what was expected. */
static void assert_body_ready(NYA_Arena* arena, const NYA_HttpResponse* response, b8 expected) {
    NYA_Object* body = nullptr;
    nya_assert(nya_deserialize(arena, response->body, response->body_size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &body).ok, "the readiness body is json");

    NYA_Value* ready = nya_object_get(body, "ready");
    nya_assert(ready != nullptr && ready->type == NYA_TYPE_B8, "the body carries a ready flag");
    nya_assert(ready->as_b8 == expected, "the body's ready flag matches the status");
}

int main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_health");
    defer      nya_arena_destroy(arena);

    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
    nya_assert(request != nullptr);

    u8 body[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };

    NYA_HttpResponse response = { 0 };
    nya_http_response_create(&response, body, sizeof(body));
    defer nya_http_response_destroy(&response);

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the table is one the server will serve.
    // ─────────────────────────────────────────────────────────────────────────────
    nya_assert(nya_http_router_check(nya_http_health_router()).ok, "the health routes are well formed");

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: liveness is 200 and depends on nothing, even with a failing readiness check registered.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        nya_http_health_checks_clear();
        CHECK_OK = false;
        nya_assert(nya_http_health_check_register("always-fails", controllable_ready, nullptr).ok);

        make_get(request, NYA_HTTP_HEALTHZ_PATH);
        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "liveness ignores readiness checks");

        nya_http_health_checks_clear();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: readiness with no checks is ready.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        nya_http_health_checks_clear();
        nya_assert(nya_http_health_check_count() == 0);

        make_get(request, NYA_HTTP_READYZ_PATH);
        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "nothing to be not-ready about");
        assert_body_ready(arena, &response, true);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: readiness flips 200 ↔ 503 as a check passes and fails.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        nya_http_health_checks_clear();
        nya_assert(nya_http_health_check_register("db", controllable_ready, nullptr).ok);
        nya_assert(nya_http_health_check_count() == 1);

        make_get(request, NYA_HTTP_READYZ_PATH);

        CHECK_OK = true;
        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "the check passes, so ready");
        assert_body_ready(arena, &response, true);

        CHECK_OK = false;
        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "the check fails, so not ready");
        assert_body_ready(arena, &response, false);

        CHECK_OK = true;
        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "and back to ready when it passes again");
        assert_body_ready(arena, &response, true);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: one failing check among several passing ones is enough for a 503.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        nya_http_health_checks_clear();
        nya_assert(nya_http_health_check_register("keyring", always_ready, nullptr).ok);
        nya_assert(nya_http_health_check_register("db", controllable_ready, nullptr).ok);

        make_get(request, NYA_HTTP_READYZ_PATH);

        CHECK_OK = false;
        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "all must pass, not just one");
        assert_body_ready(arena, &response, false);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a base_circuit breaker composes in — OPEN reads as not-ready.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_CircuitBreaker* breaker = nullptr;
        nya_assert(nya_circuit_breaker_create(arena, &breaker, .failure_threshold = 2, .open_ms = 60000).ok);

        NYA_HttpHealthCircuit circuit = { .breaker = breaker, .key = "upstream" };

        nya_http_health_checks_clear();
        nya_assert(nya_http_health_check_register("upstream", nya_http_health_circuit_ready, &circuit).ok);

        make_get(request, NYA_HTTP_READYZ_PATH);

        // A fresh breaker is CLOSED, which lets calls through, which is ready.
        nya_assert(nya_circuit_state(breaker, "upstream") == NYA_CIRCUIT_CLOSED);
        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "a closed breaker is ready");

        // Two failures in a row trip it OPEN, and an OPEN breaker is fail-fast, which is not-ready. Each
        // record pairs with an allow, which is the contract base_circuit.h states and what creates the
        // key's entry in the first place.
        nya_assert(nya_circuit_allow(breaker, "upstream"), "a closed breaker lets the call through");
        nya_circuit_record(breaker, "upstream", false);
        nya_assert(nya_circuit_allow(breaker, "upstream"), "still closed after one failure");
        nya_circuit_record(breaker, "upstream", false);
        nya_assert(nya_circuit_state(breaker, "upstream") == NYA_CIRCUIT_OPEN, "the threshold-th failure trips it");

        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "an open breaker is not ready");
        assert_body_ready(arena, &response, false);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the registry refuses what it cannot hold.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        nya_http_health_checks_clear();

        nya_assert(!nya_http_health_check_register(nullptr, always_ready, nullptr).ok, "a check needs a name");
        nya_assert(!nya_http_health_check_register("", always_ready, nullptr).ok, "an empty name is no name");
        nya_assert(!nya_http_health_check_register("x", nullptr, nullptr).ok, "a check needs a function");

        char too_long[NYA_HTTP_HEALTH_MAX_NAME + 4];
        nya_memset(too_long, 'a', sizeof(too_long));
        too_long[sizeof(too_long) - 1] = '\0';
        nya_assert(!nya_http_health_check_register(too_long, always_ready, nullptr).ok, "a name that does not fit is refused, not truncated");

        nya_assert(nya_http_health_check_count() == 0, "nothing refused was kept");

        // Fill it to the brim, then one past.
        for (u32 i = 0; i < NYA_HTTP_HEALTH_MAX_CHECKS; i++) {
            nya_assert(nya_http_health_check_register("full", always_ready, nullptr).ok, "there is room for %u", i);
        }
        nya_assert(nya_http_health_check_count() == NYA_HTTP_HEALTH_MAX_CHECKS);
        nya_assert(!nya_http_health_check_register("one-too-many", always_ready, nullptr).ok, "the table is bounded");

        nya_http_health_checks_clear();
        nya_assert(nya_http_health_check_count() == 0, "clear empties it");
    }

    printf("PASSED: http health\n");

    return EXIT_SUCCESS;
}

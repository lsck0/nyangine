/**
 * Callback-backed route handlers: a route may carry a `handler_callback` token instead of a raw handler
 * pointer, and the router resolves it through the resolver a program installs. The point is code hot
 * reload — a raw pointer baked into the route table dangles when the DLL swaps, a token is re-resolved.
 *
 * Driven through nya_http_router_dispatch, like test_router/test_health: a route table and an exchange are
 * data. A swappable global standing in for the named-callback registry proves the handler that runs
 * changes without the route table changing — exactly what a reload does.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#define TOKEN 7ULL

/* Which handler the token resolves to now; swapped between dispatches to stand in for a reload. */
static NYA_HttpHandlerFn RESOLVED = nullptr;
static char              LAST_RAN = '-';

static NYA_HttpHandlerFn resolve_handler(u64 token) {
    nya_assert(token == TOKEN, "the router passed the route's own token");
    return RESOLVED;
}

static NYA_HttpStatus handler_a(NYA_HttpExchange* exchange) {
    nya_unused(exchange);
    LAST_RAN = 'A';
    return NYA_HTTP_STATUS_OK;
}

static NYA_HttpStatus handler_b(NYA_HttpExchange* exchange) {
    nya_unused(exchange);
    LAST_RAN = 'B';
    return NYA_HTTP_STATUS_OK;
}

static const NYA_HttpRoute ROUTES[] = {
    {
     .method           = NYA_HTTP_METHOD_GET,
     .path             = "/swap",
     .auth             = NYA_HTTP_AUTH_NONE,
     .handler_callback = TOKEN,
     .summary          = "A handler carried as a reload-safe token",
     .statuses         = { NYA_HTTP_STATUS_OK },
     },
};

static const NYA_HttpRouter ROUTER = {
    .name        = "swap",
    .routes      = ROUTES,
    .route_count = nya_carray_length(ROUTES),
};

static void make_get(OUT NYA_HttpRequest* request, NYA_ConstCString path) {
    *request = (NYA_HttpRequest){ .method = NYA_HTTP_METHOD_GET, .keep_alive = true };

    NYA_UrlFailure failure = { 0 };
    NYA_EXPECT(nya_url_parse_target(path, strlen(path), &request->target, &failure), "while building a request");

    (void)snprintf(request->path, sizeof(request->path), "%.*s", (int)request->target.path.length, request->target.text + request->target.path.offset);
}

static NYA_HttpStatus dispatch(NYA_Arena* arena, const NYA_HttpRequest* request, NYA_HttpResponse* response) {
    const NYA_HttpRouter* routers[] = { &ROUTER };

    NYA_HttpExchange exchange = { .request = request, .response = response, .arena = arena, .now_s = 1700000000ULL };

    nya_http_response_reset(response);
    nya_memset(response->body, 0, response->body_capacity);

    return nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), nullptr, 0);
}

int main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_router_callback");
    defer      nya_arena_destroy(arena);

    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
    nya_assert(request != nullptr);

    u8            body[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };
    NYA_HttpResponse response = { 0 };
    nya_http_response_create(&response, body, sizeof(body));
    defer nya_http_response_destroy(&response);

    nya_http_router_resolvers_set(resolve_handler, nullptr);

    // TEST: a route table carrying a token instead of a pointer is well formed.
    nya_check(nya_http_router_check(&ROUTER).ok, "a route with a handler_callback and no handler is valid");

    // TEST: dispatch runs whatever the token resolves to.
    {
        make_get(request, "/swap");

        RESOLVED = handler_a;
        LAST_RAN = '-';
        nya_check(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "the resolved handler answers");
        nya_check(LAST_RAN == 'A', "handler_a ran, got '%c'", LAST_RAN);

        // stand in for a code reload: the same token now resolves to a different function, table untouched.
        RESOLVED = handler_b;
        LAST_RAN = '-';
        nya_check(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "the swapped handler answers");
        nya_check(LAST_RAN == 'B', "handler_b ran after the swap without changing the route, got '%c'", LAST_RAN);
    }

    // TEST: the validator rejects a route that sets both, or neither.
    {
        NYA_HttpRoute both = ROUTES[0];
        both.handler = handler_a;   // token AND pointer
        NYA_HttpRouter both_router = { .name = "both", .routes = &both, .route_count = 1 };
        nya_check(!nya_http_router_check(&both_router).ok, "a route with both a handler and a token is refused");

        NYA_HttpRoute neither      = ROUTES[0];
        neither.handler_callback   = 0;   // token cleared, no pointer either
        NYA_HttpRouter none_router = { .name = "none", .routes = &neither, .route_count = 1 };
        nya_check(!nya_http_router_check(&none_router).ok, "a route with neither is refused");
    }

    printf("test_router_callback: all passed\n");
    return 0;
}

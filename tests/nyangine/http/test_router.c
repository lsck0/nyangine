/**
 * Routing, the layer chain, and the extractor that stands between a request and a handler that takes
 * a caller.
 *
 * Driven through nya_http_router_dispatch rather than through a socket, which is why that function is
 * public: a route table and an exchange are data, and testing them needs no port.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

static const u8 SECRET[] = "0123456789abcdef0123456789abcdef";
#define SECRET_SIZE (sizeof(SECRET) - 1)

#define NOW_S 1700000000ULL

/* What the layers below record, so the order they ran in is checkable afterwards. */
static char ORDER[64]    = { 0 };
static u32  ORDER_LENGTH = 0;

static void record(char mark) {
    if (ORDER_LENGTH + 1 < sizeof(ORDER)) ORDER[ORDER_LENGTH++] = mark;
    ORDER[ORDER_LENGTH] = '\0';
}

/* ── the handlers under test ── */

static NYA_HttpStatus open_query(NYA_HttpExchange* exchange) {
    record('h');

    return nya_http_response_text(exchange->response, "open", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** The verb a browser has. Still routed, still served; it is only not what a new route is written as. */
static NYA_HttpStatus legacy_get(NYA_HttpExchange* exchange) {
    record('g');

    return nya_http_response_text(exchange->response, "legacy", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Takes a caller. There is no way to register this on a route that would not produce one. */
static NYA_HttpStatus closed_put(NYA_HttpExchange* exchange, const NYA_HttpIdentity* identity) {
    record('H');

    nya_assert(identity != nullptr && identity->subject[0] != '\0', "the extractor handed a handler an empty identity");

    return nya_http_response_text(exchange->response, identity->subject, NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK
                                                                                                 : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/* ── the layers ── */

static NYA_HttpStatus outer_layer(NYA_HttpExchange* exchange, NYA_HttpChain* next) {
    record('a');
    NYA_HttpStatus status = nya_http_chain_next(exchange, next);
    record('A');

    return status;
}

static NYA_HttpStatus inner_layer(NYA_HttpExchange* exchange, NYA_HttpChain* next) {
    record('b');
    NYA_HttpStatus status = nya_http_chain_next(exchange, next);
    record('B');

    return status;
}

/** Answers on its own, without running the rest, which is what an authorization layer does. */
static NYA_HttpStatus short_circuit_layer(NYA_HttpExchange* exchange, NYA_HttpChain* next) {
    nya_unused(next);
    record('s');

    return nya_http_response_problem(exchange, NYA_HTTP_STATUS_FORBIDDEN, "the layer said no");
}

/* ── the tables ── */

static const NYA_HttpLayerFn RESOURCE_LAYERS[] = { inner_layer };

static const NYA_HttpRoute ROUTES[] = {
    {
     .method  = NYA_HTTP_METHOD_QUERY,
     .path    = "/api/thing",
     .auth    = NYA_HTTP_AUTH_NONE,
     .handler = open_query,
     .summary = "An open read",
     // 403 because a layer can answer with one; a route declares what its whole chain can produce.
        .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method             = NYA_HTTP_METHOD_PUT,
     .path               = "/api/thing",
     .auth               = NYA_HTTP_AUTH_BEARER,
     .scope              = NYA_HTTP_SCOPE_WRITE,
     .handler_identified = closed_put,
     .summary            = "A route behind the extractor",
     .statuses           = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method   = NYA_HTTP_METHOD_GET,
     .path     = "/api/legacy",
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = legacy_get,
     .summary  = "A read a browser can reach",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

static const NYA_HttpRouter ROUTER = {
    .name        = "thing",
    .routes      = ROUTES,
    .route_count = nya_carray_length(ROUTES),
    .layers      = RESOURCE_LAYERS,
    .layer_count = nya_carray_length(RESOURCE_LAYERS),
};

/** Builds a request by hand: the parser has its own test and this one is about what comes after it. */
static void make_request(OUT NYA_HttpRequest* request, NYA_HttpMethod method, NYA_ConstCString path, NYA_ConstCString authorization) {
    *request = (NYA_HttpRequest){ .method = method, .keep_alive = true };

    (void)snprintf(request->path, sizeof(request->path), "%s", path);

    if (authorization == nullptr) return;

    request->header_count = 1;
    (void)snprintf(request->headers[0].name, sizeof(request->headers[0].name), "authorization");
    (void)snprintf(request->headers[0].value, sizeof(request->headers[0].value), "Bearer %s", authorization);
}

/** One dispatch, with the root layers named. */
static NYA_HttpStatus
dispatch(NYA_Arena* arena, const NYA_HttpRequest* request, NYA_HttpResponse* response, const NYA_HttpLayerFn* layers, u32 layer_count) {
    const NYA_HttpRouter* routers[] = { &ROUTER };

    NYA_HttpExchange exchange = {
        .request     = request,
        .response    = response,
        .arena       = arena,
        .secret      = SECRET,
        .secret_size = SECRET_SIZE,
        .now_s       = NOW_S,
    };

    nya_http_response_reset(response);

    // the body is bytes and carries no terminator, so the buffer is cleared rather than compared as a
    // string against whatever the last answer left in it.
    nya_memset(response->body, 0, response->body_capacity);

    ORDER_LENGTH = 0;
    ORDER[0]     = '\0';

    return nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), layers, layer_count);
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_router");
    defer      nya_arena_destroy(arena);

    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
    nya_assert(request != nullptr);

    u8 body[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };

    NYA_HttpResponse response = { 0 };
    nya_http_response_create(&response, body, sizeof(body));
    defer nya_http_response_destroy(&response);

    const NYA_HttpLayerFn root_layers[] = { outer_layer };

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a well formed table passes the check, and a broken one names its fault.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        nya_assert(nya_http_router_check(&ROUTER).ok);

        nya_assert(!nya_http_router_check(nullptr).ok);

        NYA_HttpRouter unnamed = ROUTER;
        unnamed.name           = nullptr;
        nya_assert(!nya_http_router_check(&unnamed).ok);

        NYA_HttpRouter empty = ROUTER;
        empty.route_count    = 0;
        nya_assert(!nya_http_router_check(&empty).ok);

        // a route that writes can be refused as cross-site, so it has to say it answers 403.
        NYA_HttpRoute  undeclared = { .method = NYA_HTTP_METHOD_POST, .path = "/api/write", .handler = ROUTES[0].handler, .summary = "x",
                                      .statuses = { NYA_HTTP_STATUS_OK } };
        NYA_HttpRouter writer     = { .name = "x", .routes = &undeclared, .route_count = 1 };
        nya_assert(!nya_http_router_check(&writer).ok, "a write route without 403 would describe a refusal it cannot make");

        /*
         * The rule that makes the extractor structural: a handler taking a caller may only sit on a
         * route that demands one, and a route that demands one may only hold that kind of handler.
         */
        NYA_HttpRoute misplaced = ROUTES[1];
        misplaced.auth          = NYA_HTTP_AUTH_NONE;

        NYA_HttpRouter wrong_slot = { .name = "x", .routes = &misplaced, .route_count = 1 };
        nya_assert(!nya_http_router_check(&wrong_slot).ok, "an identified handler on an open route would run unauthenticated");

        NYA_HttpRoute both = ROUTES[0];
        both.auth          = NYA_HTTP_AUTH_BEARER;

        NYA_HttpRouter open_behind_auth = { .name = "x", .routes = &both, .route_count = 1 };
        nya_assert(!nya_http_router_check(&open_behind_auth).ok, "a handler taking no caller behind the extractor would never see one");

        NYA_HttpRoute undocumented = ROUTES[0];
        undocumented.statuses[0]   = NYA_HTTP_STATUS_NONE;

        NYA_HttpRouter silent = { .name = "x", .routes = &undocumented, .route_count = 1 };
        nya_assert(!nya_http_router_check(&silent).ok, "a route that lists no statuses would generate a schema that describes nothing");

        NYA_HttpRoute unsummarised = ROUTES[0];
        unsummarised.summary       = nullptr;

        NYA_HttpRouter mute = { .name = "x", .routes = &unsummarised, .route_count = 1 };
        nya_assert(!nya_http_router_check(&mute).ok);

        NYA_HttpRoute relative = ROUTES[0];
        relative.path          = "api/thing";

        NYA_HttpRouter unanchored = { .name = "x", .routes = &relative, .route_count = 1 };
        nya_assert(!nya_http_router_check(&unanchored).ok);

        // a route behind the extractor has to say it can refuse, since the extractor can.
        NYA_HttpRoute optimistic = ROUTES[1];
        optimistic.statuses[1]   = NYA_HTTP_STATUS_INTERNAL_ERROR;
        optimistic.statuses[2]   = NYA_HTTP_STATUS_NONE;

        NYA_HttpRouter hopeful = { .name = "x", .routes = &optimistic, .route_count = 1 };
        nya_assert(!nya_http_router_check(&hopeful).ok);

        /*
         * The two halves of writing a read as a GET again: a request DTO on a verb whose body the
         * parser refuses, and a verb that changes nothing claiming to have created something.
         */
        NYA_HttpRoute bodiless = ROUTES[2];
        bodiless.request_type  = nya_reflect_of(NYA_HttpAccountingDto);

        NYA_HttpRouter unreachable = { .name = "x", .routes = &bodiless, .route_count = 1 };
        nya_assert(!nya_http_router_check(&unreachable).ok, "a GET taking a body is a route no request can reach");

        NYA_HttpRoute reading = ROUTES[0];
        reading.request_type  = nya_reflect_of(NYA_HttpAccountingDto);

        NYA_HttpRouter parameterised = { .name = "x", .routes = &reading, .route_count = 1 };
        nya_assert(nya_http_router_check(&parameterised).ok, "a QUERY taking a request document is the point of it");

        NYA_HttpRoute creative = ROUTES[0];
        creative.statuses[1]   = NYA_HTTP_STATUS_CREATED;

        NYA_HttpRouter productive = { .name = "x", .routes = &creative, .route_count = 1 };
        nya_assert(!nya_http_router_check(&productive).ok, "a safe verb cannot report having created anything");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: finding a route, and telling 404 from 405.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        const NYA_HttpRouter* routers[] = { &ROUTER };

        b8 exists = false;

        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_QUERY, "/api/thing", &exists) == &ROUTES[0]);
        nya_assert(exists);

        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_PUT, "/api/thing", &exists) == &ROUTES[1]);

        // a HEAD is a read whose body is dropped, so it answers from the read route: the GET where
        // there is one, and the QUERY where there is not.
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_HEAD, "/api/legacy", &exists) == &ROUTES[2]);
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_HEAD, "/api/thing", &exists) == &ROUTES[0]);

        // and never from the write on the same path, whichever order the table is in.
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_HEAD, "/api/thing", &exists) != &ROUTES[1]);

        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_DELETE, "/api/thing", &exists) == nullptr);
        nya_assert(exists, "the path exists, so this is a 405 and not a 404");

        // GET is still routed; it is only not what these routes are written as.
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_GET, "/api/legacy", &exists) == &ROUTES[2]);
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_GET, "/api/thing", &exists) == nullptr);
        nya_assert(exists, "a path that answers QUERY and not GET is a 405");

        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_QUERY, "/api/other", &exists) == nullptr);
        nya_assert(!exists);

        // matching is exact: no prefixes, no patterns.
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_QUERY, "/api/thing/", &exists) == nullptr);
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_QUERY, "/api/thing/sub", &exists) == nullptr);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the chain is an onion, root layers outside the resource's own.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        make_request(request, NYA_HTTP_METHOD_QUERY, "/api/thing", nullptr);

        nya_assert(dispatch(arena, request, &response, root_layers, nya_carray_length(root_layers)) == NYA_HTTP_STATUS_OK);

        nya_assert(nya_string_equals(ORDER, "abhBA"), "the root layer wraps the resource's, which wraps the handler");
        nya_assert(nya_string_equals((NYA_ConstCString)response.body, "open"));
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a browser's two verbs still reach a read.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        make_request(request, NYA_HTTP_METHOD_GET, "/api/legacy", nullptr);

        nya_assert(dispatch(arena, request, &response, nullptr, 0) == NYA_HTTP_STATUS_OK);
        nya_assert(nya_string_equals((NYA_ConstCString)response.body, "legacy"), "GET is not gone, it is only not the default");

        // a HEAD on a path that answers only QUERY runs the QUERY handler, with no body to read from:
        // the server drops the bytes on the way out, which is what makes it a HEAD.
        make_request(request, NYA_HTTP_METHOD_HEAD, "/api/thing", nullptr);

        nya_assert(dispatch(arena, request, &response, nullptr, 0) == NYA_HTTP_STATUS_OK);
        nya_assert(nya_string_equals(ORDER, "bhB"));
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a layer that answers on its own never reaches the handler.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        const NYA_HttpLayerFn refusing[] = { short_circuit_layer };

        make_request(request, NYA_HTTP_METHOD_QUERY, "/api/thing", nullptr);

        nya_assert(dispatch(arena, request, &response, refusing, nya_carray_length(refusing)) == NYA_HTTP_STATUS_FORBIDDEN);
        nya_assert(nya_string_equals(ORDER, "s"), "nothing inside a layer that does not call next may run");

        NYA_Object* problem = nullptr;
        nya_assert(nya_deserialize(arena, response.body, response.body_size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &problem).ok);
        nya_assert(problem != nullptr);
        nya_assert(nya_object_get(problem, "status") != nullptr);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a route that does not exist, and one that does not answer this method.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        make_request(request, NYA_HTTP_METHOD_QUERY, "/api/nothing", nullptr);
        nya_assert(dispatch(arena, request, &response, nullptr, 0) == NYA_HTTP_STATUS_NOT_FOUND);
        nya_assert(response.body_size > 0, "a refusal always carries a body a client can parse");

        make_request(request, NYA_HTTP_METHOD_DELETE, "/api/thing", nullptr);
        nya_assert(dispatch(arena, request, &response, nullptr, 0) == NYA_HTTP_STATUS_METHOD_NOT_ALLOWED);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a write from another site is refused before any layer or handler runs.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        // the token is valid and carries the scope, so a refusal here is the cross-site check and nothing else.
        NYA_HttpIdentity writer                            = { .scope = NYA_HTTP_SCOPE_WRITE, .issued_at_s = NOW_S, .expires_at_s = NOW_S + 60 };
        char             bearer[NYA_HTTP_MAX_TOKEN_BYTES]  = { 0 };
        (void)snprintf(writer.subject, sizeof(writer.subject), "writer");
        nya_assert(nya_http_jwt_encode(&writer, SECRET, SECRET_SIZE, bearer, sizeof(bearer)).ok);

        NYA_ConstCString cases[][3] = {
            // header name, value, whether the write goes through
            { "origin", "https://evil.example", "refused" },
            { "origin", "null", "refused" },
            { "origin", "http://localhost:8080", "allowed" },
            { "origin", "http://LOCALHOST:8080", "allowed" },
            { "origin", "http://localhost:8081", "refused" },
            { "origin", "localhost:8080", "refused" },
            { "sec-fetch-site", "cross-site", "refused" },
            { "sec-fetch-site", "same-site", "refused" },
            { "sec-fetch-site", "same-origin", "allowed" },
            { "sec-fetch-site", "none", "allowed" },
        };

        for (u32 index = 0; index < nya_carray_length(cases); index++) {
            make_request(request, NYA_HTTP_METHOD_PUT, "/api/thing", bearer);
            request->header_count = 3;
            (void)snprintf(request->headers[1].name, sizeof(request->headers[1].name), "host");
            (void)snprintf(request->headers[1].value, sizeof(request->headers[1].value), "localhost:8080");
            (void)snprintf(request->headers[2].name, sizeof(request->headers[2].name), "%s", cases[index][0]);
            (void)snprintf(request->headers[2].value, sizeof(request->headers[2].value), "%s", cases[index][1]);

            NYA_HttpStatus status  = dispatch(arena, request, &response, root_layers, nya_carray_length(root_layers));
            b8             allowed = nya_string_equals(cases[index][2], "allowed");

            nya_assert(allowed ? status != NYA_HTTP_STATUS_FORBIDDEN : status == NYA_HTTP_STATUS_FORBIDDEN, "%s: %s should be %s, got %d", cases[index][0],
                       cases[index][1], cases[index][2], (s32)status);
            if (!allowed) nya_assert(ORDER[0] == '\0', "a refused write reached a layer: %s", ORDER);
        }

        // a read from another site is the browser's business, not a forgery: it changes nothing.
        make_request(request, NYA_HTTP_METHOD_GET, "/api/legacy", nullptr);
        request->header_count = 1;
        (void)snprintf(request->headers[0].name, sizeof(request->headers[0].name), "origin");
        (void)snprintf(request->headers[0].value, sizeof(request->headers[0].value), "https://evil.example");
        nya_assert(dispatch(arena, request, &response, nullptr, 0) != NYA_HTTP_STATUS_FORBIDDEN, "a read is not refused for coming from elsewhere");
    }

    // TEST: the extractor stands between the request and a handler taking a caller.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        // no token at all.
        make_request(request, NYA_HTTP_METHOD_PUT, "/api/thing", nullptr);
        nya_assert(dispatch(arena, request, &response, nullptr, 0) == NYA_HTTP_STATUS_UNAUTHORIZED);
        nya_assert(nya_string_equals(ORDER, "bB"), "the layers ran around the refusal; only the handler did not run");

        // a 401 says how to authenticate, which is what a client needs to act on it.
        b8 announced = false;
        for (u32 index = 0; index < response.header_count; index++) {
            announced = announced || nya_string_equals(response.headers[index].name, "WWW-Authenticate");
        }
        nya_assert(announced);

        // a token that is not this server's.
        make_request(request, NYA_HTTP_METHOD_PUT, "/api/thing", "not.a.token");
        nya_assert(dispatch(arena, request, &response, nullptr, 0) == NYA_HTTP_STATUS_UNAUTHORIZED);

        // a valid token without the scope the route needs.
        char             token                            = 0;
        NYA_HttpIdentity reader                           = { .scope = NYA_HTTP_SCOPE_READ, .issued_at_s = NOW_S, .expires_at_s = NOW_S + 60 };
        char             bearer[NYA_HTTP_MAX_TOKEN_BYTES] = { 0 };

        nya_unused(token);
        (void)snprintf(reader.subject, sizeof(reader.subject), "reader");
        nya_assert(nya_http_jwt_encode(&reader, SECRET, SECRET_SIZE, bearer, sizeof(bearer)).ok);

        make_request(request, NYA_HTTP_METHOD_PUT, "/api/thing", bearer);
        nya_assert(dispatch(arena, request, &response, nullptr, 0) == NYA_HTTP_STATUS_FORBIDDEN, "a verified caller without the scope is forbidden");

        // and one that carries it.
        NYA_HttpIdentity writer = { .scope = NYA_HTTP_SCOPE_READ | NYA_HTTP_SCOPE_WRITE, .issued_at_s = NOW_S, .expires_at_s = NOW_S + 60 };
        (void)snprintf(writer.subject, sizeof(writer.subject), "writer");
        nya_assert(nya_http_jwt_encode(&writer, SECRET, SECRET_SIZE, bearer, sizeof(bearer)).ok);

        make_request(request, NYA_HTTP_METHOD_PUT, "/api/thing", bearer);
        nya_assert(dispatch(arena, request, &response, nullptr, 0) == NYA_HTTP_STATUS_OK);
        nya_assert(nya_string_equals(ORDER, "bHB"));
        nya_assert(nya_string_equals((NYA_ConstCString)response.body, "writer"), "the handler was handed the identity the extractor produced");

        // an expired token is refused with the same answer as a forged one.
        make_request(request, NYA_HTTP_METHOD_PUT, "/api/thing", bearer);

        const NYA_HttpRouter* routers[] = { &ROUTER };

        NYA_HttpExchange later = {
            .request     = request,
            .response    = &response,
            .arena       = arena,
            .secret      = SECRET,
            .secret_size = SECRET_SIZE,
            .now_s       = NOW_S + 600,
        };

        nya_http_response_reset(&response);
        nya_assert(nya_http_router_dispatch(&later, routers, 1, nullptr, 0) == NYA_HTTP_STATUS_UNAUTHORIZED);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a server with no secret cannot verify anybody, and says so.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        const NYA_HttpRouter* routers[] = { &ROUTER };

        make_request(request, NYA_HTTP_METHOD_PUT, "/api/thing", "anything");

        NYA_HttpExchange secretless = {
            .request  = request,
            .response = &response,
            .arena    = arena,
            .now_s    = NOW_S,
        };

        nya_http_response_reset(&response);

        nya_assert(
            nya_http_router_dispatch(&secretless, routers, 1, nullptr, 0) == NYA_HTTP_STATUS_SERVICE_UNAVAILABLE,
            "a route needing a token on a server that cannot check one is not the caller's fault"
        );
    }

    printf("PASSED: http router\n");

    return EXIT_SUCCESS;
}

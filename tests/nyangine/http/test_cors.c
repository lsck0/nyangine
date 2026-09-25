/**
 * Per-route CORS: the preflight, the actual-request headers, the allowlist that never reflects an
 * arbitrary origin, and the interaction with the cross-site write guard.
 *
 * Driven through nya_http_router_dispatch rather than a socket, the same way test_router.c is: a route
 * table, a policy and a request are all data, and testing them needs no port.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/* ── the policies under test ── */

static const NYA_ConstCString CREDS_ORIGINS[] = { "https://app.example.com", "https://admin.example.com" };
static const NYA_HttpMethod   CREDS_METHODS[] = { NYA_HTTP_METHOD_QUERY, NYA_HTTP_METHOD_POST, NYA_HTTP_METHOD_DELETE };
static const NYA_ConstCString CREDS_HEADERS[] = { "Authorization", "Content-Type" };
static const NYA_ConstCString CREDS_EXPOSE[]  = { "X-Request-Id" };

/** A credentialed policy: an exact allowlist reflected back, cookies allowed, and no wildcard anywhere. */
static const NYA_HttpCors CREDS_CORS = {
    .origins      = CREDS_ORIGINS,
    .origin_count = nya_carray_length(CREDS_ORIGINS),
    .methods      = CREDS_METHODS,
    .method_count = nya_carray_length(CREDS_METHODS),
    .headers      = CREDS_HEADERS,
    .header_count = nya_carray_length(CREDS_HEADERS),
    .expose       = CREDS_EXPOSE,
    .expose_count = nya_carray_length(CREDS_EXPOSE),
    .max_age_s    = 600,
    .credentials  = true,
};

static const NYA_ConstCString WILDCARD_ORIGINS[] = { "*" };
static const NYA_HttpMethod   WILDCARD_METHODS[] = { NYA_HTTP_METHOD_GET };

/** A public read: any origin, no credentials, so the wildcard is safe to answer. */
static const NYA_HttpCors WILDCARD_CORS = {
    .origins      = WILDCARD_ORIGINS,
    .origin_count = nya_carray_length(WILDCARD_ORIGINS),
    .methods      = WILDCARD_METHODS,
    .method_count = nya_carray_length(WILDCARD_METHODS),
    .credentials  = false,
};

/* ── handlers ── */

static NYA_HttpStatus ok_query(NYA_HttpExchange* exchange) {
    return nya_http_response_text(exchange->response, "ok", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

static NYA_HttpStatus ok_post(NYA_HttpExchange* exchange) {
    return nya_http_response_text(exchange->response, "made", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/* ── the tables ── */

static const NYA_HttpRoute THING_ROUTES[] = {
    {
     .method   = NYA_HTTP_METHOD_QUERY,
     .path     = "/api/thing",
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = ok_query,
     .summary  = "read the thing",
     .statuses = { NYA_HTTP_STATUS_OK },
     },
    {
     .method   = NYA_HTTP_METHOD_POST,
     .path     = "/api/thing",
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = ok_post,
     .summary  = "make a thing",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_FORBIDDEN },
     },
};

/** The resource carries the credentialed policy as its default; both routes inherit it. */
static const NYA_HttpRouter THING_ROUTER = {
    .name        = "thing",
    .routes      = THING_ROUTES,
    .route_count = nya_carray_length(THING_ROUTES),
    .cors        = &CREDS_CORS,
};

static const NYA_HttpRoute PUBLIC_ROUTES[] = {
    {
     // its own policy, overriding the router's absent default.
        .method   = NYA_HTTP_METHOD_GET,
     .path     = "/api/open",
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = ok_query,
     .cors     = &WILDCARD_CORS,
     .summary  = "a public read",
     .statuses = { NYA_HTTP_STATUS_OK },
     },
    {
     // no policy at all: default-deny, and the cross-site write guard stands.
        .method   = NYA_HTTP_METHOD_POST,
     .path     = "/api/guarded",
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = ok_post,
     .summary  = "a write with no CORS",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_FORBIDDEN },
     },
};

static const NYA_HttpRouter PUBLIC_ROUTER = {
    .name        = "public",
    .routes      = PUBLIC_ROUTES,
    .route_count = nya_carray_length(PUBLIC_ROUTES),
};

/* ── request building ── */

/** Builds a request by hand with a set of headers; the parser has its own tests. */
static void
make_request(OUT NYA_HttpRequest* request, NYA_HttpMethod method, NYA_ConstCString path, const NYA_HttpHeader* headers, u32 header_count) {
    *request = (NYA_HttpRequest){ .method = method, .keep_alive = true };

    NYA_UrlFailure failure = { 0 };
    NYA_EXPECT(nya_url_parse_target(path, strlen(path), &request->target, &failure), "while building a request");

    (void)
        snprintf(request->path, sizeof(request->path), "%.*s", (int)request->target.path.length, request->target.text + request->target.path.offset);

    for (u32 index = 0; index < header_count && index < NYA_HTTP_MAX_HEADERS; index++) {
        NYA_HttpHeader* stored = &request->headers[request->header_count];

        // the parser stores header names lowercased, and nya_http_request_header matches against that, so this builder does the same rather than
        // relying on the lookup to fold both sides.
        u64 length = 0;
        for (; headers[index].name[length] != '\0' && length + 1 < sizeof(stored->name); length++) {
            char character       = headers[index].name[length];
            stored->name[length] = character >= 'A' && character <= 'Z' ? (char)(character - 'A' + 'a') : character;
        }
        stored->name[length] = '\0';

        (void)snprintf(stored->value, sizeof(stored->value), "%s", headers[index].value);
        request->header_count++;
    }
}

/** One dispatch over both routers. */
static NYA_HttpStatus dispatch(NYA_Arena* arena, const NYA_HttpRequest* request, NYA_HttpResponse* response) {
    const NYA_HttpRouter* routers[] = { &THING_ROUTER, &PUBLIC_ROUTER };

    NYA_HttpExchange exchange = { .request = request, .response = response, .arena = arena, .now_s = 1700000000ULL };

    nya_http_response_reset(response);
    nya_memset(response->body, 0, response->body_capacity);

    return nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), nullptr, 0);
}

/** The value of a response header by its exact name, or null; the CORS code writes the names this asks for. */
static NYA_ConstCString response_header(const NYA_HttpResponse* response, NYA_ConstCString name) {
    for (u32 index = 0; index < response->header_count && index < NYA_HTTP_MAX_RESPONSE_HEADERS; index++) {
        if (nya_string_equals(response->headers[index].name, name)) return response->headers[index].value;
    }

    return nullptr;
}

/** Null-safe string equality, so a missing header reports a soft failure rather than asserting. */
static b8 eq(NYA_ConstCString a, NYA_ConstCString b) {
    return a != nullptr && b != nullptr && nya_string_equals(a, b);
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_cors");
    defer      nya_arena_destroy(arena);

    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
    nya_assert(request != nullptr);

    u8 body[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };

    NYA_HttpResponse response = { 0 };
    nya_http_response_create(&response, body, sizeof(body));
    defer nya_http_response_destroy(&response);

    // TEST: well formed policies pass the check, and the one vulnerability does not.
    {
        nya_check(nya_http_router_check(&THING_ROUTER).ok, "the credentialed exact-allowlist policy is well formed");
        nya_check(nya_http_router_check(&PUBLIC_ROUTER).ok, "the wildcard-without-credentials policy is well formed");
        nya_check(nya_http_cors_check(nullptr).ok, "no policy is default-deny and well formed");

        const NYA_ConstCString star[]     = { "*" };
        const NYA_HttpCors     star_creds = { .origins = star, .origin_count = 1, .credentials = true };
        nya_check(
            !nya_http_cors_check(&star_creds).ok,
            "a '*' origin with credentials is refused: it would reflect a credentialed reply to any site"
        );

        const NYA_HttpCors star_no_creds = { .origins = star, .origin_count = 1, .credentials = false };
        nya_check(nya_http_cors_check(&star_no_creds).ok, "a '*' origin without credentials is fine");

        const NYA_HttpCors too_many = { .origins = CREDS_ORIGINS, .origin_count = NYA_HTTP_CORS_MAX_ORIGINS + 1 };
        nya_check(!nya_http_cors_check(&too_many).ok, "an origin list past the bound is refused");
    }

    // TEST: a preflight for an allowed origin and method gets the negotiated headers and a bodiless 204.
    {
        const NYA_HttpHeader headers[] = {
            { .name = "Origin",                         .value = "https://app.example.com" },
            { .name = "Access-Control-Request-Method",  .value = "POST"                    },
            { .name = "Access-Control-Request-Headers", .value = "authorization"           },
        };
        make_request(request, NYA_HTTP_METHOD_OPTIONS, "/api/thing", headers, nya_carray_length(headers));

        nya_check(dispatch(arena, request, &response) == NYA_HTTP_STATUS_NO_CONTENT, "a preflight is a 204");
        nya_check(response.body_size == 0, "a preflight carries no body");
        nya_check(
            eq(response_header(&response, "Access-Control-Allow-Origin"), "https://app.example.com"),
            "the allowed origin is reflected exactly, never a wildcard for a credentialed policy"
        );
        nya_check(eq(response_header(&response, "Access-Control-Allow-Credentials"), "true"), "credentials are allowed");
        nya_check(eq(response_header(&response, "Vary"), "Origin"), "the answer varies by Origin so a cache keeps origins apart");

        NYA_ConstCString methods = response_header(&response, "Access-Control-Allow-Methods");
        nya_check(methods != nullptr && strstr(methods, "POST") != nullptr, "the allowed methods are listed");

        NYA_ConstCString allowed_headers = response_header(&response, "Access-Control-Allow-Headers");
        nya_check(allowed_headers != nullptr && strstr(allowed_headers, "Authorization") != nullptr, "the allowed request headers are listed");
        nya_check(eq(response_header(&response, "Access-Control-Max-Age"), "600"), "the preflight cache lifetime is set");
    }

    // TEST: a preflight from an origin not on the allowlist gets no CORS headers.
    {
        const NYA_HttpHeader headers[] = {
            { .name = "Origin",                        .value = "https://evil.example.net" },
            { .name = "Access-Control-Request-Method", .value = "POST"                     },
        };
        make_request(request, NYA_HTTP_METHOD_OPTIONS, "/api/thing", headers, nya_carray_length(headers));

        nya_check(
            dispatch(arena, request, &response) == NYA_HTTP_STATUS_NO_CONTENT,
            "still a 204, so the response shape does not leak the allowlist"
        );
        nya_check(response_header(&response, "Access-Control-Allow-Origin") == nullptr, "a disallowed origin gets no allow-origin header");
    }

    // TEST: a preflight for a method the policy does not grant gets no CORS headers.
    {
        const NYA_HttpHeader headers[] = {
            { .name = "Origin",                        .value = "https://app.example.com" },
            { .name = "Access-Control-Request-Method", .value = "PUT"                     }, // not in CREDS_METHODS
        };
        make_request(request, NYA_HTTP_METHOD_OPTIONS, "/api/thing", headers, nya_carray_length(headers));

        nya_check(dispatch(arena, request, &response) == NYA_HTTP_STATUS_NO_CONTENT, "a 204");
        nya_check(
            response_header(&response, "Access-Control-Allow-Origin") == nullptr,
            "a method the policy does not allow gets no allow-origin header"
        );
    }

    // TEST: an actual request from an allowed origin gets the CORS headers alongside its body.
    {
        const NYA_HttpHeader headers[] = {
            { .name = "Origin", .value = "https://admin.example.com" }
        };
        make_request(request, NYA_HTTP_METHOD_QUERY, "/api/thing", headers, nya_carray_length(headers));

        nya_check(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "the read answers");
        nya_check(nya_string_equals((NYA_ConstCString)response.body, "ok"), "with its body");
        nya_check(eq(response_header(&response, "Access-Control-Allow-Origin"), "https://admin.example.com"), "the origin is reflected");
        nya_check(eq(response_header(&response, "Access-Control-Allow-Credentials"), "true"), "credentials allowed");
        nya_check(eq(response_header(&response, "Vary"), "Origin"), "and it varies by origin");

        NYA_ConstCString expose = response_header(&response, "Access-Control-Expose-Headers");
        nya_check(expose != nullptr && strstr(expose, "X-Request-Id") != nullptr, "the exposed headers are named");
    }

    // TEST: an actual request from a disallowed origin, and one with no Origin, get no CORS headers.
    {
        const NYA_HttpHeader evil[] = {
            { .name = "Origin", .value = "https://evil.example.net" }
        };
        make_request(request, NYA_HTTP_METHOD_QUERY, "/api/thing", evil, nya_carray_length(evil));
        nya_check(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "the read still answers");
        nya_check(response_header(&response, "Access-Control-Allow-Origin") == nullptr, "but a disallowed origin reads nothing cross-origin");

        make_request(request, NYA_HTTP_METHOD_QUERY, "/api/thing", nullptr, 0);
        nya_check(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "a same-origin read answers");
        nya_check(response_header(&response, "Access-Control-Allow-Origin") == nullptr, "and carries no CORS header when there is no Origin");
    }

    // TEST: the wildcard public route answers any origin with '*' and no credentials or Vary.
    {
        const NYA_HttpHeader headers[] = {
            { .name = "Origin", .value = "https://anything.example" }
        };
        make_request(request, NYA_HTTP_METHOD_GET, "/api/open", headers, nya_carray_length(headers));

        nya_check(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "the public read answers");
        nya_check(eq(response_header(&response, "Access-Control-Allow-Origin"), "*"), "any origin, so '*'");
        nya_check(response_header(&response, "Access-Control-Allow-Credentials") == nullptr, "and never credentials with '*'");
        nya_check(response_header(&response, "Vary") == nullptr, "a wildcard answer is the same for everyone, so no Vary");
    }

    // TEST: the cross-site write guard, and how a CORS allowlist is the grant that lets a write past it.
    {
        // a cross-site POST to a route with no CORS policy: Origin's host differs from Host, so it is refused.
        const NYA_HttpHeader cross[] = {
            { .name = "Origin", .value = "https://app.example.com" },
            { .name = "Host",   .value = "api.example.com"         },
        };
        make_request(request, NYA_HTTP_METHOD_POST, "/api/guarded", cross, nya_carray_length(cross));
        nya_check(dispatch(arena, request, &response) == NYA_HTTP_STATUS_FORBIDDEN, "a cross-site write to a route with no CORS policy is refused");

        // the same cross-site POST to a route whose policy allows the origin passes the guard: the allowlist is the intentional grant.
        make_request(request, NYA_HTTP_METHOD_POST, "/api/thing", cross, nya_carray_length(cross));
        nya_check(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "the allowlist lets an allowed cross-origin write through the guard");
        nya_check(eq(response_header(&response, "Access-Control-Allow-Origin"), "https://app.example.com"), "and the answer carries the CORS header");

        // a cross-site POST from an origin the policy does not list stays refused.
        const NYA_HttpHeader cross_evil[] = {
            { .name = "Origin", .value = "https://evil.example.net" },
            { .name = "Host",   .value = "api.example.com"          },
        };
        make_request(request, NYA_HTTP_METHOD_POST, "/api/thing", cross_evil, nya_carray_length(cross_evil));
        nya_check(
            dispatch(arena, request, &response) == NYA_HTTP_STATUS_FORBIDDEN,
            "an origin the policy does not list is still refused a cross-site write"
        );
    }

    printf("PASSED: http cors\n");

    return nya_check_failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

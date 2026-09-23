/**
 * What one exchange leaves in the log, and what it must not.
 *
 * Driven through nya_http_router_dispatch with the layer installed, like test_router.c: a route table
 * and an exchange are data, and what is under test here is the record rather than the socket.
 *
 * The last block is the negative space, as a property: every reflected type in this build that carries
 * a `@redact` field anywhere gets every one of those fields filled with a marker nothing else in the
 * program contains, is sent through a route declaring that type at every level there is, and the marker
 * has to appear in no sink's output. It is written against the generated tables rather than against a
 * list, so a DTO added tomorrow is covered by this tomorrow.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#define NOW_S 1700000000ULL

/*
 * ─────────────────────────────────────────────────────────
 * THE SINKS
 * ─────────────────────────────────────────────────────────
 */

/** Everything the log has produced since the last reset, from a sink registered like any other. */
static char CAPTURED[64 * 1024] = { 0 };
static u64  CAPTURED_LENGTH     = 0;

static void capture(NYA_LogLevel level, NYA_ConstCString message, u32 length, void* user_data) {
    nya_unused(level);
    nya_unused(user_data);

    if (CAPTURED_LENGTH + length + 2 >= sizeof(CAPTURED)) return;

    nya_memcpy(CAPTURED + CAPTURED_LENGTH, message, length);
    CAPTURED_LENGTH += length;

    CAPTURED[CAPTURED_LENGTH++] = '\n';
    CAPTURED[CAPTURED_LENGTH]   = '\0';
}

static void capture_reset(void) {
    CAPTURED_LENGTH = 0;
    CAPTURED[0]     = '\0';

    nya_log_ring_clear();
}

/** Whether any sink there is holds `text`: the one registered above, and the ring a crash report prints. */
static b8 any_sink_holds(NYA_ConstCString text) {
    if (strstr(CAPTURED, text) != nullptr) return true;

    for (u32 index = 0; index < nya_log_ring_count(); index++) {
        NYA_ConstCString line = nya_log_ring_at(index);

        if (line != nullptr && strstr(line, text) != nullptr) return true;
    }

    return false;
}

/*
 * ─────────────────────────────────────────────────────────
 * THE ROUTES
 * ─────────────────────────────────────────────────────────
 */

/** Answers with the DTO the route declares, so the response body is a typed one the layer can decode. */
static NYA_HttpStatus echo_submission(NYA_HttpExchange* exchange) {
    NYA_HttpTotpSubmission submission = { 0 };

    // a body that does not fit the type is the caller's mistake; the layer still has to log it safely.
    if (!nya_http_request_reflect(exchange->request, exchange->arena, nya_reflect_of(NYA_HttpTotpSubmission), &submission).ok) {
        return NYA_HTTP_STATUS_BAD_REQUEST;
    }

    NYA_Error written =
        nya_http_response_reflect_as(exchange->response, exchange->arena, nya_reflect_of(NYA_HttpTotpSubmission), &submission, NYA_HTTP_MEDIA_JSON);

    return written.ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** A route with no DTO at all, which is the fail-closed case every unparseable body ends in. */
static NYA_HttpStatus untyped_query(NYA_HttpExchange* exchange) {
    return nya_http_response_text(exchange->response, "{\"ok\":true}", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK
                                                                                              : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

#define TYPED_PATH   "/api/typed"
#define UNTYPED_PATH "/api/untyped"

static const NYA_HttpRoute ROUTES[] = {
    {
     .method        = NYA_HTTP_METHOD_QUERY,
     .path          = TYPED_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
     .handler       = echo_submission,
     .request_type  = nya_reflect_of(NYA_HttpTotpSubmission),
     .response_type = nya_reflect_of(NYA_HttpTotpSubmission),
     .summary       = "Echoes a submitted code, so both bodies are typed",
     .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method   = NYA_HTTP_METHOD_QUERY,
     .path     = UNTYPED_PATH,
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = untyped_query,
     .summary  = "A route whose bodies nothing describes",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

static const NYA_HttpRouter ROUTER = {
    .name        = "log",
    .routes      = ROUTES,
    .route_count = nya_carray_length(ROUTES),
};

/*
 * ─────────────────────────────────────────────────────────
 * ONE EXCHANGE
 * ─────────────────────────────────────────────────────────
 */

/** Builds a request by hand: the parser has its own test and this one is about what comes after it. */
static void make_request(OUT NYA_HttpRequest* request, NYA_ConstCString target, NYA_ConstCString body) {
    *request = (NYA_HttpRequest){ .method = NYA_HTTP_METHOD_QUERY, .keep_alive = true };

    NYA_UrlFailure failure = { 0 };
    NYA_EXPECT(nya_url_parse_target(target, strlen(target), &request->target, &failure), "while building a request");

    (void)snprintf(request->path, sizeof(request->path), "%.*s", (int)request->target.path.length,
                   request->target.text + request->target.path.offset);

    request->header_count = 3;
    (void)snprintf(request->headers[0].name, sizeof(request->headers[0].name), "authorization");
    (void)snprintf(request->headers[0].value, sizeof(request->headers[0].value), "Bearer a-token-nobody-should-read");
    (void)snprintf(request->headers[1].name, sizeof(request->headers[1].name), "x-api-key");
    (void)snprintf(request->headers[1].value, sizeof(request->headers[1].value), "an-api-key-nobody-should-read");
    (void)snprintf(request->headers[2].name, sizeof(request->headers[2].name), "accept");
    (void)snprintf(request->headers[2].value, sizeof(request->headers[2].value), "application/json");

    if (body == nullptr) return;

    request->media_type = NYA_HTTP_MEDIA_JSON;
    request->body_size  = strlen(body);

    nya_assert(request->body_size < sizeof(request->body));
    nya_memcpy(request->body, body, request->body_size + 1);
}

static NYA_HttpStatus dispatch(NYA_Arena* arena, const NYA_HttpRequest* request, NYA_HttpResponse* response) {
    const NYA_HttpRouter* routers[] = { &ROUTER };
    const NYA_HttpLayerFn layers[]  = { nya_http_layer_log };

    NYA_HttpExchange exchange = {
        .request  = request,
        .response = response,
        .arena    = arena,
        .now_s    = NOW_S,
        .address  = "203.0.113.7",
    };

    nya_http_response_reset(response);
    nya_memset(response->body, 0, response->body_capacity);

    return nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), layers, nya_carray_length(layers));
}

/*
 * ─────────────────────────────────────────────────────────
 * THE PROPERTY
 * ─────────────────────────────────────────────────────────
 */

/** Writes `marker` into every `char[N]` anywhere under `type`, which is every place a marker can go. */
static u32 fill_text(const NYA_TypeReflection* type, void* address, NYA_ConstCString marker, u32 depth) {
    if (type == nullptr || depth >= NYA_REFLECT_LAYOUT_DEPTH_MAX) return 0;

    if (nya_reflect_is_char_array(type)) {
        // an array too short to hold the whole marker would go out truncated, and a truncated marker
        // is a different string to search for. Left alone and not counted instead.
        if (type->element_count <= strlen(marker)) return 0;

        (void)snprintf((char*)address, type->element_count, "%s", marker);

        return 1;
    }

    u32 filled = 0;

    if (type->kind == NYA_REFLECT_STRUCT || type->kind == NYA_REFLECT_UNION) {
        for (u32 index = 0; index < type->field_count; index++) {
            const NYA_ReflectField* field = &type->fields[index];
            if (field->type == nullptr) continue;

            filled += fill_text(field->type, (u8*)address + field->offset, marker, depth + 1);
        }

        return filled;
    }

    if (type->kind == NYA_REFLECT_ARRAY && type->element != nullptr) {
        for (u32 index = 0; index < type->element_count; index++) {
            filled += fill_text(type->element, (u8*)address + ((u64)index * type->element->size), marker, depth + 1);
        }
    }

    return filled;
}

/** The same, but only under fields that carry `@redact`, which is what must never be logged. */
static u32 fill_redacted(const NYA_TypeReflection* type, void* address, NYA_ConstCString marker, u32 depth) {
    if (type == nullptr || depth >= NYA_REFLECT_LAYOUT_DEPTH_MAX) return 0;

    u32 filled = 0;

    if (type->kind == NYA_REFLECT_STRUCT || type->kind == NYA_REFLECT_UNION) {
        for (u32 index = 0; index < type->field_count; index++) {
            const NYA_ReflectField* field = &type->fields[index];
            if (field->type == nullptr) continue;

            void* at = (u8*)address + field->offset;

            filled += field->is_redacted ? fill_text(field->type, at, marker, depth + 1) : fill_redacted(field->type, at, marker, depth + 1);
        }

        return filled;
    }

    if (type->kind == NYA_REFLECT_ARRAY && type->element != nullptr) {
        for (u32 index = 0; index < type->element_count; index++) {
            filled += fill_redacted(type->element, (u8*)address + ((u64)index * type->element->size), marker, depth + 1);
        }
    }

    return filled;
}

/** One route table built around `type`, so a body of that type can be sent through the layer. */
static NYA_HttpStatus dispatch_as(NYA_Arena* arena, const NYA_TypeReflection* type, NYA_ConstCString body, NYA_HttpResponse* response) {
    const NYA_HttpRoute routes[] = {
        {
         .method       = NYA_HTTP_METHOD_QUERY,
         .path         = "/api/property",
         .auth         = NYA_HTTP_AUTH_NONE,
         .handler      = untyped_query,
         .request_type = type,
         .summary      = "One reflected type, over the wire",
         .statuses     = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
         },
    };

    const NYA_HttpRouter  router    = { .name = "property", .routes = routes, .route_count = nya_carray_length(routes) };
    const NYA_HttpRouter* routers[] = { &router };
    const NYA_HttpLayerFn layers[]  = { nya_http_layer_log };

    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));

    make_request(request, "/api/property", body);

    NYA_HttpExchange exchange = {
        .request  = request,
        .response = response,
        .arena    = arena,
        .now_s    = NOW_S,
        .address  = "203.0.113.7",
    };

    nya_http_response_reset(response);
    nya_memset(response->body, 0, response->body_capacity);

    return nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), layers, nya_carray_length(layers));
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_http_log");
    defer      nya_arena_destroy(arena);

    nya_log_sink_add(capture, nullptr);

    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));

    u8 body[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };

    NYA_HttpResponse response = { 0 };
    nya_http_response_create(&response, body, sizeof(body));
    defer nya_http_response_destroy(&response);

    nya_assert(nya_http_router_check(&ROUTER).ok);

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the deny list, which no level and no configuration turns off
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: the header deny list\n");
    {
        nya_http_log_config_set((NYA_HttpLogConfig){ 0 });

        nya_assert(nya_http_log_header_is_denied("authorization"));
        nya_assert(nya_http_log_header_is_denied("Set-Cookie"), "the match ignores case, as header names do");
        nya_assert(nya_http_log_header_is_denied("proxy-authorization"));
        nya_assert(!nya_http_log_header_is_denied("accept"));
        nya_assert(!nya_http_log_header_is_denied("x-api-key"), "nothing is denied by configuration until it is configured");

        nya_http_log_config_set((NYA_HttpLogConfig){ .deny = "x-api-key, x-hub-signature" });

        nya_assert(nya_http_log_header_is_denied("x-api-key"), "a configured name is denied");
        nya_assert(nya_http_log_header_is_denied("X-Hub-Signature"), "and a spaced entry is still one entry");
        nya_assert(!nya_http_log_header_is_denied("x-api"), "a prefix of a denied name is not that name");
        nya_assert(nya_http_log_header_is_denied("cookie"), "configuring a list does not replace the built-in one");

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: summary carries the line and nothing else
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: summary\n");
    {
        nya_http_log_config_set((NYA_HttpLogConfig){ .level = NYA_HTTP_LOG_SUMMARY });

        capture_reset();
        make_request(request, TYPED_PATH "?token=a-secret-in-a-query&page=2", "{\"code\":\"123456\"}");

        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK);

        nya_assert(any_sink_holds("QUERY " TYPED_PATH " -> 200"), "the summary line is missing: %s", CAPTURED);
        nya_assert(any_sink_holds("203.0.113.0/24"), "the address is truncated by default");
        nya_assert(!any_sink_holds("203.0.113.7"), "and the host part is gone");

        // the route's path is logged, not the request's, so nothing of the query reaches this level.
        nya_assert(!any_sink_holds("a-secret-in-a-query"));
        nya_assert(!any_sink_holds("page=2"));
        nya_assert(!any_sink_holds("123456"), "a body is not logged at this level");
        nya_assert(!any_sink_holds("a-token-nobody-should-read"), "nor a header");

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: headers, the deny list applied, and the query redacted by name
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: headers\n");
    {
        nya_http_log_config_set((NYA_HttpLogConfig){ .level = NYA_HTTP_LOG_HEADERS, .deny = "x-api-key" });

        capture_reset();
        make_request(request, TYPED_PATH "?token=a-secret-in-a-query&page=2", "{\"code\":\"123456\"}");

        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK);

        nya_assert(any_sink_holds("accept: application/json"), "an ordinary header is the point of this level: %s", CAPTURED);
        nya_assert(any_sink_holds("authorization: " NYA_REFLECT_REDACTED));
        nya_assert(!any_sink_holds("a-token-nobody-should-read"), "the token survived the deny list");
        nya_assert(!any_sink_holds("an-api-key-nobody-should-read"), "the configured name survived");

        nya_assert(any_sink_holds("page=2"), "an ordinary query parameter is readable");
        nya_assert(any_sink_holds("token=" NYA_REFLECT_REDACTED));
        nya_assert(!any_sink_holds("a-secret-in-a-query"), "a parameter named like a secret survived");

        nya_assert(!any_sink_holds("123456"), "a body is not logged at this level either");

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: bodies, through the route's DTO
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: bodies\n");
    {
        nya_http_log_config_set((NYA_HttpLogConfig){ .level = NYA_HTTP_LOG_BODIES });

        capture_reset();
        make_request(request, TYPED_PATH, "{\"code\":\"123456\"}");

        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK);

        // the field is `@redact` on NYA_HttpTotpSubmission, so it is gone on the way in and on the way
        // back out: the response echoes it, and the response body is decoded through the same table.
        nya_assert(!any_sink_holds("123456"), "a tagged field reached a sink: %s", CAPTURED);
        nya_assert(any_sink_holds("\"code\":\"" NYA_REFLECT_REDACTED "\""), "the body was not decoded at all: %s", CAPTURED);

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: fail closed — anything that is not the route's DTO is a size and a hash
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: fail closed\n");
    {
        nya_http_log_config_set((NYA_HttpLogConfig){ .level = NYA_HTTP_LOG_BODIES });

        // a key this type has no field for. It parses as JSON and is refused all the same, because a
        // field nothing describes is a field nothing could have tagged.
        capture_reset();
        make_request(request, TYPED_PATH, "{\"code\":\"123456\",\"extra\":\"a-secret-nobody-declared\"}");

        (void)dispatch(arena, request, &response);

        nya_assert(!any_sink_holds("a-secret-nobody-declared"), "an undeclared field was logged: %s", CAPTURED);
        nya_assert(!any_sink_holds("123456"), "and the rest of that body with it");
        nya_assert(any_sink_holds("blake2b "), "a refused body is still identified: %s", CAPTURED);

        // a body that is not a document at all.
        capture_reset();
        make_request(request, TYPED_PATH, "not json at all, and-a-secret-in-it");

        (void)dispatch(arena, request, &response);

        nya_assert(!any_sink_holds("and-a-secret-in-it"));
        nya_assert(any_sink_holds("blake2b "));

        // and a route with no DTO, which is every route that reads its body as a document.
        capture_reset();
        make_request(request, UNTYPED_PATH, "{\"anything\":\"a-secret-on-an-untyped-route\"}");

        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK);

        nya_assert(!any_sink_holds("a-secret-on-an-untyped-route"), "an untyped body was logged: %s", CAPTURED);
        nya_assert(any_sink_holds("blake2b "));

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the address setting
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: addresses\n");
    {
        nya_http_log_config_set((NYA_HttpLogConfig){ .address = NYA_HTTP_LOG_ADDRESS_FULL });

        capture_reset();
        make_request(request, UNTYPED_PATH, nullptr);
        (void)dispatch(arena, request, &response);

        nya_assert(any_sink_holds("203.0.113.7"), "full means full");

        nya_http_log_config_set((NYA_HttpLogConfig){ .address = NYA_HTTP_LOG_ADDRESS_NONE });

        capture_reset();
        (void)dispatch(arena, request, &response);

        nya_assert(!any_sink_holds("203.0.113"), "none means none: %s", CAPTURED);

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the property — no marker in any `@redact` field reaches any sink, at any level
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: no tagged field reaches a sink\n");
    {
        const NYA_HttpLogLevel LEVELS[] = { NYA_HTTP_LOG_SUMMARY, NYA_HTTP_LOG_HEADERS, NYA_HTTP_LOG_BODIES };

        u32 covered = 0;

        for (u32 index = 0; index < NYA_REFLECT_ENGINE_TYPE_COUNT; index++) {
            const NYA_TypeReflection* type = NYA_REFLECT_ENGINE_TYPES[index];

            if (type->kind != NYA_REFLECT_STRUCT) continue;

            /*
             * Per type and unlike anything else in the program, so a hit is this field and not a
             * coincidence in a path, a header name or a duration. Short, because it has to fit the
             * smallest tagged `char[N]` in the tree whole: half a marker is a different string.
             */
            char marker[16] = { 0 };
            (void)snprintf(marker, sizeof(marker), "zq%04llxqz", (unsigned long long)(nya_hash_fnv1a(type->name) & 0xFFFF));

            void* instance = nya_arena_alloc(arena, type->size);
            nya_memset(instance, 0, type->size);

            if (fill_redacted(type, instance, marker, 0) == 0) continue;

            covered++;

            // the unredacted document is what a caller would have sent, which is the whole point: the
            // marker really is in the bytes the server reads.
            NYA_Object* document = nya_reflect_to_object(arena, type, instance);
            NYA_String* text     = nya_serialize(arena, document, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);

            nya_assert(text != nullptr);

            NYA_CString sent = nya_string_to_cstring(arena, text);
            nya_assert(strstr(sent, marker) != nullptr, "%s: the marker never reached the wire", type->name);

            if (strlen(sent) >= NYA_HTTP_MAX_BODY_BYTES) continue;

            for (u32 level = 0; level < nya_carray_length(LEVELS); level++) {
                nya_http_log_config_set((NYA_HttpLogConfig){ .level = LEVELS[level] });

                capture_reset();
                (void)dispatch_as(arena, type, sent, &response);

                nya_assert(!any_sink_holds(marker), "%s reached a sink at level %d: %s", type->name, (s32)LEVELS[level], CAPTURED);
            }
        }

        nya_assert(covered > 0, "no reflected type carries a @redact field, so this proved nothing");
        printf("  %u types with tagged fields\n", covered);

        printf("  PASSED\n");
    }

    nya_assert(nya_log_sink_remove(capture, nullptr));

    printf("PASSED: http log\n");

    return EXIT_SUCCESS;
}

/**
 * Calling a route by its table entry: the request DTO in, the response DTO out.
 *
 * The route table is the same data the server dispatches and the OpenAPI document is generated from, and
 * here it is the client's map too. A loopback transport dispatches straight through nya_http_router_dispatch
 * with no socket, so one route table drives the real server path and the real client path against each
 * other — a field written by the handler is the field the client reads back. A second, canned transport
 * returns crafted bytes, which is how the response-decoding half is tested on its own: a non-2xx is an
 * error carrying the server's problem, and a body that does not parse is a clean parse error.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/* the DTOs, reflected by hand: a test's types are not scanned by the reflection pass */

typedef struct {
    s64 room;
} RoomQuery;

typedef struct {
    s64  room;
    s64  doubled;
    char label[32];
} RoomReply;

static const NYA_ReflectField ROOM_QUERY_FIELDS[] = {
    { .name = "room", .type = nya_reflect_of(s64), .offset = nya_offsetof(RoomQuery, room) },
};

static const NYA_TypeReflection ROOM_QUERY_MODEL = {
    .name        = "RoomQuery",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(RoomQuery),
    .alignment   = alignof(RoomQuery),
    .fields      = ROOM_QUERY_FIELDS,
    .field_count = nya_carray_length(ROOM_QUERY_FIELDS),
};

static const NYA_TypeReflection ROOM_LABEL_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = sizeof(((RoomReply*)nullptr)->label),
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = sizeof(((RoomReply*)nullptr)->label),
};

static const NYA_ReflectField ROOM_REPLY_FIELDS[] = {
    { .name = "room", .type = nya_reflect_of(s64), .offset = nya_offsetof(RoomReply, room) },
    { .name = "doubled", .type = nya_reflect_of(s64), .offset = nya_offsetof(RoomReply, doubled) },
    { .name = "label", .type = &ROOM_LABEL_ARRAY, .offset = nya_offsetof(RoomReply, label) },
};

static const NYA_TypeReflection ROOM_REPLY_MODEL = {
    .name        = "RoomReply",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(RoomReply),
    .alignment   = alignof(RoomReply),
    .fields      = ROOM_REPLY_FIELDS,
    .field_count = nya_carray_length(ROOM_REPLY_FIELDS),
};

/* the handlers: exactly what a server's handler is, reading a DTO in and writing one out */

/** Doubles the room number and names it, so the client can check the fields it got are the ones written. */
static NYA_HttpStatus room_query(NYA_HttpExchange* exchange) {
    RoomQuery query = { 0 };
    if (!nya_http_request_reflect(exchange->request, exchange->arena, &ROOM_QUERY_MODEL, &query).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    RoomReply reply = { .room = query.room, .doubled = query.room * 2 };
    (void)snprintf(reply.label, sizeof(reply.label), "room");

    return nya_http_response_reflect(exchange->response, exchange->arena, &ROOM_REPLY_MODEL, &reply).ok ? NYA_HTTP_STATUS_OK
                                                                                                        : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Always refuses, with a problem body, so the client's non-2xx path has a real refusal to read. */
static NYA_HttpStatus room_missing(NYA_HttpExchange* exchange) {
    return nya_http_response_problem(exchange, NYA_HTTP_STATUS_NOT_FOUND, "no such room");
}

/* the table: the client calls a route through one of these entries */

enum {
    ROUTE_QUERY   = 0,
    ROUTE_MISSING = 1,
};

static const NYA_HttpRoute ROUTES[] = {
    [ROUTE_QUERY] = {
        .method        = NYA_HTTP_METHOD_QUERY,
        .path          = "/api/room",
        .handler       = room_query,
        .summary       = "Double a room number",
        .request_type  = &ROOM_QUERY_MODEL,
        .response_type = &ROOM_REPLY_MODEL,
        .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
    },
    [ROUTE_MISSING] = {
        .method        = NYA_HTTP_METHOD_QUERY,
        .path          = "/api/missing",
        .handler       = room_missing,
        .summary       = "Always answers not found",
        .request_type  = &ROOM_QUERY_MODEL,
        .response_type = &ROOM_REPLY_MODEL,
        .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_NOT_FOUND, NYA_HTTP_STATUS_INTERNAL_ERROR },
    },
};

static const NYA_HttpRouter ROUTER = {
    .name        = "room",
    .routes      = ROUTES,
    .route_count = nya_carray_length(ROUTES),
};

/* the loopback transport: a real dispatch through the table, no socket */

static NYA_Error loopback_perform(void* userdata, const NYA_HttpClientWire* wire, NYA_Arena* arena, NYA_HttpClientReply* out_reply) {
    nya_unused(userdata);

    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
    nya_assert(request != nullptr);
    *request = (NYA_HttpRequest){ .method = wire->method, .keep_alive = true, .media_type = wire->content_type };

    // the path as the parser would leave it, so router_find matches: a full URL when the base carried an authority, a bare target when it did not.
    NYA_UrlFailure failure  = { 0 };
    b8             has_authority = strstr(wire->url, "://") != nullptr;
    NYA_Error      parsed        = has_authority ? nya_url_parse(wire->url, strlen(wire->url), &request->target, &failure)
                                                 : nya_url_parse_target(wire->url, strlen(wire->url), &request->target, &failure);
    NYA_EXPECT(parsed, "loopback URL");
    (void)snprintf(request->path, sizeof(request->path), "%.*s", (s32)request->target.path.length, request->target.text + request->target.path.offset);

    if (wire->body != nullptr && wire->body_size > 0) {
        nya_assert(wire->body_size <= NYA_HTTP_MAX_BODY_BYTES);
        memcpy(request->body, wire->body, wire->body_size);
        request->body[wire->body_size] = '\0';
        request->body_size             = wire->body_size;
    }

    u8*             response_body = nya_arena_alloc(arena, NYA_HTTP_MAX_RESPONSE_BYTES);
    NYA_HttpResponse response      = { 0 };
    nya_http_response_create(&response, response_body, NYA_HTTP_MAX_RESPONSE_BYTES);

    const NYA_HttpRouter* routers[] = { &ROUTER };

    NYA_HttpExchange exchange = {
        .request  = request,
        .response = &response,
        .arena    = arena,
        .now_s    = 1700000000ULL,
    };

    NYA_HttpStatus status = nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), nullptr, 0);

    *out_reply = (NYA_HttpClientReply){
        .status     = status,
        .body       = response.body,
        .body_size  = response.body_size,
        .media_type = response.media_type,
    };

    return NYA_OK;
}

/* a canned transport: returns exactly the bytes it is handed, for the decode-half tests */

typedef struct {
    NYA_HttpStatus    status;
    NYA_ConstCString  body;
    NYA_HttpMediaType media_type;
} Canned;

static NYA_Error canned_perform(void* userdata, const NYA_HttpClientWire* wire, NYA_Arena* arena, NYA_HttpClientReply* out_reply) {
    nya_unused(wire);
    nya_unused(arena);

    const Canned* canned = userdata;

    *out_reply = (NYA_HttpClientReply){
        .status     = canned->status,
        .body       = (const u8*)canned->body,
        .body_size  = canned->body != nullptr ? strlen(canned->body) : 0,
        .media_type = canned->media_type,
    };

    return NYA_OK;
}

/** Fails every round trip, as a refused connection would, to prove a transport error is propagated. */
static NYA_Error broken_perform(void* userdata, const NYA_HttpClientWire* wire, NYA_Arena* arena, NYA_HttpClientReply* out_reply) {
    nya_unused(userdata);
    nya_unused(wire);
    nya_unused(arena);
    nya_unused(out_reply);

    return nya_error(NYA_ERROR_TIMEOUT, "connection refused");
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_client");
    defer      nya_arena_destroy(arena);

    nya_assert(nya_http_router_check(&ROUTER).ok, "the table the client calls through is one the server serves");

    const NYA_HttpClientTransport loopback = { .perform = loopback_perform };

    // TEST: a typed call round-trips through the real dispatch, fields intact.
    {
        RoomQuery           request = { .room = 21 };
        RoomReply           reply   = { 0 };
        NYA_HttpClientResult result  = { 0 };

        NYA_Error status = nya_http_client_call(arena, &loopback, "", &ROUTES[ROUTE_QUERY], &request, &reply, &result);

        nya_assert(status.ok, "the call succeeded: %s", (NYA_ConstCString)status.message);
        nya_assert(result.status == NYA_HTTP_STATUS_OK);
        nya_assert(reply.room == 21, "the room came back as it went");
        nya_assert(reply.doubled == 42, "the handler's arithmetic is what the client reads");
        nya_assert(nya_string_equals(reply.label, "room"), "a string field round-trips too");
        nya_assert(!result.problem_parsed, "a success carries no problem");
    }

    // TEST: a base URL with an authority and a trailing slash joins cleanly to the path.
    {
        RoomQuery request = { .room = 5 };
        RoomReply reply   = { 0 };

        // The loopback reads only the path out of the URL, so a real-looking base proves the join, not the transport: "http://localhost:8080/" + "/api/room" must not double the slash.
        nya_assert(nya_http_client_call(arena, &loopback, "http://localhost:8080/", &ROUTES[ROUTE_QUERY], &request, &reply, nullptr).ok);
        nya_assert(reply.doubled == 10);
    }

    // TEST: a non-2xx is an error, and carries the server's problem, never a mis-parsed DTO.
    {
        RoomQuery            request = { .room = 1 };
        RoomReply            reply   = { .room = -7, .doubled = -7 };
        NYA_HttpClientResult result  = { 0 };

        NYA_Error status = nya_http_client_call(arena, &loopback, "", &ROUTES[ROUTE_MISSING], &request, &reply, &result);

        nya_assert(!status.ok, "a 404 is a failed call");
        nya_assert(result.status == NYA_HTTP_STATUS_NOT_FOUND);
        nya_assert(result.problem_parsed, "the refusal's body parsed as a problem");
        nya_assert(result.problem.status == NYA_HTTP_STATUS_NOT_FOUND, "the problem repeats the status");
        nya_assert(reply.room == -7 && reply.doubled == -7, "the response DTO was left untouched by the refusal");
    }

    // TEST: a 2xx body that does not parse is a clean parse error, not a half-written DTO.
    {
        Canned                        canned    = { .status = NYA_HTTP_STATUS_OK, .body = "{ this is not json", .media_type = NYA_HTTP_MEDIA_JSON };
        const NYA_HttpClientTransport transport = { .perform = canned_perform, .userdata = &canned };

        RoomQuery request = { .room = 3 };
        RoomReply reply   = { .room = 99 };

        NYA_Error status = nya_http_client_call(arena, &transport, "", &ROUTES[ROUTE_QUERY], &request, &reply, nullptr);

        nya_assert(!status.ok, "a body that is not a document fails");
        nya_assert(status.kind == NYA_ERROR_PARSE, "and it fails as a parse error, got %s", NYA_ERRORKIND_NAME_MAP[status.kind]);
        nya_assert(reply.room == 99, "nothing was written over the caller's DTO");
    }

    // TEST: an empty 2xx body where a DTO was expected is a parse error.
    {
        Canned                        canned    = { .status = NYA_HTTP_STATUS_OK, .body = "", .media_type = NYA_HTTP_MEDIA_JSON };
        const NYA_HttpClientTransport transport = { .perform = canned_perform, .userdata = &canned };

        RoomQuery request = { .room = 3 };
        RoomReply reply   = { 0 };

        nya_assert(!nya_http_client_call(arena, &transport, "", &ROUTES[ROUTE_QUERY], &request, &reply, nullptr).ok, "no body, no DTO");
    }

    // TEST: a non-2xx with no parseable problem body is still surfaced as an error.
    {
        Canned                        canned    = { .status = NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, .body = nullptr, .media_type = NYA_HTTP_MEDIA_NONE };
        const NYA_HttpClientTransport transport = { .perform = canned_perform, .userdata = &canned };

        RoomQuery            request = { .room = 3 };
        RoomReply            reply   = { 0 };
        NYA_HttpClientResult result  = { 0 };

        nya_assert(!nya_http_client_call(arena, &transport, "", &ROUTES[ROUTE_QUERY], &request, &reply, &result).ok);
        nya_assert(result.status == NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "the status is reported even without a problem body");
        nya_assert(!result.problem_parsed);
    }

    // TEST: a transport failure that never got a reply is the transport's error, propagated.
    {
        const NYA_HttpClientTransport transport = { .perform = broken_perform };

        RoomQuery request = { .room = 3 };
        RoomReply reply   = { 0 };

        NYA_Error status = nya_http_client_call(arena, &transport, "", &ROUTES[ROUTE_QUERY], &request, &reply, nullptr);
        nya_assert(!status.ok && status.kind == NYA_ERROR_TIMEOUT, "the transport's own error reaches the caller");
    }

    // TEST: a request that does not match the route it names is refused before any send.
    {
        RoomReply reply = { 0 };

        // the route declares a request body, but none was handed over.
        nya_assert(!nya_http_client_call(arena, &loopback, "", &ROUTES[ROUTE_QUERY], nullptr, &reply, nullptr).ok, "a missing request DTO is caught");

        // the route answers with a body, but nowhere was given to put it.
        RoomQuery request = { .room = 3 };
        nya_assert(!nya_http_client_call(arena, &loopback, "", &ROUTES[ROUTE_QUERY], &request, nullptr, nullptr).ok, "a missing response slot is caught");
    }

    printf("PASSED: http client\n");

    return EXIT_SUCCESS;
}

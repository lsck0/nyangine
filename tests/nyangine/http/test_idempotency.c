/**
 * The idempotency layer: a retried unsafe request runs once, a concurrent duplicate is refused, a key
 * reused for a different body is refused, and an entry expires.
 *
 * Driven through nya_http_router_dispatch with the layer installed, like test_log.c: a route table and
 * an exchange are data, and what is under test is the store rather than the socket. A side-effect
 * counter stands in for the write a real handler does, so "the handler ran once" is an assertion on a
 * number. The clock is the exchange's own `now_s`, stepped by the test, so the TTL is exercised exactly
 * rather than by sleeping — the same shape as base_circuit.c's `_at` entry points, met from the layer.
 *
 * The 409 (a concurrent duplicate) is produced without a second thread: the handler, while it is still
 * in flight, dispatches the same key again from inside itself, which is exactly the state a duplicate
 * arriving on another worker would find.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

#define NOW_S 1700000000ULL
#define TTL_S 10ULL

/* THE HANDLER AND ITS SIDE EFFECT */

/** How many times the real handler has run. The whole point of the layer is that a replay does not move it. */
static u32 SIDE_EFFECTS = 0;

/** Set by a test that wants the handler to re-enter with the same key while it is in flight; see below. */
static b8             REENTER          = false;
static NYA_HttpStatus REENTER_STATUS   = NYA_HTTP_STATUS_NONE;
static b8             REENTERED_ALREADY = false;

/** Set by a test that wants the handler to fail as the server's own trouble, after it has done its write. */
static NYA_HttpStatus FAIL_WITH = NYA_HTTP_STATUS_NONE;

/** Set by the durable tests: the handler's write also lands as a row here, inside whatever transaction the layer opened. */
static NYA_Database* NOTES_DATABASE = nullptr;

static NYA_HttpStatus dispatch_note(NYA_Arena* arena, NYA_ConstCString body, NYA_ConstCString key, u64 now_s, OUT NYA_HttpResponse* out_response);

/** Rows the handler has written to NOTES_DATABASE that are still there. */
static u64 notes_stored(NYA_Arena* arena) {
    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(NOTES_DATABASE, arena, "SELECT COUNT(*) AS n FROM notes", nullptr, 0, &result));

    return (u64)nya_object_get(result.rows->items[0], "n")->as_s64;
}

/** Writes a note-shaped body and answers 201, counting the write. The create a replay must not repeat. */
static NYA_HttpStatus create_note(NYA_HttpExchange* exchange) {
    SIDE_EFFECTS++;

    if (NOTES_DATABASE != nullptr) NYA_EXPECT(nya_sql_exec(NOTES_DATABASE, "INSERT INTO notes (text) VALUES ('once')"));
    if (FAIL_WITH != NYA_HTTP_STATUS_NONE) return nya_http_response_problem(exchange, FAIL_WITH, "the disk is full");

    /* While this one is in flight its key is reserved, so a duplicate that arrives now is the 409 case. Re-entering here is how the test reaches that state without a second thread: the nested dispatch carries the same key and must be refused, and its handler must not run. */
    if (REENTER && !REENTERED_ALREADY) {
        REENTERED_ALREADY = true;

        NYA_Arena* nested = nya_arena_create(.name = "test_idempotency_nested");
        defer      nya_arena_destroy(nested);

        NYA_HttpResponse nested_response = { 0 };
        REENTER_STATUS = dispatch_note(nested, "{\"text\":\"dup\"}", "concurrent", exchange->now_s, &nested_response);
        nya_http_response_destroy(&nested_response);
    }

    NYA_ConstCString stored = "{\"id\":1,\"text\":\"once\"}";

    return nya_http_response_text(exchange->response, stored, NYA_HTTP_MEDIA_JSON).ok ? NYA_HTTP_STATUS_CREATED : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** A safe read, so the layer must let every one of them through with no dedup. */
static NYA_HttpStatus read_notes(NYA_HttpExchange* exchange) {
    SIDE_EFFECTS++;

    return nya_http_response_text(exchange->response, "{\"count\":0}", NYA_HTTP_MEDIA_JSON).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

#define NOTES_PATH "/api/notes"

static const NYA_HttpRoute ROUTES[] = {
    {
     .method   = NYA_HTTP_METHOD_POST,
     .path     = NOTES_PATH,
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = create_note,
     .summary  = "Writes a note, once per Idempotency-Key",
     .statuses = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_CONFLICT,
                     NYA_HTTP_STATUS_UNPROCESSABLE, NYA_HTTP_STATUS_INTERNAL_ERROR, NYA_HTTP_STATUS_SERVICE_UNAVAILABLE },
     },
    {
     .method   = NYA_HTTP_METHOD_QUERY,
     .path     = NOTES_PATH,
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = read_notes,
     .summary  = "Reads notes, which the layer never dedupes",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

static const NYA_HttpRouter ROUTER = {
    .name        = "idempotency",
    .routes      = ROUTES,
    .route_count = nya_carray_length(ROUTES),
};

/* ONE EXCHANGE */

/** Builds a request by hand: the parser has its own test and this one is about what runs after it. */
static void make_request(OUT NYA_HttpRequest* request, NYA_HttpMethod method, NYA_ConstCString body, NYA_ConstCString key) {
    *request = (NYA_HttpRequest){ .method = method, .keep_alive = true };

    NYA_UrlFailure failure = { 0 };
    NYA_EXPECT(nya_url_parse_target(NOTES_PATH, strlen(NOTES_PATH), &request->target, &failure), "while building a request");

    (void)snprintf(request->path, sizeof(request->path), "%.*s", (int)request->target.path.length, request->target.text + request->target.path.offset);

    if (key != nullptr) {
        (void)snprintf(request->headers[request->header_count].name, sizeof(request->headers[0].name), "idempotency-key");
        (void)snprintf(request->headers[request->header_count].value, sizeof(request->headers[0].value), "%s", key);
        request->header_count++;
    }

    if (body == nullptr) return;

    request->media_type = NYA_HTTP_MEDIA_JSON;
    request->body_size  = strlen(body);

    nya_assert(request->body_size < sizeof(request->body));
    nya_memcpy(request->body, body, request->body_size + 1);
}

/** One dispatch through the idempotency layer, at time `now_s`, leaving the answer in `out_response`. */
static NYA_HttpStatus dispatch_note(NYA_Arena* arena, NYA_ConstCString body, NYA_ConstCString key, u64 now_s, OUT NYA_HttpResponse* out_response) {
    NYA_HttpRequest request = { 0 };
    make_request(&request, NYA_HTTP_METHOD_POST, body, key);

    u8* buffer = nya_arena_alloc(arena, NYA_HTTP_MAX_BODY_BYTES + 1);
    nya_http_response_create(out_response, buffer, NYA_HTTP_MAX_BODY_BYTES + 1);

    const NYA_HttpRouter* routers[] = { &ROUTER };
    const NYA_HttpLayerFn layers[]  = { nya_http_layer_idempotency };

    NYA_HttpExchange exchange = {
        .request  = &request,
        .response = out_response,
        .arena    = arena,
        .now_s    = now_s,
        .address  = "203.0.113.7",
    };

    return nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), layers, nya_carray_length(layers));
}

/** A QUERY dispatch, for the safe-method path. */
static NYA_HttpStatus dispatch_read(NYA_Arena* arena, NYA_ConstCString key, u64 now_s, OUT NYA_HttpResponse* out_response) {
    NYA_HttpRequest request = { 0 };
    make_request(&request, NYA_HTTP_METHOD_QUERY, "{}", key);

    u8* buffer = nya_arena_alloc(arena, NYA_HTTP_MAX_BODY_BYTES + 1);
    nya_http_response_create(out_response, buffer, NYA_HTTP_MAX_BODY_BYTES + 1);

    const NYA_HttpRouter* routers[] = { &ROUTER };
    const NYA_HttpLayerFn layers[]  = { nya_http_layer_idempotency };

    NYA_HttpExchange exchange = { .request = &request, .response = out_response, .arena = arena, .now_s = now_s, .address = "203.0.113.7" };

    return nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), layers, nya_carray_length(layers));
}

/** Two null-terminated names equal, ignoring ASCII case. A header name is compared this way. */
static b8 name_equals(NYA_ConstCString a, NYA_ConstCString b) {
    u64 index = 0;
    for (; a[index] != '\0' && b[index] != '\0'; index++) {
        char la = (a[index] >= 'A' && a[index] <= 'Z') ? (char)(a[index] + 32) : a[index];
        char lb = (b[index] >= 'A' && b[index] <= 'Z') ? (char)(b[index] + 32) : b[index];
        if (la != lb) return false;
    }

    return a[index] == '\0' && b[index] == '\0';
}

/** Whether the response carries `Idempotency-Replayed`, which marks an answer the layer served from store. */
static b8 marked_replay(const NYA_HttpResponse* response) {
    for (u32 index = 0; index < response->header_count; index++) {
        if (name_equals(response->headers[index].name, "Idempotency-Replayed")) return true;
    }

    return false;
}

/** The response body as a comparable C string. */
static void body_text(const NYA_HttpResponse* response, OUT char* out, u64 capacity) {
    u64 shown = response->body_size < capacity - 1 ? response->body_size : capacity - 1;
    nya_memcpy(out, response->body, shown);
    out[shown] = '\0';
}

/* THE TESTS */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_idempotency");
    defer      nya_arena_destroy(arena);

    NYA_EXPECT(nya_http_idempotency_init(arena, .ttl_s = TTL_S), "while readying the store");

    // TEST: a first keyed request runs and is stored; a retry replays it and does not run again.
    {
        nya_http_idempotency_reset();
        SIDE_EFFECTS = 0;

        NYA_HttpResponse first = { 0 };
        NYA_HttpStatus   s1    = dispatch_note(arena, "{\"text\":\"once\"}", "key-a", NOW_S, &first);

        nya_check(s1 == NYA_HTTP_STATUS_CREATED, "the first keyed request runs and answers 201");
        nya_check(SIDE_EFFECTS == 1, "the handler ran exactly once");
        nya_check(!marked_replay(&first), "the first answer is not marked a replay");
        nya_check(nya_http_idempotency_count() == 1, "one key is held");

        char first_body[64] = { 0 };
        body_text(&first, first_body, sizeof(first_body));

        NYA_HttpResponse replay = { 0 };
        NYA_HttpStatus   s2     = dispatch_note(arena, "{\"text\":\"once\"}", "key-a", NOW_S, &replay);

        nya_check(s2 == NYA_HTTP_STATUS_CREATED, "the retry answers the stored 201");
        nya_check(SIDE_EFFECTS == 1, "the retry did NOT run the handler again");
        nya_check(marked_replay(&replay), "the retry is marked a replay");

        char replay_body[64] = { 0 };
        body_text(&replay, replay_body, sizeof(replay_body));
        nya_check(strcmp(first_body, replay_body) == 0, "the replay body matches the first answer");

        nya_http_response_destroy(&first);
        nya_http_response_destroy(&replay);
    }

    // TEST: a duplicate that arrives while the first is in flight is refused with 409.
    {
        nya_http_idempotency_reset();
        SIDE_EFFECTS      = 0;
        REENTER           = true;
        REENTERED_ALREADY = false;
        REENTER_STATUS    = NYA_HTTP_STATUS_NONE;

        NYA_HttpResponse response = { 0 };
        NYA_HttpStatus   status   = dispatch_note(arena, "{\"text\":\"dup\"}", "concurrent", NOW_S, &response);

        REENTER = false;

        nya_check(status == NYA_HTTP_STATUS_CREATED, "the first of the pair completes");
        nya_check(REENTER_STATUS == NYA_HTTP_STATUS_CONFLICT, "the in-flight duplicate is refused 409");
        nya_check(SIDE_EFFECTS == 1, "the duplicate did not run the handler");

        nya_http_response_destroy(&response);
    }

    // TEST: the same key with a different body is a client bug, refused 422, never the wrong cached answer.
    {
        nya_http_idempotency_reset();
        SIDE_EFFECTS = 0;

        NYA_HttpResponse first = { 0 };
        (void)dispatch_note(arena, "{\"text\":\"first\"}", "key-b", NOW_S, &first);
        nya_check(SIDE_EFFECTS == 1, "the first body ran");

        NYA_HttpResponse second = { 0 };
        NYA_HttpStatus   status = dispatch_note(arena, "{\"text\":\"changed\"}", "key-b", NOW_S, &second);

        nya_check(status == NYA_HTTP_STATUS_UNPROCESSABLE, "a key reused with a different body is 422");
        nya_check(SIDE_EFFECTS == 1, "the mismatched request did not run the handler");

        nya_http_response_destroy(&first);
        nya_http_response_destroy(&second);
    }

    // TEST: an entry expires after the TTL, so the same key runs again rather than replaying forever.
    {
        nya_http_idempotency_reset();
        SIDE_EFFECTS = 0;

        NYA_HttpResponse first = { 0 };
        (void)dispatch_note(arena, "{\"text\":\"exp\"}", "key-c", NOW_S, &first);
        nya_check(SIDE_EFFECTS == 1, "the first request ran");
        nya_check(nya_http_idempotency_count() == 1, "the key is held");

        // still inside the TTL: a replay, not a re-run.
        NYA_HttpResponse within = { 0 };
        (void)dispatch_note(arena, "{\"text\":\"exp\"}", "key-c", NOW_S + TTL_S - 1, &within);
        nya_check(SIDE_EFFECTS == 1, "within the TTL it replays");
        nya_check(marked_replay(&within), "and is marked a replay");

        // past the TTL: the entry is gone, so the handler runs again and the answer is fresh.
        NYA_HttpResponse after = { 0 };
        (void)dispatch_note(arena, "{\"text\":\"exp\"}", "key-c", NOW_S + TTL_S + 1, &after);
        nya_check(SIDE_EFFECTS == 2, "past the TTL the expired entry frees and the handler runs again");
        nya_check(!marked_replay(&after), "the fresh answer is not a replay");

        nya_http_response_destroy(&first);
        nya_http_response_destroy(&within);
        nya_http_response_destroy(&after);
    }

    // TEST: an unsafe request with no key, and every safe request, pass straight through with no dedup.
    {
        nya_http_idempotency_reset();
        SIDE_EFFECTS = 0;

        NYA_HttpResponse a = { 0 };
        NYA_HttpResponse b = { 0 };
        (void)dispatch_note(arena, "{\"text\":\"x\"}", nullptr, NOW_S, &a);
        (void)dispatch_note(arena, "{\"text\":\"x\"}", nullptr, NOW_S, &b);

        nya_check(SIDE_EFFECTS == 2, "an unkeyed POST is never deduped");
        nya_check(nya_http_idempotency_count() == 0, "and nothing is stored for it");

        // a safe method with a key still runs every time: there is nothing about a read to dedupe.
        NYA_HttpResponse r1 = { 0 };
        NYA_HttpResponse r2 = { 0 };
        (void)dispatch_read(arena, "key-read", NOW_S, &r1);
        (void)dispatch_read(arena, "key-read", NOW_S, &r2);

        nya_check(SIDE_EFFECTS == 4, "a safe method passes through even with a key");
        nya_check(nya_http_idempotency_count() == 0, "and stores nothing");

        nya_http_response_destroy(&a);
        nya_http_response_destroy(&b);
        nya_http_response_destroy(&r1);
        nya_http_response_destroy(&r2);
    }

    // TEST: a malformed key is refused 400 rather than stored under a truncated or garbage name.
    {
        nya_http_idempotency_reset();
        SIDE_EFFECTS = 0;

        // a space is not printable-token, so this is a header this server would never have sent.
        NYA_HttpResponse bad = { 0 };
        NYA_HttpStatus   status = dispatch_note(arena, "{\"text\":\"x\"}", "has a space", NOW_S, &bad);

        nya_check(status == NYA_HTTP_STATUS_BAD_REQUEST, "a non-printable key is 400");
        nya_check(SIDE_EFFECTS == 0, "and the handler never ran");
        nya_check(nya_http_idempotency_count() == 0, "and nothing was stored");

        nya_http_response_destroy(&bad);
    }

    // TEST: a 5xx is the server's trouble and is not stored, so the retry after it clears runs the handler.
    {
        nya_http_idempotency_reset();
        SIDE_EFFECTS = 0;

        FAIL_WITH                = NYA_HTTP_STATUS_SERVICE_UNAVAILABLE;
        NYA_HttpResponse failing = { 0 };
        nya_check(dispatch_note(arena, "{\"text\":\"x\"}", "key-outage", NOW_S, &failing) == NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "the outage answers 503");
        nya_check(nya_http_idempotency_count() == 0, "and holds no key for it");
        FAIL_WITH = NYA_HTTP_STATUS_NONE;

        NYA_HttpResponse healed = { 0 };
        nya_check(dispatch_note(arena, "{\"text\":\"x\"}", "key-outage", NOW_S, &healed) == NYA_HTTP_STATUS_CREATED, "the retry once it cleared is answered for real");
        nya_check(SIDE_EFFECTS == 2 && !marked_replay(&healed), "by running the handler again, not by replaying the 503");

        nya_http_response_destroy(&failing);
        nya_http_response_destroy(&healed);
    }

    // the durable store: the same layer over a database, where the answer and the handler's write commit together.
    nya_http_idempotency_deinit();

    NYA_EXPECT(nya_sql_open(arena, ":memory:", &NOTES_DATABASE));
    defer nya_sql_close(NOTES_DATABASE);
    NYA_EXPECT(nya_sql_exec(NOTES_DATABASE, "CREATE TABLE notes (id INTEGER PRIMARY KEY, text TEXT NOT NULL)"));
    NYA_EXPECT(nya_http_idempotency_init(arena, .ttl_s = TTL_S, .database = NOTES_DATABASE), "while readying the durable store");

    // TEST: a restart forgets the memory table, and the retry after it replays the stored answer without running the handler.
    {
        nya_http_idempotency_reset();
        SIDE_EFFECTS = 0;

        NYA_HttpResponse first = { 0 };
        nya_check(dispatch_note(arena, "{\"text\":\"once\"}", "key-durable", NOW_S, &first) == NYA_HTTP_STATUS_CREATED, "the first request runs");

        nya_http_idempotency_reset();
        nya_check(nya_http_idempotency_count() == 0, "the restart left nothing in memory");

        NYA_HttpResponse after = { 0 };
        nya_check(dispatch_note(arena, "{\"text\":\"once\"}", "key-durable", NOW_S, &after) == NYA_HTTP_STATUS_CREATED, "the retry after the restart answers 201");
        nya_check(SIDE_EFFECTS == 1 && marked_replay(&after), "as a replay, with the handler run once");
        nya_check(notes_stored(arena) == 1, "and one note written, got " FMTu64, notes_stored(arena));

        char first_body[64] = { 0 };
        char after_body[64] = { 0 };
        body_text(&first, first_body, sizeof(first_body));
        body_text(&after, after_body, sizeof(after_body));
        nya_check(strcmp(first_body, after_body) == 0, "the replay is the first answer, byte for byte");

        NYA_HttpResponse other = { 0 };
        nya_http_idempotency_reset();
        nya_check(dispatch_note(arena, "{\"text\":\"else\"}", "key-durable", NOW_S, &other) == NYA_HTTP_STATUS_UNPROCESSABLE, "a different body under the stored key is 422 after a restart too");

        nya_http_response_destroy(&first);
        nya_http_response_destroy(&after);
        nya_http_response_destroy(&other);
    }

    // TEST: a handler that fails as the server's own trouble has its write undone with it, and nothing is stored.
    {
        SIDE_EFFECTS = 0;
        u64 before   = notes_stored(arena);

        FAIL_WITH                = NYA_HTTP_STATUS_SERVICE_UNAVAILABLE;
        NYA_HttpResponse failing = { 0 };
        nya_check(dispatch_note(arena, "{\"text\":\"y\"}", "key-undone", NOW_S, &failing) == NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "the failure keeps its status");
        FAIL_WITH = NYA_HTTP_STATUS_NONE;

        nya_check(notes_stored(arena) == before, "and its half of the write is gone");

        NYA_HttpResponse healed = { 0 };
        nya_check(dispatch_note(arena, "{\"text\":\"y\"}", "key-undone", NOW_S, &healed) == NYA_HTTP_STATUS_CREATED && SIDE_EFFECTS == 2, "the retry runs for real");
        nya_check(notes_stored(arena) == before + 1, "and writes exactly once");

        nya_http_response_destroy(&failing);
        nya_http_response_destroy(&healed);
    }

    // gives the lock back to the arena before it is destroyed, and exercises deinit's own path.
    nya_http_idempotency_deinit();

    printf("test_idempotency: all assertions passed.\n");

    return nya_check_failures() == 0 ? 0 : 1;
}

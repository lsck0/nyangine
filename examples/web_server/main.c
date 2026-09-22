/**
 * @file examples/web_server/main.c
 *
 * An HTTP server: a resource of its own, typed requests and responses through reflection, and the
 * OpenAPI document generated from the route table rather than written.
 *
 * ```
 * ./build run example web_server            # serves on 127.0.0.1:47800 until interrupted
 * ./web_server.example --port 8080
 * ```
 *
 * Then, from another terminal:
 *
 * ```
 * curl -X QUERY localhost:47800/api/notes -H 'Content-Type: application/json' -d '{}'
 * curl -X QUERY localhost:47800/api/notes -H 'Accept: application/nya' -d '{}'   # the native format
 * curl -X QUERY localhost:47800/api/notes -H 'Accept: application/nya-binary' -d '{}' -o notes.bin
 * curl -X QUERY localhost:47800/api/metrics -H 'Accept: application/nya-binary' -d '{}' -o metrics.bin  # a DTO, typed
 * curl -X QUERY 'localhost:47800/api/notes?contains=first+note' -d '{}'         # only the notes containing it
 * curl -X POST  localhost:47800/api/notes -d '{"text":"the first note"}'
 * curl -X DELETE localhost:47800/api/notes -d '{"id":1}'
 * curl -X POST localhost:47800/api/otp/enrol    # the URI to scan, the secret to type, the recovery codes
 * # the three below want -H 'Content-Type: application/json', since a body without one is not a document
 * curl -X POST localhost:47800/api/otp/activate -d '{"code":"123456"}'   # the factor is off until this passes
 * curl -X POST localhost:47800/api/otp/verify   -d '{"code":"123456"}'   # 204, or 401, or 429
 * curl -X POST localhost:47800/api/otp/recover  -d '{"code":"ABCDEFGH-IJKLMNOP"}'
 * curl localhost:47800/docs                 # the generated page
 * curl localhost:47800/openapi.json         # the document it is generated from
 * curl -X QUERY localhost:47800/api/metrics -d '{}'
 * ```
 *
 * ## This is net_echo's sibling
 *
 * `examples/net_echo/main.c` says the web server did not exist when it was written, and that when it
 * did this example would be the one to copy for a web app. This is that one. The two are unrelated
 * pieces of the engine: net_echo is the game transport, encrypted UDP between a world and its
 * players; this is TCP, HTTP and a router, and they share nothing but the word server.
 *
 * ## Why these are NYA_Object and not reflected structs
 *
 * The server's nicest shape is a `// @reflect` DTO: `nya_http_request_reflect` fills one straight from
 * the body and `nya_http_response_reflect` renders one back, with the OpenAPI schema generated from
 * the same reflection. That is what `debug_metrics.c` does, and it is what a resource inside the engine
 * should do.
 *
 * It cannot be done here. The reflection preprocessor scans two trees, `src/nyangine` and `src/gnyame`,
 * and headers only — so a type declared in an example, or in anybody's program that merely links this
 * engine, has no reflection generated for it and `nya_reflect_of` does not resolve. See "Nyangine as a
 * dependency" in TODO.md: this is the same gap, met from the outside.
 *
 * So the bodies here are built and read as `NYA_Object`, which is the vocabulary type underneath the
 * reflected path anyway and needs no generation step. Everything else — the router, the verbs, the
 * layers, the generated document — is exactly what a reflected resource uses.
 *
 * ## QUERY rather than GET
 *
 * A read is a QUERY here. A request in this server is a reflected struct and a GET has nowhere to put
 * one, so the four verbs a resource is written in are QUERY, POST, PUT and DELETE; see
 * `http_router.h`. GET still works and `/docs` is one, because a browser has no other verb.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#include "SDL3/SDL_timer.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE RESOURCE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NOTES_PATH "/api/notes"

/** Notes held at once. A fixed ceiling, as everything in this engine has; see nya_ceiling_register. */
#define NOTES_MAX 64

/** Longest note kept, terminator included. */
#define NOTE_TEXT_MAX 256

/** One note. */
typedef struct {
    u32  id;
    char text[NOTE_TEXT_MAX];
    f64  written_at_s;
} ExampleNote;

/*
 * The notes themselves. Static rather than allocated, because this outlives every request and a
 * request's arena does not: an arena here is scratch for one exchange and is gone when it answers.
 */
static ExampleNote NOTES[NOTES_MAX] = { 0 };
static u32         NOTE_COUNT       = 0;
static u32         NEXT_ID          = 1;

/** One note as a document, which is what goes out over the wire. */
NYA_INTERNAL NYA_Value note_to_value(NYA_Arena* arena, const ExampleNote* note) {
    NYA_Object* object = nya_object_create(arena);

    nya_object_set(object, "id", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = note->id });
    nya_object_set(object, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)note->text });
    nya_object_set(object, "written_at_s", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = note->written_at_s });

    return (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *object };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Every note, or with `?contains=text` only the ones whose text contains it. */
NYA_INTERNAL NYA_HttpStatus notes_query(NYA_HttpExchange* exchange) {
    NYA_Object* body  = nya_object_create(exchange->arena);
    NYA_ArrayᐸNYA_Valueᐳ* notes = nya_array_create(exchange->arena, NYA_Value);

    /*
     * Decoded by the parser that read the target, so "first+note" and "first%20note" are both "first note".
     * Asked of the target directly: a repeated name or an overlong filter answers 400, where
     * nya_http_request_query_param would read either as no filter and answer with everything.
     */
    char contains[NOTE_TEXT_MAX] = { 0 };
    b8   filtered                = false;

    if (!nya_url_query_find(&exchange->request->target, "contains", contains, sizeof(contains), &filtered).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    for (u32 i = 0; i < NOTE_COUNT; i++) {
        if (filtered && strstr(NOTES[i].text, contains) == nullptr) continue;

        NYA_Value value = note_to_value(exchange->arena, &NOTES[i]);
        nya_array_push_back(notes, value);
    }

    nya_object_set(body, "count", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = notes->length });
    nya_object_set(body, "notes", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *notes });

    /*
     * In whatever the caller asked for. A nyangine client sends Accept: application/nya-binary (or
     * application/nya for text) and gets the native document; anything else gets JSON. The notes are
     * an NYA_Object rather than a DTO, so the binary answer is untyped.
     */
    const NYA_HttpMediaType answer = nya_http_request_accepts(exchange->request);

    if (!nya_http_response_document(exchange->response, exchange->arena, body, answer).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_OK;
}

/** Adds one, and answers with the note as it was stored so the caller learns its id. */
NYA_INTERNAL NYA_HttpStatus notes_post(NYA_HttpExchange* exchange) {
    // A body that is not an object at all is the caller's mistake and answers 400, not 500.
    NYA_Object* incoming = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &incoming).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_Value* text = nya_object_get(incoming, "text");
    if (text == nullptr || text->type != NYA_TYPE_STRING || text->as_string[0] == '\0') return NYA_HTTP_STATUS_BAD_REQUEST;

    // A full store is the caller asking for more than this server holds, not a server fault.
    if (NOTE_COUNT >= NOTES_MAX) return NYA_HTTP_STATUS_UNPROCESSABLE;

    ExampleNote* note = &NOTES[NOTE_COUNT];

    *note = (ExampleNote){ .id = NEXT_ID, .written_at_s = exchange->now_s };
    (void)snprintf(note->text, sizeof(note->text), "%s", text->as_string);

    NEXT_ID++;
    NOTE_COUNT++;

    NYA_Value stored = note_to_value(exchange->arena, note);
        const NYA_HttpMediaType answer = nya_http_request_accepts(exchange->request);

    if (!nya_http_response_document(exchange->response, exchange->arena, &stored.as_object, answer).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_CREATED;
}

/** Removes one by id. */
NYA_INTERNAL NYA_HttpStatus notes_delete(NYA_HttpExchange* exchange) {
    NYA_Object* incoming = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &incoming).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    /*
     * S64, not U64: serde parses every JSON integer as signed, because JSON does not say which it is.
     * Asking for U64 here refused every well formed delete, which is what running it found.
     */
    NYA_Value* id = nya_object_get(incoming, "id");
    if (id == nullptr || id->type != NYA_TYPE_S64 || id->as_s64 < 0) return NYA_HTTP_STATUS_BAD_REQUEST;

    for (u32 i = 0; i < NOTE_COUNT; i++) {
        if (NOTES[i].id != (u32)id->as_s64) continue;

        // Order is not promised, so the last one fills the hole rather than shifting the rest.
        NOTES[i] = NOTES[NOTE_COUNT - 1];
        NOTE_COUNT--;

        return NYA_HTTP_STATUS_NO_CONTENT;
    }

    return NYA_HTTP_STATUS_NOT_FOUND;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE SECOND FACTOR
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define OTP_ENROL_PATH    "/api/otp/enrol"
#define OTP_ACTIVATE_PATH "/api/otp/activate"
#define OTP_VERIFY_PATH   "/api/otp/verify"
#define OTP_RECOVER_PATH  "/api/otp/recover"

/**
 * The one account this example has a factor for.
 *
 * The engine holds none of this: http_totp.h takes the secret and the guard as arguments and hands
 * them straight back, precisely so that where an account lives is the program's business. In a real
 * server these fields are columns on a user row; there is no user store in this engine yet, which is
 * why they are a struct here. See TODO.md, "Web".
 *
 * `active` is the second half of RFC 6238 enrolment: a secret exists from the moment /enrol answers,
 * and it is not a factor until one code has been verified against it at /activate. Until then losing
 * the QR code costs nothing, because nothing has been turned on.
 * */
typedef struct {
    b8                       enrolled;
    b8                       active;
    NYA_CryptoTotpSecret     secret;
    NYA_HttpTotpRecoveryHash recovery[NYA_HTTP_TOTP_RECOVERY_CODES];
    NYA_HttpTotpGuard        guard;
} ExampleFactor;

static ExampleFactor FACTOR = { 0 };

/** The code out of a request body, or null when the body is not `{"code":"..."}`. */
NYA_INTERNAL NYA_ConstCString otp_submitted_code(NYA_HttpExchange* exchange) {
    NYA_Object* incoming = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &incoming).ok) return nullptr;

    NYA_Value* code = nya_object_get(incoming, "code");
    if (code == nullptr || code->type != NYA_TYPE_STRING) return nullptr;

    return (NYA_ConstCString)code->as_string;
}

/**
 * One verdict as a status.
 *
 * A refusal is 401 whatever caused it — a wrong code, a replayed one, or a factor that was never
 * turned on — because a caller that could tell those apart could tell which accounts have a factor
 * and which of its guesses had once been real. Only the limit answers differently, and it has to:
 * 429 is not a statement about the code, it is a statement about how often this one asked.
 * */
NYA_INTERNAL NYA_HttpStatus otp_status(NYA_HttpTotpVerdict verdict) {
    switch (verdict) {
        case NYA_HTTP_TOTP_ACCEPTED: return NYA_HTTP_STATUS_NO_CONTENT;
        case NYA_HTTP_TOTP_RATE_LIMITED: return NYA_HTTP_STATUS_TOO_MANY_REQUESTS;
        case NYA_HTTP_TOTP_REFUSED:
        default: return NYA_HTTP_STATUS_UNAUTHORIZED;
    }
}

/**
 * Starts an enrolment and answers with everything that is only ever shown once.
 *
 * The URI is what a QR code on a real page would encode; here it goes out as text, because this
 * example has no UI. Every field in the answer is the secret or is derived from it, which is why the
 * enrolment is wiped before this returns and why a real deployment serves this over TLS alone.
 * */
NYA_INTERNAL NYA_HttpStatus otp_enrol(NYA_HttpExchange* exchange) {
    NYA_HttpTotpEnrolment enrolment = { 0 };
    if (!nya_http_totp_enrol_create("nyangine web_server", "luca", &enrolment).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    defer nya_http_totp_enrol_destroy(&enrolment);

    NYA_Object* body = nya_object_create(exchange->arena);
    NYA_ArrayᐸNYA_Valueᐳ* codes = nya_array_create(exchange->arena, NYA_Value);

    for (u32 i = 0; i < NYA_HTTP_TOTP_RECOVERY_CODES; i++) {
        // copied into the response's arena: the enrolment itself is wiped on the way out of here.
        NYA_CString text = nya_string_to_cstring(exchange->arena, nya_string_from(exchange->arena, enrolment.recovery[i]));
        nya_array_push_back(codes, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = text }));
    }

    nya_object_set(body, "uri", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)enrolment.uri });
    nya_object_set(body, "secret", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)enrolment.secret_base32 });
    nya_object_set(body, "recovery", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *codes });

    // the factor is not on yet: /activate has to see one code first.
    FACTOR = (ExampleFactor){ .enrolled = true, .secret = enrolment.secret };
    for (u32 i = 0; i < NYA_HTTP_TOTP_RECOVERY_CODES; i++) FACTOR.recovery[i] = enrolment.recovery_hash[i];

    if (!nya_http_response_document(exchange->response, exchange->arena, body, nya_http_request_accepts(exchange->request)).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_CREATED;
}

/** Turns the pending enrolment on, once one code proves the authenticator holds the same secret. */
NYA_INTERNAL NYA_HttpStatus otp_activate(NYA_HttpExchange* exchange) {
    NYA_ConstCString code = otp_submitted_code(exchange);
    if (code == nullptr) return NYA_HTTP_STATUS_BAD_REQUEST;

    // no enrolment is the same refusal as a wrong code; see otp_status.
    if (!FACTOR.enrolled) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_HttpTotpVerdict verdict = nya_http_totp_verify(&FACTOR.guard, &FACTOR.secret, code, exchange->now_s);
    if (verdict == NYA_HTTP_TOTP_ACCEPTED) FACTOR.active = true;

    return otp_status(verdict);
}

/** One code against the factor that is on. What a login route will call once there is one. */
NYA_INTERNAL NYA_HttpStatus otp_verify(NYA_HttpExchange* exchange) {
    NYA_ConstCString code = otp_submitted_code(exchange);
    if (code == nullptr) return NYA_HTTP_STATUS_BAD_REQUEST;

    if (!FACTOR.active) return NYA_HTTP_STATUS_UNAUTHORIZED;

    return otp_status(nya_http_totp_verify(&FACTOR.guard, &FACTOR.secret, code, exchange->now_s));
}

/** One recovery code, for the phone that is gone. Spent by the call, so it works exactly once. */
NYA_INTERNAL NYA_HttpStatus otp_recover(NYA_HttpExchange* exchange) {
    NYA_ConstCString code = otp_submitted_code(exchange);
    if (code == nullptr) return NYA_HTTP_STATUS_BAD_REQUEST;

    if (!FACTOR.active) return NYA_HTTP_STATUS_UNAUTHORIZED;

    return otp_status(nya_http_totp_recovery_redeem(&FACTOR.guard, FACTOR.recovery, nya_carray_length(FACTOR.recovery), code, exchange->now_s));
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE ROUTE TABLE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Data, not code. The OpenAPI document and the /docs page are generated by walking this, so a route
 * that is served and a route that is documented cannot drift apart: there is one of them.
 *
 * `statuses` is the full set each route may answer with. A debug build asserts a handler never
 * returns one it did not declare, which is why the refusals above are listed alongside the successes.
 */
NYA_INTERNAL const NYA_HttpRoute NOTE_ROUTES[] = {
    {
     .method        = NYA_HTTP_METHOD_QUERY,
     .path          = NOTES_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
     .handler       = notes_query,
     .summary       = "Every note",
     .description   = "A read, and therefore a QUERY rather than a GET. `?contains=text` keeps the notes containing it.",
     .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method        = NYA_HTTP_METHOD_POST,
     .path          = NOTES_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
     .handler       = notes_post,
     .summary       = "Writes a note",
     .description   = "Answers with the note as stored, so the caller learns the id it was given.",
     .statuses      = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_UNPROCESSABLE,
                           NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method       = NYA_HTTP_METHOD_DELETE,
     .path         = NOTES_PATH,
     .auth         = NYA_HTTP_AUTH_NONE,
     .handler      = notes_delete,
     .summary      = "Removes a note by id",
     .statuses     = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_NOT_FOUND },
     },
};

NYA_INTERNAL const NYA_HttpRouter NOTE_ROUTER = {
    .name        = "notes",
    .routes      = NOTE_ROUTES,
    .route_count = nya_carray_length(NOTE_ROUTES),
};

/*
 * The second factor's own table. A separate router rather than four more rows above, because these
 * are not a resource: they are three steps of one flow, and a program that wants notes without a
 * factor merges one and not the other.
 */
NYA_INTERNAL const NYA_HttpRoute OTP_ROUTES[] = {
    {
     .method      = NYA_HTTP_METHOD_POST,
     .path        = OTP_ENROL_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .handler     = otp_enrol,
     .summary     = "Starts enrolling an authenticator",
     .description = "Answers with the otpauth URI to scan, the secret as base32 to type, and the recovery codes. All three are "
                        "shown once and never again, and the factor is not on until a code is posted to /api/otp/activate.",
     .statuses    = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method      = NYA_HTTP_METHOD_POST,
     .path        = OTP_ACTIVATE_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .handler     = otp_activate,
     .summary     = "Turns the enrolment on",
     .description = "`{\"code\":\"123456\"}`. The second step of enrolment: proves the authenticator holds the same secret before "
                        "anything depends on it.",
     .statuses    = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN,
                          NYA_HTTP_STATUS_TOO_MANY_REQUESTS },
     },
    {
     .method      = NYA_HTTP_METHOD_POST,
     .path        = OTP_VERIFY_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .handler     = otp_verify,
     .summary     = "Answers one code",
     .description = "`{\"code\":\"123456\"}`. One step of clock skew either side is accepted, and a code works once: the same "
                        "digits inside the same thirty seconds are refused the second time.",
     .statuses    = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN,
                          NYA_HTTP_STATUS_TOO_MANY_REQUESTS },
     },
    {
     .method      = NYA_HTTP_METHOD_POST,
     .path        = OTP_RECOVER_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .handler     = otp_recover,
     .summary     = "Spends one recovery code",
     .description = "`{\"code\":\"ABCDEFGH-IJKLMNOP\"}`, for the phone that is gone. Case and the dash do not matter; the code "
                        "does, and it works exactly once.",
     .statuses    = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN,
                          NYA_HTTP_STATUS_TOO_MANY_REQUESTS },
     },
};

NYA_INTERNAL const NYA_HttpRouter OTP_ROUTER = {
    .name        = "second factor",
    .routes      = OTP_ROUTES,
    .route_count = nya_carray_length(OTP_ROUTES),
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE PROGRAM
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Default port. Loopback only unless `address` is set; see NYA_HttpConfig. */
#define DEFAULT_PORT 47800

/** How long the loop sleeps between drains. The server does no work of its own between requests. */
#define TICK_SLEEP_MS 5

/** Set by the signal handler, so ctrl-c leaves through the same shutdown a clean exit does. */
static volatile sig_atomic_t RUNNING = 1;

static void stop(int signal_number) {
    nya_unused(signal_number);
    RUNNING = 0;
}

s32 main(s32 argc, char** argv) {
    u16 port = DEFAULT_PORT;

    // Deliberately not base_args: one option, and the point of the file is the server.
    for (s32 i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--port") != 0) continue;

        // atoi would turn "80a" into 80 and "http" into 0, serving on a port nobody asked for
        NYA_ConstCString text = argv[i + 1];
        if (!nya_type_parse(NYA_TYPE_U16, (const u8*)text, strlen(text), &port)) {
            nya_log_error("--port expects a number from 0 to 65535, got '%s'.", text);
            return EXIT_FAILURE;
        }
    }

    nya_log_level_set(NYA_LOG_LEVEL_INFO);

    (void)signal(SIGINT, stop);

    /*
     * No window, no renderer, no world. An HTTP server needs none of the engine's frame, which is
     * what makes a dedicated one of these deployable as a plain binary.
     */
    NYA_Error started = nya_system_http_init((NYA_HttpConfig){ .port = port });

    if (!started.ok) {
        u8 message[256];
        (void)nya_error_format(&started, message, sizeof(message));
        nya_log_error("Could not start the server: %s", (NYA_ConstCString)message);

        return EXIT_FAILURE;
    }

    defer nya_system_http_deinit();

    // Merged at the root.
    NYA_EXPECT(nya_http_server_merge(&NOTE_ROUTER), "while merging the notes resource");
    defer nya_http_server_unmerge(&NOTE_ROUTER);

    /*
     * The TOTP second factor. Merged here and not inside the engine for the reason the notes are: the
     * secret and the guard belong to whoever owns the account, which is this program.
     */
    NYA_EXPECT(nya_http_server_merge(&OTP_ROUTER), "while merging the second factor");
    defer nya_http_server_unmerge(&OTP_ROUTER);

    /*
     * The engine's own metrics, which are reflected DTOs: asked for as application/nya-binary they go
     * out carrying their layout hash, the typed half the notes above cannot show. The server does not
     * merge them on its own: without this line /api/metrics is a 404.
     */
    NYA_EXPECT(nya_http_server_merge(nya_http_metrics_router()), "while merging the metrics resource");
    defer nya_http_server_unmerge(nya_http_metrics_router());

    /*
     * /openapi.json and /docs, generated by walking every merged route table. Opt in rather than
     * automatic: a server that does not want to publish its own shape should not have to unmerge it.
     */
    NYA_EXPECT(nya_http_server_merge(nya_http_openapi_router()), "while merging the generated document");
    defer nya_http_server_unmerge(nya_http_openapi_router());

    nya_log_info("Serving on http://127.0.0.1:%u — /docs for the generated page, ctrl-c to stop.", nya_http_server_port());

    /*
     * The drain is the whole loop. nya_system_http_tick accepts what is waiting, reads what has
     * arrived and answers what is complete, and returns rather than blocking, so a program that has
     * other work to do puts this beside it instead of around it.
     */
    while (RUNNING) {
        nya_system_http_tick();
        // as net_echo and the frame limiter do: a real sleep, so the loop does not spin a core.
        SDL_Delay(TICK_SLEEP_MS);
    }

    nya_log_info("Stopping after " FMTu64 " requests.", nya_http_server_request_count());

    return EXIT_SUCCESS;
}

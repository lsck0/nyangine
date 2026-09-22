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
 * curl -X QUERY localhost:47800/api/notes -d '{}'
 * curl -X POST  localhost:47800/api/notes -d '{"text":"the first note"}'
 * curl -X DELETE localhost:47800/api/notes -d '{"id":1}'
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
 * the same reflection. That is what `http_metrics.c` does, and it is what a resource inside the engine
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

/** Every note. */
NYA_INTERNAL NYA_HttpStatus notes_query(NYA_HttpExchange* exchange) {
    NYA_Object* body  = nya_object_create(exchange->arena);
    NYA_ArrayᐸNYA_Valueᐳ* notes = nya_array_create(exchange->arena, NYA_Value);

    for (u32 i = 0; i < NOTE_COUNT; i++) {
        NYA_Value value = note_to_value(exchange->arena, &NOTES[i]);
        nya_array_push_back(notes, value);
    }

    nya_object_set(body, "count", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NOTE_COUNT });
    nya_object_set(body, "notes", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *notes });

    if (!nya_http_response_json(exchange->response, exchange->arena, body).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_OK;
}

/** Adds one, and answers with the note as it was stored so the caller learns its id. */
NYA_INTERNAL NYA_HttpStatus notes_post(NYA_HttpExchange* exchange) {
    // A body that is not an object at all is the caller's mistake and answers 400, not 500.
    NYA_Object* incoming = nullptr;
    if (!nya_http_request_json(exchange->request, exchange->arena, &incoming).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

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
    if (!nya_http_response_json(exchange->response, exchange->arena, &stored.as_object).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_CREATED;
}

/** Removes one by id. */
NYA_INTERNAL NYA_HttpStatus notes_delete(NYA_HttpExchange* exchange) {
    NYA_Object* incoming = nullptr;
    if (!nya_http_request_json(exchange->request, exchange->arena, &incoming).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

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
     .description   = "A read, and therefore a QUERY rather than a GET.",
     .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method        = NYA_HTTP_METHOD_POST,
     .path          = NOTES_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
     .handler       = notes_post,
     .summary       = "Writes a note",
     .description   = "Answers with the note as stored, so the caller learns the id it was given.",
     .statuses      = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNPROCESSABLE,
                           NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method       = NYA_HTTP_METHOD_DELETE,
     .path         = NOTES_PATH,
     .auth         = NYA_HTTP_AUTH_NONE,
     .handler      = notes_delete,
     .summary      = "Removes a note by id",
     .statuses     = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_NOT_FOUND },
     },
};

NYA_INTERNAL const NYA_HttpRouter NOTE_ROUTER = {
    .name        = "notes",
    .routes      = NOTE_ROUTES,
    .route_count = nya_carray_length(NOTE_ROUTES),
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
        if (strcmp(argv[i], "--port") == 0) port = (u16)atoi(argv[i + 1]);
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

    // Merged at the root. The engine's own metrics resource is merged by the server already.
    NYA_EXPECT(nya_http_server_merge(&NOTE_ROUTER), "while merging the notes resource");
    defer nya_http_server_unmerge(&NOTE_ROUTER);

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

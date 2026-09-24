/**
 * @file examples/web_server/main.c
 *
 * An HTTP server: a resource of its own, typed requests and responses through reflection, the OpenAPI
 * document generated from the route table rather than written, and a page served out of the asset
 * system that talks to the resource.
 *
 * ```
 * ./build run example web_server            # serves on 127.0.0.1:47800 until interrupted
 * ./web_server.example --port 8080
 * ./web_server.example --address 0.0.0.0 --port 8000   # bind every interface, as the container does
 *
 *
 * # and over TLS, with a certificate this machine made for itself:
 * openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost \
 *     -addext subjectAltName=DNS:localhost,IP:127.0.0.1 -keyout key.pem -out cert.pem
 * ./web_server.example --certificate cert.pem --key key.pem
 * ```
 *
 * Open `http://127.0.0.1:47800/` for the page. Then, from another terminal:
 *
 * ```
 * curl -i localhost:47800/                  # the entry point: an ETag and no-cache
 * curl -i localhost:47800/static/app.*.css  # the same bytes, immutable for a year
 * curl -X QUERY localhost:47800/api/notes -H 'Content-Type: application/json' -d '{}'
 * curl -X QUERY localhost:47800/api/notes -H 'Accept: application/nya' -d '{}'   # the native format
 * curl -X QUERY localhost:47800/api/notes -H 'Accept: application/nya-binary' -d '{}' -o notes.bin
 * curl -X QUERY localhost:47800/api/metrics -H 'Accept: application/nya-binary' -d '{}' -o metrics.bin  # a DTO, typed
 * curl -X QUERY 'localhost:47800/api/notes?contains=first+note' -d '{}'         # only the notes containing it
 * curl -X POST  localhost:47800/api/notes -d '{"text":"the first note"}'
 * # send the same Idempotency-Key twice: the second is a replay of the first, not a second note.
 * curl -X POST localhost:47800/api/notes -H 'Idempotency-Key: k-1' -d '{"text":"once only"}'   # 201, writes it
 * curl -i -X POST localhost:47800/api/notes -H 'Idempotency-Key: k-1' -d '{"text":"once only"}' # 201 + Idempotency-Replayed: true, no new note
 * curl -X POST localhost:47800/api/notes -H 'Idempotency-Key: k-1' -d '{"text":"different"}'    # 422: same key, different body
 * curl -X DELETE localhost:47800/api/notes -d '{"id":1}'
 * # stop the server, start it again, and the notes above are still there: they are rows, not an array
 * curl -X POST localhost:47800/api/otp/enrol    # the URI to scan, the secret to type, the recovery codes
 * # the three below want -H 'Content-Type: application/json', since a body without one is not a document
 * curl -X POST localhost:47800/api/otp/activate -d '{"code":"123456"}'   # the factor is off until this passes
 * curl -X POST localhost:47800/api/otp/verify   -d '{"code":"123456"}'   # 204, or 401, or 429
 * curl -X POST localhost:47800/api/otp/recover  -d '{"code":"ABCDEFGH-IJKLMNOP"}'
 * curl -i localhost:47800/healthz           # liveness: 200 while the process is up
 * curl -i localhost:47800/readyz            # readiness: 200 when the db answers and its breaker is closed, else 503
 * curl localhost:47800/docs                 # the generated page
 * curl localhost:47800/openapi.json         # the document it is generated from
 * curl -X QUERY localhost:47800/api/metrics -d '{}'
 * websocat ws://127.0.0.1:47800/ws/notes    # the stream: a snapshot a second, and one per write
 * ```
 *
 * ## This is net_echo's sibling
 *
 * `examples/net_echo/main.c` says the web server did not exist when it was written, and that when it
 * did this example would be the one to copy for a web app. This is that one. The two are unrelated
 * pieces of the engine: net_echo is the game transport, encrypted UDP between a world and its
 * players; this is TCP, HTTP and a router, and they share nothing but the word server.
 *
 * ## The notes are in a database
 *
 * They are rows in a sqlite file under the save root, through the `db` module: `notes.db`, one table,
 * created and kept level with the struct by `nya_orm_schema_migrate` at startup. Stop the server and
 * start it again and the notes are still there, which is the only version of this example that is
 * worth copying — an array that empties on restart is a demonstration of a router, not of a server.
 *
 * Three things that are worth reading for: the table is derived from the Model's reflection rather
 * than written as SQL; every value a client sends crosses as a bound parameter, so a note whose text
 * is `'); DROP TABLE notes;--` is stored and handed back as that text; and the file is *not*
 * encrypted, because SQLCipher is not vendored yet — see the encryption note in `db.h` before putting
 * anything in a database that would matter if somebody read the disk.
 *
 * ## Why the bodies are NYA_Object and not reflected structs
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
 * layers, the generated document — is exactly what a reflected resource uses. The note Model gets
 * around it by writing out the description the generator would have emitted, which is what the ORM
 * reads; it is six declarations where a resource inside the engine has one comment.
 *
 * The second factor routes are the exception, and they show why the rest is a limitation rather than a
 * style: their DTOs are the engine's own, so `nya_reflect_of` resolves, the OpenAPI document describes
 * the bodies, and the log layer has a type to redact through. A handler that reads its body out of a
 * document by hand is handing the log layer bytes it cannot describe, and an undescribed body is logged
 * as a size and a hash.
 *
 * ## What this server logs
 *
 * `nya_http_layer_log` is installed as the one root layer, at `bodies`, which is the loudest level
 * there is and is chosen here so that running the example shows what the levels do. Send a code to
 * `/api/otp/verify` and the record holds `{"code":"<redacted>"}`: the field is `@redact` on
 * `NYA_HttpTotpSubmission`, so the substitution happens while the record is built and before any sink
 * is handed it. `Authorization` and `Cookie` come out the same way, `?token=...` in a query does too,
 * and a body that does not parse as its route's DTO — every `/api/notes` body, since those are
 * documents and not DTOs — is logged as its size and a hash. See `http_log.h`.
 *
 * ## The stream
 *
 * `/ws/notes` is a WebSocket, and it is here because a poll is the wrong shape for "tell me when
 * something changes": a page that wants to stay current has to ask every second and is wrong for most
 * of that second, where a socket is told once, when it happens. This one pushes a snapshot when a peer
 * connects, again whenever a note is written or removed, and once a second regardless so an idle page
 * can still see the server is alive. A client may send `now` to ask for one out of turn; anything else
 * it sends is ignored, because a stream that takes commands is an API and this one is a view.
 *
 * ## Four workers, and which routes are allowed on them
 *
 * The server runs threaded: a listener thread of its own and `WORKER_COUNT` workers, so a request is
 * answered when it arrives rather than when this loop comes round, and the session's signature
 * checking happens off the loop entirely.
 *
 * What may run on a worker is decided route by route, in the tables below. The session routes are a
 * JWT over a secret that has not changed since startup, and the bundle and the generated document are
 * read-only once mounted, so those run on workers. The notes and the second factor are this program's
 * own state — an array, a counter, a replay guard — held in plain statics with no lock, so they are
 * NYA_HTTP_AFFINITY_MAIN and are answered inside `nya_system_http_tick` below, where the loop and the
 * stream already are. The metrics resource is the engine's and declares the same for itself.
 *
 * ## QUERY rather than GET
 *
 * A read is a QUERY here. A request in this server is a reflected struct and a GET has nowhere to put
 * one, so the four verbs a resource is written in are QUERY, POST, PUT and DELETE; see
 * `http_router.h`. GET still works and `/docs` is one, because a browser has no other verb.
 *
 * ## The page is three assets, not three string literals
 *
 * `assets/web/` holds the html, the css and the js, and they are assets like a texture or a font: the
 * generated handles below are what `src/genyarated/assets.h` wrote, a debug build reads the files and a
 * release reads them out of the baked blob. `nya_http_static_mount` hashes each one and serves it at
 * two names, so the browser caches the stylesheet for a year and still sees a change immediately; see
 * `http_static.h`.
 *
 * That, and the storage, are what this example needs the engine for beyond the socket: reading an
 * asset goes through the asset system and the notes go through the save root, so those systems come
 * up below. They are cheap and they warn about audio and fonts on a machine with neither, which is a
 * server and is fine.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * This server opens no window, no renderer and no frame loop, so the only SDL it touches is the one
 * call that brings the library's own state up for the asset system to hang off, and the sleep between
 * ticks — and that second one is the os layer's own primitive, `nya_os_time_sleep_ms`, which is the
 * same in either build and is what is used below in place of SDL_Delay.
 *
 * The include is guarded because a server has no reason to name SDL at all: the day the engine grows a
 * headless build this file compiles straight through under NYA_NO_SDL. That day is not here yet —
 * core (the asset, save, callback and event systems this uses), http and crypto all sit behind the
 * same NYA_NO_SDL wall in nyangine.h today, so the guard buys the seam, not a headless binary. See
 * this example's Dockerfile and deploy/README.md for what that means for shipping one.
 */
#ifndef NYA_NO_SDL
#include "SDL3/SDL_init.h"
#endif

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

/** One note. This is the Model: it is what a row is, and it never leaves this file as itself. */
typedef struct {
    s64  id;
    char text[NOTE_TEXT_MAX];
    f64  written_at_s;
} ExampleNote;

/*
 * The description the ORM builds the table from, written by hand for the reason this file's block
 * gives about DTOs: the reflection pass scans src/nyangine and src/gnyame, so a type declared in an
 * example has no generated table and nya_reflect_of does not resolve. Inside the engine these six
 * declarations are one `// @reflect` comment.
 */
static const NYA_TypeReflection NOTE_TEXT_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = NOTE_TEXT_MAX,
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = NOTE_TEXT_MAX,
};

static const NYA_ReflectField NOTE_FIELDS[] = {
    // @key: the database assigns it, because an id that is zero on the way in is one the row has not
    // got yet. That is what replaced the NEXT_ID counter this example used to keep.
    { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(ExampleNote, id), .is_key = true },
    { .name = "text", .type = &NOTE_TEXT_ARRAY, .offset = nya_offsetof(ExampleNote, text) },
    { .name = "written_at_s", .type = nya_reflect_of(f64), .offset = nya_offsetof(ExampleNote, written_at_s) },
};

static const NYA_TypeReflection NOTE_MODEL = {
    .name        = "ExampleNote",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(ExampleNote),
    .alignment   = alignof(ExampleNote),
    .fields      = NOTE_FIELDS,
    .field_count = nya_carray_length(NOTE_FIELDS),
};

/*
 * The notes live in a database under the save root, so they are still there after a restart. The
 * connection and the table outlive every request, which is why they are static: an exchange's arena
 * is scratch for one answer and is gone when it is sent.
 */
static NYA_Arena*    NOTES_ARENA = nullptr;
static NYA_Database* NOTES_DB    = nullptr;
static NYA_OrmTable* NOTES_TABLE = nullptr;

/*
 * A breaker over the notes database. Every handler that touches the database records the outcome into
 * it, so a run of failures trips it OPEN and calls are failed fast; the readiness route reads that same
 * breaker's state rather than keeping a second idea of whether the database is healthy. This is the
 * base_circuit tie-in http_health.h describes: the thing that fail-fasts the calls is the thing
 * readiness reflects. See base_circuit.h.
 */
static NYA_CircuitBreaker* DB_BREAKER = nullptr;

/** Keyed once, reused by both the breaker records and the readiness check that reads its state. */
#define DB_CIRCUIT_KEY "notes-db"

/** Ties the breaker to the readiness registry; its address is registered, so it is static. */
static NYA_HttpHealthCircuit DB_CIRCUIT = { 0 };

/** One note as a document, which is what goes out over the wire. The DTO, built by hand. */
NYA_INTERNAL NYA_Value note_to_value(NYA_Arena* arena, const ExampleNote* note) {
    NYA_Object* object = nya_object_create(arena);

    nya_object_add(object, "id", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = note->id });
    nya_object_add(object, "text", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)note->text });
    nya_object_add(object, "written_at_s", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = note->written_at_s });

    return (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *object };
}

/** How many rows there are, which is the ceiling check and the stream's snapshot both. */
NYA_INTERNAL u64 note_count(void) {
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "note_count");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_SqlResult result = { 0 };

    // A database that cannot be read answers zero rather than a number it made up; the handler that
    // matters answers 500 on its own failure, and the stream would rather be wrong than stop.
    if (!nya_sql_query(NOTES_DB, &scratch, "SELECT COUNT(*) AS notes FROM notes", nullptr, 0, &result).ok) return 0;
    if (result.rows->length == 0) return 0;

    NYA_Value* notes = nya_object_get(result.rows->items[0], "notes");

    return notes != nullptr && notes->type == NYA_TYPE_S64 && notes->as_s64 > 0 ? (u64)notes->as_s64 : 0;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * READINESS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The real readiness check: whether the notes database answers a trivial query right now. Registered as
 * "db", so `GET /readyz` is 503 with `{"name":"db","ready":false}` in its list while the file is not
 * open — which is what keeps traffic off this instance until its storage is up, rather than answering
 * requests it can only 500. It reads program state, which is why the route is NYA_HTTP_AFFINITY_MAIN.
 * */
NYA_INTERNAL b8 example_db_ready(void* user) {
    nya_unused(user);

    if (NOTES_DB == nullptr) return false;

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "readyz_db");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_SqlResult result = { 0 };

    return nya_sql_query(NOTES_DB, &scratch, "SELECT 1", nullptr, 0, &result).ok;
}

/**
 * Records a database call's outcome into the breaker, so readiness and fail-fast share one verdict. The
 * allow is what base_circuit pairs with every record and what creates the breaker's entry; this example
 * always makes the call rather than fail-fasting on it, since the point here is that /readyz reflects
 * the breaker, not the fail-fast path itself.
 * */
NYA_INTERNAL void db_record(b8 ok) {
    if (DB_BREAKER == nullptr) return;

    (void)nya_circuit_allow(DB_BREAKER, DB_CIRCUIT_KEY);
    nya_circuit_record(DB_BREAKER, DB_CIRCUIT_KEY, ok);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE STREAM
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NOTES_STREAM_PATH "/ws/notes"

/** How often a snapshot goes out to everyone, whether or not anything changed. */
#define STREAM_INTERVAL_MS 1000

/** One snapshot as compact json: what is stored, and what this server has been doing. */
NYA_INTERNAL NYA_CString stream_snapshot(NYA_Arena* arena) {
    NYA_Object* body = nya_object_create(arena);

    nya_object_add(body, "notes", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = note_count() });
    nya_object_add(body, "requests", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = nya_http_server_request_count() });
    nya_object_add(body, "connections", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = nya_http_server_connection_count() });
    nya_object_add(body, "listeners", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = nya_http_websocket_count() });
    nya_object_add(body, "uptime_s", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)nya_clock_get_monotonic_ns() / 1e9 });

    NYA_String* text = nya_serialize(arena, body, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);
    if (text == nullptr) return "{}";

    return nya_string_to_cstring(arena, text);
}

/** Pushes one to everybody on the stream. Called on a tick and whenever the notes change. */
NYA_INTERNAL void stream_push(void) {
    if (nya_http_websocket_count() == 0) return;

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "stream_snapshot");
    defer     nya_arena_destroy_on_stack(&scratch);

    (void)nya_http_websocket_broadcast_text(NOTES_STREAM_PATH, stream_snapshot(&scratch));
}

/** A peer that has just connected gets the current state rather than waiting for the next tick. */
NYA_INTERNAL void stream_open(NYA_HttpWebSocket* socket) {
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "stream_snapshot");
    defer     nya_arena_destroy_on_stack(&scratch);

    // Its own failure and nobody else's: a peer whose queue is full is dropped by the server's own
    // pending-write bound, and the loop carries on.
    (void)nya_http_websocket_send_text(socket, stream_snapshot(&scratch));
}

NYA_INTERNAL void stream_message(NYA_HttpWebSocket* socket, b8 is_text, const u8* data, u64 size) {
    // "now" and nothing else. A view does not take commands, so anything else is read and dropped.
    if (!is_text || size != 3 || nya_memcmp(data, "now", 3) != 0) return;

    stream_open(socket);
}

NYA_INTERNAL const NYA_HttpWebSocketRoute NOTES_STREAM = {
    .path       = NOTES_STREAM_PATH,
    .summary    = "a snapshot of the notes and this server, pushed",
    .on_open    = stream_open,
    .on_message = stream_message,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Every note, or with `?contains=text` only the ones whose text contains it.
 *
 * NYA_INTERNAL_CALLBACK, not NYA_INTERNAL, because its route carries it as a `nya_callback` token
 * rather than a raw pointer: the name has to stay findable so a code reload can re-resolve it. See the
 * token wired onto the QUERY route in main() and nya_http_router_reloadable in core_http_reload.h.
 * */
NYA_INTERNAL_CALLBACK NYA_HttpStatus notes_query(NYA_HttpExchange* exchange) {
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

    /*
     * Every row, oldest first, as structs in this exchange's arena. The clause is a literal in this
     * source and the filter below is not part of it: `WHERE text LIKE ?` would read as the obvious
     * thing to write, and it would hand the client a pattern language, since `%` in what it sent is
     * a wildcard. Reading the rows and filtering here keeps "contains" meaning contains.
     */
    void* rows  = nullptr;
    u32   count = 0;

    NYA_Error selected = nya_orm_select(NOTES_TABLE, exchange->arena, "ORDER BY id", nullptr, 0, &rows, &count);

    db_record(selected.ok);
    if (!selected.ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    for (u32 i = 0; i < count; i++) {
        const ExampleNote* note = nya_orm_at(NOTES_TABLE, rows, i);

        if (filtered && strstr(note->text, contains) == nullptr) continue;

        NYA_Value value = note_to_value(exchange->arena, note);
        nya_array_push_back(notes, value);
    }

    nya_object_add(body, "count", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = notes->length });
    nya_object_add(body, "notes", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *notes });

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

    // A full store is the caller asking for more than this server holds, not a server fault. The
    // ceiling is this example's and not the database's; it is here so the file cannot grow forever.
    if (note_count() >= NOTES_MAX) return NYA_HTTP_STATUS_UNPROCESSABLE;

    // The id is left at zero, so the database assigns it and nya_orm_insert writes it back. The text
    // is bound as a parameter, whatever quotes and semicolons the client put in it.
    ExampleNote note = { .written_at_s = exchange->now_s };
    (void)snprintf(note.text, sizeof(note.text), "%s", text->as_string);

    NYA_Error inserted = nya_orm_insert(NOTES_TABLE, &note);

    db_record(inserted.ok);
    if (!inserted.ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // what the stream is for: whoever is watching hears about this now rather than on their next poll.
    stream_push();

    NYA_Value stored = note_to_value(exchange->arena, &note);
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

    NYA_Error removed = nya_orm_delete(NOTES_TABLE, nya_sql_s64(id->as_s64));

    // NOT_FOUND is the database working fine and the row simply not being there, so it counts as a
    // healthy call to the breaker; only a real failure is recorded against it.
    db_record(removed.ok || removed.kind == NYA_ERROR_NOT_FOUND);

    // An id that matches no row is NYA_ERROR_NOT_FOUND, which is a 404 and not a 500: absence is in
    // the return rather than in the data, so the two failures do not have to be told apart here.
    if (removed.kind == NYA_ERROR_NOT_FOUND) return NYA_HTTP_STATUS_NOT_FOUND;
    if (!removed.ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    stream_push();

    return NYA_HTTP_STATUS_NO_CONTENT;
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
#define SESSION_PATH      "/api/session"

/**
 * How long a session lasts here. Short on purpose: an access token is checked by its signature alone
 * and nothing can call it back, so the only thing limiting a stolen one is how soon it stops working.
 * A real server pairs this with a refresh token in a session row; see TODO.md, "Sessions".
 * */
#define SESSION_SECONDS 900

/**
 * The secret this server signs its sessions with, made once at startup from the CSPRNG.
 *
 * Made rather than configured, because an example that shipped a secret would be an example of how to
 * lose one. It also means every restart invalidates every session it issued, which is the honest
 * behaviour for a server that keeps nothing.
 * */
static u8 SESSION_SECRET[32] = { 0 };

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

/**
 * The submitted code, through the engine's own DTO rather than by reaching into a document.
 *
 * `code` is `@redact` on that type, which is what keeps it out of the log at every level: the log layer
 * decodes this body through this same reflection and writes `<redacted>` where the tag is. A handler
 * reading the field out of an NYA_Object by hand would be handing the layer a body it cannot describe,
 * and the layer would fall back to logging a size and a hash — safe, and less useful.
 * */
NYA_INTERNAL b8 otp_submitted_code(NYA_HttpExchange* exchange, OUT NYA_HttpTotpSubmission* out_submission) {
    if (!nya_http_request_reflect(exchange->request, exchange->arena, nya_reflect_of(NYA_HttpTotpSubmission), out_submission).ok) return false;

    return out_submission->code[0] != '\0';
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

    /*
     * The engine's DTO, every field of it `@redact`. The caller gets all of it — the answer is written
     * by nya_http_response_reflect_as, which writes the fields — and the log gets none of it, because
     * the log layer decodes this same body through nya_reflect_to_object_redacted. One type, two
     * readings of it, and neither of them is a rule anybody has to remember here.
     */
    NYA_HttpTotpEnrolmentDto answer = { 0 };
    defer nya_memset(&answer, 0, sizeof(answer));

    (void)snprintf(answer.uri, sizeof(answer.uri), "%s", enrolment.uri);
    (void)snprintf(answer.secret, sizeof(answer.secret), "%s", enrolment.secret_base32);

    for (u32 i = 0; i < NYA_HTTP_TOTP_RECOVERY_CODES; i++) {
        (void)snprintf(answer.recovery[i].code, sizeof(answer.recovery[i].code), "%s", enrolment.recovery[i]);
    }

    // the factor is not on yet: /activate has to see one code first.
    FACTOR = (ExampleFactor){ .enrolled = true, .secret = enrolment.secret };
    for (u32 i = 0; i < NYA_HTTP_TOTP_RECOVERY_CODES; i++) FACTOR.recovery[i] = enrolment.recovery_hash[i];

    NYA_Error written = nya_http_response_reflect_as(
        exchange->response, exchange->arena, nya_reflect_of(NYA_HttpTotpEnrolmentDto), &answer, nya_http_request_accepts(exchange->request)
    );

    if (!written.ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_CREATED;
}

/** Turns the pending enrolment on, once one code proves the authenticator holds the same secret. */
NYA_INTERNAL NYA_HttpStatus otp_activate(NYA_HttpExchange* exchange) {
    NYA_HttpTotpSubmission submission = { 0 };
    defer                  nya_memset(&submission, 0, sizeof(submission));

    if (!otp_submitted_code(exchange, &submission)) return NYA_HTTP_STATUS_BAD_REQUEST;

    // no enrolment is the same refusal as a wrong code; see otp_status.
    if (!FACTOR.enrolled) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_HttpTotpVerdict verdict = nya_http_totp_verify(&FACTOR.guard, &FACTOR.secret, submission.code, exchange->now_s);
    if (verdict == NYA_HTTP_TOTP_ACCEPTED) FACTOR.active = true;

    return otp_status(verdict);
}

/**
 * Mints the access token for the account the second factor just proved, and puts it in the cookie a
 * browser will send back on its own.
 *
 * `__Host-` is the prefix that makes the name belong to this host alone, `HttpOnly` keeps it away from
 * script, and `SameSite=Strict` is what makes it safe for a browser to send it at all: dispatch already
 * refuses a cross site write, and the two together are the CSRF defence.
 * */
NYA_INTERNAL NYA_HttpStatus session_issue(NYA_HttpExchange* exchange) {
    NYA_HttpIdentity identity = {
        .scope        = NYA_HTTP_SCOPE_READ,
        .issued_at_s  = exchange->now_s,
        .expires_at_s = exchange->now_s + SESSION_SECONDS,
    };

    (void)snprintf(identity.subject, sizeof(identity.subject), "%s", "the one account");

    char token[NYA_HTTP_MAX_TOKEN_BYTES] = { 0 };

    if (!nya_http_jwt_encode(&identity, SESSION_SECRET, sizeof(SESSION_SECRET), token, sizeof(token)).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    NYA_Error set = nya_http_response_cookie(exchange->response,
                                             &(NYA_HttpCookie){
                                                 .name      = NYA_HTTP_SESSION_COOKIE,
                                                 .value     = token,
                                                 .max_age_s = SESSION_SECONDS,
                                                 .http_only = true,
                                                 .secure    = true,
                                                 .same_site = NYA_HTTP_SAME_SITE_STRICT,
                                             });

    return set.ok ? NYA_HTTP_STATUS_NO_CONTENT : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Who the cookie says you are. Reached by the cookie alone, which is the point of the route. */
NYA_INTERNAL NYA_HttpStatus session_read(NYA_HttpExchange* exchange, const NYA_HttpIdentity* identity) {
    NYA_Object* body = nya_object_create(exchange->arena);

    nya_object_add(body, "subject", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)identity->subject });
    nya_object_add(body, "expires_at_s", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = identity->expires_at_s });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * Signing out. The cookie is cleared with the attributes it was set with, because a browser matches on
 * those: a clear with a different path leaves the original in place and the session alive.
 * */
NYA_INTERNAL NYA_HttpStatus session_clear(NYA_HttpExchange* exchange) {
    NYA_Error cleared = nya_http_response_cookie_clear(exchange->response, NYA_HTTP_SESSION_COOKIE, "/", true);

    return cleared.ok ? NYA_HTTP_STATUS_NO_CONTENT : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** One code against the factor that is on. What a login route will call once there is one. */
NYA_INTERNAL NYA_HttpStatus otp_verify(NYA_HttpExchange* exchange) {
    NYA_HttpTotpSubmission submission = { 0 };
    defer                  nya_memset(&submission, 0, sizeof(submission));

    if (!otp_submitted_code(exchange, &submission)) return NYA_HTTP_STATUS_BAD_REQUEST;

    if (!FACTOR.active) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_HttpStatus verified = otp_status(nya_http_totp_verify(&FACTOR.guard, &FACTOR.secret, submission.code, exchange->now_s));

    // the factor proven is what a session is issued against, which is the half of a login this example
    // can honestly do: there is no password and no user store, so the second factor stands for both.
    return verified == NYA_HTTP_STATUS_NO_CONTENT ? session_issue(exchange) : verified;
}

/** One recovery code, for the phone that is gone. Spent by the call, so it works exactly once. */
NYA_INTERNAL NYA_HttpStatus otp_recover(NYA_HttpExchange* exchange) {
    NYA_HttpTotpSubmission submission = { 0 };
    defer                  nya_memset(&submission, 0, sizeof(submission));

    if (!otp_submitted_code(exchange, &submission)) return NYA_HTTP_STATUS_BAD_REQUEST;

    if (!FACTOR.active) return NYA_HTTP_STATUS_UNAUTHORIZED;

    return otp_status(
        nya_http_totp_recovery_redeem(&FACTOR.guard, FACTOR.recovery, nya_carray_length(FACTOR.recovery), submission.code, exchange->now_s)
    );
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
/*
 * NYA_HTTP_AFFINITY_MAIN on all three: the notes are a static array and two counters with nothing
 * guarding them, so they are read and written where this program's loop is and nowhere else. Making
 * them a worker's would mean a lock around every touch of NOTES, which is a bigger decision than an
 * example should make quietly; the affinity is that decision, written down.
 */
// Not const: the QUERY route below carries its handler as a `nya_callback` token rather than a raw
// pointer, and that token is only known once the callback registry is up, so main() writes it in
// before the router is merged. See nya_http_router_reloadable and the assignment in main().
NYA_INTERNAL NYA_HttpRoute NOTE_ROUTES[] = {
    {
     .method        = NYA_HTTP_METHOD_QUERY,
     .path          = NOTES_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
     .affinity      = NYA_HTTP_AFFINITY_MAIN,
     // .handler_callback is set in main() to nya_callback(notes_query): a reload-safe stand-in for
     // `.handler = notes_query` that the router re-resolves each dispatch, so a code reload never
     // leaves this route pointing into the old image.
     .summary       = "Every note",
     .description   = "A read, and therefore a QUERY rather than a GET. `?contains=text` keeps the notes containing it.",
     // UNAUTHORIZED is here for the optional proof-of-work layer below, which walls the whole notes
     // resource when `--proof-of-work` is set and answers 401 with a challenge; a route's statuses cover
     // its whole chain, and declaring one the chain can produce is safe whether or not the layer is on.
     .statuses      = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method        = NYA_HTTP_METHOD_POST,
     .path          = NOTES_PATH,
     .auth          = NYA_HTTP_AUTH_NONE,
     .affinity      = NYA_HTTP_AFFINITY_MAIN,
     .handler       = notes_post,
     .summary       = "Writes a note",
     .description   = "Answers with the note as stored, so the caller learns the id it was given. Send `Idempotency-Key: <token>` and a "
                        "retry with the same key returns the first note rather than writing a second; see the idempotency layer below.",
     // CONFLICT, and UNPROCESSABLE beyond the handler's own: the idempotency layer on this router can
     // answer 409 for a duplicate still in flight and 422 for a key reused with a different body, and a
     // route's statuses cover its whole chain. BAD_REQUEST covers the layer's malformed-key answer too.
     .statuses      = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN,
                           NYA_HTTP_STATUS_UNPROCESSABLE, NYA_HTTP_STATUS_CONFLICT, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method       = NYA_HTTP_METHOD_DELETE,
     .path         = NOTES_PATH,
     .auth         = NYA_HTTP_AUTH_NONE,
     .affinity     = NYA_HTTP_AFFINITY_MAIN,
     .handler      = notes_delete,
     .summary      = "Removes a note by id",
     // CONFLICT and UNPROCESSABLE for the same reason as the POST: an unsafe verb behind the
     // idempotency layer can be answered by it, so it declares what the whole chain can produce.
     .statuses     = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN,
                          NYA_HTTP_STATUS_NOT_FOUND, NYA_HTTP_STATUS_CONFLICT, NYA_HTTP_STATUS_UNPROCESSABLE },
     },
};

/*
 * The idempotency layer, around the notes resource only: a POST retried with the same `Idempotency-Key`
 * runs once and replays its first answer, so a dropped response never doubles a note. The QUERY passes
 * straight through it, being safe; the DELETE is deduped like the POST. The store is readied in main()
 * before this router is merged. See http_idempotency.h.
 */
NYA_INTERNAL const NYA_HttpLayerFn NOTE_LAYERS[] = { nya_http_layer_idempotency };

/*
 * The same chain with the proof-of-work wall outermost, wired in when `--proof-of-work` is set. Outermost
 * on purpose: a request that has not paid is turned away before the idempotency store is even consulted,
 * so the wall is the cheapest thing in the chain and everything behind it only ever sees paid traffic.
 * This is the abuse brake an onion service reaches for, having no client IP to rate-limit; see http_pow.h.
 */
NYA_INTERNAL const NYA_HttpLayerFn NOTE_LAYERS_POW[] = { nya_http_layer_pow, nya_http_layer_idempotency };

// Not const: main() points its layers at NOTE_LAYERS_POW when the flag is set, before the router is merged.
NYA_INTERNAL NYA_HttpRouter NOTE_ROUTER = {
    .name        = "notes",
    .routes      = NOTE_ROUTES,
    .route_count = nya_carray_length(NOTE_ROUTES),
    .layers      = NOTE_LAYERS,
    .layer_count = nya_carray_length(NOTE_LAYERS),
};

/*
 * The second factor's own table. A separate router rather than four more rows above, because these
 * are not a resource: they are three steps of one flow, and a program that wants notes without a
 * factor merges one and not the other.
 */
/*
 * Main-thread too, and for a sharper reason than the notes: the replay guard is what makes a code work
 * once, and a guard two threads can update at the same time is a guard that lets a replayed code
 * through. The verification itself is cheap; it is FACTOR that is not shareable.
 */
NYA_INTERNAL const NYA_HttpRoute OTP_ROUTES[] = {
    {
     .method      = NYA_HTTP_METHOD_POST,
     .path        = OTP_ENROL_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .affinity    = NYA_HTTP_AFFINITY_MAIN,
     .handler       = otp_enrol,
     .response_type = nya_reflect_of(NYA_HttpTotpEnrolmentDto),
     .summary     = "Starts enrolling an authenticator",
     .description = "Answers with the otpauth URI to scan, the secret as base32 to type, and the recovery codes. All three are "
                        "shown once and never again, and the factor is not on until a code is posted to /api/otp/activate.",
     .statuses    = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method      = NYA_HTTP_METHOD_POST,
     .path        = OTP_ACTIVATE_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .affinity    = NYA_HTTP_AFFINITY_MAIN,
     .handler      = otp_activate,
     .request_type = nya_reflect_of(NYA_HttpTotpSubmission),
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
     .affinity    = NYA_HTTP_AFFINITY_MAIN,
     .handler      = otp_verify,
     .request_type = nya_reflect_of(NYA_HttpTotpSubmission),
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
     .affinity    = NYA_HTTP_AFFINITY_MAIN,
     .handler      = otp_recover,
     .request_type = nya_reflect_of(NYA_HttpTotpSubmission),
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
 * The session itself: what the factor above issues, and the two things a caller does with it. There is
 * no route that creates one here, because creating one is logging in and this example has no password
 * and no user store — /api/otp/verify stands in for that, and says so.
 */
NYA_INTERNAL const NYA_HttpRoute SESSION_ROUTES[] = {
    {
     .method              = NYA_HTTP_METHOD_GET,
     .path                = SESSION_PATH,
     .auth                = NYA_HTTP_AUTH_BEARER,
     .handler_identified  = session_read,
     .summary             = "Who the session cookie says you are",
     .description         = "Reached by the `" NYA_HTTP_SESSION_COOKIE "` cookie alone, which the browser sends on its own, or by "
                            "`Authorization: Bearer` for a caller that is not a browser.",
     .statuses            = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN },
     },
    {
     .method      = NYA_HTTP_METHOD_DELETE,
     .path        = SESSION_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .handler     = session_clear,
     .summary     = "Signs out",
     .description = "Clears the cookie with the attributes it was set with, which is what a browser matches on. The token itself "
                        "stays valid until it expires, since nothing can call a signature back; that is what the short lifetime is for.",
     .statuses    = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

NYA_INTERNAL const NYA_HttpRouter SESSION_ROUTER = {
    .name        = "session",
    .routes      = SESSION_ROUTES,
    .route_count = nya_carray_length(SESSION_ROUTES),
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE MIRROR ATTESTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * A signed statement of who this origin is and what it serves, published at a well-known path so a mirror
 * can serve it unchanged and a client that has pinned this origin's public key can prove a mirror faithful.
 * The manifest is made once at startup — the origin, the moment, and a digest of the mounted bundle — and
 * signed with the origin's Ed25519 key; the handler only renders it. See http_attestation.h, and
 * `--verify-mirror` below for the other half. The key is a configured secret, never checked in: a seed
 * from WEB_SERVER_ATTESTATION_SEED if one is set, otherwise a throwaway pair made here whose public half
 * is logged so an operator can pin it.
 */
static NYA_CryptoSignKeyPair       ORIGIN_KEY      = { 0 };
static NYA_HttpAttestationManifest ATTESTATION     = { 0 };
static NYA_CryptoSignature         ATTESTATION_SIG = { 0 };
static b8                          ATTESTATION_READY = false;

/** The digest of the served bundle, folded in mount_bundle so the manifest names exactly what is mounted. */
static NYA_CryptoSha256Digest BUNDLE_DIGEST = { 0 };

/** Renders the signed statement. Reads the statics made at startup, so NYA_HTTP_AFFINITY_MAIN. */
NYA_INTERNAL NYA_HttpStatus attestation_get(NYA_HttpExchange* exchange) {
    // Not signed yet — no origin key, or the bundle was not mounted — so there is nothing honest to serve.
    if (!ATTESTATION_READY) return NYA_HTTP_STATUS_SERVICE_UNAVAILABLE;

    NYA_Object* document = nullptr;

    if (!nya_http_attestation_to_json(exchange->arena, &ATTESTATION, &ORIGIN_KEY.public_key, &ATTESTATION_SIG, &document).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return nya_http_response_json(exchange->response, exchange->arena, document).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

NYA_INTERNAL const NYA_HttpRoute ATTESTATION_ROUTES[] = {
    {
     .method      = NYA_HTTP_METHOD_GET,
     .path        = NYA_HTTP_ATTESTATION_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .affinity    = NYA_HTTP_AFFINITY_MAIN,
     .handler     = attestation_get,
     .summary     = "The signed mirror attestation",
     .description = "An Ed25519 signature, over a length-prefixed manifest of this origin and a digest of the bundle it serves, "
                        "so a mirror serving these bytes can be proven to serve the origin's and not tampered ones. GET, because a "
                        "browser and a fetch have no other verb for it.",
     .statuses    = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

NYA_INTERNAL const NYA_HttpRouter ATTESTATION_ROUTER = {
    .name        = "attestation",
    .routes      = ATTESTATION_ROUTES,
    .route_count = nya_carray_length(ATTESTATION_ROUTES),
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE PAGE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Three assets, each served twice: once at the name the html links it by, which revalidates, and once
 * at a name containing the hash of its bytes, which a browser may keep for a year. The generated
 * handles are what makes this a list of files rather than a directory to walk, and a path a request
 * asks for is compared whole against these: nothing a caller sends ever becomes part of a file name.
 */
NYA_INTERNAL const struct {
    NYA_AssetHandle  asset;
    NYA_ConstCString path;
} BUNDLE_SOURCES[] = {
    { NYA_ASSET_WEB_INDEX_HTML, "/"        },
    { NYA_ASSET_WEB_APP_CSS,    "/app.css" },
    { NYA_ASSET_WEB_APP_JS,     "/app.js"  },
};

/** Reads the three through the asset system and hands them to the bundle. */
NYA_INTERNAL NYA_Error mount_bundle(NYA_Arena* scratch) {
    NYA_HttpStaticFile files[nya_carray_length(BUNDLE_SOURCES)] = { 0 };

    for (u64 index = 0; index < nya_carray_length(BUNDLE_SOURCES); index++) {
        u8* data = nullptr;
        u64 size = 0;

        // html, css and js are assets like a texture or a font: this reads the file in a debug build
        // and the baked blob in a release, and there is no second pipeline for web files.
        NYA_TRY(nya_asset_read(scratch, BUNDLE_SOURCES[index].asset, &data, &size));

        files[index] = (NYA_HttpStaticFile){
            .asset = BUNDLE_SOURCES[index].asset,
            .path  = BUNDLE_SOURCES[index].path,
            .data  = data,
            .size  = size,
        };
    }

    /*
     * The digest the mirror attestation names, folded over the same files in mount order: each path and
     * its bytes, length-prefixed, so a verifier that fetches these paths from a mirror recomputes exactly
     * this. Done here because this is where the bytes are in hand; the manifest is signed in main().
     */
    NYA_HttpAttestationFile attested[nya_carray_length(BUNDLE_SOURCES)] = { 0 };

    for (u64 index = 0; index < nya_carray_length(files); index++) {
        attested[index] = (NYA_HttpAttestationFile){ .path = files[index].path, .data = files[index].data, .size = files[index].size };
    }

    nya_http_attestation_bundle_digest(attested, nya_carray_length(attested), &BUNDLE_DIGEST);

    // the mount copies what it is handed, so `scratch` is the caller's to drop after this returns.
    return nya_http_static_mount((NYA_HttpStaticConfig){ .files = files, .count = nya_carray_length(files) });
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MIRROR VERIFICATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Loads the origin's signing key. A base64url seed — from `--attestation-seed` or WEB_SERVER_ATTESTATION_SEED
 * — gives the same key every restart, which is what a mirror verifier pins; with none, a throwaway pair is
 * made and its public half logged, so a run without a configured secret still works and says how to pin it.
 *
 * The seed is a secret and is never checked in: an example that shipped one would be shipping the power to
 * forge its own attestation. The throwaway path is the honest default for a demo.
 * */
NYA_INTERNAL b8 origin_key_load(NYA_ConstCString seed_b64) {
    if (seed_b64 != nullptr && seed_b64[0] != '\0') {
        NYA_CryptoKey32 seed = { 0 };
        defer           nya_memset(&seed, 0, sizeof(seed));

        u64 size = 0;
        if (!nya_crypto_base64url_decode(seed_b64, strlen(seed_b64), seed.bytes, sizeof(seed.bytes), &size) || size != sizeof(seed.bytes)) {
            nya_log_error("The attestation seed must be 32 bytes of base64url.");
            return false;
        }

        nya_crypto_sign_key_pair_from_seed(&seed, &ORIGIN_KEY);
        return true;
    }

    if (!nya_crypto_sign_key_pair_create(&ORIGIN_KEY).ok) {
        nya_log_error("The system random source failed, so no attestation key could be made.");
        return false;
    }

    char pub_b64[64] = { 0 };
    u64  pub_size    = 0;
    (void)nya_crypto_base64url_encode(ORIGIN_KEY.public_key.bytes, sizeof(ORIGIN_KEY.public_key.bytes), pub_b64, sizeof(pub_b64), &pub_size);

    nya_log_info("No attestation seed set: made a throwaway key. Pin this public key to verify a mirror: %s", pub_b64);

    return true;
}

/**
 * The verifier half of the attestation, run as a client and then exited — the shape `--healthcheck` takes.
 *
 * Fetches `<base_url>/.well-known/mirror-attestation`, checks the signature against the pinned origin key
 * (not the key the document carries — a mirror could present its own), then fetches the bundle from the
 * same host and recomputes the content digest, so a mirror is proven to serve the origin's exact bytes.
 * Returns EXIT_SUCCESS only when both hold. `base_url` has no trailing slash: "http://<host>.onion".
 * */
NYA_INTERNAL s32 verify_mirror(NYA_ConstCString base_url, NYA_ConstCString pin_b64) {
    if (pin_b64 == nullptr || pin_b64[0] == '\0') {
        nya_log_error("--verify-mirror needs --origin-key <base64url of the 32-byte pinned public key>.");
        return EXIT_FAILURE;
    }

    NYA_CryptoSignPublicKey pinned = { 0 };
    u64                     pin_size = 0;
    if (!nya_crypto_base64url_decode(pin_b64, strlen(pin_b64), pinned.bytes, sizeof(pinned.bytes), &pin_size) || pin_size != sizeof(pinned.bytes)) {
        nya_log_error("--origin-key must be 32 bytes of base64url.");
        return EXIT_FAILURE;
    }

    NYA_Arena* arena = nya_arena_create(.name = "verify_mirror");
    defer      nya_arena_destroy(arena);

    // The signed statement, from the mirror.
    char attestation_url[512] = { 0 };
    (void)snprintf(attestation_url, sizeof(attestation_url), "%s%s", base_url, NYA_HTTP_ATTESTATION_PATH);

    NYA_Response response = { 0 };
    NYA_Error    got      = nya_request_get(arena, attestation_url, &response);

    if (!got.ok || response.status != 200 || response.body == nullptr) {
        nya_log_error("Could not fetch the attestation from %s (status %u).", attestation_url, response.status);
        return EXIT_FAILURE;
    }

    NYA_HttpAttestationManifest manifest  = { 0 };
    NYA_CryptoSignPublicKey     presented = { 0 };
    NYA_CryptoSignature         signature = { 0 };

    if (!nya_http_attestation_from_json(response.body, &manifest, &presented, &signature).ok) {
        nya_log_error("The attestation document is malformed.");
        return EXIT_FAILURE;
    }

    // The key the document carries must be the pinned one: a mirror presenting its own key and a matching
    // signature has signed nothing the verifier asked about, so this is checked before the signature is.
    if (nya_memcmp(presented.bytes, pinned.bytes, sizeof(pinned.bytes)) != 0) {
        nya_log_error("The attestation is signed by a different key than the pinned origin key.");
        return EXIT_FAILURE;
    }

    if (!nya_http_attestation_verify(&pinned, &manifest, &signature)) {
        nya_log_error("The attestation signature does not verify against the pinned origin key.");
        return EXIT_FAILURE;
    }

    // The signature is the origin's. Now prove the mirror actually serves the bytes it names: fetch the
    // same bundle from this host and recompute the digest the manifest carries.
    NYA_HttpAttestationFile files[nya_carray_length(BUNDLE_SOURCES)] = { 0 };

    for (u64 index = 0; index < nya_carray_length(BUNDLE_SOURCES); index++) {
        char file_url[512] = { 0 };
        (void)snprintf(file_url, sizeof(file_url), "%s%s", base_url, BUNDLE_SOURCES[index].path);

        NYA_Response file_response = { 0 };
        if (!nya_request_get(arena, file_url, &file_response).ok || file_response.status != 200 || file_response.raw_body == nullptr) {
            nya_log_error("Could not fetch %s from the mirror (status %u).", file_url, file_response.status);
            return EXIT_FAILURE;
        }

        files[index] = (NYA_HttpAttestationFile){
            .path = BUNDLE_SOURCES[index].path,
            .data = file_response.raw_body->items,
            .size = file_response.raw_body->length,
        };
    }

    NYA_CryptoSha256Digest recomputed = { 0 };
    nya_http_attestation_bundle_digest(files, nya_carray_length(files), &recomputed);

    if (nya_memcmp(recomputed.bytes, manifest.content.bytes, sizeof(recomputed.bytes)) != 0) {
        nya_log_error("The mirror serves different bytes than the attestation names: the bundle does not match.");
        return EXIT_FAILURE;
    }

    nya_log_info("Mirror verified: %s serves %s's attested bundle, signed at %llu.", base_url, manifest.origin, (unsigned long long)manifest.issued_at_s);

    return EXIT_SUCCESS;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE PROGRAM
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Default port. Loopback only unless `address` is set; see NYA_HttpConfig. */
#define DEFAULT_PORT 47800

/** How long the loop sleeps between ticks. The sockets are the listener thread's; see the file note. */
#define TICK_SLEEP_MS 5

/**
 * Workers behind the listener.
 *
 * Four is past what a handful of connections needs and well inside NYA_HTTP_MAX_WORKERS. What it buys
 * here is that a signature check or a TOTP verification never waits behind another request, and that
 * neither of them is paid for by this loop.
 * */
#define WORKER_COUNT 4

/** Set by the signal handler, so ctrl-c leaves through the same shutdown a clean exit does. */
static volatile sig_atomic_t RUNNING = 1;

static void stop(int signal_number) {
    nya_unused(signal_number);
    RUNNING = 0;
}

s32 main(s32 argc, char** argv) {
    u16 port = DEFAULT_PORT;

    NYA_ConstCString certificate_path = "";
    NYA_ConstCString key_path         = "";

    /*
     * What to bind. Empty is the NYA_HttpConfig default, which is loopback only: the safe thing for an
     * example on a laptop, where nothing outside the machine should reach it. A deployment behind
     * nothing — a container whose only job is this server — has to be told to bind the wildcard, which
     * is what `--address 0.0.0.0` is for; the Dockerfile passes exactly that. See NYA_HttpConfig.
     */
    NYA_ConstCString address = "";

    // A lone flag rather than a pair: --healthcheck turns this binary into a client of itself for one
    // request and then exits, which is what a container's HEALTHCHECK runs. See below.
    b8 health_check = false;

    // Proof of work in front of the notes resource. Off by default so the curl recipes in this file's
    // block still work as written; on, every note request is walled behind a challenge. See http_pow.h.
    b8 proof_of_work = false;

    // The mirror-verification client mode, and its inputs: a mirror to check and the origin key to trust.
    NYA_ConstCString verify_mirror_url = "";
    NYA_ConstCString origin_key_pin    = "";

    // The attestation origin string and the seed its key is derived from. The seed may also come from the
    // environment, which is where a deployment keeps a secret rather than on the command line.
    NYA_ConstCString attestation_origin = "";
    NYA_ConstCString attestation_seed   = getenv("WEB_SERVER_ATTESTATION_SEED");

    // Deliberately not base_args: a handful of options, and the point of the file is the server.
    for (s32 i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--healthcheck") == 0) health_check = true;
        if (strcmp(argv[i], "--proof-of-work") == 0) proof_of_work = true;

        // The rest come in pairs, so a value has to follow.
        if (i + 1 >= argc) continue;

        if (strcmp(argv[i], "--certificate") == 0) certificate_path = argv[i + 1];
        if (strcmp(argv[i], "--key") == 0) key_path = argv[i + 1];
        if (strcmp(argv[i], "--address") == 0) address = argv[i + 1];
        if (strcmp(argv[i], "--verify-mirror") == 0) verify_mirror_url = argv[i + 1];
        if (strcmp(argv[i], "--origin-key") == 0) origin_key_pin = argv[i + 1];
        if (strcmp(argv[i], "--attestation-origin") == 0) attestation_origin = argv[i + 1];
        if (strcmp(argv[i], "--attestation-seed") == 0) attestation_seed = argv[i + 1];

        if (strcmp(argv[i], "--port") != 0) continue;

        // atoi would turn "80a" into 80 and "http" into 0, serving on a port nobody asked for
        NYA_ConstCString text = argv[i + 1];
        if (!nya_type_parse(NYA_TYPE_U16, (const u8*)text, strlen(text), &port)) {
            nya_log_error("--port expects a number from 0 to 65535, got '%s'.", text);
            return EXIT_FAILURE;
        }
    }

    /*
     * The container's liveness probe, and the one an image built FROM scratch can run: no curl in the
     * image, so the binary probes itself. It GETs `/` on the loopback port through the curl client the
     * server already links and exits 0 only on a 200, which is exactly what a Docker HEALTHCHECK wants —
     * a zero or a non-zero. It starts none of the server, so it is cheap and safe to run every few
     * seconds. Pass the same --port the server was given; the address is always loopback here, which the
     * wildcard the server binds includes. See this example's Dockerfile.
     */
    if (health_check) {
        NYA_Arena* arena = nya_arena_create(.name = "healthcheck");
        defer       nya_arena_destroy(arena);

        char url[64] = { 0 };
        (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u/", port);

        NYA_Response response = { 0 };
        NYA_Error    got      = nya_request_get(arena, url, &response);

        return got.ok && response.status == 200 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /*
     * The other client mode: fetch a mirror's attestation, check it against the pinned origin key, and
     * prove the mirror serves the origin's exact bundle. Runs and exits like the healthcheck, starting
     * none of the server. See verify_mirror above, and the deploy README for the curl form of the same.
     */
    if (verify_mirror_url[0] != '\0') {
        nya_log_level_set(NYA_LOG_LEVEL_INFO);
        return verify_mirror(verify_mirror_url, origin_key_pin);
    }

    b8 secure = certificate_path[0] != '\0' || key_path[0] != '\0';

    nya_log_level_set(NYA_LOG_LEVEL_INFO);

    (void)signal(SIGINT, stop);

    /*
     * No window, no renderer, no world, and no frame loop. What does come up is the asset system,
     * because the page below is three assets: an app instance for it to hang off, the callback and
     * event registries it hooks into, and then itself. That is the whole of the engine this needs.
     */
#ifndef NYA_NO_SDL
    b8 sdl_started = SDL_Init(0);

    if (!sdl_started) {
        nya_log_error("SDL could not start: %s", SDL_GetError());
        return EXIT_FAILURE;
    }
#endif

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    NYA_EXPECT(nya_system_events_init(), "while starting the event registry the asset system hooks into");
    defer nya_system_events_deinit();

    nya_system_asset_init();
    defer nya_system_asset_deinit();

    /*
     * The notes. Under the save root rather than beside the binary, so it lands where the rest of
     * this program's data does and is created along with its directory. Everything the server knows
     * is in that one file, which is what makes a restart uneventful: stop it, start it, and the
     * notes are still there. It is not encrypted — see the encryption note in db.h — so nothing goes
     * in it here that would matter if somebody read the disk.
     */
    NYA_Error saves = nya_system_save_init();

    if (!saves.ok) {
        // Fatal here, where it is a warning in a game: a game with no writable directory can still
        // be played, and a server whose whole job is to keep what it is sent cannot run without one.
        nya_log_error("No save root, so there is nowhere to keep the notes: %s", (NYA_ConstCString)saves.message);

        return EXIT_FAILURE;
    }

    defer nya_system_save_deinit();

    NOTES_ARENA = nya_arena_create(.name = "notes_db");
    defer       nya_arena_destroy(NOTES_ARENA);

    NYA_Error stored = nya_save_database_open(NOTES_ARENA, "notes.db", &NOTES_DB);

    if (!stored.ok) {
        nya_log_error("Could not open the notes database: %s", (NYA_ConstCString)stored.message);

        return EXIT_FAILURE;
    }

    defer nya_sql_close(NOTES_DB);

    NYA_EXPECT(nya_orm_open(NOTES_ARENA, NOTES_DB, &NOTE_MODEL, "notes", &NOTES_TABLE), "while binding the note model to its table");
    defer nya_orm_close(NOTES_TABLE);

    // Creates the table on a first run, and on any later one brings it level with the struct above:
    // add a field and the column appears, and the notes written before it read it as zero. What it
    // will not do is guess at a rename or a drop; see db_migrate.h.
    NYA_EXPECT(nya_orm_schema_migrate(NOTES_TABLE), "while bringing the notes table level with the model");

    /*
     * The database breaker and the two readiness checks it feeds. A handful of consecutive database
     * failures trips the breaker OPEN, and while it is OPEN the "notes-db" readiness check reports
     * not-ready, so /readyz answers 503 and this instance drops out of a load balancer's rotation until
     * the database recovers. The "db" check is the direct one — does the file answer a query now — and
     * the breaker check is the composed one, reflecting a state base_circuit already keeps. See
     * base_circuit.h and http_health.h.
     */
    NYA_EXPECT(nya_circuit_breaker_create(NOTES_ARENA, &DB_BREAKER, .failure_threshold = 3, .open_ms = 5000), "while making the database breaker");

    DB_CIRCUIT = (NYA_HttpHealthCircuit){ .breaker = DB_BREAKER, .key = DB_CIRCUIT_KEY };

    NYA_EXPECT(nya_http_health_check_register("db", example_db_ready, nullptr), "while registering the database readiness check");
    NYA_EXPECT(nya_http_health_check_register("notes-db-breaker", nya_http_health_circuit_ready, &DB_CIRCUIT), "while registering the breaker readiness check");

    // the secret the sessions are signed with, made here so that nothing ships one and a restart ends
    // every session this server issued.
    if (!nya_os_random_bytes(SESSION_SECRET, sizeof(SESSION_SECRET))) {
        nya_log_error("The system random source failed, so no session secret could be made.");

        return EXIT_FAILURE;
    }

    /*
     * The loudest level, on purpose: this example exists to be run and read, and a summary line would
     * show none of what the layer does. Nothing turns the deny list off, and `deny` adds the API key
     * header a reverse proxy in front of a server like this one usually forwards. A program with a
     * config file sets this section in `engine.nya` instead and gets it back on every reload.
     */
    nya_http_log_config_set((NYA_HttpLogConfig){
        .level   = NYA_HTTP_LOG_BODIES,
        .address = NYA_HTTP_LOG_ADDRESS_NETWORK,
        .deny    = "x-api-key",
    });

    /*
     * The idempotency store the notes router's layer shares. Readied here, before the layer can run,
     * so its lock exists for the four workers: a layer runs on whichever worker answers the request,
     * and a store without a lock would race across them. A short TTL, because a client's retries are
     * seconds apart and a key nobody sends again should not hold a slot for long. See http_idempotency.h.
     */
    NYA_EXPECT(nya_http_idempotency_init(NOTES_ARENA, .ttl_s = 120), "while readying the idempotency store");
    defer nya_http_idempotency_deinit();

    /*
     * The proof-of-work wall, when asked for. Its spent-nonce store is readied here, before the layer can
     * run, so its lock exists for the four workers; and the notes router is pointed at the chain that has
     * the wall outermost. A modest difficulty for a demo — a browser clears it unnoticed, a script pays it
     * every request — and the challenge lives a couple of minutes. See http_pow.h.
     */
    if (proof_of_work) {
        NYA_EXPECT(nya_http_pow_init(NOTES_ARENA, .difficulty = 18, .ttl_s = 120), "while readying the proof-of-work store");

        NOTE_ROUTER.layers      = NOTE_LAYERS_POW;
        NOTE_ROUTER.layer_count = nya_carray_length(NOTE_LAYERS_POW);
    }

    defer nya_http_pow_deinit();

    // one root layer, outermost, so its record covers the whole exchange.
    static const NYA_HttpLayerFn LAYERS[] = { nya_http_layer_log };

    NYA_HttpConfig http_config = {
        .port        = port,
        .secret      = SESSION_SECRET,
        .secret_size = sizeof(SESSION_SECRET),
        .workers     = WORKER_COUNT,
        .layers      = LAYERS,
        .layer_count = nya_carray_length(LAYERS),

        // Both or neither, which the server checks: see NYA_HttpConfig. Without them this is plain
        // HTTP on loopback, which is what an example on a laptop wants.
        .certificate_path = certificate_path,
        .key_path         = key_path,
    };

    // A fixed char array on the config, not a pointer, so it is copied in rather than aliased. Empty
    // stays empty, which the server reads as loopback.
    (void)snprintf(http_config.address, sizeof(http_config.address), "%s", address);

    NYA_Error started = nya_system_http_init(http_config);

    if (!started.ok) {
        u8 message[256];
        (void)nya_error_format(&started, message, sizeof(message));
        nya_log_error("Could not start the server: %s", (NYA_ConstCString)message);

        return EXIT_FAILURE;
    }

    defer nya_system_http_deinit();

    /*
     * Reload-safe handlers, in one call: it installs the resolvers that turn a route's
     * `handler_callback` token back into a function on every dispatch, so a handler carried as a token
     * survives a code hot reload where a raw pointer baked into the table would dangle. See
     * core_http_reload.h. With it installed, the notes QUERY route below carries its handler as a token.
     */
    nya_http_router_reloadable();

    // The reload-safe stand-in for `.handler = notes_query`: a token the router re-resolves each
    // dispatch. Written here rather than in the table because nya_callback needs the registry, which
    // is only up now, and because in a hot-reloading build the macro expands to a call, not a constant.
    NOTE_ROUTES[0].handler_callback = nya_callback(notes_query);

    // Merged at the root.
    NYA_EXPECT(nya_http_server_merge(&NOTE_ROUTER), "while merging the notes resource");
    defer nya_http_server_unmerge(&NOTE_ROUTER);

    /*
     * The TOTP second factor. Merged here and not inside the engine for the reason the notes are: the
     * secret and the guard belong to whoever owns the account, which is this program.
     */
    NYA_EXPECT(nya_http_server_merge(&OTP_ROUTER), "while merging the second factor");
    defer nya_http_server_unmerge(&OTP_ROUTER);

    NYA_EXPECT(nya_http_server_merge(&SESSION_ROUTER), "while merging the session");
    defer nya_http_server_unmerge(&SESSION_ROUTER);

    /*
     * The engine's own metrics, which are reflected DTOs: asked for as application/nya-binary they go
     * out carrying their layout hash, the typed half the notes above cannot show. The server does not
     * merge them on its own: without this line /api/metrics is a 404.
     */
    NYA_EXPECT(nya_http_server_merge(nya_http_metrics_router()), "while merging the metrics resource");
    defer nya_http_server_unmerge(nya_http_metrics_router());

    /*
     * Liveness and readiness. /healthz is always 200 while the process can answer; /readyz runs the two
     * checks registered above and answers 503 while the database is down or its breaker is OPEN. What an
     * orchestrator polls to know whether to restart this process and whether to send it traffic.
     */
    NYA_EXPECT(nya_http_server_merge(nya_http_health_router()), "while merging the health routes");
    defer nya_http_server_unmerge(nya_http_health_router());

    /*
     * /openapi.json and /docs, generated by walking every merged route table. Opt in rather than
     * automatic: a server that does not want to publish its own shape should not have to unmerge it.
     */
    NYA_EXPECT(nya_http_server_merge(nya_http_openapi_router()), "while merging the generated document");
    defer nya_http_server_unmerge(nya_http_openapi_router());

    /*
     * The page. The three files are read and hashed once here; merging puts the routes that built on
     * the server, which is the same two steps every other resource here takes. The scratch arena is
     * only alive for the read, because the mount keeps a copy of its own.
     */
    NYA_Arena* scratch = nya_arena_create(.name = "web_bundle_scratch");

    NYA_EXPECT(mount_bundle(scratch), "while reading the web bundle");
    defer nya_http_static_unmount();

    nya_arena_destroy(scratch);

    NYA_EXPECT(nya_http_server_merge(nya_http_static_router()), "while merging the web bundle");
    defer nya_http_server_unmerge(nya_http_static_router());

    /*
     * The mirror attestation. The bundle is mounted and its digest folded above, so now the origin key is
     * loaded, the manifest — this origin, now, that digest — is signed once, and the route that serves it
     * is merged. A key that cannot be loaded is fatal here: an origin that means to publish an attestation
     * and cannot make one should not come up pretending it did. See http_attestation.h and verify_mirror.
     */
    if (!origin_key_load(attestation_seed)) return EXIT_FAILURE;

    defer nya_crypto_sign_key_pair_destroy(&ORIGIN_KEY);

    // The origin string names where these bytes are served from, which is what a client pins alongside the
    // key: the .onion under Tor, set with --attestation-origin or WEB_SERVER_ORIGIN. Loopback is the honest
    // default for a laptop, and says plainly that a mirror check against it only means anything on one host.
    NYA_ConstCString origin = attestation_origin[0] != '\0' ? attestation_origin : getenv("WEB_SERVER_ORIGIN");

    ATTESTATION = (NYA_HttpAttestationManifest){ .issued_at_s = nya_clock_get_timestamp_s(), .content = BUNDLE_DIGEST };
    (void)snprintf(ATTESTATION.origin, sizeof(ATTESTATION.origin), "%s", origin != nullptr && origin[0] != '\0' ? origin : "http://127.0.0.1:8000");

    NYA_EXPECT(nya_http_attestation_sign(&ORIGIN_KEY.secret_key, &ATTESTATION, &ATTESTATION_SIG), "while signing the mirror attestation");
    ATTESTATION_READY = true;

    NYA_EXPECT(nya_http_server_merge(&ATTESTATION_ROUTER), "while merging the mirror attestation");
    defer nya_http_server_unmerge(&ATTESTATION_ROUTER);

    /*
     * The stream. A websocket route is not in the OpenAPI document — the specification describes
     * requests and answers, and this is neither — so it is mounted on its own and documented in the
     * line below and in this file's comment.
     */
    NYA_EXPECT(nya_http_websocket_route_add(&NOTES_STREAM), "while mounting the notes stream");
    defer nya_http_websocket_route_remove(&NOTES_STREAM);

    // What the log tells a person to open: the address that was bound, or loopback when none was given.
    NYA_ConstCString host = address[0] != '\0' ? address : "127.0.0.1";

    nya_log_info("Serving on %s://%s:%u — / for the page, /docs for the generated one, ctrl-c to stop.", secure ? "https" : "http", host,
                 nya_http_server_port());
    nya_log_info("The stylesheet is also at %s, cached for a year.", nya_http_static_url(NYA_ASSET_WEB_APP_CSS));
    nya_log_info("Streaming on %s://%s:%u" NOTES_STREAM_PATH " — a snapshot a second, and one per write.", secure ? "wss" : "ws", host,
                 nya_http_server_port());
    nya_log_info("Liveness at " NYA_HTTP_HEALTHZ_PATH " and readiness at " NYA_HTTP_READYZ_PATH " — %u readiness checks registered.",
                 nya_http_health_check_count());
    nya_log_info("Logging at level %d, addresses as %d: a code posted to " OTP_VERIFY_PATH " is logged as \"" NYA_REFLECT_REDACTED "\".",
                 (s32)nya_http_log_config_get().level, (s32)nya_http_log_config_get().address);
    nya_log_info("Serving a signed mirror attestation at " NYA_HTTP_ATTESTATION_PATH " for origin %s. Verify a mirror with --verify-mirror <url> --origin-key <key>.",
                 ATTESTATION.origin);
    if (proof_of_work) {
        nya_log_info("Proof-of-work is on: the notes resource is walled behind a challenge (see the " NYA_HTTP_POW_TOKEN_HEADER " headers).");
    }

    /*
     * The sockets are the listener thread's now, so what this loop owes the server is the other half:
     * nya_system_http_tick answers the exchanges whose routes asked for this thread and drains the
     * WebSockets. It returns rather than blocking, exactly as it did when it was the whole drain, so a
     * program with other work to do still puts this beside it instead of around it.
     */
    u64 pushed_at_ms = 0;

    while (RUNNING) {
        nya_system_http_tick();

        // The other half of a stream: the server has something to say on its own schedule rather than
        // only when it is asked. A tick with nobody listening builds nothing.
        u64 now_ms = nya_clock_get_timestamp_ms();

        if (now_ms - pushed_at_ms >= STREAM_INTERVAL_MS) {
            pushed_at_ms = now_ms;
            stream_push();
        }

        // as net_echo and the frame limiter do: a real sleep, so the loop does not spin a core. The os
        // layer's own, not SDL's: this loop is the one part of the server that is timing and nothing
        // else, so it has no reason to reach through the window library for a sleep.
        nya_os_time_sleep_ms(TICK_SLEEP_MS);
    }

    nya_log_info("Stopping after " FMTu64 " requests.", nya_http_server_request_count());

    return EXIT_SUCCESS;
}

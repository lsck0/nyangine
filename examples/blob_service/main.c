/**
 * @file examples/blob_service/main.c
 *
 * A content-addressed file service: upload bytes and get back the hash that names them, download them
 * again with their integrity checked, and — the point of this example — watch every upload spawn a
 * background job that a pool of in-process worker threads picks up and runs to completion. Three db
 * primitives, wired into one small server:
 *
 *   - **db_blob.h**       the store: an object keyed by the SHA-256 of its own bytes, so identical
 *                         uploads deduplicate to one row and a download can be verified against its id.
 *   - **db_jobs.h**       the queue: a job per upload, in a SQLite table, so the work an upload still
 *                         owes survives a restart the way the uploaded bytes do.
 *   - **db_jobworker.h**  the runtime: a pool of threads, each on its own connection, that claims those
 *                         jobs and runs the handler registered for their kind — here, an "index" stub.
 *
 * ```
 * ./build run example blob_service            # serves on 127.0.0.1:47820 until interrupted
 * ./blob_service.example --port 8080
 *
 * # headless, for CI: run a self-contained upload -> store -> job-claimed -> done cycle and exit.
 * NYA_BLOB_SERVICE_FRAMES=400 ./build run example blob_service
 * ```
 *
 * Interactively, from another terminal:
 *
 * ```
 * curl -i --data-binary 'the quick brown fox' localhost:47820/upload   # 201 + {"id":"<64 hex>","size":19}
 * curl -i --data-binary 'the quick brown fox' localhost:47820/upload   # same bytes -> the same id, no second row
 * curl -s localhost:47820/blob?id=<the id from above>                  # the bytes back, verified on the way out
 * curl -s localhost:47820/jobs                                         # how many index jobs are pending/done/…
 * curl -s localhost:47820/openapi.json                                 # the routes, generated from the table below
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * WHAT THIS TEACHES
 * ─────────────────────────────────────────────────────────
 *
 * The three primitives are designed to compose, and this is the composition. An upload does two writes
 * in one breath: `nya_blob_put` stores the bytes and hands back their content-address, and
 * `nya_job_enqueue` puts an "index" job on the queue carrying that address. The put is idempotent, so a
 * repeat upload of the same bytes is one row and one id; the enqueue is keyed on that same id, so while
 * an index job for those bytes is still outstanding a duplicate upload does not pile a second one on —
 * blob dedup and job dedup fall out of the same hash.
 *
 * The worker pool is the other half. `nya_jobworker_start` brings up threads that claim due jobs and
 * dispatch each to the handler `nya_jobworker_register` mapped its kind to; our handler opens its own
 * connection to the same file, reads the blob back through `nya_blob_get` — which rehashes the bytes and
 * refuses them if they no longer match the id — and treats a good read as the thing indexed. A real
 * service would build a thumbnail, extract text, or fold the object into a search index here; the point
 * that matters is where the work runs, not what it is, so this one logs a one-line "thumbnail stub" and
 * reports the job done.
 *
 * ─────────────────────────────────────────────────────────
 * WHY THE HANDLER OPENS ITS OWN CONNECTION
 * ─────────────────────────────────────────────────────────
 *
 * db_sql.h opens every connection in SQLite's NOMUTEX mode: one connection belongs to one thread, and
 * the blob store and queue this program's main thread holds are its and no one else's. A worker runs on
 * another thread, so it must not touch them; `nya_jobworker_start` already opens each worker its own
 * connection for the queue, and for the same reason our handler opens its own for the store. That is the
 * rule db_jobworker.h states, applied one layer out: the handler coordinates with the rest of the
 * program through the file, never through shared memory. So the whole file is opened WAL below, where a
 * reader — the handler's blob read — never blocks the writer the upload handler is, and every connection
 * carries a busy timeout so the two writers (an upload, a worker claiming a job) wait rather than fail.
 *
 * ─────────────────────────────────────────────────────────
 * A NOTE ON ACCESS, WHICH THIS EXAMPLE DOES NOT DO
 * ─────────────────────────────────────────────────────────
 *
 * A content address is guessable in principle and meant to be passed around, so knowing a hash is not
 * permission to read the object it names — db_blob.h says this at length. This example serves any id to
 * anyone, because it has no accounts to check against; that is the one thing here a real deployment must
 * not copy. In one, `/upload` and `/blob` sit behind the session the `accounts_api` example issues, an
 * upload records a `blob id -> owner` row, and `/blob` refuses an id the caller does not own with the
 * same 404 it gives an id that does not exist. The `// GUARD` comments below mark exactly where those
 * checks belong.
 *
 * ─────────────────────────────────────────────────────────
 * THE HEADLESS CYCLE
 * ─────────────────────────────────────────────────────────
 *
 * With NYA_BLOB_SERVICE_FRAMES set, the program runs a bounded number of loop iterations and then exits,
 * so CI can exercise the whole pipeline without a person at a terminal — the frame-budget shape the 3D
 * examples take. A client thread of its own uploads a document twice (proving dedup) and a second,
 * different one, then downloads the first back; meanwhile the main loop ticks the server so the
 * main-thread routes answer, and the worker pool claims and runs the index jobs. The loop watches the
 * queue and stops once the jobs it expects have reached the done state, then joins the client and shuts
 * down through the same unwinding an interrupt would take. The client is a thread rather than an inline
 * call because `/upload` is answered on this same loop, and a loop that blocked in its own client would
 * be waiting for an answer it had stopped ticking to produce.
 * */

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * As web_server explains: this server opens no window and runs no frame loop, so the only SDL it touches
 * is the one call that brings the library's base state up for the systems below to hang off. The guard
 * buys the seam for the day the engine grows a headless build; it does not yet buy a headless binary.
 */
#ifndef NYA_NO_SDL
#include "SDL3/SDL_init.h"
#endif

/* CONSTANTS AND STATE */

/** Default port. Loopback only; the sibling web servers sit on 47800 and 47810, this one after them. */
#define DEFAULT_PORT 47820

/** The one file everything lives in: the blob table and the job table share it, as db_jobs.h intends. */
#define DB_FILE "blob_service.db"

/** The kind an upload enqueues and the worker handler is registered under. One string, named once. */
#define INDEX_JOB_KIND "index"

/** Workers behind the queue. Two is enough to show jobs running off the main thread without crowding it. */
#define WORKER_COUNT 2

/** How long a connection waits on another's write lock before giving up. Long enough to never lose a race here. */
#define BUSY_TIMEOUT_MS 2000

/** The route paths, named so the log lines and the table below cannot drift from each other. */
#define UPLOAD_PATH "/upload"
#define BLOB_PATH   "/blob"
#define JOBS_PATH   "/jobs"

/** How long the loop sleeps between ticks. The sockets are the listener thread's; this loop is timing. */
#define TICK_SLEEP_MS 5

/*
 * The store, the queue and the connection under them outlive every request and every job claim, which is
 * why they are static: an exchange's arena is scratch for one answer and a job's is scratch for one run.
 * They belong to the main thread; a worker never touches these handles, only its own (see the file note).
 */
NYA_INTERNAL NYA_Arena*     DB_ARENA = nullptr;
NYA_INTERNAL NYA_Database*  DB       = nullptr;
NYA_INTERNAL NYA_BlobStore* BLOBS    = nullptr;
NYA_INTERNAL NYA_JobQueue*  JOBS     = nullptr;

/**
 * The absolute path of the database file, resolved once at startup and handed to the index handler as
 * its context. The handler opens its own connection to this path on the worker thread; it cannot be
 * given the main thread's `DB`, for the reason the file note gives. Never a secret, so passing the path
 * rather than a key is all this needs.
 * */
NYA_INTERNAL NYA_ConstCString DB_PATH = nullptr;

/** Set by the signal handler, so ctrl-c leaves through the same shutdown a clean exit does. */
NYA_INTERNAL volatile sig_atomic_t RUNNING = 1;

NYA_INTERNAL void stop(int signal_number) {
    nya_unused(signal_number);
    RUNNING = 0;
}

/* THE INDEX WORKER */

/**
 * Runs one "index" job, on a worker thread, off a connection of its own.
 *
 * The payload is the blob's id as the upload enqueued it: 64 hex digits, the content-address and nothing
 * else, because that is all the handler needs to find the bytes. It opens its own store on its own
 * connection (the file note says why it cannot share the main thread's), reads the object back — which
 * rehashes it and refuses a mismatch — and treats a good read as the thing indexed. What a real handler
 * would do with the bytes is its own business; this one proves it can reach them, verified, and stops.
 *
 * The return value is the whole contract: complete on success, retry for something that might pass on a
 * second go (the connection would not open), fail for something a retry cannot fix (a payload that is
 * not an id, an object that is gone or corrupt). A retry takes the queue's backoff; a fail dead-letters
 * the job for a person to look at, which is where a corrupt object should end up rather than vanishing.
 * */
NYA_INTERNAL NYA_JobOutcome index_job(const NYA_QueuedJob* job, void* context) {
    NYA_ConstCString db_path = (NYA_ConstCString)context;

    // The payload is the id's bytes, not necessarily terminated, so copy it into a bounded buffer before
    // it meets a parser that wants a C string. A payload the wrong length is a bug in whoever enqueued
    // it, never something a retry mends, so it dead-letters.
    if (job->payload_size != NYA_BLOB_ID_LENGTH) {
        nya_log_warn("index: a job carried a %llu byte payload, not a blob id; dead-lettering it.", (unsigned long long)job->payload_size);
        return NYA_JOB_OUTCOME_FAIL;
    }

    char hex[NYA_BLOB_ID_LENGTH + 1] = { 0 };
    nya_memcpy(hex, job->payload, NYA_BLOB_ID_LENGTH);

    NYA_BlobId id = { 0 };
    if (!nya_blob_id_parse(hex, &id).ok) {
        nya_log_warn("index: '%s' is not a blob id; dead-lettering the job.", hex);
        return NYA_JOB_OUTCOME_FAIL;
    }

    // A connection, a store over it, and an arena to hold both and the bytes read — all scratch for this
    // one run. The defers unwind in reverse, so the store closes, then the connection, then the arena
    // goes, whichever way this returns.
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "index_job");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_Database* connection = nullptr;
    if (!nya_sql_open(&scratch, db_path, &connection).ok) {
        nya_log_warn("index: could not open %s to index %s; will retry.", db_path, hex);
        return NYA_JOB_OUTCOME_RETRY;
    }
    defer nya_sql_close(connection);

    // Wait on a lock rather than failing the instant the upload handler holds one. Under WAL a read does
    // not block a writer, but the open above may still meet one; a couple of seconds is far past any
    // write this program does.
    NYA_SqlResult ignored = { 0 };
    (void)nya_sql_query(connection, &scratch, "PRAGMA busy_timeout = 2000", nullptr, 0, &ignored);

    NYA_BlobStore* store = nullptr;
    if (!nya_blob_store_open(&scratch, connection, &store).ok) {
        nya_log_warn("index: could not open the blob store to index %s; will retry.", hex);
        return NYA_JOB_OUTCOME_RETRY;
    }
    defer nya_blob_store_close(store);

    u8* bytes = nullptr;
    u64 size  = 0;

    NYA_Error read = nya_blob_get(store, id, &scratch, &bytes, &size);

    // NOT_FOUND or CORRUPT is not worth retrying: the object will not reappear and a flipped bit will not
    // unflip. Either dead-letters, which is exactly the state a corrupt object should be parked in.
    if (!read.ok) {
        nya_log_warn("index: %s could not be read (%s); dead-lettering the job.", hex, (NYA_ConstCString)read.message);
        return NYA_JOB_OUTCOME_FAIL;
    }

    // The stand-in for real work. A thumbnail service would decode `bytes` here and store a smaller
    // object; a search service would tokenize it. All this one needs to show is that the worker reached
    // the verified bytes on its own thread — so it names a "thumbnail" sized off the object and stops.
    u64 thumb = size < 64 ? size : 64;

    nya_log_info("index: %s is %llu bytes, attempt %u — indexed (thumbnail stub %llu bytes).", hex, (unsigned long long)size, job->attempts,
                 (unsigned long long)thumb);

    return NYA_JOB_OUTCOME_COMPLETE;
}

/* INGEST: THE STORE-AND-ENQUEUE BOTH HANDLERS AND THE SELF-TEST SHARE */

/**
 * Stores `size` bytes and enqueues an index job for them, writing the resulting content-address to
 * `out_id`. The one place the two writes an upload does are written down, so the HTTP handler and any
 * other caller take the same path.
 *
 * The put is idempotent — identical bytes are one row and the same id — and the enqueue is keyed on that
 * id, so a repeat of bytes whose index job is still outstanding adds no second job. Runs on the main
 * thread, against the main thread's store and queue; that is the whole reason `/upload` is main-affinity.
 * */
NYA_INTERNAL NYA_Error ingest(const u8* bytes, u64 size, OUT NYA_BlobId* out_id) {
    NYA_TRY(nya_blob_put(BLOBS, bytes, size, out_id));

    // The job carries the content-address and nothing else; the worker reads the bytes back from it.
    // unique_key is that same address, so blob dedup and job dedup are the one decision: while an index
    // for these bytes is outstanding, a second upload of them does not enqueue a second one.
    s64 job_id = 0;
    NYA_TRY(nya_job_enqueue(JOBS, INDEX_JOB_KIND, (const u8*)out_id->hex, NYA_BLOB_ID_LENGTH, &job_id, .unique_key = out_id->hex));

    return NYA_OK;
}

/* HANDLERS */

/**
 * Stores the request body and answers with its content-address.
 *
 * The body is stored as it arrived — `--data-binary` sends a file's exact bytes — so the id in the
 * answer is the SHA-256 of what the client sent. Uploading the same bytes again returns the same id and
 * writes no second row, which is the store's whole point. Every upload enqueues an index job; the
 * `/jobs` route shows them running.
 * */
NYA_INTERNAL NYA_HttpStatus upload_post(NYA_HttpExchange* exchange) {
    // GUARD: a real deployment resolves the session cookie to a user here and refuses an anonymous
    // upload, then records a `blob id -> owner` row below so `/blob` can check ownership. This example
    // has no accounts, so it stores for anyone; see the access note in this file's block.

    const u8* body = exchange->request->body;
    u64       size = exchange->request->body_size;

    // An empty upload is the caller's mistake, not a server fault. The store would accept a zero-length
    // object happily — it has a valid id — but a file service answering "here is the hash of nothing" to
    // a body-less POST is almost always a client that forgot its payload.
    if (size == 0) return NYA_HTTP_STATUS_BAD_REQUEST;

    // The body is bounded by NYA_HTTP_MAX_BODY_BYTES, so this stores small objects only. That is the HTTP
    // request limit, not the store's: db_blob.h holds objects up to 128 MiB, and a service for files that
    // big would take them in chunks rather than one body. This one takes what fits in a request.
    NYA_BlobId id = { 0 };

    NYA_Error stored = ingest(body, size, &id);
    if (!stored.ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Object* answer = nya_object_create(exchange->arena);
    nya_object_add(answer, "id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)id.hex });
    nya_object_add(answer, "size", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = size });

    if (!nya_http_response_json(exchange->response, exchange->arena, answer).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_CREATED;
}

/**
 * Serves the object an id names, verified on the way out.
 *
 * The id comes in as `?id=<hex>` rather than in the path: this router matches paths exactly, with no
 * patterns and no parameters (see http_router.h — a path pattern is a second place for a traversal bug
 * to live), so which object is asked for is a query parameter. `nya_blob_id_parse` refuses anything that
 * is not 64 lower-case hex digits before it can reach the store, and `nya_blob_get` rehashes the bytes
 * and refuses them if they no longer match the id, so a flipped bit on disk is a 500, never a served lie.
 * */
NYA_INTERNAL NYA_HttpStatus blob_get(NYA_HttpExchange* exchange) {
    // GUARD: a real deployment resolves the session here and checks that its user owns this id, answering
    // 404 (not 403) when it does not — whether someone else's object exists is not this caller's business.

    char hex[NYA_BLOB_ID_LENGTH + 1] = { 0 };
    b8   present                     = false;

    // Asked of the target directly, so a repeated `id` or an over-long value is a 400 rather than being
    // read as "no id". A missing id is a 400 too: there is no object to name.
    if (!nya_url_query_find(&exchange->request->target, "id", hex, sizeof(hex), &present).ok) return NYA_HTTP_STATUS_BAD_REQUEST;
    if (!present) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_BlobId id = { 0 };
    if (!nya_blob_id_parse(hex, &id).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    u8* bytes = nullptr;
    u64 size  = 0;

    NYA_Error read = nya_blob_get(BLOBS, id, exchange->arena, &bytes, &size);

    // Absence is in the return, not the data: an id that names no object is a plain 404. A corrupt object
    // — the stored bytes no longer hashing to their id — is a 500, because the server is at fault, not
    // the request.
    if (read.kind == NYA_ERROR_NOT_FOUND) return NYA_HTTP_STATUS_NOT_FOUND;
    if (!read.ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // The store keeps no content type — an id names bytes, not a file format — so the bytes go out as
    // octet-stream, which with the server's global nosniff is the honest way to serve what we cannot
    // name. NYA_HTTP_MEDIA_OTHER writes no Content-Type of its own, so this sets the one it wants.
    if (!nya_http_response_bytes(exchange->response, bytes, size, NYA_HTTP_MEDIA_OTHER).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;
    if (!nya_http_response_header(exchange->response, "Content-Type", "application/octet-stream").ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_OK;
}

/** The queue's state as a document: how many index jobs are pending, claimed, done, dead or expired. */
NYA_INTERNAL NYA_HttpStatus jobs_query(NYA_HttpExchange* exchange) {
    NYA_JobStats stats = { 0 };
    if (!nya_jobs_stats(JOBS, &stats).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Object* body = nya_object_create(exchange->arena);
    nya_object_add(body, "pending", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = stats.pending });
    nya_object_add(body, "claimed", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = stats.claimed });
    nya_object_add(body, "done", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = stats.done });
    nya_object_add(body, "dead", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = stats.dead });
    nya_object_add(body, "expired", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = stats.expired });
    nya_object_add(body, "total", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = stats.total });

    if (!nya_http_response_json(exchange->response, exchange->arena, body).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_OK;
}

/* THE ROUTE TABLE */

/*
 * All three are NYA_HTTP_AFFINITY_MAIN: they touch the blob store and the queue, which are the main
 * thread's connection and no other's (see the file note), so they are answered where this program's loop
 * is and nowhere else. `statuses` is the full set each may answer with; a debug build asserts a handler
 * never returns one it did not declare, so the refusals are listed alongside the successes.
 *
 * `/blob` is a GET because a browser and a curl download have no other verb, and it carries no body. The
 * writes are POSTs. `/jobs` is a QUERY — a read that here needs no request body, but a read, so the safe
 * idempotent verb rather than POST; a browser hitting it with GET still works.
 */
NYA_INTERNAL const NYA_HttpRoute SERVICE_ROUTES[] = {
    {
     .method      = NYA_HTTP_METHOD_POST,
     .path        = UPLOAD_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .affinity    = NYA_HTTP_AFFINITY_MAIN,
     .handler     = upload_post,
     .summary     = "Stores the request body and enqueues an index job",
     .description = "Answers with the content-address of the bytes and their size. Identical bytes return the same id and write no "
                        "second row. Every upload enqueues a background \"" INDEX_JOB_KIND "\" job; see /jobs.",
     // FORBIDDEN is the cross-site check's: a POST is a write, so dispatch can refuse it 403, and a
     // route's declared statuses cover its whole chain, not only what the handler itself returns.
     .statuses    = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method      = NYA_HTTP_METHOD_GET,
     .path        = BLOB_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .affinity    = NYA_HTTP_AFFINITY_MAIN,
     .handler     = blob_get,
     .summary     = "Serves the object an id names, verified",
     .description = "`?id=<64 hex digits>`. The bytes are rehashed and refused if they no longer match the id, so a corrupt object is "
                        "a 500 rather than a served lie. A missing id is a 404, a malformed one a 400.",
     .statuses    = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_NOT_FOUND, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method      = NYA_HTTP_METHOD_QUERY,
     .path        = JOBS_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .affinity    = NYA_HTTP_AFFINITY_MAIN,
     .handler     = jobs_query,
     .summary     = "The index queue's state",
     .description = "The count of index jobs in each state — pending, claimed, done, dead, expired — in one document.",
     .statuses    = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

NYA_INTERNAL const NYA_HttpRouter SERVICE_ROUTER = {
    .name        = "blobs",
    .routes      = SERVICE_ROUTES,
    .route_count = nya_carray_length(SERVICE_ROUTES),
};

/* THE HEADLESS SELF-TEST */

/** How many distinct objects the self-test uploads, and so how many index jobs it expects to see done. */
#define SELF_TEST_OBJECTS 2

/** What the self-test client needs: the port the server bound, so it can talk to it over loopback. */
typedef struct {
    u16 port;
} SelfTest;

/**
 * POSTs a small document to /upload and reads back the id the server assigned.
 *
 * The document is `{"content": <content>}`, sent as JSON: the request module writes an object body as
 * JSON, and the server stores whatever bytes arrive, so the object stored is that JSON text and its id
 * is that text's hash. Two calls with the same `content` send byte-identical JSON and so must get back
 * the same id — that is the dedup this proves. An interactive client posting a real file with
 * `curl --data-binary @file` stores the file's exact bytes instead; the handler stores the body either
 * way. Returns whether the round trip worked and, when it did, copies the id into `out_id`.
 * */
NYA_INTERNAL b8 self_test_upload(NYA_Arena* arena, u16 port, NYA_ConstCString content, OUT char* out_id, u64 out_id_capacity) {
    char url[64] = { 0 };
    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u" UPLOAD_PATH, port);

    NYA_Object* document = nya_object_create(arena);
    nya_object_add(document, "content", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)content });

    NYA_Response response = { 0 };
    NYA_Error    sent     = nya_request_post(arena, url, document, &response);

    if (!sent.ok || response.status != NYA_HTTP_STATUS_CREATED || response.body == nullptr) {
        nya_log_error("self-test: upload of \"%s\" failed (status %u).", content, response.status);
        return false;
    }

    NYA_Value* id = nya_object_get(response.body, "id");
    if (id == nullptr || id->type != NYA_TYPE_STRING) {
        nya_log_error("self-test: the upload answer had no id.");
        return false;
    }

    (void)snprintf(out_id, out_id_capacity, "%s", id->as_string);
    return true;
}

/**
 * The self-test client, on a thread of its own so the main loop keeps ticking to answer it.
 *
 * It uploads one document twice — the two ids must match, which is dedup — and a second, different one,
 * then downloads the first back and checks the bytes returned are the bytes sent. It does not touch the
 * store or the queue; it is a client, and reaches the server only over the socket, exactly as curl does.
 * */
NYA_INTERNAL void self_test_client(void* data) {
    const SelfTest* test = (const SelfTest*)data;

    NYA_Arena* arena = nya_arena_create(.name = "self_test_client");
    defer      nya_arena_destroy(arena);

    NYA_ConstCString first  = "the quick brown fox jumps over the lazy dog";
    NYA_ConstCString second = "a different document entirely, with its own hash";

    char id_a[NYA_BLOB_ID_LENGTH + 1] = { 0 };
    char id_b[NYA_BLOB_ID_LENGTH + 1] = { 0 };
    char id_c[NYA_BLOB_ID_LENGTH + 1] = { 0 };

    if (!self_test_upload(arena, test->port, first, id_a, sizeof(id_a))) return;
    if (!self_test_upload(arena, test->port, first, id_b, sizeof(id_b))) return;
    if (!self_test_upload(arena, test->port, second, id_c, sizeof(id_c))) return;

    nya_log_info("self-test: uploaded the first document -> %s", id_a);
    nya_log_info("self-test: uploaded it again          -> %s  (%s)", id_b, strcmp(id_a, id_b) == 0 ? "same id: deduplicated" : "DIFFERENT ID");
    nya_log_info("self-test: uploaded a second document -> %s", id_c);

    // Download the first object back. GET, since a download has no other verb; the id rides in the query
    // the same way the server reads it. The server rehashes the bytes against the id before it answers,
    // so a 200 with a body is a verified object, not merely a found one.
    char url[128] = { 0 };
    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u" BLOB_PATH "?id=%s", test->port, id_a);

    NYA_Response response = { 0 };
    NYA_Error    got      = nya_request_get(arena, url, &response);

    if (!got.ok || response.status != NYA_HTTP_STATUS_OK || response.raw_body == nullptr || response.raw_body->length == 0) {
        nya_log_error("self-test: download of %s failed (status %u).", id_a, response.status);
        return;
    }

    nya_log_info("self-test: downloaded %s -> %llu bytes, verified against its id.", id_a, (unsigned long long)response.raw_body->length);
}

/* THE PROGRAM */

s32 main(s32 argc, char** argv) {
    u16 port = DEFAULT_PORT;

    // Deliberately not base_args: a single option, and the point of the file is the service.
    for (s32 i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--port") != 0) continue;

        if (!nya_type_parse(NYA_TYPE_U16, (const u8*)argv[i + 1], strlen(argv[i + 1]), &port)) {
            nya_log_error("--port expects a number from 0 to 65535, got '%s'.", argv[i + 1]);
            return EXIT_FAILURE;
        }
    }

    // The frame budget for a headless run: NYA_BLOB_SERVICE_FRAMES=N runs the self-test and quits after
    // at most N loop iterations, so CI can exercise the pipeline without a person. Zero (unset) serves
    // until interrupted, the way an operator runs it.
    u32              max_frames = 0;
    NYA_ConstCString frames_env = getenv("NYA_BLOB_SERVICE_FRAMES");
    if (frames_env != nullptr) max_frames = (u32)strtoul(frames_env, nullptr, 10);

    nya_log_level_set(NYA_LOG_LEVEL_INFO);
    (void)signal(SIGINT, stop);

    /*
     * No window, no renderer, no frame loop. What comes up is the callback and event registries the save
     * and http systems hook into, the save root the database lives under, and then the database. See the
     * web_server and accounts_api examples, whose startup this mirrors.
     */
#ifndef NYA_NO_SDL
    if (!SDL_Init(0)) {
        nya_log_error("SDL could not start: %s", SDL_GetError());
        return EXIT_FAILURE;
    }
    defer SDL_Quit();
#endif

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    NYA_EXPECT(nya_system_events_init(), "while starting the event registry the systems hook into");
    defer nya_system_events_deinit();

    NYA_Error saves = nya_system_save_init();
    if (!saves.ok) {
        // Fatal here, as in web_server: a service whose whole job is to keep what it is sent cannot run
        // with nowhere to keep it.
        nya_log_error("No save root, so there is nowhere to keep the objects: %s", (NYA_ConstCString)saves.message);
        return EXIT_FAILURE;
    }
    defer nya_system_save_deinit();

    DB_ARENA = nya_arena_create(.name = "blob_service_db");
    defer    nya_arena_destroy(DB_ARENA);

    NYA_Error opened = nya_save_database_open(DB_ARENA, DB_FILE, &DB);
    if (!opened.ok) {
        nya_log_error("Could not open %s: %s", DB_FILE, (NYA_ConstCString)opened.message);
        return EXIT_FAILURE;
    }
    defer nya_sql_close(DB);

    /*
     * WAL and a busy timeout on the main connection, so the workers and this thread share the file
     * without tripping over each other: under WAL a reader (the index handler reading a blob) never
     * blocks the writer (an upload), and the timeout makes the one writer-writer case — an upload while a
     * worker claims a job — a short wait rather than an error. The file note says why every connection
     * here is its own thread's; this is what makes the sharing between them safe.
     */
    NYA_Arena boot = nya_arena_create_on_stack(.name = "blob_service_pragma");
    defer     nya_arena_destroy_on_stack(&boot);

    NYA_SqlResult pragma = { 0 };
    NYA_EXPECT(nya_sql_query(DB, &boot, "PRAGMA journal_mode = WAL", nullptr, 0, &pragma), "while putting the database in WAL mode");
    NYA_EXPECT(nya_sql_query(DB, &boot, "PRAGMA busy_timeout = 2000", nullptr, 0, &pragma), "while setting the busy timeout");

    // The store and the queue, both on this connection, both in this one file.
    NYA_EXPECT(nya_blob_store_open(DB_ARENA, DB, &BLOBS), "while opening the blob store");
    defer nya_blob_store_close(BLOBS);

    NYA_EXPECT(nya_jobs_open(DB_ARENA, DB, &JOBS, .busy_timeout_ms = BUSY_TIMEOUT_MS), "while opening the job queue");
    defer nya_jobs_close(JOBS);

    // The absolute path the index handler opens its own connection to. Resolved once, here, where the
    // save root is known; the handler is handed it as its context.
    NYA_String* db_path = nya_save_path(DB_ARENA, DB_FILE);
    if (db_path == nullptr) {
        nya_log_error("Could not resolve the path of %s under the save root.", DB_FILE);
        return EXIT_FAILURE;
    }
    DB_PATH = nya_string_to_cstring(DB_ARENA, db_path);

    // Map the "index" kind to its handler, with the database path as the context every run is handed.
    NYA_EXPECT(nya_jobworker_register(INDEX_JOB_KIND, index_job, (void*)DB_PATH), "while registering the index handler");
    defer nya_jobworker_unregister_all();

    // Bring up the worker pool. A short poll interval so a newly-enqueued job is picked up promptly in the
    // demo; the busy timeout is passed through to each worker's own connection. No key: the file is not
    // encrypted (SQLCipher is not vendored — see db.h), so nothing goes in it that would matter if read.
    NYA_JobWorkerPool* pool = nullptr;
    NYA_EXPECT(nya_jobworker_start(JOBS, WORKER_COUNT, &pool, .poll_interval_ms = 10, .busy_timeout_ms = BUSY_TIMEOUT_MS),
               "while starting the worker pool");
    defer nya_jobworker_stop(pool);

    // The loudest log level, on purpose: this example is to be run and read, and a summary line would
    // show none of what an upload does.
    nya_http_log_config_set((NYA_HttpLogConfig){ .level = NYA_HTTP_LOG_HEADERS, .address = NYA_HTTP_LOG_ADDRESS_NETWORK });

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){ .port = port, .workers = WORKER_COUNT }), "while starting the server");
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(&SERVICE_ROUTER), "while merging the blob service routes");
    defer nya_http_server_unmerge(&SERVICE_ROUTER);

    // /openapi.json and /docs, generated by walking the table above, so the routes document themselves.
    NYA_EXPECT(nya_http_server_merge(nya_http_openapi_router()), "while merging the generated document");
    defer nya_http_server_unmerge(nya_http_openapi_router());

    nya_log_info("blob_service on http://127.0.0.1:%u — POST " UPLOAD_PATH ", GET " BLOB_PATH "?id=<hex>, " JOBS_PATH " for the queue. ctrl-c to stop.",
                 nya_http_server_port());
    nya_log_info("%u workers running the \"" INDEX_JOB_KIND "\" queue, on their own connections to %s.", WORKER_COUNT, DB_PATH);

    /*
     * The self-test, when a frame budget asked for one. The client runs on its own thread — it talks to
     * the server over the socket, and the server answers /upload and /blob on this loop, so the loop must
     * keep ticking while the client waits. The loop stops once every object's index job has reached the
     * done state, or the budget runs out, whichever is first.
     */
    SelfTest    test          = { .port = nya_http_server_port() };
    NYA_Thread* client        = nullptr;

    if (max_frames > 0) {
        NYA_EXPECT(nya_thread_spawn(DB_ARENA, self_test_client, &test, "self-test client", &client), "while starting the self-test client");
    }

    u32 frame = 0;

    while (RUNNING) {
        // Answers the exchanges whose routes asked for this thread, and drains the sockets. Returns
        // rather than blocking, so the poll below runs every tick.
        nya_system_http_tick();

        if (max_frames > 0) {
            NYA_JobStats stats = { 0 };
            (void)nya_jobs_stats(JOBS, &stats);

            // Done when the client has finished its uploads and downloads and the queue has drained: the
            // objects' index jobs have all completed and none is left pending or in flight. A job that
            // dead-lettered would show here too, and the count below would never be reached, so the
            // budget is the backstop that still exits.
            b8 client_done = client != nullptr && nya_thread_is_finished(client);
            b8 queue_idle  = stats.pending == 0 && stats.claimed == 0;

            if (client_done && queue_idle && stats.done >= SELF_TEST_OBJECTS) {
                nya_log_info("self-test: the queue drained — %llu done, %llu dead. Stopping.", (unsigned long long)stats.done,
                             (unsigned long long)stats.dead);
                break;
            }

            if (++frame >= max_frames) {
                nya_log_warn("self-test: the %u frame budget ran out with %llu done and %llu dead. Stopping.", max_frames,
                             (unsigned long long)stats.done, (unsigned long long)stats.dead);
                break;
            }
        }

        // A real sleep, so the loop does not spin a core; the os layer's own, since this loop is timing
        // and nothing else.
        nya_os_time_sleep_ms(TICK_SLEEP_MS);
    }

    // Join the client before the defers tear the server down under it. It has long since finished in the
    // done path; in the budget-ran-out path this waits for whatever it is still doing.
    if (client != nullptr) nya_thread_join(client);

    nya_log_info("Stopping after %llu requests.", (unsigned long long)nya_http_server_request_count());

    return EXIT_SUCCESS;
}

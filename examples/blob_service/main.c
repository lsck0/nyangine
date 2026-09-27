/**
 * @file examples/blob_service/main.c
 *
 * A file service with owners: log in, upload a file as `multipart/form-data`, get it back only as the
 * account that put it there — and, on a schedule, have a pool of worker threads re-verify every stored
 * object against the hash that names it. Four pieces, wired into one small server:
 *
 *   - **http_accounts.h** register, log in, and the `__Host-session` cookie every other route reads.
 *   - **http_files.h**    the upload and download routes: the multipart body parsed by http_multipart.h,
 *                         the bytes put in the db_blob.h store, a row recording who owns them.
 *   - **db_jobs.h**       the queue: one "scrub" job on a cron schedule, a single row that reschedules
 *                         itself after every run instead of adding a row per night.
 *   - **db_jobworker.h**  the runtime: threads, each on its own connection, that claim the scrub when it
 *                         is due and run it.
 *
 * ```
 * ./build run example blob_service            # serves on 127.0.0.1:47820 until interrupted
 * ./blob_service.example --port 8080
 *
 * # headless, for CI: log in, upload, download with and without the session, run the scrub, and exit.
 * NYA_BLOB_SERVICE_FRAMES=400 ./build run example blob_service
 * ```
 *
 * Interactively, from another terminal. The cookie is `__Host-` and `Secure`, so it is passed by hand
 * rather than through a jar that would not send it over plain http:
 *
 * ```
 * curl -H 'Content-Type: application/json' localhost:47820/api/register -d '{"username":"ada","password":"a long passphrase"}'
 * curl -si -H 'Content-Type: application/json' localhost:47820/api/login -d '{"username":"ada","password":"a long passphrase"}' | grep -i set-cookie
 * curl -H 'Cookie: __Host-session=<token>' -F file=@notes.txt localhost:47820/api/files   # 201 {"id":1,"blob":"<hex>",…}
 * curl -H 'Cookie: __Host-session=<token>' 'localhost:47820/api/files?id=1'              # the bytes, as an attachment
 * curl -i 'localhost:47820/api/files?id=1'                                                # 401: no session is nobody
 * curl -s -X QUERY localhost:47820/jobs                                                   # the queue, and when the scrub fires next
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * WHAT THIS TEACHES
 * ─────────────────────────────────────────────────────────
 *
 * None of the upload path is written here. The files facade parses the multipart body, stores the file
 * part content-addressed (identical bytes are one blob however many rows name them), and answers a
 * download only to its owner: 403 to another account, 401 to nobody. What this file adds is the upkeep a
 * store of bytes owes: a nightly scrub that reads every object back through `nya_blob_get`, which rehashes
 * it and refuses a mismatch, so bit rot is found by a job at 03:00 rather than by a user.
 *
 * The scrub is one queue row with `.cron` set and `.unique_key` naming it. Completing it reschedules the
 * same row to the next 03:00 UTC, and the key with `.replace` makes a restart rewrite that row rather than
 * fork a second schedule — which is also how the headless run says "now, not tonight": the same enqueue
 * with `run_at` pinned, after which the job falls back onto its cron.
 *
 * ─────────────────────────────────────────────────────────
 * WHY THE SCRUB OPENS ITS OWN CONNECTION
 * ─────────────────────────────────────────────────────────
 *
 * db_sql.h opens every connection in SQLite's NOMUTEX mode: one connection belongs to one thread, and the
 * store the file routes hold and the queue below are the main thread's. `nya_jobworker_start` opens each
 * worker its own connection for the queue, and for the same reason the scrub opens its own for the store.
 * So the file is opened WAL, where the scrub's reads never block an upload's write, and every connection
 * carries a busy timeout so two writers (an upload, a worker claiming a job) wait rather than fail.
 *
 * ─────────────────────────────────────────────────────────
 * THE HEADLESS CYCLE
 * ─────────────────────────────────────────────────────────
 *
 * With NYA_BLOB_SERVICE_FRAMES set, a client thread registers and logs in, uploads one file twice (two
 * rows, one blob), and downloads it with its session and without one. The main loop ticks the server
 * meanwhile, since every route here is answered on it; once the client is done it pins the scrub to now,
 * and stops when the scrub has run and rescheduled itself, or when the frame budget runs out.
 * */

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/* As web_server explains: this server opens no window and runs no frame loop, so the only SDL it touches is the one call that brings the library's base state up for the systems below to hang off. The guard buys the seam for the day the engine grows a headless build; it does not yet buy a headless binary. */
#ifndef NYA_NO_SDL
#include "SDL3/SDL_init.h"
#endif

/* CONSTANTS AND STATE */

/** Default port. Loopback only; the sibling web servers sit on 47800 and 47810, this one after them. */
#define DEFAULT_PORT 47820

/** The one file everything lives in: accounts, file rows, blobs and the job table share it. */
#define DB_FILE "blob_service.db"

/** The scrub's kind, and the unique key that keeps it one row. */
#define SCRUB_JOB_KIND "scrub"

/** Every day at 03:00 UTC: the quiet hour, and reading the store end to end once a day is plenty. */
#define SCRUB_CRON "0 3 * * *"

/** Workers behind the queue. Two is enough to show jobs running off the main thread without crowding it. */
#define WORKER_COUNT 2

/** How long a connection waits on another's write lock before giving up. Long enough to never lose a race here. */
#define BUSY_TIMEOUT_MS 2000

#define JOBS_PATH "/jobs"

/** How long the loop sleeps between ticks. The sockets are the listener thread's; this loop is timing. */
#define TICK_SLEEP_MS 5

/* The connection and the queue outlive every request and every claim; they are the main thread's, and a worker never touches them (see the file note). */
NYA_INTERNAL NYA_Arena*    DB_ARENA = nullptr;
NYA_INTERNAL NYA_Database* DB       = nullptr;
NYA_INTERNAL NYA_JobQueue* JOBS     = nullptr;
NYA_INTERNAL s64           SCRUB_ID = 0;

/** Set by the signal handler, so ctrl-c leaves through the same shutdown a clean exit does. */
NYA_INTERNAL volatile sig_atomic_t RUNNING = 1;

NYA_INTERNAL void stop(int signal_number) {
    nya_unused(signal_number);
    RUNNING = 0;
}

/* THE SCRUB */

typedef struct {
    NYA_BlobStore* store;
    NYA_Arena*     arena;
    u64            verified;
    u64            corrupt;
} Scrub;

/** One object: nya_blob_get rehashes what it reads and refuses a mismatch, so a read that succeeds is the whole check. */
NYA_INTERNAL b8 scrub_visit(NYA_BlobId id, u64 size, u64 created, void* data) {
    nya_unused(size);
    nya_unused(created);
    Scrub* scrub = (Scrub*)data;

    u8* bytes     = nullptr;
    u64 read_size = 0;

    if (nya_blob_get(scrub->store, id, scrub->arena, &bytes, &read_size).ok) {
        scrub->verified++;
    } else {
        scrub->corrupt++;
        nya_log_warn("scrub: %s no longer hashes to its id.", id.hex);
    }
    return true;
}

/**
 * Runs the scrub on a worker thread, off a connection of its own to the file `context` names.
 *
 * Corruption is reported, not failed: a failing job retries and then dead-letters, and a dead-lettered
 * cron job stops recurring, which would turn one rotten object into a scrub that never runs again. Only a
 * store it could not reach is worth a retry.
 * */
NYA_INTERNAL NYA_JobOutcome scrub_job(const NYA_QueuedJob* job, void* context) {
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "scrub_job");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_Database* connection = nullptr;
    if (!nya_sql_open(&scratch, (NYA_ConstCString)context, &connection).ok) return NYA_JOB_OUTCOME_RETRY;
    defer nya_sql_close(connection);

    NYA_SqlResult ignored = { 0 };
    (void)nya_sql_query(connection, &scratch, "PRAGMA busy_timeout = 2000", nullptr, 0, &ignored);

    Scrub scrub = { .arena = &scratch };
    if (!nya_blob_store_open(&scratch, connection, &scrub.store).ok) return NYA_JOB_OUTCOME_RETRY;
    defer nya_blob_store_close(scrub.store);

    if (!nya_blob_list(scrub.store, scrub_visit, &scrub).ok) return NYA_JOB_OUTCOME_RETRY;

    nya_log_info("scrub: %llu objects verified, %llu corrupt (cron '%s', attempt %u).", (unsigned long long)scrub.verified,
                 (unsigned long long)scrub.corrupt, job->cron, job->attempts);
    return NYA_JOB_OUTCOME_COMPLETE;
}

/** Puts the scrub on its cron, or with `run_at` pinned runs it then and lets it fall back onto the cron. The key and `replace` keep it one row. */
NYA_INTERNAL NYA_Error scrub_schedule(NYA_Instant run_at) {
    return nya_job_enqueue(JOBS, SCRUB_JOB_KIND, nullptr, 0, &SCRUB_ID, .cron = SCRUB_CRON, .unique_key = SCRUB_JOB_KIND, .replace = true,
                           .run_at = run_at);
}

/** When the scrub fires next, as `YYYY-MM-DD HH:MM UTC` into `out` and as an instant into `out_at`. False when the row cannot be read. */
NYA_INTERNAL b8 scrub_next(NYA_Arena* arena, OUT NYA_Instant* out_at, OUT char* out, u64 capacity) {
    NYA_QueuedJob job = { 0 };
    if (!nya_job_get(JOBS, SCRUB_ID, arena, &job).ok) return false;

    NYA_Date      date = { 0 };
    NYA_TimeOfDay time = { 0 };
    nya_instant_to_utc(job.run_at, &date, &time);

    *out_at = job.run_at;
    (void)snprintf(out, capacity, "%04d-%02u-%02u %02u:%02u UTC", date.year, date.month, date.day, time.hour, time.minute);
    return true;
}

/* THE ROUTE: only this example's own; register, login and the file routes are the mounted modules', merged in main. */

/** The queue's state as a document: how many jobs are in each state, and when the scrub fires next. */
NYA_INTERNAL NYA_HttpStatus jobs_query(NYA_HttpExchange* exchange) {
    NYA_JobStats stats = { 0 };
    if (!nya_jobs_stats(JOBS, &stats).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Instant at       = { 0 };
    char        next[32] = { 0 };
    if (!scrub_next(exchange->arena, &at, next, sizeof(next))) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Object* body = nya_object_create(exchange->arena);
    nya_object_add(body, "pending", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = stats.pending });
    nya_object_add(body, "claimed", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = stats.claimed });
    nya_object_add(body, "done", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = stats.done });
    nya_object_add(body, "dead", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = stats.dead });
    nya_object_add(body, "scrub_next", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = next });

    if (!nya_http_response_json(exchange->response, exchange->arena, body).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_OK;
}

NYA_INTERNAL const NYA_HttpRoute SERVICE_ROUTES[] = {
    {
     .method      = NYA_HTTP_METHOD_QUERY,
     .path        = JOBS_PATH,
     .auth        = NYA_HTTP_AUTH_NONE,
     .affinity    = NYA_HTTP_AFFINITY_MAIN,
     .handler     = jobs_query,
     .summary     = "The job queue's state",
     .description = "The count of jobs in each state, and when the nightly scrub fires next, in one document.",
     .statuses    = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

NYA_INTERNAL const NYA_HttpRouter SERVICE_ROUTER = {
    .name        = "blobs",
    .routes      = SERVICE_ROUTES,
    .route_count = nya_carray_length(SERVICE_ROUTES),
};

/* THE HEADLESS SELF-TEST */

#define SELF_TEST_USER     "blob_self_test"
#define SELF_TEST_PASSWORD "a correct horse battery staple"
#define SELF_TEST_BOUNDARY "nyangine-self-test"
#define SELF_TEST_DOCUMENT "the quick brown fox jumps over the lazy dog"

/** The port the server bound, so the client can reach it over loopback. */
typedef struct {
    u16 port;
} SelfTest;

/** POSTs the document as a multipart file part, the body a browser's `<input type=file>` sends, and answers whether it was stored. */
NYA_INTERNAL b8 self_test_upload(NYA_Arena* arena, NYA_ConstCString base, NYA_ConstCString cookie, OUT NYA_Response* out_response) {
    NYA_ConstCString body = "--" SELF_TEST_BOUNDARY "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"fox.txt\"\r\n"
                            "Content-Type: text/plain\r\n\r\n" SELF_TEST_DOCUMENT "\r\n--" SELF_TEST_BOUNDARY "--\r\n";

    NYA_Request request = {
        .method        = NYA_REQUEST_METHOD_POST,
        .url           = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s" NYA_HTTP_FILES_PATH, base)),
        .raw_body      = (const u8*)body,
        .raw_body_size = strlen(body),
        .headers       = { { "Content-Type", "multipart/form-data; boundary=" SELF_TEST_BOUNDARY }, { "Cookie", cookie } },
    };

    return nya_request_perform(arena, request, out_response).ok && out_response->status == NYA_HTTP_STATUS_CREATED && out_response->body != nullptr;
}

/** GETs file `id`, with the session when `cookie` is set and as nobody when it is null, and answers the status. */
NYA_INTERNAL u32 self_test_download(NYA_Arena* arena, NYA_ConstCString base, s64 id, NYA_ConstCString cookie, OUT NYA_Response* out_response) {
    NYA_Request request = {
        .method  = NYA_REQUEST_METHOD_GET,
        .url     = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s" NYA_HTTP_FILES_PATH "?id=" FMTs64, base, id)),
        .headers = { { cookie != nullptr ? "Cookie" : nullptr, cookie } },
    };

    // A non-2xx is an error here as well as a status; the status is the answer this wants, and zero when nothing came back.
    (void)nya_request_perform(arena, request, out_response);
    return out_response->status;
}

/** The client, on its own thread so the main loop keeps ticking to answer it; it reaches the server only over the socket, as curl does. */
NYA_INTERNAL void self_test_client(void* data) {
    const SelfTest* test = (const SelfTest*)data;

    NYA_Arena* arena = nya_arena_create(.name = "self_test_client");
    defer      nya_arena_destroy(arena);

    char base[32] = { 0 };
    (void)snprintf(base, sizeof(base), "http://127.0.0.1:%u", test->port);

    NYA_Object* credentials = nya_object_create(arena);
    nya_object_add(credentials, "username", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)SELF_TEST_USER });
    nya_object_add(credentials, "password", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)SELF_TEST_PASSWORD });

    // A rerun against the same file finds the account already there, so the register's answer is not checked; the login's is.
    NYA_Response response = { 0 };
    (void)nya_request_post(arena, nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s" NYA_HTTP_ACCOUNTS_REGISTER_PATH, base)), credentials,
                           &response);

    char      cookie[256] = { 0 };
    NYA_Error login = nya_request_post(arena, nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s" NYA_HTTP_ACCOUNTS_LOGIN_PATH, base)),
                                       credentials, &response);
    if (!login.ok || !nya_response_header(&response, "set-cookie", cookie, sizeof(cookie))) {
        nya_log_error("self-test: login failed (status %u).", response.status);
        return;
    }

    // `__Host-session=<token>; Path=/; …`: what a browser sends back is the pair before the attributes.
    char* attributes = strchr(cookie, ';');
    if (attributes != nullptr) *attributes = '\0';

    NYA_Response first  = { 0 };
    NYA_Response second = { 0 };
    if (!self_test_upload(arena, base, cookie, &first) || !self_test_upload(arena, base, cookie, &second)) {
        nya_log_error("self-test: an upload failed (status %u, %u).", first.status, second.status);
        return;
    }

    s64              id   = nya_object_get(first.body, "id")->as_s64;
    NYA_ConstCString blob = nya_object_get(first.body, "blob")->as_string;
    nya_log_info("self-test: uploaded fox.txt twice -> rows " FMTs64 " and " FMTs64 ", blob %s (%s)", id, nya_object_get(second.body, "id")->as_s64,
                 blob, strcmp(blob, nya_object_get(second.body, "blob")->as_string) == 0 ? "one blob: deduplicated" : "TWO BLOBS");

    u32 owner  = self_test_download(arena, base, id, cookie, &response);
    b8  intact = owner == NYA_HTTP_STATUS_OK && response.raw_body->length == strlen(SELF_TEST_DOCUMENT) &&
                memcmp(response.raw_body->items, SELF_TEST_DOCUMENT, response.raw_body->length) == 0;
    u32 nobody = self_test_download(arena, base, id, nullptr, &response);

    nya_log_info("self-test: download with the session -> %u (%s), without -> %u (%s)", owner, intact ? "bytes intact" : "BYTES DIFFER", nobody,
                 nobody == NYA_HTTP_STATUS_UNAUTHORIZED ? "refused" : "NOT REFUSED");
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

    // The frame budget for a headless run: NYA_BLOB_SERVICE_FRAMES=N runs the self-test and quits after at most N loop iterations, so CI can exercise the pipeline without a person. Zero (unset) serves until interrupted, the way an operator runs it.
    u32              max_frames = 0;
    NYA_ConstCString frames_env = getenv("NYA_BLOB_SERVICE_FRAMES");
    if (frames_env != nullptr) max_frames = (u32)strtoul(frames_env, nullptr, 10);

    nya_log_level_set(NYA_LOG_LEVEL_INFO);
    (void)signal(SIGINT, stop);

    /* No window, no renderer, no frame loop. What comes up is the callback and event registries the save and http systems hook into, the save root the database lives under, and then the database. See the web_server and accounts_api examples, whose startup this mirrors. */
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
        // Fatal here, as in web_server: a service whose whole job is to keep what it is sent cannot run with nowhere to keep it.
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

    /* WAL and a busy timeout on the main connection, so the workers and this thread share the file without tripping over each other; see the file note. */
    NYA_Arena boot = nya_arena_create_on_stack(.name = "blob_service_boot");
    defer     nya_arena_destroy_on_stack(&boot);

    NYA_SqlResult pragma = { 0 };
    NYA_EXPECT(nya_sql_query(DB, &boot, "PRAGMA journal_mode = WAL", nullptr, 0, &pragma), "while putting the database in WAL mode");
    NYA_EXPECT(nya_sql_query(DB, &boot, "PRAGMA busy_timeout = 2000", nullptr, 0, &pragma), "while setting the busy timeout");

    NYA_EXPECT(nya_accounts_open(DB_ARENA, DB), "while opening the accounts tables");
    defer nya_accounts_close();

    // The key the accounts routes seal their pending second-factor cookie with. Fresh per process: a half-finished login need not survive a restart.
    u8 seal[32] = { 0 };
    if (!nya_os_random_bytes(seal, sizeof(seal))) {
        nya_log_error("No entropy for the login seal key.");
        return EXIT_FAILURE;
    }

    const NYA_HttpRouter* accounts = nya_http_accounts_open((NYA_HttpAccountsConfig){
        .arena                  = DB_ARENA,
        .database               = DB,
        .registration           = NYA_ACCOUNT_REGISTRATION_OPEN,
        .totp_issuer            = "blob_service",
        .login_seal_secret      = seal,
        .login_seal_secret_size = sizeof(seal),
    });
    if (accounts == nullptr) return EXIT_FAILURE;
    defer nya_http_accounts_close();

    const NYA_HttpRouter* files = nya_http_files_open((NYA_HttpFilesConfig){ .arena = DB_ARENA, .database = DB });
    if (files == nullptr) return EXIT_FAILURE;
    defer nya_http_files_close();

    NYA_EXPECT(nya_jobs_open(DB_ARENA, DB, &JOBS, .busy_timeout_ms = BUSY_TIMEOUT_MS), "while opening the job queue");
    defer nya_jobs_close(JOBS);

    NYA_EXPECT(scrub_schedule((NYA_Instant){ 0 }), "while scheduling the nightly scrub");

    // The absolute path the scrub opens its own connection to, handed to it as its context.
    NYA_String* db_path = nya_save_path(DB_ARENA, DB_FILE);
    if (db_path == nullptr) {
        nya_log_error("Could not resolve the path of %s under the save root.", DB_FILE);
        return EXIT_FAILURE;
    }

    NYA_EXPECT(nya_jobworker_register(SCRUB_JOB_KIND, scrub_job, (void*)nya_string_to_cstring(DB_ARENA, db_path)), "while registering the scrub");
    defer nya_jobworker_unregister_all();

    NYA_JobWorkerPool* pool = nullptr;
    NYA_EXPECT(nya_jobworker_start(JOBS, WORKER_COUNT, &pool, .poll_interval_ms = 10, .busy_timeout_ms = BUSY_TIMEOUT_MS),
               "while starting the worker pool");
    defer nya_jobworker_stop(pool);

    // The loudest log level, on purpose: this example is to be run and read, and a summary line would show none of what an upload does.
    nya_http_log_config_set((NYA_HttpLogConfig){ .level = NYA_HTTP_LOG_HEADERS, .address = NYA_HTTP_LOG_ADDRESS_NETWORK });

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){ .port = port, .workers = WORKER_COUNT }), "while starting the server");
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(accounts), "while merging the accounts routes");
    defer nya_http_server_unmerge(accounts);

    NYA_EXPECT(nya_http_server_merge(files), "while merging the file routes");
    defer nya_http_server_unmerge(files);

    NYA_EXPECT(nya_http_server_merge(&SERVICE_ROUTER), "while merging the queue route");
    defer nya_http_server_unmerge(&SERVICE_ROUTER);

    // /openapi.json and /docs, generated by walking every merged table, so the mounted routes document themselves too.
    NYA_EXPECT(nya_http_server_merge(nya_http_openapi_router()), "while merging the generated document");
    defer nya_http_server_unmerge(nya_http_openapi_router());

    NYA_Instant scrub_at = { 0 };
    char        next[32] = { 0 };
    if (!scrub_next(&boot, &scrub_at, next, sizeof(next))) {
        nya_log_error("The scrub was scheduled but its row could not be read back.");
        return EXIT_FAILURE;
    }

    nya_log_info("blob_service on http://127.0.0.1:%u — " NYA_HTTP_FILES_PATH " behind /api/login, " JOBS_PATH " for the queue. ctrl-c to stop.",
                 nya_http_server_port());
    nya_log_info("scrub: job " FMTs64 " on cron '" SCRUB_CRON "', next at %s.", SCRUB_ID, next);

    // The client runs on its own thread: it talks to the server over the socket, and the server answers on this loop, so the loop must keep ticking while the client waits.
    SelfTest    test   = { .port = nya_http_server_port() };
    NYA_Thread* client = nullptr;

    if (max_frames > 0) {
        NYA_EXPECT(nya_thread_spawn(DB_ARENA, self_test_client, &test, "self-test client", &client), "while starting the self-test client");
    }

    u32 frame  = 0;
    b8  kicked = false;

    while (RUNNING) {
        // Answers the exchanges whose routes asked for this thread, and drains the sockets. Returns rather than blocking, so the poll below runs every tick.
        nya_system_http_tick();

        if (max_frames > 0) {
            NYA_Arena tick = nya_arena_create_on_stack(.name = "blob_service_tick");
            defer     nya_arena_destroy_on_stack(&tick);

            // Once the uploads are in, run tonight's scrub now so it has something to verify; it reschedules itself onto the cron when done.
            if (!kicked && nya_thread_is_finished(client)) {
                NYA_EXPECT(scrub_schedule(nya_instant_now()), "while pinning the scrub to now");
                kicked = true;
            }

            // Pinned to now, the row's run_at is in the past until a worker completes it and the cron moves it to the next 03:00.
            if (kicked && scrub_next(&tick, &scrub_at, next, sizeof(next)) && scrub_at.ns > nya_instant_now().ns) {
                nya_log_info("self-test: the scrub ran and rescheduled itself to %s. Stopping.", next);
                break;
            }

            if (++frame >= max_frames) {
                nya_log_warn("self-test: the %u frame budget ran out before the scrub rescheduled. Stopping.", max_frames);
                break;
            }
        }

        // A real sleep, so the loop does not spin a core; the os layer's own, since this loop is timing and nothing else.
        nya_os_time_sleep_ms(TICK_SLEEP_MS);
    }

    // Join the client before the defers tear the server down under it.
    if (client != nullptr) nya_thread_join(client);

    nya_log_info("Stopping after %llu requests.", (unsigned long long)nya_http_server_request_count());

    return EXIT_SUCCESS;
}

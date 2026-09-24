/**
 * @file examples/headless_server/main.c
 *
 * The proof the layering split was for: an HTTP server that compiles and **links** with no libSDL3 on
 * the line and no `core` in the binary at all. Every other server example here — web_server,
 * web_frontend, blob_service, ui_ssr — opens no window and runs no frame loop, yet still links the whole
 * engine, because `core` (the asset, save, callback and event systems they lean on) sits behind
 * `NYA_NO_SDL` in nyangine.h and embeds the renderer by value. This one leans on none of it.
 *
 * It is built with `-DNYA_NO_SDL -DNYA_SERVER`, the seam docs/layering-core-split.md cut: nyangine.h
 * declares `net` and `http` behind it and nyangine.c compiles their translation units behind it, so the
 * reachable engine is `base`, `os`, `math`, `crypto`, `tls`, `acme`, `net`, `http`, `serde` and
 * `template` — and nothing above them. There is no `nya_app_get`, no `nya_asset_read`, no `core_*` and no
 * renderer symbol anywhere below, which is the whole point: reach for one and this example would not
 * link, which is exactly the wall it is here to prove is down.
 *
 * ```
 * ./build --server run example headless_server     # builds the headless artifact (release, stripped)
 * ./headless_server.example --port 8000             # then run it; ctrl-c to stop
 *
 * # headless, for CI: serve, answer its own request, and exit 0.
 * NYA_HEADLESS_SERVER_FRAMES=200 ./headless_server.example
 * ```
 *
 * From another terminal:
 *
 * ```
 * curl -s  localhost:47830/            # the embedded page, served from a C array — no asset system
 * curl -s  localhost:47830/healthz     # 200: liveness, from the engine's own health router
 * curl -s  localhost:47830/status      # {"status":"ok","requests":<n>} — a hand-written JSON handler
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * WHAT THIS TEACHES
 * ─────────────────────────────────────────────────────────
 *
 * A server needs no engine loop to serve. `nya_system_http_init` binds the port and (with workers) starts
 * a listener thread; `nya_system_http_tick` drains the sockets or answers the main-thread routes. A
 * windowed program lets `core_app.c` call the tick once a frame; a headless program owns the loop and
 * calls it itself. That is the entire difference, and it is what the last paragraph of http_server.h's
 * note describes: "a program with no app drives this itself".
 *
 * The two routes are registered by hand, no generated bundle and no asset read:
 *
 *   - **`/`** serves an HTML page whose bytes are a `static const` C array in this file. The response is
 *     written with `nya_http_response_bytes`, which is all http_static does under the hood once the caller
 *     has the bytes — and here the caller *is* the bytes, so there is no asset handle and no file to open.
 *   - **`/status`** is a hand-written JSON handler: it reads the server's own request counter and writes a
 *     small document with `nya_http_response_printf`. No DTO, no reflection, no serde table — the smallest
 *     thing a JSON route can be.
 *
 * The engine's health router is merged for `/healthz` and `/readyz`, since an orchestrator polls them and
 * it costs one line and no core (see http_health.h).
 * */

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS AND STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Default port. Loopback only; the sibling web servers sit on 47800, 47810 and 47820, this one after them. */
#define DEFAULT_PORT 47830

/** How long the loop sleeps between ticks, so it does not spin a core. The os layer's own; this loop is timing. */
#define TICK_SLEEP_MS 5

/** The route paths, named once so the log lines and the table below cannot drift. */
#define ROOT_PATH   "/"
#define STATUS_PATH "/status"

/**
 * The page served at `/`, bytes and all, baked into the binary. No asset system reads this and no file
 * holds it: it is `.rodata`, handed straight to `nya_http_response_bytes`. `sizeof - 1` drops the C
 * string's terminator, since the response carries a length and not a null.
 * */
NYA_INTERNAL const char INDEX_HTML[] =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head><meta charset=\"utf-8\"><title>nyangine headless server</title></head>\n"
    "<body>\n"
    "  <h1>headless_server</h1>\n"
    "  <p>An HTTP server with no SDL and no core linked in. These bytes are a C array in the binary.</p>\n"
    "  <ul>\n"
    "    <li><a href=\"/status\">/status</a> &mdash; a hand-written JSON handler</li>\n"
    "    <li><a href=\"/healthz\">/healthz</a> &mdash; liveness</li>\n"
    "  </ul>\n"
    "</body>\n"
    "</html>\n";

/** Set by the signal handler, so ctrl-c leaves through the same shutdown a clean exit does. */
NYA_INTERNAL volatile sig_atomic_t RUNNING = 1;

NYA_INTERNAL void stop(int signal_number) {
    nya_unused(signal_number);
    RUNNING = 0;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Serves the embedded page. The bytes are `.rodata`, copied into the response as they are. */
NYA_INTERNAL NYA_HttpStatus index_get(NYA_HttpExchange* exchange) {
    if (!nya_http_response_bytes(exchange->response, (const u8*)INDEX_HTML, sizeof(INDEX_HTML) - 1, NYA_HTTP_MEDIA_HTML).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}

/**
 * A minimal JSON handler, written by hand rather than reflected: it reads the server's own request
 * counter and answers a two-field document. `nya_http_response_printf` writes the body and sets the
 * Content-Type from the media type, so nothing here touches serde or a DTO.
 * */
NYA_INTERNAL NYA_HttpStatus status_get(NYA_HttpExchange* exchange) {
    u64 requests = nya_http_server_request_count();

    if (!nya_http_response_printf(exchange->response, NYA_HTTP_MEDIA_JSON, "{\"status\":\"ok\",\"requests\":%llu}", (unsigned long long)requests).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE ROUTE TABLE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Both are NYA_HTTP_AFFINITY_MAIN so they are answered on the loop this program owns rather than a worker,
 * which keeps `nya_http_server_request_count` read where it is written and needs no lock. Neither reads or
 * writes anything else, so WORKER would be safe too; MAIN is the conservative default a hand-written
 * server takes. `statuses` lists every status the route can answer with, a debug build asserting a handler
 * never returns one it did not declare — so FORBIDDEN, the cross-site check's, rides along even though the
 * handlers themselves never return it.
 */
NYA_INTERNAL const NYA_HttpRoute SERVER_ROUTES[] = {
    {
     .method   = NYA_HTTP_METHOD_GET,
     .path     = ROOT_PATH,
     .auth     = NYA_HTTP_AUTH_NONE,
     .affinity = NYA_HTTP_AFFINITY_MAIN,
     .handler  = index_get,
     .summary  = "The embedded landing page",
     .description = "Served from a C array baked into the binary; no asset system and no file are touched.",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method   = NYA_HTTP_METHOD_GET,
     .path     = STATUS_PATH,
     .auth     = NYA_HTTP_AUTH_NONE,
     .affinity = NYA_HTTP_AFFINITY_MAIN,
     .handler  = status_get,
     .summary  = "The server's own request count, as JSON",
     .description = "A hand-written JSON body: {\"status\":\"ok\",\"requests\":<n>}. No DTO and no reflection.",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

NYA_INTERNAL const NYA_HttpRouter SERVER_ROUTER = {
    .name        = "headless",
    .routes      = SERVER_ROUTES,
    .route_count = nya_carray_length(SERVER_ROUTES),
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE HEADLESS SELF-TEST
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How long the self-test waits on any one socket step before giving up. Loopback needs none of it. */
#define SELF_TEST_TIMEOUT_MS 2000

/** The prefix a success answers with, checked byte for byte. */
#define STATUS_LINE_200 "HTTP/1.1 200"

/**
 * Proves the server answers, without a person or an HTTP client, and so without the curl plugin this
 * build deliberately leaves out. It opens a plain TCP socket to the loopback port through the `os` layer
 * — the same non-blocking primitive the server's own listener uses — writes a bare HTTP/1.1 request for
 * `/status`, and checks the reply begins with `HTTP/1.1 200`. That exercises the accept path, the router
 * and a handler over a real socket, the whole meaning of "it serves", using nothing above `os`.
 *
 * The sockets here are non-blocking, so each step loops over NYA_OS_SOCKET_WOULD_BLOCK behind a bounded
 * nya_os_socket_wait rather than blocking — which is exactly how http_server.c drives its own. Returns
 * whether the round trip saw a 200.
 * */
NYA_INTERNAL b8 self_test(u16 port) {
    // Refcounted, so this pairs with the server's own start/stop and never actually stops the library
    // out from under the still-running listener.
    if (nya_os_socket_start() != NYA_OS_SOCKET_OK) {
        nya_log_error("self-test: the host's socket library could not start");
        return false;
    }
    defer nya_os_socket_stop();

    NYA_OsAddress address = { .kind = NYA_OS_ADDRESS_V4, .bytes = { 127, 0, 0, 1 }, .port = port };

    NYA_OsSocket       socket = NYA_OS_SOCKET_NONE;
    NYA_OsSocketStatus st     = nya_os_socket_connect(address, &socket);
    if (st != NYA_OS_SOCKET_OK && st != NYA_OS_SOCKET_WOULD_BLOCK) {
        nya_log_error("self-test: could not connect to 127.0.0.1:%u", port);
        return false;
    }
    defer nya_os_socket_close(socket);

    // A pending connect finishes when the socket turns writable; on loopback it often connected already.
    if (st == NYA_OS_SOCKET_WOULD_BLOCK) {
        NYA_OsSocketWait wait  = { .socket = socket, .writable = true };
        u32              ready = 0;
        if (nya_os_socket_wait(&wait, 1, SELF_TEST_TIMEOUT_MS, &ready) != NYA_OS_SOCKET_OK || ready == 0 || !wait.is_writable) {
            nya_log_error("self-test: the connection never completed");
            return false;
        }
        if (nya_os_socket_error(socket) != NYA_OS_SOCKET_OK) {
            nya_log_error("self-test: the connection failed");
            return false;
        }
    }

    NYA_ConstCString request = "GET " STATUS_PATH " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    u64              total   = strlen(request);
    u64              sent    = 0;

    while (sent < total) {
        u64                went = 0;
        NYA_OsSocketStatus ws   = nya_os_socket_send(socket, (const u8*)request + sent, total - sent, &went);
        if (ws == NYA_OS_SOCKET_OK) {
            sent += went;
            continue;
        }
        if (ws == NYA_OS_SOCKET_WOULD_BLOCK) {
            NYA_OsSocketWait wait  = { .socket = socket, .writable = true };
            u32              ready = 0;
            if (nya_os_socket_wait(&wait, 1, SELF_TEST_TIMEOUT_MS, &ready) != NYA_OS_SOCKET_OK || ready == 0) {
                nya_log_error("self-test: the request could not be sent");
                return false;
            }
            continue;
        }
        nya_log_error("self-test: the request could not be sent");
        return false;
    }

    // The status line arrives in the first packet on loopback, so one good read is enough to judge it.
    u8  reply[512] = { 0 };
    u64 got        = 0;

    while (got < sizeof(reply) - 1) {
        NYA_OsSocketWait wait  = { .socket = socket, .readable = true };
        u32              ready = 0;
        if (nya_os_socket_wait(&wait, 1, SELF_TEST_TIMEOUT_MS, &ready) != NYA_OS_SOCKET_OK || ready == 0) break;

        u64                read = 0;
        NYA_OsSocketStatus rs   = nya_os_socket_receive(socket, reply + got, sizeof(reply) - 1 - got, &read);
        if (rs == NYA_OS_SOCKET_OK) {
            got += read;
            break;
        }
        if (rs == NYA_OS_SOCKET_WOULD_BLOCK) continue;
        break;
    }

    b8 ok = got >= strlen(STATUS_LINE_200) && strncmp((const char*)reply, STATUS_LINE_200, strlen(STATUS_LINE_200)) == 0;
    nya_log_info("self-test: GET " STATUS_PATH " -> %.*s  (%s)", (s32)strlen(STATUS_LINE_200), (const char*)reply, ok ? "ok" : "NOT 200");

    return ok;
}

/** What the self-test thread is handed: the bound port, and where to write its verdict. */
typedef struct {
    u16 port;
    b8* out;
} SelfTestArgs;

/** The self-test thread body: run it once and record whether the server answered 200. */
NYA_INTERNAL void self_test_thread(void* data) {
    SelfTestArgs* args = (SelfTestArgs*)data;
    *args->out         = self_test(args->port);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE PROGRAM
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(s32 argc, char** argv) {
    u16 port = DEFAULT_PORT;

    // Deliberately not base_args: one option, and the file is about the server, not its command line.
    for (s32 i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--port") != 0) continue;

        if (!nya_type_parse(NYA_TYPE_U16, (const u8*)argv[i + 1], strlen(argv[i + 1]), &port)) {
            nya_log_error("--port expects a number from 0 to 65535, got '%s'.", argv[i + 1]);
            return EXIT_FAILURE;
        }
    }

    // The frame budget for a headless run: NYA_HEADLESS_SERVER_FRAMES=N runs the self-test on its own
    // thread and quits once it has answered or after at most N ticks, so CI can prove the server serves
    // without a person. Unset serves until interrupted, the way an operator runs it.
    u32              max_frames = 0;
    NYA_ConstCString frames_env = getenv("NYA_HEADLESS_SERVER_FRAMES");
    if (frames_env != nullptr) max_frames = (u32)strtoul(frames_env, nullptr, 10);

    nya_log_level_set(NYA_LOG_LEVEL_INFO);
    (void)signal(SIGINT, stop);

    /*
     * No SDL, no window, no frame loop, and no core to bring up — not the callback, event or save systems
     * the other server examples start, because none of them is linked. The server is a standalone
     * subsystem driven by the three calls below and nothing else; see http_server.h.
     */
    nya_http_log_config_set((NYA_HttpLogConfig){ .level = NYA_HTTP_LOG_HEADERS, .address = NYA_HTTP_LOG_ADDRESS_NETWORK });

    // One worker, so the listener runs on its own thread and the accept path is exercised the way a real
    // server's is; the MAIN routes are still answered on this loop's tick. Zero would be one drain a
    // tick on this thread, which serves too — the loop below is identical either way.
    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){ .port = port, .workers = 1 }), "while starting the server");
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(&SERVER_ROUTER), "while merging the routes");
    defer nya_http_server_unmerge(&SERVER_ROUTER);

    // /healthz and /readyz, for an orchestrator. The engine ships the router; what "ready" means is the
    // program's, so with no check registered /readyz simply passes. See http_health.h.
    NYA_EXPECT(nya_http_server_merge(nya_http_health_router()), "while merging the health router");
    defer nya_http_server_unmerge(nya_http_health_router());

    nya_log_info("headless_server on http://127.0.0.1:%u — GET " ROOT_PATH ", " STATUS_PATH ", /healthz. ctrl-c to stop.", nya_http_server_port());

    /*
     * The self-test, when a frame budget asked for one. It runs on its own thread because it talks to the
     * server over a socket and the server answers the MAIN routes on this loop — a loop that blocked in
     * its own client would wait for an answer it had stopped ticking to produce, the shape blob_service
     * takes for the same reason.
     */
    b8           passed = false;
    NYA_Thread*  client = nullptr;
    SelfTestArgs args   = { .port = nya_http_server_port(), .out = &passed };

    // A small arena the thread handle lives on, for the frame-budget path only.
    NYA_Arena* thread_arena = nya_arena_create(.name = "headless_self_test_thread");
    defer      nya_arena_destroy(thread_arena);

    if (max_frames > 0) {
        NYA_EXPECT(nya_thread_spawn(thread_arena, self_test_thread, &args, "headless self-test", &client), "while starting the self-test client");
    }

    u32 frame = 0;

    while (RUNNING) {
        // Answers the MAIN routes queued for this thread and drains the sockets. Returns rather than
        // blocking, so the budget check below runs every tick.
        nya_system_http_tick();

        if (max_frames > 0) {
            b8 client_done = client != nullptr && nya_thread_is_finished(client);

            if (client_done) {
                nya_log_info("self-test: %s. Stopping.", passed ? "the server answered 200" : "the server did not answer 200");
                break;
            }

            if (++frame >= max_frames) {
                nya_log_warn("self-test: the %u frame budget ran out before the client finished. Stopping.", max_frames);
                break;
            }
        }

        nya_os_time_sleep_ms(TICK_SLEEP_MS);
    }

    // Join the client before the defers tear the server down under it.
    if (client != nullptr) nya_thread_join(client);

    nya_log_info("Stopping after %llu requests.", (unsigned long long)nya_http_server_request_count());

    // A frame-budgeted run is a test: its exit code is the verdict, so CI can gate on it.
    if (max_frames > 0) return passed ? EXIT_SUCCESS : EXIT_FAILURE;

    return EXIT_SUCCESS;
}

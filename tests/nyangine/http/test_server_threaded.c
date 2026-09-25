/**
 * The same server with workers behind it: answers that overlap, bounds that do not move, a handler
 * that takes its time, and a shutdown that happens while one is still running.
 *
 * Nothing here is timing-sensitive in the sense that matters. Every wait is bounded and every
 * assertion is about an outcome — this answer came back, that count is still two, this one finished
 * while that one was still inside its handler — rather than about an order two threads agreed on. The
 * deterministic, step-at-a-time mode is `workers = 0` and it is what test_server.c drives.
 **/

// first, and before any libc or SDL header: base_basic.h is what settles which POSIX this translation unit asks for, and a thread sanitizer build of this file compiles the engine into it rather than linking one that was compiled on its own.
#include "nyangine-core/nyangine.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "SDL3/SDL_init.h"

#include "nyangine-core/nyangine.c"

/* Under ThreadSanitizer the crash prevention below is skipped: it is a setjmp in one frame and a longjmp out of the crash sink, and tsan's interceptor cannot follow that pair across a thread it did not start the stack of ("can't find longjmp buf"). Everything else in this file is exactly what the ordinary build runs, which is the point of running it under tsan at all. */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TEST_NO_CRASH_PREVENTION 1
#endif
#endif

/* A RESOURCE TO ASK THINGS OF */

#define TEST_FAST_PATH   "/api/test/fast"
#define TEST_SLOW_PATH   "/api/test/slow"
#define TEST_WORKER_PATH "/api/test/worker"
#define TEST_MAIN_PATH   "/api/test/main"

/** How long the slow handler stays inside. Well under NYA_HTTP_SHUTDOWN_GRACE_MS, on purpose. */
#define SLOW_HANDLER_MS 300

/** How many slow handlers are inside one right now, so another test can assert "while it was". */
static atomic u32 SLOW_RUNNING = 0;

static void sleep_ms(u32 milliseconds) {
    struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
    (void)nanosleep(&request, nullptr);
}

static NYA_HttpStatus fast_query(NYA_HttpExchange* exchange) {
    return nya_http_response_text(exchange->response, "fast", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

static NYA_HttpStatus slow_query(NYA_HttpExchange* exchange) {
    (void)atomic_fetch_add(&SLOW_RUNNING, 1);
    sleep_ms(SLOW_HANDLER_MS);
    (void)atomic_fetch_sub(&SLOW_RUNNING, 1);

    return nya_http_response_text(exchange->response, "slow", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * The promise a worker route makes, checked from inside one: this is not the main thread, and the
 * modules that belong to the main thread refuse to be entered from here.
 * */
static NYA_HttpStatus worker_query(NYA_HttpExchange* exchange) {
    if (nya_thread_main_is_current()) return NYA_HTTP_STATUS_INTERNAL_ERROR;

#ifndef TEST_NO_CRASH_PREVENTION
    // the registry is the frame's, and saying so is not a comment. This crashes, and the prevention frame is this thread's, so the assertion is caught here and reported as an answer.
    nya_expect_crash(nya_system_accounting_enable());

    if (nya_crash_caught() == nullptr) return NYA_HTTP_STATUS_INTERNAL_ERROR;
#endif

    return nya_http_response_text(exchange->response, "guarded", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** And the other half: a route that asked for the main thread gets it, and may touch what lives there. */
static NYA_HttpStatus main_query(NYA_HttpExchange* exchange) {
    if (!nya_thread_main_is_current()) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    nya_system_accounting_enable();
    nya_system_accounting_disable();

    return nya_http_response_text(exchange->response, "main", NYA_HTTP_MEDIA_TEXT).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

static const NYA_HttpRoute TEST_ROUTES[] = {
    {
     .method   = NYA_HTTP_METHOD_QUERY,
     .path     = TEST_FAST_PATH,
     .handler  = fast_query,
     .summary  = "Answers at once",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method   = NYA_HTTP_METHOD_QUERY,
     .path     = TEST_SLOW_PATH,
     .handler  = slow_query,
     .summary  = "Sits inside the handler for a while",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method   = NYA_HTTP_METHOD_QUERY,
     .path     = TEST_WORKER_PATH,
     .handler  = worker_query,
     .summary  = "Checks that a worker route runs on a worker and is held to it",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method   = NYA_HTTP_METHOD_QUERY,
     .path     = TEST_MAIN_PATH,
     .affinity = NYA_HTTP_AFFINITY_MAIN,
     .handler  = main_query,
     .summary  = "Checks that a main route runs on the ticking thread",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

static const NYA_HttpRouter TEST_ROUTER = {
    .name        = "test",
    .routes      = TEST_ROUTES,
    .route_count = nya_carray_length(TEST_ROUTES),
};

/* A CLIENT */

#define REQUEST(path) "QUERY " path " HTTP/1.1\r\nHost: localhost\r\n\r\n"

/** Starts on a port the system chose, the way test_server.c does and for the same reason. */
static u16 start_server(NYA_HttpConfig config) {
    u16 port = 0;
    NYA_EXPECT(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port), "the system had no free TCP port");

    config.port = port;
    NYA_EXPECT(nya_system_http_init(config), "while starting the test server");

    return port;
}

static NYA_OsSocket connect_to(u16 port) {
  NYA_OsAddress address = { 0 };
  nya_assert(nya_os_address_resolve("127.0.0.1", port, NYA_OS_ADDRESS_V4, &address) == NYA_OS_SOCKET_OK);

  NYA_OsSocket       socket    = NYA_OS_SOCKET_NONE;
  NYA_OsSocketStatus connected = nya_os_socket_connect(address, &socket);

  nya_assert(connected == NYA_OS_SOCKET_OK || connected == NYA_OS_SOCKET_WOULD_BLOCK);

  // a non-blocking connect is under way rather than done, and writability is how the host says it finished; loopback usually beats the first wait to it.
  NYA_OsSocketWait watched = { .socket = socket, .writable = true };
  u32              ready   = 0;

  nya_assert(nya_os_socket_wait(&watched, 1, 1000, &ready) == NYA_OS_SOCKET_OK);
  nya_assert(nya_os_socket_error(socket) == NYA_OS_SOCKET_OK);

  return socket;
}

static void send_request(NYA_OsSocket socket, NYA_ConstCString text) {
    {
    u64 wrote = 0;
    nya_assert(nya_os_socket_send(socket, (const u8*)text, strlen(text), &wrote) == NYA_OS_SOCKET_OK && wrote == strlen(text));
  }
}

/**
 * Reads one whole answer, or gives up after `timeout_ms` and returns zero.
 *
 * The tick is called around the read because a threaded server still owes the main thread two things:
 * the exchanges whose route asked for it, and the WebSockets.
 * */
static u64 read_answer(NYA_OsSocket socket, OUT char* buffer, u64 capacity, u32 timeout_ms) {
    u64 filled   = 0;
    u64 expected = 0;

    u64 deadline_ns = nya_clock_get_monotonic_ns() + ((u64)timeout_ms * 1000000ULL);

    buffer[0] = '\0';

    while (nya_clock_get_monotonic_ns() < deadline_ns && filled + 1 < capacity) {
        nya_system_http_tick();

        u64                read   = 0;
        NYA_OsSocketStatus status = nya_os_socket_receive(socket, (u8*)(buffer + filled), capacity - filled - 1, &read);

        if (status != NYA_OS_SOCKET_OK && status != NYA_OS_SOCKET_WOULD_BLOCK) break;

        filled         += read;
        buffer[filled]  = '\0';

        if (expected == 0) {
            const char* blank  = strstr(buffer, "\r\n\r\n");
            const char* length = strstr(buffer, "Content-Length: ");

            if (blank != nullptr && length != nullptr) expected = (u64)(blank + 4 - buffer) + strtoull(length + 16, nullptr, 10);
        }

        if (expected > 0 && filled >= expected) return filled;

        sleep_ms(1);
    }

    return expected > 0 && filled >= expected ? filled : 0;
}

/** Ticks and waits, for the cases that are waiting on the server rather than on an answer. */
static void pump(u32 milliseconds) {
    for (u32 elapsed = 0; elapsed < milliseconds; elapsed++) {
        nya_system_http_tick();
        sleep_ms(1);
    }
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_server_threaded");
    defer      nya_arena_destroy(arena);

    char answer[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };

    // TEST: six peers at once, all answered, without anybody driving the drain.
    {
        u16 port = start_server((NYA_HttpConfig){ .workers = 4, .max_connections = 8, .max_connections_per_address = 8 });
        defer nya_system_http_deinit();

        nya_assert(nya_http_server_merge(&TEST_ROUTER).ok);

        NYA_OsSocket clients[6] = { 0 };

        for (u32 index = 0; index < nya_carray_length(clients); index++) clients[index] = connect_to(port);
        defer {
            for (u32 index = 0; index < nya_carray_length(clients); index++) nya_os_socket_close(clients[index]);
        }

        // every request written before any answer is read, so they really are in flight together.
        for (u32 index = 0; index < nya_carray_length(clients); index++) send_request(clients[index], REQUEST(TEST_FAST_PATH));

        for (u32 index = 0; index < nya_carray_length(clients); index++) {
            nya_assert(read_answer(clients[index], answer, sizeof(answer), 5000) > 0, "peer %u went unanswered", index);
            nya_assert(nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 200 OK\r\n"), "peer %u got '%s'", index, answer);
        }

        nya_assert(nya_http_server_request_count() == nya_carray_length(clients));
    }

    // TEST: a handler that takes its time holds up its own connection and nothing else.
    {
        u16 port = start_server((NYA_HttpConfig){ .workers = 2, .max_connections = 4, .max_connections_per_address = 4 });
        defer nya_system_http_deinit();

        nya_assert(nya_http_server_merge(&TEST_ROUTER).ok);

        NYA_OsSocket patient = connect_to(port);
        defer             nya_os_socket_close(patient);

        NYA_OsSocket impatient = connect_to(port);
        defer             nya_os_socket_close(impatient);

        send_request(patient, REQUEST(TEST_SLOW_PATH));

        // waited for rather than assumed: from here there is a handler inside its sleep.
        for (u32 attempt = 0; attempt < 2000 && atomic_load(&SLOW_RUNNING) == 0; attempt++) {
            nya_system_http_tick();
            sleep_ms(1);
        }

        nya_assert(atomic_load(&SLOW_RUNNING) == 1, "the slow handler never started");

        send_request(impatient, REQUEST(TEST_FAST_PATH));

        nya_assert(read_answer(impatient, answer, sizeof(answer), 2000) > 0, "the fast request went unanswered");
        nya_assert(nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 200 OK\r\n"));

        nya_assert(atomic_load(&SLOW_RUNNING) == 1, "the fast answer was not delivered while the slow one was still inside its handler");

        nya_assert(read_answer(patient, answer, sizeof(answer), 5000) > 0, "the slow request went unanswered");
        nya_assert(nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 200 OK\r\n"));
    }

    // TEST: a worker route runs on a worker and is held to what that means; a main route gets the tick.
    {
        u16 port = start_server((NYA_HttpConfig){ .workers = 2 });
        defer nya_system_http_deinit();

        nya_assert(nya_http_server_merge(&TEST_ROUTER).ok);

        NYA_OsSocket client = connect_to(port);
        defer             nya_os_socket_close(client);

        send_request(client, REQUEST(TEST_WORKER_PATH));
        nya_assert(read_answer(client, answer, sizeof(answer), 5000) > 0);
        nya_assert(
            nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 200 OK\r\n"),
            "a worker route did not run on a worker, or the main thread guard did not fire: '%s'",
            answer
        );

        send_request(client, REQUEST(TEST_MAIN_PATH));
        nya_assert(read_answer(client, answer, sizeof(answer), 5000) > 0);
        nya_assert(
            nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 200 OK\r\n"),
            "a main route did not run on the ticking thread: '%s'",
            answer
        );

        nya_assert(!nya_system_accounting_is_enabled(), "the main route left the registry as it found it");
    }

    // TEST: the connection bounds are the same bounds, with threads behind them.
    {
        u16 port = start_server((NYA_HttpConfig){ .workers = 4, .max_connections = 8, .max_connections_per_address = 2 });
        defer nya_system_http_deinit();

        nya_assert(nya_http_server_merge(&TEST_ROUTER).ok);

        NYA_OsSocket sockets[4] = { 0 };
        for (u32 index = 0; index < nya_carray_length(sockets); index++) sockets[index] = connect_to(port);
        defer {
            for (u32 index = 0; index < nya_carray_length(sockets); index++) nya_os_socket_close(sockets[index]);
        }

        pump(200);

        nya_assert(nya_http_server_connection_count() == 2, "a thread does not get to hold more connections than a frame could");
    }

    // TEST: and so is the address's budget, spent from several connections at once.
    {
        u16 port = start_server((NYA_HttpConfig){
            .workers                     = 4,
            .max_connections             = 8,
            .max_connections_per_address = 8,
            .requests_per_second         = 1,
            .request_burst               = 3,
        });
        defer nya_system_http_deinit();

        nya_assert(nya_http_server_merge(&TEST_ROUTER).ok);

        NYA_OsSocket clients[5] = { 0 };
        for (u32 index = 0; index < nya_carray_length(clients); index++) clients[index] = connect_to(port);
        defer {
            for (u32 index = 0; index < nya_carray_length(clients); index++) nya_os_socket_close(clients[index]);
        }

        for (u32 index = 0; index < nya_carray_length(clients); index++) send_request(clients[index], REQUEST(TEST_FAST_PATH));

        u32 served  = 0;
        u32 refused = 0;

        for (u32 index = 0; index < nya_carray_length(clients); index++) {
            nya_assert(read_answer(clients[index], answer, sizeof(answer), 5000) > 0, "peer %u got no answer at all", index);

            NYA_String* received = nya_string_from(arena, answer);

            if (nya_string_starts_with(received, "HTTP/1.1 200 OK\r\n")) served++;
            if (nya_string_starts_with(received, "HTTP/1.1 429 Too Many Requests\r\n")) refused++;
        }

        nya_assert(served == 3, "the burst is three however many connections it arrives on, got %u", served);
        nya_assert(refused == 2, "and everything past it is refused, got %u", refused);
    }

    // TEST: shutdown with a handler still inside. It waits for it, and it comes back.
    {
        u16 port = start_server((NYA_HttpConfig){ .workers = 2 });

        nya_assert(nya_http_server_merge(&TEST_ROUTER).ok);

        NYA_OsSocket client = connect_to(port);
        defer             nya_os_socket_close(client);

        send_request(client, REQUEST(TEST_SLOW_PATH));

        for (u32 attempt = 0; attempt < 2000 && atomic_load(&SLOW_RUNNING) == 0; attempt++) {
            nya_system_http_tick();
            sleep_ms(1);
        }

        nya_assert(atomic_load(&SLOW_RUNNING) == 1, "the slow handler never started");

        u64 started_ns = nya_clock_get_monotonic_ns();
        nya_system_http_deinit();
        u64 took_ms = (nya_clock_get_monotonic_ns() - started_ns) / 1000000ULL;

        nya_assert(atomic_load(&SLOW_RUNNING) == 0, "deinit returned while a handler was still running");
        nya_assert(took_ms < NYA_HTTP_SHUTDOWN_GRACE_MS, "deinit took " FMTu64 " ms, past its own deadline", took_ms);
        nya_assert(!nya_http_server_is_running() && nya_http_server_port() == 0);

        // and it is startable again, which is what says nothing was left half closed.
        u16 again = start_server((NYA_HttpConfig){ .workers = 1 });
        nya_assert(again != 0 && nya_http_server_is_running());
        nya_system_http_deinit();
    }

    printf("PASSED: http server threaded\n");

    return EXIT_SUCCESS;
}

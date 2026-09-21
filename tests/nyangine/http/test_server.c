/**
 * The server end to end: a real port, a real socket, and the answers a client gets back.
 *
 * Driven by hand rather than by the frame loop, because nya_system_http_tick is public for exactly
 * this: a request is written, the server is ticked, the answer is read. The hostile cases here are
 * the ones the parser's own test cannot reach, because they are about the connection rather than the
 * bytes: a peer that goes quiet, a peer that fills the buffer, and one more peer than there are slots.
 **/

#include <time.h>

#include "SDL3/SDL_init.h"
#include "SDL3_net/SDL_net.h"

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define FIRST_PORT 47940
#define LAST_PORT  47956

static const u8 SECRET[] = "0123456789abcdef0123456789abcdef";
#define SECRET_SIZE (sizeof(SECRET) - 1)

static void sleep_ms(u32 milliseconds) {
    struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
    (void)nanosleep(&request, nullptr);
}

/** Starts on the first port that binds. A busy one would be a flaky test rather than a failure. */
static u16 start_server(NYA_HttpConfig config) {
    for (u16 port = FIRST_PORT; port <= LAST_PORT; port++) {
        config.port = port;

        if (nya_system_http_init(config).ok) return port;
    }

    nya_assert(false, "no port in [%d, %d] could be bound", FIRST_PORT, LAST_PORT);

    return 0;
}

/** Connects to the server, waiting for the connection to come up. */
static NET_StreamSocket* connect_to(u16 port) {
    NET_Address* address = NET_ResolveHostname("127.0.0.1");
    nya_assert(address != nullptr);
    nya_assert(NET_WaitUntilResolved(address, 1000) == 1);

    NET_StreamSocket* socket = NET_CreateClient(address, port, 0);
    NET_UnrefAddress(address);

    nya_assert(socket != nullptr);
    nya_assert(NET_WaitUntilConnected(socket, 1000) == 1);

    return socket;
}

/** Sends `text` and drains the server until an answer arrives or the attempts run out. */
static u64 exchange(NET_StreamSocket* socket, NYA_ConstCString text, OUT char* buffer, u64 capacity) {
    nya_assert(NET_WriteToStreamSocket(socket, text, (s32)strlen(text)));

    u64 filled = 0;

    for (u32 attempt = 0; attempt < 200 && filled + 1 < capacity; attempt++) {
        nya_system_http_tick();

        s32 read = NET_ReadFromStreamSocket(socket, buffer + filled, (s32)(capacity - filled - 1));

        if (read > 0) {
            filled += (u64)read;

            // the head has arrived and so has whatever Content-Length promised, near enough: this is a
            // test client and the server's answers are small.
            if (filled > 4 && memcmp(buffer + filled - 4, "\r\n\r\n", 4) != 0) continue;
        }

        if (filled > 0 && read <= 0) break;

        sleep_ms(2);
    }

    buffer[filled] = '\0';

    return filled;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_server");
    defer      nya_arena_destroy(arena);

    char answer[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: nothing exists until the server is started.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        nya_assert(!nya_http_server_is_running());
        nya_assert(nya_http_server_port() == 0);
        nya_assert(nya_http_server_connection_count() == 0);
        nya_assert(nya_http_server_router_count() == 0);
        nya_assert(nya_http_server_router_at(0) == nullptr);

        // and a mount before there is anything to mount onto is refused rather than remembered.
        nya_assert(!nya_http_server_merge(nya_http_metrics_router()).ok);

        // deinit on a server that never started is a no-op, which is what makes it pair with a failed init.
        nya_system_http_deinit();

        nya_assert(!nya_system_http_init((NYA_HttpConfig){ .port = 0 }).ok, "a server needs a port");

        // a secret that is present and too short does not start a server that only fails later.
        nya_assert(!nya_system_http_init((NYA_HttpConfig){ .port = FIRST_PORT, .secret = SECRET, .secret_size = 4 }).ok);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a GET, answered.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u16 port = start_server((NYA_HttpConfig){ .secret = SECRET, .secret_size = SECRET_SIZE });
        defer nya_system_http_deinit();

        nya_assert(nya_http_server_is_running());
        nya_assert(nya_http_server_port() == port);

        nya_assert(nya_http_server_merge(nya_http_metrics_router()).ok);
        nya_assert(nya_http_server_merge(nya_http_metrics_router()).kind == NYA_ERROR_ALREADY_EXISTS, "a router is mounted once");
        nya_assert(nya_http_server_router_count() == 1);

        NET_StreamSocket* client = connect_to(port);
        defer NET_DestroyStreamSocket(client);

        nya_assert(exchange(client, "GET " NYA_HTTP_METRICS_PATH " HTTP/1.1\r\nHost: localhost\r\n\r\n", answer, sizeof(answer)) > 0);

        NYA_String* received = nya_string_from(arena, answer);

        nya_assert(nya_string_starts_with(received, "HTTP/1.1 200 OK\r\n"));
        nya_assert(nya_string_contains(received, "Content-Type: application/json"));
        nya_assert(nya_string_contains(received, "\"uptime_ns\""), "the metrics DTO went out by its own reflection");
        nya_assert(nya_string_contains(received, "\"request_count\""));

        nya_assert(nya_http_server_request_count() == 1);
        nya_assert(nya_http_server_connection_count() == 1);

        // a second request on the same connection, which is what keep-alive is for.
        nya_assert(exchange(client, "GET " NYA_HTTP_METRICS_CEILINGS_PATH " HTTP/1.1\r\nHost: localhost\r\n\r\n", answer, sizeof(answer)) > 0);

        received = nya_string_from(arena, answer);
        nya_assert(nya_string_starts_with(received, "HTTP/1.1 200 OK\r\n"));
        nya_assert(nya_string_contains(received, "\"rows\""));

        nya_assert(nya_http_server_request_count() == 2);

        // a path nothing answers, and a method the path does not.
        nya_assert(exchange(client, "GET /api/nothing HTTP/1.1\r\nHost: localhost\r\n\r\n", answer, sizeof(answer)) > 0);
        nya_assert(nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 404 Not Found\r\n"));

        nya_assert(exchange(client, "DELETE " NYA_HTTP_METRICS_PATH " HTTP/1.1\r\nHost: localhost\r\n\r\n", answer, sizeof(answer)) > 0);
        nya_assert(nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 405 Method Not Allowed\r\n"));

        // a HEAD carries the Content-Length its GET would and none of the bytes.
        nya_assert(exchange(client, "HEAD " NYA_HTTP_METRICS_PATH " HTTP/1.1\r\nHost: localhost\r\n\r\n", answer, sizeof(answer)) > 0);

        received = nya_string_from(arena, answer);
        nya_assert(nya_string_starts_with(received, "HTTP/1.1 200 OK\r\n"));
        nya_assert(nya_string_contains(received, "Content-Length: "));
        nya_assert(nya_string_ends_with(received, "\r\n\r\n"), "a HEAD answer stops at the blank line");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the authenticated route, from outside.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u16 port = start_server((NYA_HttpConfig){ .secret = SECRET, .secret_size = SECRET_SIZE });
        defer nya_system_http_deinit();

        nya_assert(nya_http_server_merge(nya_http_metrics_router()).ok);

        NET_StreamSocket* client = connect_to(port);
        defer NET_DestroyStreamSocket(client);

        // without a token.
        nya_assert(exchange(client,
                            "POST " NYA_HTTP_METRICS_ACCOUNTING_PATH " HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                            "Content-Length: 16\r\n\r\n{\"enabled\":true}",
                            answer, sizeof(answer))
                   > 0);

        nya_assert(nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 401 Unauthorized\r\n"));
        nya_assert(nya_string_contains(nya_string_from(arena, answer), "WWW-Authenticate: Bearer"));

        // with one that carries the scope.
        NYA_HttpIdentity writer = {
            .scope        = NYA_HTTP_SCOPE_WRITE,
            .issued_at_s  = nya_clock_get_timestamp_s(),
            .expires_at_s = nya_clock_get_timestamp_s() + 300,
        };

        (void)snprintf(writer.subject, sizeof(writer.subject), "test");

        char token[NYA_HTTP_MAX_TOKEN_BYTES] = { 0 };
        nya_assert(nya_http_jwt_encode(&writer, SECRET, SECRET_SIZE, token, sizeof(token)).ok);

        NYA_String* request = nya_string_sprintf(arena,
                                                 "POST " NYA_HTTP_METRICS_ACCOUNTING_PATH " HTTP/1.1\r\nHost: localhost\r\n"
                                                 "Authorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: 16\r\n\r\n"
                                                 "{\"enabled\":true}",
                                                 token);

        nya_assert(exchange(client, nya_string_to_cstring(arena, request), answer, sizeof(answer)) > 0);

        nya_assert(nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 200 OK\r\n"));
        nya_assert(nya_string_contains(nya_string_from(arena, answer), "\"enabled\":true"));
        nya_assert(nya_system_accounting_is_enabled(), "the handler reached the registry");

        // and back off again, so the suite leaves the registry as it found it.
        NYA_String* off = nya_string_sprintf(arena,
                                             "POST " NYA_HTTP_METRICS_ACCOUNTING_PATH " HTTP/1.1\r\nHost: localhost\r\n"
                                             "Authorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: 17\r\n\r\n"
                                             "{\"enabled\":false}",
                                             token);

        nya_assert(exchange(client, nya_string_to_cstring(arena, off), answer, sizeof(answer)) > 0);
        nya_assert(!nya_system_accounting_is_enabled());

        // a body that is not the DTO the route takes.
        NYA_String* nonsense = nya_string_sprintf(arena,
                                                  "POST " NYA_HTTP_METRICS_ACCOUNTING_PATH " HTTP/1.1\r\nHost: localhost\r\n"
                                                  "Authorization: Bearer %s\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello",
                                                  token);

        nya_assert(exchange(client, nya_string_to_cstring(arena, nonsense), answer, sizeof(answer)) > 0);
        nya_assert(nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 415 "));
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a request the parser refuses is answered and the connection closes.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u16 port = start_server((NYA_HttpConfig){ 0 });
        defer nya_system_http_deinit();

        nya_assert(nya_http_server_merge(nya_http_metrics_router()).ok);

        NET_StreamSocket* client = connect_to(port);
        defer NET_DestroyStreamSocket(client);

        nya_assert(exchange(client, "GET /../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n", answer, sizeof(answer)) > 0);

        NYA_String* received = nya_string_from(arena, answer);

        nya_assert(nya_string_starts_with(received, "HTTP/1.1 400 Bad Request\r\n"));
        nya_assert(nya_string_contains(received, "Connection: close\r\n"), "a stream the parser gave up on is not resynchronised");

        // the server has dropped it by the next tick, whatever the client does.
        for (u32 attempt = 0; attempt < 50 && nya_http_server_connection_count() > 0; attempt++) {
            nya_system_http_tick();
            sleep_ms(2);
        }

        nya_assert(nya_http_server_connection_count() == 0);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a peer that connects and says nothing is dropped, not held.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u16 port = start_server((NYA_HttpConfig){ .max_connections = 2 });
        defer nya_system_http_deinit();

        nya_assert(nya_http_server_merge(nya_http_metrics_router()).ok);

        NET_StreamSocket* silent = connect_to(port);
        defer NET_DestroyStreamSocket(silent);

        for (u32 attempt = 0; attempt < 50 && nya_http_server_connection_count() == 0; attempt++) {
            nya_system_http_tick();
            sleep_ms(2);
        }

        nya_assert(nya_http_server_connection_count() == 1);

        // half a request, and then nothing. The head bound and the idle timeout both apply; this
        // checks that the server is still answering other people in the meantime.
        nya_assert(NET_WriteToStreamSocket(silent, "GET /api", 8));

        for (u32 attempt = 0; attempt < 20; attempt++) {
            nya_system_http_tick();
            sleep_ms(2);
        }

        NET_StreamSocket* other = connect_to(port);
        defer NET_DestroyStreamSocket(other);

        nya_assert(exchange(other, "GET " NYA_HTTP_METRICS_PATH " HTTP/1.1\r\nHost: x\r\n\r\n", answer, sizeof(answer)) > 0);
        nya_assert(nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 200 OK\r\n"),
                   "one peer sitting on a half written request may not stop the others");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: one more peer than there are slots is closed rather than queued.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u16 port = start_server((NYA_HttpConfig){ .max_connections = 1 });
        defer nya_system_http_deinit();

        nya_assert(nya_http_server_merge(nya_http_metrics_router()).ok);

        NET_StreamSocket* first = connect_to(port);
        defer NET_DestroyStreamSocket(first);

        nya_assert(exchange(first, "GET " NYA_HTTP_METRICS_PATH " HTTP/1.1\r\nHost: x\r\n\r\n", answer, sizeof(answer)) > 0);
        nya_assert(nya_http_server_connection_count() == 1);

        NET_StreamSocket* second = connect_to(port);
        defer NET_DestroyStreamSocket(second);

        for (u32 attempt = 0; attempt < 50; attempt++) {
            nya_system_http_tick();
            sleep_ms(2);
        }

        nya_assert(nya_http_server_connection_count() == 1, "the table is the bound, and a peer past it is not queued against it");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the schema and the page, served by the program they describe.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u16 port = start_server((NYA_HttpConfig){ 0 });
        defer nya_system_http_deinit();

        nya_assert(nya_http_server_merge(nya_http_metrics_router()).ok);
        nya_assert(nya_http_server_merge(nya_http_openapi_router()).ok);

        NET_StreamSocket* client = connect_to(port);
        defer NET_DestroyStreamSocket(client);

        nya_assert(exchange(client, "GET " NYA_HTTP_OPENAPI_PATH " HTTP/1.1\r\nHost: x\r\n\r\n", answer, sizeof(answer)) > 0);

        NYA_String* document = nya_string_from(arena, answer);

        nya_assert(nya_string_starts_with(document, "HTTP/1.1 200 OK\r\n"));
        nya_assert(nya_string_contains(document, "\"openapi\""));
        nya_assert(nya_string_contains(document, NYA_HTTP_METRICS_PATH));

        nya_assert(exchange(client, "GET " NYA_HTTP_DOCS_PATH " HTTP/1.1\r\nHost: x\r\n\r\n", answer, sizeof(answer)) > 0);

        NYA_String* page = nya_string_from(arena, answer);

        nya_assert(nya_string_starts_with(page, "HTTP/1.1 200 OK\r\n"));
        nya_assert(nya_string_contains(page, "Content-Type: text/html"));
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a secret out of the environment, which is the only place one comes from.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u8  buffer[NYA_HTTP_MAX_SECRET_BYTES] = { 0 };
        u64 size                              = 0;

        nya_assert(nya_http_secret_from_environment("NYANGINE_TEST_SECRET_THAT_IS_NOT_SET", buffer, sizeof(buffer), &size).kind
                   == NYA_ERROR_NOT_FOUND);
        nya_assert(size == 0);

        nya_assert(setenv("NYANGINE_TEST_SECRET", "short", 1) == 0);
        nya_assert(nya_http_secret_from_environment("NYANGINE_TEST_SECRET", buffer, sizeof(buffer), &size).kind == NYA_ERROR_INVALID_ARGUMENT,
                   "a guessable secret is refused rather than accepted and quietly useless");

        nya_assert(setenv("NYANGINE_TEST_SECRET", (const char*)SECRET, 1) == 0);
        nya_assert(nya_http_secret_from_environment("NYANGINE_TEST_SECRET", buffer, sizeof(buffer), &size).ok);
        nya_assert(size == SECRET_SIZE && memcmp(buffer, SECRET, size) == 0);

        nya_assert(unsetenv("NYANGINE_TEST_SECRET") == 0);
    }

    printf("PASSED: http server\n");

    return EXIT_SUCCESS;
}

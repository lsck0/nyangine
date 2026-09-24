/**
 * Development live reload, in its three separable halves.
 *
 * The change detection is pure: a fingerprint against the last one, so it is checked directly with
 * numbers, and separately over a real mount of the static bundle, where the point is that the fingerprint
 * moves when the bytes do and not otherwise. The push is checked over a real socket the way
 * test_websocket.c does it — a client on /livereload, a signalled change, and the "reload" that lands on
 * it — because a watch that fires into nothing is not a live reload. And the client snippet is checked for
 * the shape a page depends on, since it is served as-is and a page cannot patch it.
 *
 * No SDL and no app: a mount is handed its bytes here rather than reading them through the asset system,
 * and the http server comes up on its own, so this test is the module and the socket and nothing else.
 **/

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#define FIRST_PORT 47940
#define LAST_PORT  47956

/** How long the two ends are pumped against each other before a step is called lost. */
#define PUMP_STEPS 400

/** Bytes handed to a mount, so it is the fingerprint being tested and never a read that went wrong. */
static const u8 BYTES_A[] = "<!doctype html><title>a</title>";
static const u8 BYTES_B[] = "<!doctype html><title>b</title>";
#define BYTES_A_SIZE (sizeof(BYTES_A) - 1)
#define BYTES_B_SIZE (sizeof(BYTES_B) - 1)

static void sleep_ms(u32 milliseconds) {
    struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
    (void)nanosleep(&request, nullptr);
}

/** Mounts one html file of `data` bytes, so the bundle has a fingerprint to read. */
static void mount_one(const u8* data, u64 size) {
    NYA_HttpStaticFile file = { .asset = "./assets/web/livereload_test.html", .path = "/", .data = data, .size = size };
    NYA_EXPECT(nya_http_static_mount((NYA_HttpStaticConfig){ .files = &file, .count = 1 }));
}

/* THE CLIENT, LIFTED FROM test_websocket.c */

#define CLIENT_MESSAGE_BYTES 4096

typedef struct {
    NYA_OsSocket socket;

    NYA_WebSocketProtocol protocol;
    u8                    send[CLIENT_MESSAGE_BYTES];
    u8                    message[CLIENT_MESSAGE_BYTES + 1];

    u8  receive[CLIENT_MESSAGE_BYTES];
    u64 receive_size;

    u32 texts;

    u8  last[CLIENT_MESSAGE_BYTES];
    u64 last_size;
} Client;

static u16 start_server(void) {
    for (u16 port = FIRST_PORT; port <= LAST_PORT; port++) {
        if (nya_system_http_init((NYA_HttpConfig){ .port = port }).ok) return port;
    }

    nya_assert(false, "no port in [%d, %d] could be bound", FIRST_PORT, LAST_PORT);

    return 0;
}

static NYA_OsSocket connect_to(u16 port) {
    NYA_OsAddress address = { 0 };
    nya_assert(nya_os_address_resolve("127.0.0.1", port, NYA_OS_ADDRESS_V4, &address) == NYA_OS_SOCKET_OK);

    NYA_OsSocket       socket    = NYA_OS_SOCKET_NONE;
    NYA_OsSocketStatus connected = nya_os_socket_connect(address, &socket);

    nya_assert(connected == NYA_OS_SOCKET_OK || connected == NYA_OS_SOCKET_WOULD_BLOCK);

    NYA_OsSocketWait watched = { .socket = socket, .writable = true };
    u32              ready   = 0;

    nya_assert(nya_os_socket_wait(&watched, 1, 1000, &ready) == NYA_OS_SOCKET_OK);
    nya_assert(nya_os_socket_error(socket) == NYA_OS_SOCKET_OK);

    return socket;
}

static NYA_CString key_make(NYA_Arena* arena) {
    u8 nonce[NYA_WEBSOCKET_KEY_BYTES] = { 0 };
    nya_assert(nya_os_random_bytes(nonce, sizeof(nonce)));

    NYA_String* encoded = nya_string_create(arena);
    nya_base64_encode(encoded, nonce, sizeof(nonce));

    return nya_string_to_cstring(arena, encoded);
}

static u64 handshake(NYA_OsSocket socket, NYA_ConstCString request, OUT char* buffer, u64 capacity) {
    {
        u64 wrote = 0;
        nya_assert(nya_os_socket_send(socket, (const u8*)request, strlen(request), &wrote) == NYA_OS_SOCKET_OK && wrote == strlen(request));
    }

    u64 filled = 0;

    for (u32 attempt = 0; attempt < PUMP_STEPS && filled + 1 < capacity; attempt++) {
        nya_system_http_tick();

        u64                read   = 0;
        NYA_OsSocketStatus status = nya_os_socket_receive(socket, (u8*)(buffer + filled), capacity - filled - 1, &read);

        if (status != NYA_OS_SOCKET_OK && status != NYA_OS_SOCKET_WOULD_BLOCK) break;

        filled         += read;
        buffer[filled]  = '\0';

        if (strstr(buffer, "\r\n\r\n") != nullptr) break;

        sleep_ms(2);
    }

    buffer[filled] = '\0';

    return filled;
}

static void client_open(Client* client, NYA_Arena* arena, u16 port, NYA_ConstCString path) {
    nya_memset(client, 0, sizeof(*client));

    client->socket = connect_to(port);

    NYA_CString key                                       = key_make(arena);
    char        expected[NYA_WEBSOCKET_ACCEPT_LENGTH + 1] = { 0 };
    NYA_EXPECT(nya_websocket_accept_from_key(key, expected));

    NYA_ConstCString request = nya_string_to_cstring(
        arena,
        nya_string_sprintf(
            arena,
            "GET %s HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
            path,
            key
        )
    );

    char answer[2048] = { 0 };
    nya_assert(handshake(client->socket, request, answer, sizeof(answer)) > 0);
    nya_assert(nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 101 Switching Protocols\r\n"), "got:\n%s", answer);

    NYA_EXPECT(nya_websocket_protocol_open(
        &client->protocol,
        (NYA_WebSocketProtocolConfig){
            .role             = NYA_WEBSOCKET_ROLE_CLIENT,
            .message          = client->message,
            .message_capacity = CLIENT_MESSAGE_BYTES,
            .send             = client->send,
            .send_capacity    = sizeof(client->send),
        }
    ));
}

static void client_destroy(Client* client) {
    if (client->socket.handle == 0) return;

    nya_os_socket_close(client->socket);
    client->socket = NYA_OS_SOCKET_NONE;
}

static void client_step(Client* client) {
    u64       pending = 0;
    const u8* queued  = nya_websocket_protocol_pending(&client->protocol, &pending);

    if (pending > 0) {
        {
            u64 wrote = 0;
            nya_assert(nya_os_socket_send(client->socket, (const u8*)queued, pending, &wrote) == NYA_OS_SOCKET_OK);
        }
        nya_websocket_protocol_flushed(&client->protocol, pending);
    }

    nya_system_http_tick();

    u64 room = sizeof(client->receive) - client->receive_size;

    if (room > 0) {
        u64 read = 0;
        (void)nya_os_socket_receive(client->socket, client->receive + client->receive_size, room, &read);
        if (read > 0) client->receive_size += read;
    }

    for (u32 step = 0; step < 16; step++) {
        NYA_WebSocketEvent event    = { 0 };
        u64                consumed = 0;

        b8 produced = nya_websocket_protocol_receive(&client->protocol, client->receive, client->receive_size, &consumed, &event);

        client->receive_size -= consumed;
        if (client->receive_size > 0 && consumed > 0) nya_memmove(client->receive, client->receive + consumed, client->receive_size);

        if (!produced) break;

        if (event.kind == NYA_WEBSOCKET_EVENT_TEXT) client->texts++;

        if (event.size > 0 && event.size <= sizeof(client->last)) {
            nya_memcpy(client->last, event.data, event.size);
            client->last_size = event.size;
        }
    }

    sleep_ms(1);
}

static void client_wait(Client* client, const u32* counter, u32 target) {
    for (u32 step = 0; step < PUMP_STEPS && *counter < target; step++) client_step(client);
}

/** Pumps the server a while so a push that should NOT have happened has every chance to arrive. */
static void client_settle(Client* client) {
    for (u32 step = 0; step < 40; step++) client_step(client);
}

/* THE TESTS */

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_livereload");
    defer      nya_arena_destroy(arena);

    // TEST: this build has live reload at all. Everything below depends on it.
    {
        nya_assert(nya_http_livereload_available(), "a test build is not a shipping build; live reload is compiled in");

        printf("  live reload is compiled into this build\n");
    }

    // TEST: the change detector fires exactly when the fingerprint moves, and never otherwise.
    {
        nya_http_livereload_reset();

        // The first fingerprint is recorded, not reloaded on: a page that just connected is not thrown out from under itself.
        nya_assert(!nya_http_livereload_signal(0x1111), "the first sight of a fingerprint is a baseline, not a reload");

        // The same number, again and again, is no change and no reload. A timer would have fired here.
        nya_assert(!nya_http_livereload_signal(0x1111), "an unchanged fingerprint is not a reload");
        nya_assert(!nya_http_livereload_signal(0x1111), "still not");

        // A different number is a change, exactly once.
        nya_assert(nya_http_livereload_signal(0x2222), "a moved fingerprint is a reload");
        nya_assert(!nya_http_livereload_signal(0x2222), "and then it is the new baseline");

        // Moving back is a change too — the detector compares, it does not only ratchet forward.
        nya_assert(nya_http_livereload_signal(0x1111), "moving back to an old fingerprint is still a change");

        // A reset makes the next sight a first sight again: recorded, not reloaded on.
        nya_http_livereload_reset();
        nya_assert(!nya_http_livereload_signal(0x9999), "after a reset the next fingerprint is a baseline again");

        printf("  the change detector fired on every move of the fingerprint and on nothing else\n");
    }

    // TEST: the bundle fingerprint is the bytes' — it moves when they do and not when they do not.
    {
        nya_assert(nya_http_static_fingerprint() == 0, "an unmounted bundle has no fingerprint");

        mount_one(BYTES_A, BYTES_A_SIZE);
        u64 first = nya_http_static_fingerprint();
        nya_assert(first != 0, "a mounted bundle has a fingerprint");

        // The same bytes mounted again hash to the same fingerprint: a rebuild that changed nothing is not a change, so a poll over it would not reload.
        nya_http_static_unmount();
        mount_one(BYTES_A, BYTES_A_SIZE);
        nya_assert(nya_http_static_fingerprint() == first, "the same bytes are the same fingerprint");

        // Different bytes are a different fingerprint, which is the whole signal.
        nya_http_static_unmount();
        mount_one(BYTES_B, BYTES_B_SIZE);
        u64 second = nya_http_static_fingerprint();
        nya_assert(second != first, "different bytes are a different fingerprint");
        nya_assert(second != 0);

        nya_http_static_unmount();
        nya_assert(nya_http_static_fingerprint() == 0, "an unmounted bundle is back to no fingerprint");

        printf("  the bundle fingerprint tracked the bytes: stable across an identical remount, moved on a different one\n");
    }

    // TEST: poll() reflects the served bundle, firing once when a remount changes the bytes.
    {
        nya_http_livereload_reset();

        mount_one(BYTES_A, BYTES_A_SIZE);
        nya_assert(!nya_http_livereload_poll(), "the first poll records the bundle it found");
        nya_assert(!nya_http_livereload_poll(), "a poll over a bundle that did not move is not a reload");

        nya_http_static_unmount();
        mount_one(BYTES_A, BYTES_A_SIZE);
        nya_assert(!nya_http_livereload_poll(), "a remount of the same bytes is not a change");

        nya_http_static_unmount();
        mount_one(BYTES_B, BYTES_B_SIZE);
        nya_assert(nya_http_livereload_poll(), "a remount with different bytes is a reload");
        nya_assert(!nya_http_livereload_poll(), "and only once");

        nya_http_static_unmount();

        printf("  poll() read the served bundle and fired once, on the remount that changed the bytes\n");
    }

    // TEST: the client snippet is the shape a page depends on.
    {
        NYA_ConstCString js = nya_http_livereload_client_js();
        nya_assert(js != nullptr && js[0] != '\0', "the client snippet is served, so it exists");

        NYA_String* snippet = nya_string_from(arena, js);

        nya_assert(nya_string_contains(snippet, "new WebSocket("), "the snippet opens a socket");
        nya_assert(nya_string_contains(snippet, NYA_HTTP_LIVERELOAD_PATH), "on /livereload");
        nya_assert(nya_string_contains(snippet, "location.reload()"), "and reloads the page");
        nya_assert(nya_string_contains(snippet, "\"" NYA_HTTP_LIVERELOAD_MESSAGE "\""), "on the reload message and no other");
        nya_assert(nya_string_contains(snippet, "setTimeout"), "reconnecting on a bounded backoff, not a tight loop");

        // Balanced braces and parens: a snippet with a stray one is a script that does not parse, and a page cannot fix what it is handed.
        s32 braces = 0;
        s32 parens = 0;
        for (u64 i = 0; js[i] != '\0'; i++) {
            if (js[i] == '{') braces++;
            if (js[i] == '}') braces--;
            if (js[i] == '(') parens++;
            if (js[i] == ')') parens--;
            nya_assert(braces >= 0 && parens >= 0, "the snippet closes a bracket it never opened");
        }
        nya_assert(braces == 0 && parens == 0, "the snippet's braces and parens balance");

        printf("  the client snippet opens the socket, reloads on the message, backs off, and parses\n");
    }

    // TEST: the router serves that snippet at /livereload.js as JavaScript.
    {
        const NYA_HttpRouter* router = nya_http_livereload_router();
        nya_assert(router != nullptr && router->route_count == 1, "the router serves one file");
        nya_assert(strcmp(router->routes[0].path, NYA_HTTP_LIVERELOAD_SCRIPT_PATH) == 0, "at /livereload.js");
        nya_assert(router->routes[0].method == NYA_HTTP_METHOD_GET);

        printf("  the router mounts one GET at %s\n", NYA_HTTP_LIVERELOAD_SCRIPT_PATH);
    }

    // TEST: a signalled change pushes "reload" to a page on /livereload, and an unchanged one does not.
    {
        // The route cannot be mounted before there is a server, like every websocket route.
        nya_assert(!nya_http_livereload_route_add().ok, "a stream needs a server");

        u16   port = start_server();
        defer nya_system_http_deinit();

        NYA_EXPECT(nya_http_livereload_route_add());

        nya_http_livereload_reset();

        Client page = { 0 };
        client_open(&page, arena, port, NYA_HTTP_LIVERELOAD_PATH);
        defer client_destroy(&page);

        // The first signal records the baseline and pushes nothing, so the page is not reloaded the moment it connected.
        nya_assert(!nya_http_livereload_signal(0xA1), "the first signal is a baseline");
        client_settle(&page);
        nya_assert(page.texts == 0, "a baseline is not a push");

        // A change pushes exactly one "reload".
        nya_assert(nya_http_livereload_signal(0xB2), "a moved fingerprint is a reload");
        client_wait(&page, &page.texts, 1);
        nya_assert(page.texts == 1, "the reload was pushed");
        nya_assert(page.last_size == strlen(NYA_HTTP_LIVERELOAD_MESSAGE) && nya_memcmp(page.last, NYA_HTTP_LIVERELOAD_MESSAGE, page.last_size) == 0, "and it is the reload message");

        // The same fingerprint again pushes nothing.
        nya_assert(!nya_http_livereload_signal(0xB2), "an unchanged fingerprint pushes nothing");
        client_settle(&page);
        nya_assert(page.texts == 1, "no second push arrived");

        nya_http_livereload_route_remove();

        printf("  a change pushed one 'reload' to the page, and an unchanged fingerprint pushed nothing\n");
    }

    printf("PASSED: http livereload\n");

    return EXIT_SUCCESS;
}

/**
 * The WebSocket server end to end: a real port, a real socket, and the frames a client gets back.
 *
 * Driven by hand like test_server.c, because nya_system_http_tick is public for exactly this: the
 * handshake is written, the server is ticked, the answer is read. The client end of every exchange
 * here is the same nya_websocket_protocol_* the curl plugin runs, which is the point of the shared
 * codec: if the two ends disagreed about a frame, this file would not link a message together.
 *
 * The hostile cases are the ones the fuzzer cannot reach, because they are about the connection and
 * the handshake rather than the bytes of one frame: an upgrade from another site, an upgrade on a path
 * nothing serves, a client that does not mask, and one address taking every socket.
 **/

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "SDL3/SDL_init.h"

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#define FIRST_PORT 47980
#define LAST_PORT  47996

/** How long the two ends are pumped against each other before a step is called lost. */
#define PUMP_STEPS 400

#define ECHO_PATH "/ws/echo"
#define PUSH_PATH "/ws/push"

/*
 * ─────────────────────────────────────────────────────────
 * THE ROUTES UNDER TEST
 * ─────────────────────────────────────────────────────────
 */

static u32 OPENS    = 0;
static u32 MESSAGES = 0;
static u32 CLOSES   = 0;

static NYA_WebSocketClose LAST_CLOSE = NYA_WEBSOCKET_CLOSE_NONE;

static void on_open(NYA_HttpWebSocket* socket) {
    OPENS++;

    NYA_EXPECT(nya_http_websocket_send_text(socket, "welcome"));
}

/** An echo, which is the one thing a test can check a byte at a time in both directions. */
static void on_message(NYA_HttpWebSocket* socket, b8 is_text, const u8* data, u64 size) {
    MESSAGES++;

    if (is_text) {
        NYA_EXPECT(nya_websocket_protocol_send(nya_http_websocket_protocol(socket), NYA_WEBSOCKET_OPCODE_TEXT, data, size));
        return;
    }

    NYA_EXPECT(nya_websocket_protocol_send(nya_http_websocket_protocol(socket), NYA_WEBSOCKET_OPCODE_BINARY, data, size));
}

static void on_close(NYA_HttpWebSocket* socket, NYA_WebSocketClose code) {
    nya_unused(socket);

    CLOSES++;
    LAST_CLOSE = code;
}

static const NYA_HttpWebSocketRoute ECHO_ROUTE = {
    .path       = ECHO_PATH,
    .summary    = "echoes whatever it is sent",
    .on_open    = on_open,
    .on_message = on_message,
    .on_close   = on_close,
};

static const NYA_HttpWebSocketRoute PUSH_ROUTE = {
    .path    = PUSH_PATH,
    .summary = "says nothing until the program broadcasts",
};

/*
 * ─────────────────────────────────────────────────────────
 * THE CLIENT
 * ─────────────────────────────────────────────────────────
 */

/** What a message on this connection may be. Smaller than the server's, which is what it is testing. */
#define CLIENT_MESSAGE_BYTES 8192

typedef struct {
    NYA_OsSocket socket;

    NYA_WebSocketProtocol protocol;
    u8                    send[CLIENT_MESSAGE_BYTES];
    u8                    message[CLIENT_MESSAGE_BYTES + 1];

    u8  receive[CLIENT_MESSAGE_BYTES];
    u64 receive_size;

    u32 texts;
    u32 binaries;
    u32 pongs;
    u32 closes;

    NYA_WebSocketClose code;

    u8  last[CLIENT_MESSAGE_BYTES];
    u64 last_size;
} Client;

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

static NYA_OsSocket connect_to(u16 port) {
  NYA_OsAddress address = { 0 };
  nya_assert(nya_os_address_resolve("127.0.0.1", port, NYA_OS_ADDRESS_V4, &address) == NYA_OS_SOCKET_OK);

  NYA_OsSocket       socket    = NYA_OS_SOCKET_NONE;
  NYA_OsSocketStatus connected = nya_os_socket_connect(address, &socket);

  nya_assert(connected == NYA_OS_SOCKET_OK || connected == NYA_OS_SOCKET_WOULD_BLOCK);

  // a non-blocking connect is under way rather than done, and writability is how the host says it
  // finished; loopback usually beats the first wait to it.
  NYA_OsSocketWait watched = { .socket = socket, .writable = true };
  u32              ready   = 0;

  nya_assert(nya_os_socket_wait(&watched, 1, 1000, &ready) == NYA_OS_SOCKET_OK);
  nya_assert(nya_os_socket_error(socket) == NYA_OS_SOCKET_OK);

  return socket;
}

/** A fresh Sec-WebSocket-Key, made the way a client makes one. */
static NYA_CString key_make(NYA_Arena* arena) {
    u8 nonce[NYA_WEBSOCKET_KEY_BYTES] = { 0 };
    nya_assert(nya_os_random_bytes(nonce, sizeof(nonce)));

    NYA_String* encoded = nya_string_create(arena);
    nya_base64_encode(encoded, nonce, sizeof(nonce));

    return nya_string_to_cstring(arena, encoded);
}

/**
 * Writes an upgrade request and reads the whole answer, whatever it is: the 101 with nothing after it,
 * or a refusal with its problem body.
 * */
static u64 handshake(NYA_OsSocket socket, NYA_Arena* arena, NYA_ConstCString request, OUT char* buffer, u64 capacity) {
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

    nya_unused(arena);

    return filled;
}

/** The request a browser would send, with `extra` for whatever the case under test adds. */
static NYA_ConstCString upgrade_request(NYA_Arena* arena, NYA_ConstCString path, NYA_ConstCString key, NYA_ConstCString extra) {
    return nya_string_to_cstring(
        arena,
        nya_string_sprintf(
            arena,
            "GET %s HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s\r\n",
            path,
            key,
            extra
        )
    );
}

/** Upgrades, checks the accept key against the one the client itself computes, and opens the framing. */
static void client_open(Client* client, NYA_Arena* arena, u16 port, NYA_ConstCString path) {
    nya_memset(client, 0, sizeof(*client));

    client->socket = connect_to(port);

    NYA_CString key                                       = key_make(arena);
    char        expected[NYA_WEBSOCKET_ACCEPT_LENGTH + 1] = { 0 };
    NYA_EXPECT(nya_websocket_accept_from_key(key, expected));

    char answer[2048] = { 0 };
    nya_assert(handshake(client->socket, arena, upgrade_request(arena, path, key, ""), answer, sizeof(answer)) > 0);

    NYA_String* received = nya_string_from(arena, answer);

    nya_assert(nya_string_starts_with(received, "HTTP/1.1 101 Switching Protocols\r\n"), "got:\n%s", answer);
    nya_assert(nya_string_contains(received, "Upgrade: websocket\r\n"));
    nya_assert(nya_string_contains(received, nya_string_to_cstring(arena, nya_string_sprintf(arena, "Sec-WebSocket-Accept: %s\r\n", expected))));
    nya_assert(nya_string_ends_with(received, "\r\n\r\n"), "a 101 carries no body");

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

/** Pushes what the protocol queued, ticks the server, reads what came back, and collects the events. */
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

        // a client that has nothing waiting is the ordinary case here; the test drives both ends.
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

        switch (event.kind) {
            case NYA_WEBSOCKET_EVENT_TEXT:   client->texts++; break;
            case NYA_WEBSOCKET_EVENT_BINARY: client->binaries++; break;
            case NYA_WEBSOCKET_EVENT_PONG:   client->pongs++; break;

            case NYA_WEBSOCKET_EVENT_CLOSED: {
                client->closes++;
                client->code = event.code;
            } break;

            default: break;
        }

        if (event.size > 0 && event.size <= sizeof(client->last)) {
            nya_memcpy(client->last, event.data, event.size);
            client->last_size = event.size;
        }
    }

    sleep_ms(1);
}

/** Steps until `counter` reaches `target`, or gives up so a failure is an assertion and not a hang. */
static void client_wait(Client* client, const u32* counter, u32 target) {
    for (u32 step = 0; step < PUMP_STEPS && *counter < target; step++) client_step(client);
}

/*
 * ─────────────────────────────────────────────────────────
 * THE TESTS
 * ─────────────────────────────────────────────────────────
 */

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_websocket");
    defer      nya_arena_destroy(arena);

    char answer[4096] = { 0 };

    // TEST: a route is mounted on a running server, and only there.
    {
        nya_assert(!nya_http_websocket_route_add(&ECHO_ROUTE).ok, "a stream cannot be mounted before there is a server");
        nya_assert(nya_http_websocket_count() == 0);

        u16   port = start_server((NYA_HttpConfig){ 0 });
        defer nya_system_http_deinit();

        nya_unused(port);

        nya_assert(nya_http_websocket_route_add(&ECHO_ROUTE).ok);
        nya_assert(nya_http_websocket_route_add(&ECHO_ROUTE).kind == NYA_ERROR_ALREADY_EXISTS, "a path is mounted once");

        // Not RELATIVE: windows.h (wingdi.h) defines that as a macro, and this test compiles there too.
        static const NYA_HttpWebSocketRoute RELATIVE_ROUTE = { .path = "ws/relative", .summary = "no" };
        static const NYA_HttpWebSocketRoute SILENT         = { .path = "/ws/silent" };

        nya_assert(nya_http_websocket_route_add(&RELATIVE_ROUTE).kind == NYA_ERROR_INVALID_ARGUMENT);
        nya_assert(nya_http_websocket_route_add(&SILENT).kind == NYA_ERROR_INVALID_ARGUMENT, "a route says what it is, for the log line");

        nya_http_websocket_route_remove(&ECHO_ROUTE);
        nya_http_websocket_route_remove(&ECHO_ROUTE);

        printf("  a stream needs a server, an absolute path and a summary\n");
    }

    // TEST: the handshake, a message each way, fragments, a ping, and a close.
    {
        u16   port = start_server((NYA_HttpConfig){ 0 });
        defer nya_system_http_deinit();

        NYA_EXPECT(nya_http_websocket_route_add(&ECHO_ROUTE));

        OPENS    = 0;
        MESSAGES = 0;
        CLOSES   = 0;

        Client client = { 0 };
        client_open(&client, arena, port, ECHO_PATH);
        defer client_destroy(&client);

        // the route's on_open said hello, which is a server frame and so is never masked.
        client_wait(&client, &client.texts, 1);

        nya_assert(OPENS == 1, "the route was never told a peer arrived");
        nya_assert(client.texts == 1 && client.last_size == 7 && nya_memcmp(client.last, "welcome", 7) == 0);
        nya_assert(nya_http_websocket_count() == 1);

        NYA_HttpWebSocket* connected = nya_http_websocket_at(0);
        nya_assert(connected != nullptr);
        nya_assert(strcmp(nya_http_websocket_path(connected), ECHO_PATH) == 0);
        nya_assert(strncmp(nya_http_websocket_address(connected), "127.0.0.1", 9) == 0, "the peer is the socket's, in full");

        // a message each way, through the same codec on both ends.
        NYA_EXPECT(nya_websocket_protocol_send(&client.protocol, NYA_WEBSOCKET_OPCODE_TEXT, (const u8*)"hello server", 12));
        client_wait(&client, &client.texts, 2);

        nya_assert(client.texts == 2 && client.last_size == 12 && nya_memcmp(client.last, "hello server", 12) == 0);
        nya_assert(MESSAGES == 1);

        u8 bytes[] = { 0x00, 0xFF, 0x7F, 0x80, 0x01 };
        NYA_EXPECT(nya_websocket_protocol_send(&client.protocol, NYA_WEBSOCKET_OPCODE_BINARY, bytes, sizeof(bytes)));
        client_wait(&client, &client.binaries, 1);

        nya_assert(client.binaries == 1 && client.last_size == sizeof(bytes) && nya_memcmp(client.last, bytes, sizeof(bytes)) == 0);

        /*
         * A message in three masked fragments, built by hand: nya_websocket_protocol_send writes one
         * final frame, and what is being checked here is the server joining what a browser splits.
         */
        {
            static const struct {
                NYA_WebSocketOpcode opcode;
                b8                  fin;
                NYA_ConstCString    text;
            } PIECES[] = {
                { NYA_WEBSOCKET_OPCODE_TEXT,         false, "frag" },
                { NYA_WEBSOCKET_OPCODE_CONTINUATION, false, "men"  },
                { NYA_WEBSOCKET_OPCODE_CONTINUATION, true,  "ted"  },
            };

            for (u32 piece = 0; piece < nya_carray_length(PIECES); piece++) {
                u64 size                                   = strlen(PIECES[piece].text);
                u8  mask[4]                                = { 0x11, 0x22, 0x33, 0x44 };
                u8  header[NYA_WEBSOCKET_MAX_HEADER_BYTES] = { 0 };
                u64 header_size                            = 0;

                NYA_EXPECT(nya_websocket_frame_encode(PIECES[piece].opcode, PIECES[piece].fin, size, mask, header, &header_size));

                u8 frame[32] = { 0 };
                nya_memcpy(frame, header, header_size);
                for (u64 i = 0; i < size; i++) frame[header_size + i] = (u8)((u8)PIECES[piece].text[i] ^ mask[i & 3U]);

                {
          u64 wrote = 0;
          nya_assert(nya_os_socket_send(client.socket, (const u8*)frame, header_size + size, &wrote) == NYA_OS_SOCKET_OK);
        }
            }
        }

        client_wait(&client, &client.texts, 3);

        nya_assert(client.texts == 3, "the fragmented message never came back");
        nya_assert(client.last_size == 10 && nya_memcmp(client.last, "fragmented", 10) == 0);

        // a ping is answered by the protocol itself, with the same payload and nothing else.
        NYA_EXPECT(nya_websocket_protocol_send(&client.protocol, NYA_WEBSOCKET_OPCODE_PING, (const u8*)"beat", 4));
        client_wait(&client, &client.pongs, 1);

        nya_assert(client.pongs == 1, "the ping was never answered");
        nya_assert(client.last_size == 4 && nya_memcmp(client.last, "beat", 4) == 0);

        // and the closing handshake: the server echoes the code and lets the connection go.
        NYA_EXPECT(nya_websocket_protocol_close(&client.protocol, NYA_WEBSOCKET_CLOSE_NORMAL, "done"));
        client_wait(&client, &client.closes, 1);

        nya_assert(client.closes == 1, "the close was never answered");
        nya_assert(client.code == NYA_WEBSOCKET_CLOSE_NORMAL, "closed with %s", nya_websocket_close_name(client.code));

        for (u32 step = 0; step < PUMP_STEPS && nya_http_websocket_count() > 0; step++) {
            nya_system_http_tick();
            sleep_ms(2);
        }

        nya_assert(nya_http_websocket_count() == 0, "the server still holds a socket that said goodbye");
        nya_assert(CLOSES == 1 && LAST_CLOSE == NYA_WEBSOCKET_CLOSE_NORMAL, "the route was told why");
        nya_assert(nya_http_server_connection_count() == 0, "the HTTP connection went with it");

        printf("  upgraded, echoed text and binary, joined three fragments, answered a ping and closed\n");
    }

    // TEST: a push to everyone on a path, which is what a stream is for.
    {
        u16   port = start_server((NYA_HttpConfig){ 0 });
        defer nya_system_http_deinit();

        NYA_EXPECT(nya_http_websocket_route_add(&PUSH_ROUTE));
        NYA_EXPECT(nya_http_websocket_route_add(&ECHO_ROUTE));

        Client listener = { 0 };
        client_open(&listener, arena, port, PUSH_PATH);
        defer client_destroy(&listener);

        nya_assert(nya_http_websocket_broadcast_text(PUSH_PATH, "{\"tick\":1}") == 1);
        nya_assert(nya_http_websocket_broadcast_text(ECHO_PATH, "{\"tick\":1}") == 0, "a push goes to one path's peers and nobody else's");
        nya_assert(nya_http_websocket_broadcast_text("/ws/nothing", "{}") == 0);

        client_wait(&listener, &listener.texts, 1);

        nya_assert(listener.texts == 1 && listener.last_size == 10 && nya_memcmp(listener.last, "{\"tick\":1}", 10) == 0);

        // and unmounting the stream tells whoever is on it, rather than leaving them on a dead route.
        nya_http_websocket_route_remove(&PUSH_ROUTE);

        client_wait(&listener, &listener.closes, 1);

        nya_assert(listener.closes == 1 && listener.code == NYA_WEBSOCKET_CLOSE_GOING_AWAY);

        printf("  a broadcast reached one path's peers, and unmounting it said goodbye\n");
    }

    // TEST: a client that does not mask is refused, which RFC 6455 requires.
    {
        u16   port = start_server((NYA_HttpConfig){ 0 });
        defer nya_system_http_deinit();

        NYA_EXPECT(nya_http_websocket_route_add(&ECHO_ROUTE));

        Client client = { 0 };
        client_open(&client, arena, port, ECHO_PATH);
        defer client_destroy(&client);

        // a text frame with the mask bit clear, which only a server may send.
        u8 unmasked[] = { 0x81, 0x02, 'h', 'i' };
        {
          u64 wrote = 0;
          nya_assert(nya_os_socket_send(client.socket, (const u8*)unmasked, sizeof(unmasked), &wrote) == NYA_OS_SOCKET_OK);
        }

        client_wait(&client, &client.closes, 1);

        nya_assert(client.closes == 1, "an unmasked client frame was tolerated");
        nya_assert(client.code == NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "closed with %s", nya_websocket_close_name(client.code));

        printf("  an unmasked client frame closed the connection with 1002\n");
    }

    // TEST: a frame larger than the bound is refused on its header alone.
    {
        u16   port = start_server((NYA_HttpConfig){ 0 });
        defer nya_system_http_deinit();

        NYA_EXPECT(nya_http_websocket_route_add(&ECHO_ROUTE));

        Client client = { 0 };
        client_open(&client, arena, port, ECHO_PATH);
        defer client_destroy(&client);

        // a megabyte announced and not one byte of it sent.
        u8 header[] = { 0x82, 0xFF, 0, 0, 0, 0, 0, 0x10, 0, 0, 0x01, 0x02, 0x03, 0x04 };
        {
          u64 wrote = 0;
          nya_assert(nya_os_socket_send(client.socket, (const u8*)header, sizeof(header), &wrote) == NYA_OS_SOCKET_OK);
        }

        client_wait(&client, &client.closes, 1);

        nya_assert(client.closes == 1, "a frame past the bound was not refused");
        nya_assert(client.code == NYA_WEBSOCKET_CLOSE_TOO_LARGE, "closed with %s", nya_websocket_close_name(client.code));

        printf("  a frame announcing a megabyte closed the connection with 1009\n");
    }

    // TEST: the handshakes this server refuses.
    {
        u16   port = start_server((NYA_HttpConfig){ 0 });
        defer nya_system_http_deinit();

        NYA_EXPECT(nya_http_websocket_route_add(&ECHO_ROUTE));

        NYA_CString key = key_make(arena);

        struct {
            NYA_ConstCString what;
            NYA_ConstCString request;
            NYA_ConstCString status;
        } cases[] = {
            {
             "a page on another site", upgrade_request(arena, ECHO_PATH, key, "Origin: http://evil.example\r\n"),
             "HTTP/1.1 403 Forbidden\r\n", },
            {
             "a fetch from another site", upgrade_request(arena, ECHO_PATH, key, "Sec-Fetch-Site: cross-site\r\n"),
             "HTTP/1.1 403 Forbidden\r\n", },
            {
             "a path with no stream on it", upgrade_request(arena, "/ws/nothing", key, ""),
             "HTTP/1.1 404 Not Found\r\n", },
            {
             "a version this server does not speak", nya_string_to_cstring(
                    arena, nya_string_sprintf(
                        arena, "GET " ECHO_PATH " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 8\r\n\r\n", key
                    )
                ), "HTTP/1.1 400 Bad Request\r\n",
             },
            {
             "a key that is not a key", upgrade_request(arena, ECHO_PATH, "not-a-key", ""),
             "HTTP/1.1 400 Bad Request\r\n", },
            {
             "no key at all", nya_string_to_cstring(
                    arena, nya_string_sprintf(
                        arena, "GET " ECHO_PATH " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n"
                    )
                ), "HTTP/1.1 400 Bad Request\r\n",
             },
            {
             "a verb that is not GET", nya_string_to_cstring(
                    arena, nya_string_sprintf(
                        arena, "POST " ECHO_PATH " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nContent-Length: 0\r\n\r\n", key
                    )
                ), "HTTP/1.1 400 Bad Request\r\n",
             },
            {
             "frames sent before the answer", nya_string_to_cstring(
                    arena, nya_string_sprintf(
                        arena, "GET " ECHO_PATH " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n\x81\x80\x01\x02\x03\x04", key
                    )
                ), "HTTP/1.1 400 Bad Request\r\n",
             },
        };

        for (u32 which = 0; which < nya_carray_length(cases); which++) {
            NYA_OsSocket client = connect_to(port);
            defer             nya_os_socket_close(client);

            nya_assert(handshake(client, arena, cases[which].request, answer, sizeof(answer)) > 0, "'%s' was not answered", cases[which].what);

            NYA_String* received = nya_string_from(arena, answer);

            nya_assert(nya_string_starts_with(received, cases[which].status), "'%s' got:\n%s", cases[which].what, answer);
            nya_assert(nya_string_contains(received, "Connection: close\r\n"), "'%s' left the connection open", cases[which].what);
            nya_assert(nya_http_websocket_count() == 0, "'%s' still took a socket", cases[which].what);
        }

        printf("  eight handshakes refused: another site twice, no route, the version, the key twice, the verb, and early frames\n");
    }

    // TEST: an upgrade spends a token like every other request.
    {
        u16   port = start_server((NYA_HttpConfig){ .requests_per_second = 1, .request_burst = 1 });
        defer nya_system_http_deinit();

        NYA_EXPECT(nya_http_websocket_route_add(&ECHO_ROUTE));

        Client first = { 0 };
        client_open(&first, arena, port, ECHO_PATH);
        defer client_destroy(&first);

        NYA_OsSocket second = connect_to(port);
        defer             nya_os_socket_close(second);

        nya_assert(handshake(second, arena, upgrade_request(arena, ECHO_PATH, key_make(arena), ""), answer, sizeof(answer)) > 0);

        NYA_String* refused = nya_string_from(arena, answer);

        nya_assert(nya_string_starts_with(refused, "HTTP/1.1 429 Too Many Requests\r\n"), "got:\n%s", answer);
        nya_assert(nya_string_contains(refused, "Retry-After: "));
        nya_assert(nya_http_websocket_count() == 1, "the address's budget is spent before the socket is taken");

        printf("  an upgrade past the address's budget is a 429 and not a socket\n");
    }

    // TEST: one address may not take every socket.
    {
        u16   port = start_server((NYA_HttpConfig){ 0 });
        defer nya_system_http_deinit();

        NYA_EXPECT(nya_http_websocket_route_add(&ECHO_ROUTE));

        Client held[NYA_HTTP_MAX_WEBSOCKETS_PER_ADDRESS] = { 0 };

        for (u32 index = 0; index < NYA_HTTP_MAX_WEBSOCKETS_PER_ADDRESS; index++) client_open(&held[index], arena, port, ECHO_PATH);

        defer {
            for (u32 index = 0; index < NYA_HTTP_MAX_WEBSOCKETS_PER_ADDRESS; index++) client_destroy(&held[index]);
        }

        nya_assert(nya_http_websocket_count() == NYA_HTTP_MAX_WEBSOCKETS_PER_ADDRESS);

        NYA_OsSocket extra = connect_to(port);
        defer             nya_os_socket_close(extra);

        nya_assert(handshake(extra, arena, upgrade_request(arena, ECHO_PATH, key_make(arena), ""), answer, sizeof(answer)) > 0);

        nya_assert(nya_string_starts_with(nya_string_from(arena, answer), "HTTP/1.1 503 Service Unavailable\r\n"), "got:\n%s", answer);
        nya_assert(nya_http_websocket_count() == NYA_HTTP_MAX_WEBSOCKETS_PER_ADDRESS, "the bound is the bound");

        printf("  the socket past one address's share is refused while the table has room\n");
    }

    printf("PASSED: http websocket server\n");

    return EXIT_SUCCESS;
}

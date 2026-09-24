/**
 * The WebSocket net transport end to end: a real HTTP server on a real port, a real socket for the
 * browser, and the transport's own events on the other side of the same wire.
 *
 * There is no browser in the sandbox, so the client is hand-rolled the way test_http_websocket.c's is:
 * the same nya_websocket_protocol_* codec a browser's WebSocket runs, over a loopback socket. What it
 * proves is the transport's contract, not the framing (the framing has its own test): a peer that
 * presents an allowlisted key and the matching version connects and round-trips a message; a peer with
 * a key that is not on the list is refused at the join with a reason it can read; and a peer whose
 * version does not match is disconnected at once with the reason, rather than dropped in silence.
 **/

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#define FIRST_PORT 47960
#define LAST_PORT  47979

/** How long the two ends are pumped against each other before a step is called lost. */
#define PUMP_STEPS 400

#define NET_PATH "/ws/net"

/** The version this server speaks. A client presenting anything else is refused at the join. */
#define SERVER_VERSION 7U

/*
 * ─────────────────────────────────────────────────────────
 * THE TRANSPORT UNDER TEST, AND WHAT ITS POLL HAS SEEN
 * ─────────────────────────────────────────────────────────
 */

static NYA_NetTransport* TRANSPORT = nullptr;

static u32 CONNECTS    = 0;
static u32 MESSAGES    = 0;
static u32 DISCONNECTS = 0;

static NYA_NetPeerId      LAST_PEER      = { 0 };
static NYA_NetDisconnect  LAST_REASON    = NYA_NET_DISCONNECT_NONE;
static u8                 LAST_MESSAGE[4096];
static u64                LAST_MESSAGE_SIZE = 0;

/** Drains every transport event the last tick produced into the counters above. */
static void transport_drain(void) {
    NYA_NetTransportEvent event = { 0 };

    while (nya_net_transport_poll(TRANSPORT, &event)) {
        switch (event.kind) {
            case NYA_NET_TRANSPORT_EVENT_CONNECTED: {
                CONNECTS++;
                LAST_PEER = event.peer;
            } break;

            case NYA_NET_TRANSPORT_EVENT_MESSAGE: {
                MESSAGES++;
                LAST_PEER = event.peer;
                if (event.size <= sizeof(LAST_MESSAGE)) {
                    nya_memcpy(LAST_MESSAGE, event.data, event.size);
                    LAST_MESSAGE_SIZE = event.size;
                }
            } break;

            case NYA_NET_TRANSPORT_EVENT_DISCONNECTED: {
                DISCONNECTS++;
                LAST_PEER   = event.peer;
                LAST_REASON = event.reason;
            } break;

            default: break;
        }
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * THE CLIENT — a browser stood in for by a socket and the codec
 * ─────────────────────────────────────────────────────────
 */

#define CLIENT_MESSAGE_BYTES 8192

typedef struct {
    NYA_OsSocket socket;

    NYA_WebSocketProtocol protocol;
    u8                    send[CLIENT_MESSAGE_BYTES];
    u8                    message[CLIENT_MESSAGE_BYTES + 1];

    u8  receive[CLIENT_MESSAGE_BYTES];
    u64 receive_size;

    b8 accepted;

    u32 datas;
    u32 closes;

    NYA_WebSocketClose code;
    char               reason[128];

    u8  last[CLIENT_MESSAGE_BYTES];
    u64 last_size;
} Client;

static void sleep_ms(u32 milliseconds) {
    struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
    (void)nanosleep(&request, nullptr);
}

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

/** Upgrades on `path`, checks the accept key, and opens the client-side framing. */
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

/** Flushes the client's queue, ticks the server, reads the answer, and drains both ends' events. */
static void step(Client* client) {
    u64       pending = 0;
    const u8* queued  = nya_websocket_protocol_pending(&client->protocol, &pending);

    if (pending > 0) {
        u64 wrote = 0;
        nya_assert(nya_os_socket_send(client->socket, queued, pending, &wrote) == NYA_OS_SOCKET_OK);
        nya_websocket_protocol_flushed(&client->protocol, pending);
    }

    nya_system_http_tick();
    transport_drain();

    u64 room = sizeof(client->receive) - client->receive_size;

    if (room > 0) {
        u64 read = 0;
        (void)nya_os_socket_receive(client->socket, client->receive + client->receive_size, room, &read);
        if (read > 0) client->receive_size += read;
    }

    for (u32 iteration = 0; iteration < 16; iteration++) {
        NYA_WebSocketEvent event    = { 0 };
        u64                consumed = 0;

        b8 produced = nya_websocket_protocol_receive(&client->protocol, client->receive, client->receive_size, &consumed, &event);

        client->receive_size -= consumed;
        if (client->receive_size > 0 && consumed > 0) nya_memmove(client->receive, client->receive + consumed, client->receive_size);

        if (!produced) break;

        switch (event.kind) {
            case NYA_WEBSOCKET_EVENT_BINARY: {
                // A binary message is either the server's acceptance or a carried net message; the tag
                // in the first byte says which, the way the transport's own frames do.
                if (event.size >= 1 && event.data[0] == (u8)NYA_NET_WS_TAG_ACCEPT) {
                    client->accepted = true;
                } else if (event.size >= 1 && event.data[0] == (u8)NYA_NET_WS_TAG_DATA) {
                    client->datas++;
                    client->last_size = event.size - 1;
                    if (client->last_size <= sizeof(client->last)) nya_memcpy(client->last, event.data + 1, client->last_size);
                }
            } break;

            case NYA_WEBSOCKET_EVENT_CLOSED: {
                client->closes++;
                client->code = event.code;
                (void)snprintf(client->reason, sizeof(client->reason), "%s", event.reason);
            } break;

            default: break;
        }
    }

    sleep_ms(1);
}

/** Sends the transport join frame a browser sends first: the version and the player key it presents. */
static void client_join(Client* client, u32 version, const u8 key[NYA_NET_KEY_SIZE]) {
    u8 frame[NYA_NET_WS_JOIN_SIZE] = { 0 };
    nya_net_ws_join_encode(version, key, frame);

    NYA_EXPECT(nya_websocket_protocol_send(&client->protocol, NYA_WEBSOCKET_OPCODE_BINARY, frame, sizeof(frame)));
}

/** A carried net message, as the client frames one: the data tag and then the payload. */
static void client_send_data(Client* client, const u8* payload, u64 size) {
    u8 frame[CLIENT_MESSAGE_BYTES];
    frame[0] = (u8)NYA_NET_WS_TAG_DATA;
    nya_memcpy(frame + 1, payload, size);

    NYA_EXPECT(nya_websocket_protocol_send(&client->protocol, NYA_WEBSOCKET_OPCODE_BINARY, frame, size + 1));
}

/** Steps until `counter` reaches `target`, or gives up so a failure is an assertion and not a hang. */
static void wait_for(Client* client, const u32* counter, u32 target) {
    for (u32 iteration = 0; iteration < PUMP_STEPS && *counter < target; iteration++) step(client);
}

/** Closes the client and pumps the server until it holds no socket, so the next case starts clean. */
static void drain_close(Client* client) {
    client_destroy(client);

    for (u32 iteration = 0; iteration < PUMP_STEPS && nya_http_websocket_count() > 0; iteration++) {
        nya_system_http_tick();
        transport_drain();
        sleep_ms(1);
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * THE TESTS
 * ─────────────────────────────────────────────────────────
 */

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_net_ws_transport");
    defer      nya_arena_destroy(arena);

    // Two real player keys: one the server will allow, one it never heard of.
    NYA_NetKeyPair allowed = { 0 };
    NYA_NetKeyPair stranger = { 0 };
    NYA_EXPECT(nya_net_key_pair_create(&allowed));
    NYA_EXPECT(nya_net_key_pair_create(&stranger));

    u16   port = start_server((NYA_HttpConfig){ 0 });
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_net_transport_ws_create(arena, (NYA_NetWsOptions){ .path = NET_PATH, .version = SERVER_VERSION }, &TRANSPORT));
    defer nya_net_transport_destroy(TRANSPORT);

    // A second one is refused: the callbacks and the HTTP server it mounts on are one per process.
    {
        NYA_NetTransport* second = nullptr;
        nya_assert(nya_net_transport_ws_create(arena, (NYA_NetWsOptions){ 0 }, &second).kind == NYA_ERROR_ALREADY_EXISTS);
    }

    // Listening needs the server, and it reports the server's port.
    NYA_EXPECT(nya_net_transport_listen(TRANSPORT, port));
    nya_assert(nya_net_transport_port(TRANSPORT) == port, "the transport reports the HTTP server's port");

    // The allowlist: the one key is allowed, the stranger is not, and it is idempotent.
    NYA_EXPECT(nya_net_transport_ws_allow(TRANSPORT, allowed.public_key));
    NYA_EXPECT(nya_net_transport_ws_allow(TRANSPORT, allowed.public_key));
    nya_assert(nya_net_transport_ws_is_allowed(TRANSPORT, allowed.public_key));
    nya_assert(!nya_net_transport_ws_is_allowed(TRANSPORT, stranger.public_key));

    // A key can be taken off the list again: the stranger is allowed, then disallowed, and is out.
    NYA_EXPECT(nya_net_transport_ws_allow(TRANSPORT, stranger.public_key));
    nya_assert(nya_net_transport_ws_is_allowed(TRANSPORT, stranger.public_key));
    nya_net_transport_ws_disallow(TRANSPORT, stranger.public_key);
    nya_assert(!nya_net_transport_ws_is_allowed(TRANSPORT, stranger.public_key), "a disallowed key is out");

    u8 zero[NYA_NET_KEY_SIZE] = { 0 };
    nya_assert(nya_net_transport_ws_allow(TRANSPORT, zero).kind == NYA_ERROR_INVALID_ARGUMENT, "an all-zero key is not a key");

    // TEST: an allowlisted key and the matching version connects and round-trips a message.
    {
        CONNECTS = MESSAGES = DISCONNECTS = 0;

        Client client = { 0 };
        client_open(&client, arena, port, NET_PATH);

        client_join(&client, SERVER_VERSION, allowed.public_key);
        wait_for(&client, &CONNECTS, 1);

        nya_assert(CONNECTS == 1, "the join was never accepted");
        nya_assert(client.accepted, "the client was never told it is in");
        nya_assert(nya_net_peer_is_set(LAST_PEER));

        NYA_NetPeerId peer = LAST_PEER;

        // The key the peer proved it holds is the one it presented.
        const u8* peer_key = nya_net_transport_peer_key(TRANSPORT, peer);
        nya_assert(peer_key != nullptr && nya_memcmp(peer_key, allowed.public_key, NYA_NET_KEY_SIZE) == 0, "the peer's key is the one it joined with");

        // Client → server.
        const u8 up[] = { 0x10, 'p', 'i', 'n', 'g' };
        client_send_data(&client, up, sizeof(up));
        wait_for(&client, &MESSAGES, 1);

        nya_assert(MESSAGES == 1 && LAST_MESSAGE_SIZE == sizeof(up) && nya_memcmp(LAST_MESSAGE, up, sizeof(up)) == 0, "the message did not arrive intact");

        // Server → client, over the same wire.
        const u8 down[] = { 0x11, 'p', 'o', 'n', 'g', '!' };
        NYA_EXPECT(nya_net_transport_send(TRANSPORT, peer, NYA_NET_CHANNEL_RELIABLE, down, sizeof(down)));
        wait_for(&client, &client.datas, 1);

        nya_assert(client.datas == 1 && client.last_size == sizeof(down) && nya_memcmp(client.last, down, sizeof(down)) == 0, "the reply did not arrive intact");

        // And the peer going away is reported once, with a reason.
        client_destroy(&client);
        wait_for(&client, &DISCONNECTS, 1);

        nya_assert(DISCONNECTS == 1, "the peer's departure was never reported");
        nya_assert(nya_net_peer_equals(LAST_PEER, peer));

        for (u32 iteration = 0; iteration < PUMP_STEPS && nya_http_websocket_count() > 0; iteration++) { nya_system_http_tick(); transport_drain(); sleep_ms(1); }

        printf("  an allowlisted key and the matching version connected, round-tripped, and disconnected\n");
    }

    // TEST: a key that is not on the allowlist is refused at the join, with a reason.
    {
        CONNECTS = MESSAGES = DISCONNECTS = 0;

        Client client = { 0 };
        client_open(&client, arena, port, NET_PATH);

        client_join(&client, SERVER_VERSION, stranger.public_key);
        wait_for(&client, &client.closes, 1);

        nya_assert(client.closes == 1, "the refused client was not told");
        nya_assert(client.code == NYA_WEBSOCKET_CLOSE_POLICY, "closed with %s", nya_websocket_close_name(client.code));
        nya_assert(strcmp(client.reason, "player key not on allowlist") == 0, "the reason was '%s'", client.reason);
        nya_assert(!client.accepted, "a refused client must not be accepted");
        nya_assert(CONNECTS == 0, "a refused join must not connect a peer");
        nya_assert(DISCONNECTS == 0, "a peer that never connected raises no disconnect");

        drain_close(&client);

        printf("  a key off the allowlist was refused with 1008 and the reason, and never connected\n");
    }

    // TEST: a version that does not match disconnects at once, with the reason.
    {
        CONNECTS = MESSAGES = DISCONNECTS = 0;

        Client client = { 0 };
        client_open(&client, arena, port, NET_PATH);

        // The allowlisted key, but the wrong version: it is the version that must stop this join.
        client_join(&client, SERVER_VERSION + 1, allowed.public_key);
        wait_for(&client, &client.closes, 1);

        nya_assert(client.closes == 1, "the version-mismatched client was not told");
        nya_assert(client.code == NYA_WEBSOCKET_CLOSE_POLICY, "closed with %s", nya_websocket_close_name(client.code));
        nya_assert(strcmp(client.reason, "protocol version mismatch") == 0, "the reason was '%s'", client.reason);
        nya_assert(!client.accepted && CONNECTS == 0, "a version mismatch must not connect a peer");

        drain_close(&client);

        printf("  a version mismatch closed with 1008 and the reason, before any state was exchanged\n");
    }

    // TEST: a first frame that is not a well-formed join is refused as a protocol error.
    {
        CONNECTS = MESSAGES = DISCONNECTS = 0;

        Client client = { 0 };
        client_open(&client, arena, port, NET_PATH);

        // A data frame where a join is owed: the peer is not speaking this protocol.
        const u8 payload[] = { 'n', 'o', 'p', 'e' };
        client_send_data(&client, payload, sizeof(payload));
        wait_for(&client, &client.closes, 1);

        nya_assert(client.closes == 1);
        nya_assert(client.code == NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "closed with %s", nya_websocket_close_name(client.code));
        nya_assert(CONNECTS == 0 && DISCONNECTS == 0);

        drain_close(&client);

        printf("  a first frame that was not a join closed with 1002 and never connected\n");
    }

    printf("PASSED: websocket net transport\n");

    return EXIT_SUCCESS;
}

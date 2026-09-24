/**
 * @file examples/net_echo/main.c
 *
 * A server and a client over the engine's own transport: encrypted UDP datagrams, reliable and
 * unreliable channels, events drained by polling. No window, no world, no entities.
 *
 * ```
 * ./build run example net_echo          # both ends in one process, on a port the system picks
 * ./net_echo.example --listen 47900     # just the server, on a port a client can be told in advance
 * ./net_echo.example --connect 127.0.0.1 47900   # just the client, from another terminal
 * ```
 *
 * ## This is not the web server
 *
 * The web is in scope and being worked on; at the time this was written none of it had landed.
 * `src/nyangine/net` is a game transport and nothing else: `nya_net_transport_udp_create` opens a
 * UDP socket, `nya_net_server_start` makes a world the authority over it. Nothing in the tree binds
 * a TCP listener, parses a request line or routes a path. The curl plugin
 * (`src/nyangine/plugins/curl/request.h`) is the client half of HTTP and has no server in it, and
 * nothing calls it either.
 *
 * What is wanted, per TODO.md: an HTTP server with routing, middleware, typed request and response
 * structs and JSON through `serde`; OpenAPI generated from the handler definitions rather than
 * written; a wasm and WebGPU target; a UI backend emitting HTML, CSS and JS from the same
 * `nya_ui_*` calls the native backend draws; client side apps sharing their types with the server;
 * and an outgoing WebSocket client. When those exist this example gets a sibling, and that sibling
 * is the one to copy for a web app.
 *
 * Nothing below is a sketch of any of it. It is the UDP transport, which is a different thing that
 * happens to also have a server and a client in it.
 *
 * ## What the layer above adds
 *
 * This is the transport on its own, which is the right level for a message exchange. A game
 * replicating a world uses `nya_net_server_start` and `nya_net_client_connect` instead — see
 * `examples/pong_multiplayer`, which is that layer.
 * */
// nyangine.h first, always: base_basic.h defines _POSIX_C_SOURCE and _XOPEN_SOURCE before it pulls in libc, and a system header included ahead of it has already fixed them at another value.
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"
#include "SDL3/SDL_timer.h"

#include "nyangine/nyangine.c"

/* CONSTANTS */

/** Round trips the client asks for before it disconnects. */
#define ROUNDS 5

/** How long either end waits for the other before it stops. The handshake is two round trips. */
#define TIMEOUT_MS 5000

/** Between polls. The transport's retransmit and keepalive timers are in milliseconds. */
#define POLL_INTERVAL_MS 2

/** Longest message this example sends or prints, terminator included. */
#define MESSAGE_MAX 64

/* PEERS */

/** What one end has seen so far, so the loops below read as a state machine rather than as flags. */
typedef struct {
    NYA_ConstCString label;

    b8            connected;
    NYA_NetPeerId peer;

    u32 received;
    b8  finished;
} Endpoint;

/** Copies a message out of the transport's buffer, which the next poll invalidates. */
NYA_INTERNAL void message_copy(const NYA_NetTransportEvent* event, OUT char* out, u64 capacity) {
    nya_assert(event != nullptr);
    nya_assert(out != nullptr);
    nya_assert(capacity > 1);

    // A peer's message is untrusted input: its length is whatever the wire said, so it is clamped rather than trusted, and terminated here because nothing on the wire has to be.
    u64 length = nya_min(event->size, capacity - 1);

    nya_memcpy(out, event->data, length);
    out[length] = '\0';
}

/**
 * Drains one transport, answers what it hears, and records what it saw.
 *
 * The server echoes; the client counts and stops at ROUNDS. One function for both, because the two
 * differ only in what they say back.
 * */
NYA_INTERNAL void endpoint_pump(NYA_NetTransport* transport, Endpoint* endpoint, b8 is_server) {
    nya_assert(transport != nullptr);
    nya_assert(endpoint != nullptr);

    NYA_NetTransportEvent event = { 0 };

    while (nya_net_transport_poll(transport, &event)) {
        switch (event.kind) {
            case NYA_NET_TRANSPORT_EVENT_CONNECTED: {
                endpoint->connected = true;
                endpoint->peer      = event.peer;

                nya_log_info("%s: %s connected.", endpoint->label, nya_net_transport_peer_address(transport, event.peer));

                // The client speaks first; the server has nothing to say until it is spoken to.
                if (!is_server) {
                    char opening[MESSAGE_MAX];
                    (void)snprintf(opening, sizeof(opening), "ping 1");

                    NYA_Error sent = nya_net_transport_send(transport, event.peer, NYA_NET_CHANNEL_RELIABLE, (const u8*)opening, strlen(opening));
                    if (!sent.ok) nya_log_warn("%s: could not send: %s", endpoint->label, (NYA_ConstCString)sent.message);
                }
            } break;

            case NYA_NET_TRANSPORT_EVENT_MESSAGE: {
                char message[MESSAGE_MAX];
                message_copy(&event, message, sizeof(message));

                endpoint->received++;
                nya_log_info("%s: <- %s", endpoint->label, message);

                char reply[MESSAGE_MAX];

                if (is_server) {
                    (void)snprintf(reply, sizeof(reply), "pong %u", endpoint->received);
                } else {
                    // ROUNDS replies is the whole conversation. Leaving is main's job: dropping the peer here would release the slot the stats below are read from, and the report would be all zeroes.
                    if (endpoint->received >= ROUNDS) {
                        endpoint->finished = true;
                        break;
                    }

                    (void)snprintf(reply, sizeof(reply), "ping %u", endpoint->received + 1);
                }

                NYA_Error sent = nya_net_transport_send(transport, event.peer, NYA_NET_CHANNEL_RELIABLE, (const u8*)reply, strlen(reply));
                if (!sent.ok) nya_log_warn("%s: could not reply: %s", endpoint->label, (NYA_ConstCString)sent.message);
            } break;

            case NYA_NET_TRANSPORT_EVENT_DISCONNECTED: {
                nya_log_info("%s: peer left (reason %d).", endpoint->label, (s32)event.reason);

                endpoint->connected = false;
                endpoint->finished  = true;
            } break;

            default: break;
        }
    }
}

/** One line of what the link cost, from the transport's own counters. */
NYA_INTERNAL void endpoint_report(NYA_NetTransport* transport, const Endpoint* endpoint) {
    nya_assert(transport != nullptr);
    nya_assert(endpoint != nullptr);

    // A peer that has already left takes its counters with it: the slot is recycled, and the stats for a stale id come back zeroed rather than remembered. Say so instead of printing zeroes.
    if (!endpoint->connected) {
        nya_log_info("%s: %u messages; the peer left, so its counters are gone.", endpoint->label, endpoint->received);
        return;
    }

    NYA_NetPeerStats stats = nya_net_transport_stats(transport, endpoint->peer);

    nya_log_info("%s: %u messages, %llu bytes sent, %llu received, rtt %.1f ms.", endpoint->label, endpoint->received,
                 (unsigned long long)stats.bytes_sent, (unsigned long long)stats.bytes_received, (f64)stats.rtt_ms);
}

/* MAIN */

s32 main(s32 argc, NYA_CString* argv) {
    nya_backtrace_init();

    // SDL_net sits on SDL, and nya_net_transport_udp_create calls NET_Init itself. Zero subsystems: this program wants no video, no audio and no gamepads.
    if (!SDL_Init(0)) {
        nya_log_error("SDL could not start: %s", SDL_GetError());
        return EXIT_FAILURE;
    }
    defer SDL_Quit();

    NYA_Arena* arena = nya_arena_create(.name = "net_echo");
    defer      nya_arena_destroy(arena);

    // which halves to run
    b8               listen_only  = false;
    b8               connect_only = false;
    /* Zero means the system picks, which is what the in-process run wants: two copies of this example, or a test suite beside it, must not have to agree on a number to stay out of each other's way. --listen overrides it, because a client in another terminal has to be told where to go. */
    u16              port         = 0;
    NYA_ConstCString address      = "127.0.0.1";

    for (s32 i = 1; i < argc; i++) {
        if (nya_string_equals(argv[i], "--listen") && i + 1 < argc) {
            listen_only = true;
            port        = (u16)strtoul(argv[++i], nullptr, 10);
            continue;
        }

        if (nya_string_equals(argv[i], "--connect") && i + 2 < argc) {
            connect_only = true;
            address      = argv[++i];
            port         = (u16)strtoul(argv[++i], nullptr, 10);
            continue;
        }

        nya_log_error("Unknown argument '%s'. Use --listen <port> or --connect <host> <port>.", argv[i]);
        return EXIT_FAILURE;
    }

    b8 run_server = !connect_only;
    b8 run_client = !listen_only;

    // the server
    NYA_NetTransport* server = nullptr;
    Endpoint          server_endpoint = { .label = "server" };

    if (run_server) {
        // Zeroed options mean a throwaway identity, generated here. A server players are meant to come back to keeps one instead; see nya_net_key_pair_load.
        NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &server), "while creating the server transport");

        NYA_Error listening = nya_net_transport_listen(server, port);
        if (!listening.ok) {
            nya_log_error("Could not listen: %s", (NYA_ConstCString)listening.message);
            return EXIT_FAILURE;
        }

        // Read back rather than echoed: with port zero the number is the system's, and the client below is about to connect to it.
        port = nya_net_transport_port(server);
        nya_log_info("server: listening on %u.", port);
    }

    // the client
    NYA_NetTransport* client = nullptr;
    Endpoint          client_endpoint = { .label = "client" };

    if (run_client) {
        // `server_key` left zero trusts whatever key the server presents, which is fine on loopback and is exactly what --server-key pins against in the game.
        NYA_EXPECT(nya_net_transport_udp_create(arena, (NYA_NetUdpOptions){ 0 }, &client), "while creating the client transport");

        NYA_EXPECT(nya_net_transport_connect(client, address, port), "while connecting");
        nya_log_info("client: connecting to %s:%u.", address, port);
    }

    // the loop
    u64 deadline = nya_clock_get_monotonic_ms() + TIMEOUT_MS;

    while (nya_clock_get_monotonic_ms() < deadline) {
        if (server != nullptr) endpoint_pump(server, &server_endpoint, true);
        if (client != nullptr) endpoint_pump(client, &client_endpoint, false);

        // The client decides when the conversation is over. A server alone runs until the timeout, which is what a server does.
        if (run_client && client_endpoint.finished) break;

        SDL_Delay(POLL_INTERVAL_MS);
    }

    // Before the disconnect: a dropped peer's slot is recycled, and its counters go with it.
    if (server != nullptr) endpoint_report(server, &server_endpoint);
    if (client != nullptr) endpoint_report(client, &client_endpoint);

    if (client != nullptr && client_endpoint.connected) {
        nya_net_transport_disconnect(client, client_endpoint.peer, NYA_NET_DISCONNECT_REQUESTED);
    }

    // Paired with the creates above rather than deferred beside them: a defer inside the `if` that created each one would fire at the end of that block, before a single packet moved.
    if (client != nullptr) nya_net_transport_destroy(client);
    if (server != nullptr) nya_net_transport_destroy(server);

    // An exchange that never happened is a failure, not a quiet success.
    if (run_client && run_server && client_endpoint.received < ROUNDS) {
        nya_log_error("Only %u of %u replies arrived before the %u ms timeout.", client_endpoint.received, ROUNDS, TIMEOUT_MS);
        return EXIT_FAILURE;
    }

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}

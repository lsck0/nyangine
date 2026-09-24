/**
 * @file websocket.h
 *
 * An outgoing WebSocket client, RFC 6455, over `ws://` and `wss://`. What talks to obs-websocket, a
 * streaming overlay, a chat bridge, or anything else that expects a long lived socket rather than a
 * request.
 *
 * The protocol itself is not here. Framing, masking, fragmentation and the closing handshake are
 * `http/http_websocket.h`, which this end drives as a client and the server in
 * `http/http_websocket_server.h` drives as a server; what is left below is the dialling out: a url, a
 * connect through curl, the upgrade request, and a poll that moves bytes between the socket and the
 * protocol.
 *
 * Nothing here blocks. A socket is created, polled once a frame, and written to whenever; the
 * connect, the TLS handshake, the upgrade and every read and write are advanced a little at a time by
 * the poll, so the frame is never waiting on a peer.
 *
 * ```
 * nya_websocket_create        starts connecting. Returns immediately, state CONNECTING
 * nya_websocket_destroy       the pair. Closes whatever stage it had reached
 * nya_websocket_poll          advances the connection and hands out one event. Never blocks
 * nya_websocket_state         where the connection is
 *
 * nya_websocket_send_text     queues a text message
 * nya_websocket_send_binary   queues a binary message
 * nya_websocket_send_object   queues an object as compact json text. The common case
 * nya_websocket_ping          queues a ping; the pong arrives as an event
 * nya_websocket_close         starts the closing handshake
 * ```
 *
 * The framing, the masking, the fragment assembly, the pongs and the close codes are
 * http_websocket.h's: nya_websocket_frame_encode, nya_websocket_frame_decode,
 * nya_websocket_accept_from_key and nya_websocket_close_name are declared there and are the same
 * functions the server runs.
 *
 * ```c
 * NYA_WebSocket* socket = nullptr;
 * NYA_EXPECT(nya_websocket_create(arena, (NYA_WebSocketOptions){ .url = "ws://127.0.0.1:4455" }, &socket));
 * defer nya_websocket_destroy(socket);
 *
 * // once a frame
 * NYA_WebSocketEvent event = { 0 };
 * while (nya_websocket_poll(socket, &event)) {
 *     switch (event.kind) {
 *         case NYA_WEBSOCKET_EVENT_OPEN:   nya_log_info("obs connected"); break;
 *         case NYA_WEBSOCKET_EVENT_TEXT:   handle(event.data, event.size); break;
 *         case NYA_WEBSOCKET_EVENT_CLOSED: nya_log_warn("obs closed: %s", nya_websocket_close_name(event.code)); break;
 *         default: break;
 *     }
 * }
 * ```
 *
 * ── why curl and not the engine's own sockets ──
 *
 * os_socket.h gives a TCP stream and nothing else, so `wss://` would mean
 * vendoring a TLS stack or shipping a client that only works in plaintext on localhost. curl is
 * already linked for the REST client, already carries a TLS backend on both platforms (OpenSSL on
 * Linux, schannel on Windows) and already uses the system trust store, and CURLOPT_CONNECT_ONLY hands
 * back exactly what this needs: a connected, TLS wrapped socket with curl_easy_send and
 * curl_easy_recv on top of it and no transfer machinery in the way.
 *
 * curl's own websocket support is not used and stays disabled in the vendor build. It is marked
 * experimental upstream, it cannot be driven a frame at a time the way this is, and turning it on
 * would put an unfinished protocol implementation in the link for every program that wants a REST
 * call. The framing is http_websocket.c's, shared with the server and fuzzed as
 * tests/fuzz/fuzz_websocket_frame.c.
 *
 * The connect is driven by curl's multi interface rather than curl_easy_perform on a job thread.
 * curl_multi_perform returns after whatever progress it could make without blocking, which makes the
 * whole connection a state machine this file steps once per frame. A thread was the alternative and
 * it loses on every count: it needs a handoff, it needs the socket to belong to one side at a time,
 * and it makes a failure depend on when the scheduler ran.
 *
 * ── what a hostile server may do ──
 *
 * The server is untrusted, and what that means is http_websocket.h's "what a hostile peer may do":
 * every bound, every refusal and the rule that a server never masks are the protocol's, so both ends
 * of this engine refuse the same things. A message past `max_message_bytes` is the one bound this file
 * chooses, and it chooses it per socket. None of it asserts; all of it arrives as
 * NYA_WEBSOCKET_EVENT_CLOSED with a code saying which.
 *
 * Thread safety: none. One thread owns a socket for its whole life.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_reconnect.h"
#include "nyangine/base/base_types.h"
// The protocol both ends share. A plugin depending on a module is the direction the layering allows; http names nothing here.
#include "nyangine/http/http_websocket.h"
#include "nyangine/plugins/curl/request.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/**
 * Bytes of upgrade response headers read before the connection is given up on.
 *
 * A conforming 101 is a few hundred bytes. This is the point past which a server is either not
 * speaking HTTP or is feeding an endless header to see whether the buffer grows.
 * */
#define NYA_WEBSOCKET_MAX_HANDSHAKE_BYTES 8192

/**
 * Queued outgoing bytes, frame headers included.
 *
 * A message has to fit this whole, since a half written frame cannot be taken back. Sixty four
 * kilobytes is far past any control protocol's request; something that sends megabytes wants a
 * different transport, not a bigger buffer here.
 * */
#define NYA_WEBSOCKET_SEND_BYTES 65536

/** What one read takes from the socket at a time. */
#define NYA_WEBSOCKET_RECEIVE_BYTES 16384

/**
 * The default ceiling on one assembled message, fragments included.
 *
 * Sized for the largest thing a control protocol answers with, which for obs-websocket is a scene
 * list of a few tens of kilobytes. Raise it per socket through the options when a peer is known to
 * send more; the buffer is allocated once at create, so the cost is visible and paid up front.
 * */
#define NYA_WEBSOCKET_DEFAULT_MAX_MESSAGE_BYTES 262144

/** What the whole connect, TLS and upgrade are given before the socket gives up. */
#define NYA_WEBSOCKET_DEFAULT_TIMEOUT_MS 30000

// ───────────────────────────────────── TYPES ─────────────────────────────────────

// The opcodes, close codes, events and frame are http_websocket.h's; what a connection is made of is not this end's to define.
typedef enum NYA_WebSocketState     NYA_WebSocketState;
typedef struct NYA_WebSocketOptions NYA_WebSocketOptions;
typedef struct NYA_WebSocket        NYA_WebSocket;

enum NYA_WebSocketState {
    /** Resolving, connecting and negotiating TLS. Nothing may be sent yet. */
    NYA_WEBSOCKET_STATE_CONNECTING,

    /** The upgrade request is on the wire and the 101 has not come back. */
    NYA_WEBSOCKET_STATE_HANDSHAKING,

    /** Messages move in both directions. */
    NYA_WEBSOCKET_STATE_OPEN,

    /** A close frame has been sent and the peer's has not come back. */
    NYA_WEBSOCKET_STATE_CLOSING,

    /**
     * Dropped, and waiting out the backoff before dialling again. Only ever reached when the options
     * asked for a reconnect policy; without one a drop goes straight to CLOSED. The socket returns to
     * CONNECTING on its own when the delay is up, and a caller keeps polling it exactly as before.
     * */
    NYA_WEBSOCKET_STATE_RECONNECTING,

    /** Done, for any reason. The socket never leaves this state; create another one. */
    NYA_WEBSOCKET_STATE_CLOSED,

    NYA_WEBSOCKET_STATE_COUNT,
};

struct NYA_WebSocketOptions {
    /** Required. `ws://host[:port][/path]` or `wss://...`. Anything else is refused at create. */
    NYA_ConstCString url;

    /** Sent as Sec-WebSocket-Protocol, and checked against what the server picks. Null sends none. */
    NYA_ConstCString subprotocol;

    /**
     * Added to the upgrade request. Terminated by the first entry with a null name. The handshake's own
     * headers cannot be overridden; an entry naming one is refused at create.
     * */
    NYA_RequestHeader headers[NYA_REQUEST_MAX_HEADERS];

    /** Sent as `Authorization: Bearer <token>` on the upgrade request. */
    NYA_ConstCString bearer_token;

    /** Connect, TLS and upgrade together. Zero means NYA_WEBSOCKET_DEFAULT_TIMEOUT_MS. */
    u64 handshake_timeout_ms;

    /**
     * Ceiling on one assembled message. Zero means NYA_WEBSOCKET_DEFAULT_MAX_MESSAGE_BYTES. A peer that
     * exceeds it is closed with NYA_WEBSOCKET_CLOSE_TOO_LARGE.
     * */
    u64 max_message_bytes;

    /** Accept any TLS certificate. Only for a test against a local server. */
    b8 insecure_skip_tls_verify;

    /**
     * Opt-in automatic reconnect. Disabled by default (a zeroed policy), which keeps the old behaviour:
     * an unexpected drop is a single NYA_WEBSOCKET_EVENT_CLOSED and the socket is terminal.
     *
     * Enabled, a drop the caller did not ask for moves the socket to NYA_WEBSOCKET_STATE_RECONNECTING
     * and dials again after nya_backoff_ms(attempt, ...) — full jitter, capped delay, capped attempts —
     * rather than reporting CLOSED. A successful redial resets the backoff and reports OPEN again, which
     * is the caller's cue to re-run whatever the connection needs re-established (re-subscribe, re-auth).
     * When the attempts run out, or when the caller closes the socket itself, the drop is final and
     * CLOSED is reported as always. See base_reconnect.h.
     * */
    NYA_ReconnectPolicy reconnect;
};

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

// ───────────────────────────────────── LIFETIME ─────────────────────────────────────

/**
 * Parses the options, allocates the socket's buffers from `arena`, and starts connecting.
 *
 * Returns as soon as the connect is under way, so a failure here is a bad option rather than an
 * unreachable server: NYA_ERROR_INVALID_ARGUMENT for a url that is not ws or wss, a header the
 * handshake owns, or a message ceiling below what a frame needs. An unreachable server arrives later
 * as NYA_WEBSOCKET_EVENT_CLOSED.
 * */
NYA_API NYA_Error nya_websocket_create(NYA_Arena* arena, NYA_WebSocketOptions options, OUT NYA_WebSocket** out_socket) __attr_no_discard;

/**
 * Closes the connection at whatever stage it reached and frees the socket. Null is a no-op.
 *
 * Does not wait for a closing handshake. Call nya_websocket_close first and poll until CLOSED when the
 * peer should be told why.
 * */
NYA_API void nya_websocket_destroy(NYA_WebSocket* socket);

// ───────────────────────────────────── OPERATIONS ─────────────────────────────────────

/**
 * Advances the connection and hands out one event, or returns false when there is nothing to report.
 *
 * This is where everything happens: the connect, the TLS handshake, the upgrade, reads, writes, and
 * the pong for every ping. It never blocks and does a bounded amount of work per call, so calling it
 * in a loop until it returns false is the intended use and cannot be driven by the peer.
 * */
NYA_API b8 nya_websocket_poll(NYA_WebSocket* socket, OUT NYA_WebSocketEvent* out_event);

NYA_API NYA_WebSocketState nya_websocket_state(const NYA_WebSocket* socket) __attr_no_discard;

/**
 * Queues `text` as one text message.
 *
 * Legal before the connection is open: it goes out with the first flush after the upgrade, which is
 * what lets a caller create and send in one breath. NYA_ERROR_OUT_OF_MEMORY when the queue cannot hold
 * it, NYA_ERROR_IO once the socket is closing or closed.
 * */
NYA_API NYA_Error nya_websocket_send_text(NYA_WebSocket* socket, NYA_ConstCString text) __attr_no_discard;

NYA_API NYA_Error nya_websocket_send_binary(NYA_WebSocket* socket, const u8* data, u64 size) __attr_no_discard;

/**
 * Serializes `body` as compact json and sends it as text. What a control protocol wants.
 *
 * ```c
 * NYA_Object* request = nya_object_create(arena);
 * nya_object_add(request, "op", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = 6 });
 * NYA_TRY(nya_websocket_send_object(socket, arena, request));
 * ```
 * */
NYA_API NYA_Error nya_websocket_send_object(NYA_WebSocket* socket, NYA_Arena* arena, const NYA_Object* body) __attr_no_discard;

/** Queues a ping. `size` is at most NYA_WEBSOCKET_MAX_CONTROL_BYTES; the pong arrives as an event. */
NYA_API NYA_Error nya_websocket_ping(NYA_WebSocket* socket, const u8* data, u64 size) __attr_no_discard;

/**
 * Starts the closing handshake: sends a close frame and moves to CLOSING. The peer's answer arrives as
 * NYA_WEBSOCKET_EVENT_CLOSED. Calling it twice is a no-op.
 *
 * `reason` is at most 123 bytes once encoded, which is the control frame limit minus the code, and a
 * longer one is truncated rather than refused, because a close must not fail.
 * */
NYA_API NYA_Error nya_websocket_close(NYA_WebSocket* socket, NYA_WebSocketClose code, NYA_ConstCString reason) __attr_no_discard;

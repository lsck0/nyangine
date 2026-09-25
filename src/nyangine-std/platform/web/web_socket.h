/**
 * @file web_socket.h
 *
 * A client WebSocket over the browser's `WebSocket`, polled the way the engine polls a net transport.
 *
 * ```
 * nya_web_socket_open     dial a server, returns a handle to poll
 * nya_web_socket_close    hang up and release the handle
 * nya_web_socket_phase    connecting, open, or closed
 * nya_web_socket_send     one binary message out
 * nya_web_socket_receive  one binary message in, from the inbound queue
 * ```
 *
 * ── where this sits ──
 *
 * The engine already speaks WebSocket at two other seams, and this is neither. http/http_websocket*.h is
 * the RFC 6455 framing a *native* server and the curl-plugin client run over their own sockets; a wasm
 * module has no socket and cannot do its own framing — the browser owns the protocol and hands the module
 * whole messages. http/http_net_websocket.h is the server end a browser dials *into*. This is the browser
 * end that dials *out*: the CSR client's link to that server, or to any WebSocket endpoint.
 *
 * ── why polled, not the plugin's shape ──
 *
 * plugins/curl/websocket.h is the engine's client WebSocket, and it is already frame-stepped rather than
 * blocking, which is the right shape here too. But it is a rank-12 plugin built on http's framing and
 * curl's socket, neither of which exists in a wasm module, so this cannot be a backend of it — it is a
 * platform primitive that stands on the browser's own WebSocket instead. It keeps the same cooperative
 * rhythm: opening does not block, and `receive` drains an inbound queue one message at a time, the way
 * `nya_net_transport_poll` drains a transport after each tick. Every message is binary
 * (`binaryType = "arraybuffer"`); a text frame is delivered as its bytes.
 *
 * The handle is arena-owned and valid until `nya_web_socket_close`. Off wasm there is no browser
 * `WebSocket`; the native tree has the curl plugin for a client socket. The fallback here refuses —
 * `open` returns null — so this stays a browser-only seam that still compiles natively.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_WebSocketLink NYA_WebSocketLink;

/** Where a link is in its life. A browser `WebSocket` moves connecting → open → closed and never back. */
typedef enum {
    /** Dialing: the handshake is in flight and nothing may be sent yet. */
    NYA_WEB_SOCKET_CONNECTING,

    /** Established: messages move in both directions. */
    NYA_WEB_SOCKET_OPEN,

    /** Hung up, by either end or by a connection error. Nothing more will arrive. */
    NYA_WEB_SOCKET_CLOSED,
} NYA_WebSocketPhase;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Dials `url` (a `ws://` or `wss://` endpoint) and returns a handle to poll, or null when `arena` or
 * `url` is null, and off wasm where there is no browser `WebSocket`. The connection is not open yet on
 * return; poll `nya_web_socket_phase` until it reads NYA_WEB_SOCKET_OPEN before sending.
 * */
NYA_API NYA_WebSocketLink* nya_web_socket_open(NYA_Arena* arena, NYA_ConstCString url) __attr_no_discard;

/** Closes `link` and releases the handle and its inbound queue. Null is a no-op. After this the handle must not be used. */
NYA_API void nya_web_socket_close(NYA_WebSocketLink* link);

/** Where `link` is in its life. NYA_WEB_SOCKET_CLOSED for a null handle, so a caller can treat null as hung up. */
NYA_API NYA_WebSocketPhase nya_web_socket_phase(const NYA_WebSocketLink* link) __attr_no_discard;

/**
 * Sends `size` bytes of `bytes` as one binary message. False when `link` is null, when the link is not
 * open, or when the browser refused the send; a zero `size` sends an empty message. The bytes are copied
 * at the call, so `bytes` need not outlive it.
 * */
NYA_API b8 nya_web_socket_send(NYA_WebSocketLink* link, const u8* bytes, u64 size) __attr_no_discard;

/**
 * Takes the oldest queued inbound message into `buffer`, at most `capacity` bytes, and returns its full
 * length. When the message is longer than `capacity` it is left on the queue and its length returned, so
 * a caller can size a buffer and read again; otherwise it is removed. Returns -1 when the queue is empty
 * or `link` is null. Never writes past `capacity`.
 * */
NYA_API s64 nya_web_socket_receive(NYA_WebSocketLink* link, OUT u8* buffer, u64 capacity) __attr_no_discard;

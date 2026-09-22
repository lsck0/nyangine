/**
 * @file http_websocket_server.h
 *
 * The server end of RFC 6455: the upgrade, and a connection that outlives the exchange that made it.
 *
 * ```
 * nya_http_websocket_route_add     mounts one path a client may upgrade on
 * nya_http_websocket_route_remove  the pair. A removed path refuses the next upgrade
 *
 * nya_http_websocket_send_text     one text message to one connection
 * nya_http_websocket_broadcast_text  the same message to everyone on one path. The push case
 * nya_http_websocket_protocol      the connection's framing, for binary, a ping or a goodbye
 *
 * nya_http_websocket_count / _at   what is connected right now
 * nya_http_websocket_path / _address   which route it upgraded on, and who it is
 * ```
 *
 * ```c
 * NYA_INTERNAL void on_message(NYA_HttpWebSocket* socket, b8 is_text, const u8* data, u64 size) {
 *     if (is_text && size == 3 && nya_memcmp(data, "now", 3) == 0) push_a_snapshot(socket);
 * }
 *
 * NYA_INTERNAL const NYA_HttpWebSocketRoute _METRICS = {
 *     .path       = "/ws/metrics",
 *     .summary    = "this program's numbers, pushed on a tick",
 *     .on_message = on_message,
 * };
 *
 * NYA_EXPECT(nya_http_websocket_route_add(&_METRICS));
 * defer nya_http_websocket_route_remove(&_METRICS);
 *
 * // whenever there is something to say, from the program's own loop
 * (void)nya_http_websocket_broadcast_text("/ws/metrics", json);
 * ```
 *
 * ── an upgrade is a request like any other ──
 *
 * It arrives on the listener in http_server.h, on a connection that was accepted under
 * NYA_HTTP_MAX_CONNECTIONS and NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS, it is parsed by the same parser,
 * and it spends a token from its address's bucket before it is looked at. A socket does not escape the
 * server's limits by becoming a WebSocket: it keeps the slot it was accepted into, so a peer that
 * upgrades has taken one of the connections it could have taken anyway, and it is counted again here
 * against NYA_HTTP_MAX_WEBSOCKETS and NYA_HTTP_MAX_WEBSOCKETS_PER_ADDRESS.
 *
 * It also goes through the origin check every unsafe request goes through
 * (Sec-Fetch-Site and Origin against Host): an upgrade is a GET, so nothing about the method would
 * have stopped a page on another site from opening a socket into this program and reading whatever it
 * pushes. A cross-site upgrade is refused with 403.
 *
 * What refuses an upgrade, all with a problem body a client can read: 404 for a path with no route,
 * 403 for another site, 400 for a request that is not a well formed handshake — the wrong method, no
 * key, a key that is not a key, a version that is not 13, or a byte of anything after the request,
 * since a client may not send frames before the 101 — and 503 once the table or the address's share of
 * it is full.
 *
 * ── what a connected peer may do ──
 *
 * Whatever it likes, inside http_websocket.h's bounds and the ones below. It may send a frame at a
 * time and never finish a message: the message bound and the fragment bound both apply, and it is
 * dropped after NYA_HTTP_WEBSOCKET_IDLE_TIMEOUT_MS of silence with a ping sent at
 * NYA_HTTP_WEBSOCKET_PING_INTERVAL_MS before that, so a half open connection cannot be held forever.
 * It may stop reading: the server queues what it has to say and drops the connection once more than
 * NYA_HTTP_MAX_PENDING_WRITE_BYTES is outstanding, which is the same bound an HTTP answer gets. It may
 * send a frame that is not a frame, or one that is not masked: both are a close with 1002 and then the
 * socket. None of it asserts, and none of it allocates.
 *
 * ── what is not here ──
 *
 * Authentication: a route is open to anyone who can reach the port, which is what the loopback bind
 * and the origin check are doing the work of. A browser cannot set an Authorization header on a
 * WebSocket, so a token would have to arrive in the query string or in the first message, and neither
 * is a decision to make as a side effect of adding framing. Subprotocol negotiation: a
 * Sec-WebSocket-Protocol offer is ignored rather than answered, which RFC 6455 allows and which means
 * a client must not require one. permessage-deflate and every other extension: see http_websocket.h.
 *
 * Thread safety: none. Everything here runs on the thread that drains the server.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/http/http_types.h"
#include "nyangine/http/http_websocket.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * WebSocket connections held at once, out of the NYA_HTTP_MAX_CONNECTIONS the listener accepts.
 *
 * Half the table. A WebSocket is a connection that never ends on its own, so every one of them is a
 * slot no request can use: leaving the other half means a browser holding a socket open never stops
 * the same browser from loading a page, and a program that wants more of them raises both bounds
 * together rather than discovering this one at the wrong moment.
 * */
#ifndef NYA_HTTP_MAX_WEBSOCKETS
#define NYA_HTTP_MAX_WEBSOCKETS 4
#endif

/**
 * WebSocket connections one address may hold.
 *
 * A page opens one. Two leaves room for a reload's socket while the old one is still timing out, and
 * stops one address from taking every slot the way NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS stops it
 * taking every connection. The upgrade past it is refused with 503 rather than queued.
 * */
#ifndef NYA_HTTP_MAX_WEBSOCKETS_PER_ADDRESS
#define NYA_HTTP_MAX_WEBSOCKETS_PER_ADDRESS 2
#endif

/** Paths a program may mount. One per kind of stream, so this is a count of streams. */
#define NYA_HTTP_MAX_WEBSOCKET_ROUTES 4

/**
 * Bytes of payload one frame may announce.
 *
 * The tighter of the two bounds and the one that acts first, on the header alone, before a byte of the
 * payload has been read into anything. Four kilobytes is what a request's head gets, and a message to
 * a program here is the same kind of small document a request is.
 * */
#ifndef NYA_HTTP_WEBSOCKET_MAX_FRAME_BYTES
#define NYA_HTTP_WEBSOCKET_MAX_FRAME_BYTES 4096
#endif

/**
 * Bytes of one assembled message, every fragment counted.
 *
 * The request body bound, for the same reason: a message a client sends is the same kind of document a
 * body is, and nothing here reads a larger one. A peer that fragments past it is closed with 1009
 * rather than given more room, and NYA_WEBSOCKET_MAX_FRAGMENTS bounds the fragments themselves.
 * */
#ifndef NYA_HTTP_WEBSOCKET_MAX_MESSAGE_BYTES
#define NYA_HTTP_WEBSOCKET_MAX_MESSAGE_BYTES NYA_HTTP_MAX_BODY_BYTES
#endif

/**
 * Queued outgoing bytes per connection, frame headers included.
 *
 * A message is framed into it whole or not at all, since a half written frame cannot be taken back.
 * Twice the largest message this server accepts, so a push may be queued while the previous one is
 * still being taken by the kernel; past that a send fails and the caller has something to decide,
 * which is better than a queue that grows because a peer reads slowly.
 * */
#define NYA_HTTP_WEBSOCKET_SEND_BYTES (NYA_HTTP_WEBSOCKET_MAX_MESSAGE_BYTES * 2)

/**
 * What one read takes off the socket at a time.
 *
 * A frame's payload is consumed as it arrives rather than buffered whole, so only a header has to fit
 * here; the size is a read's worth, not a frame's.
 * */
#define NYA_HTTP_WEBSOCKET_RECEIVE_BYTES 4096

/**
 * Silence before a connection is dropped.
 *
 * Longer than NYA_HTTP_IDLE_TIMEOUT_MS on purpose: an idle WebSocket is doing exactly what it is for,
 * where an idle request is a peer that never finished one. What makes the silence mean something is
 * the ping below, so thirty seconds is "two pings went unanswered", not "nobody said anything".
 * */
#define NYA_HTTP_WEBSOCKET_IDLE_TIMEOUT_MS 30000

/**
 * Silence before the server pings.
 *
 * A peer whose connection is gone without a FIN — a laptop that slept, a NAT that dropped the entry —
 * looks exactly like a quiet one, and the only way to tell them apart is to ask. A live peer's pong
 * resets the clock, so this is what turns the timeout above from "you were quiet" into "you are gone".
 * */
#define NYA_HTTP_WEBSOCKET_PING_INTERVAL_MS 10000

/**
 * Messages handed to a handler from one connection in one tick.
 *
 * The frame is the thing being protected, the same way NYA_HTTP_MAX_REQUESTS_PER_TICK protects it: a
 * peer that pipelines messages gets this much of the tick and the rest waits for the next one.
 * */
#define NYA_HTTP_WEBSOCKET_MAX_MESSAGES_PER_TICK 8

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A connection that upgraded. A route's callbacks are handed one; nothing else makes or frees one. */
typedef struct NYA_HttpWebSocket      NYA_HttpWebSocket;
typedef struct NYA_HttpWebSocketRoute NYA_HttpWebSocketRoute;

/** Called once, after the 101 has gone out and before any message. */
typedef void (*NYA_HttpWebSocketOpenFn)(NYA_HttpWebSocket* socket);

/**
 * Called for one whole message, fragments already joined. `data` is the connection's own buffer and is
 * valid until this returns; a text message carries a NUL one past `size`.
 * */
typedef void (*NYA_HttpWebSocketMessageFn)(NYA_HttpWebSocket* socket, b8 is_text, const u8* data, u64 size);

/**
 * Called once when the connection ends, for any reason, including a refusal. Nothing may be sent from
 * it: the socket is already gone by the time a program hears about it.
 * */
typedef void (*NYA_HttpWebSocketCloseFn)(NYA_HttpWebSocket* socket, NYA_WebSocketClose code);

/**
 * One path a client may upgrade on, and what happens when it does.
 *
 * Plain data, so a program's route is a `static const` the compiler lays out, the way an
 * NYA_HttpRoute is. It is not copied and has to outlive the mount.
 * */
struct NYA_HttpWebSocketRoute {
    /** The whole path, absolute and matched exactly: "/ws/metrics". No patterns, as in http_router.h. */
    NYA_ConstCString path;

    /** One line, for the log when a peer connects. Required. */
    NYA_ConstCString summary;

    NYA_HttpWebSocketOpenFn    on_open;
    NYA_HttpWebSocketMessageFn on_message;
    NYA_HttpWebSocketCloseFn   on_close;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * ROUTES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Mounts `route`, so an upgrade on its path is answered rather than refused.
 *
 * The server has to be running: a route is mounted on a server the way an NYA_HttpRouter is, and
 * stopping it unmounts everything. NYA_ERROR_NOT_FOUND when it is not, NYA_ERROR_INVALID_ARGUMENT for
 * a route with no absolute path or no summary, NYA_ERROR_ALREADY_EXISTS for a path already mounted,
 * and NYA_ERROR_OUT_OF_MEMORY past NYA_HTTP_MAX_WEBSOCKET_ROUTES.
 * */
NYA_API NYA_Error nya_http_websocket_route_add(const NYA_HttpWebSocketRoute* route) __attr_no_discard;

/**
 * Unmounts it. A route that was never mounted is a no-op.
 *
 * Connections already open on it are closed with 1001, so a program that unmounts a stream is not
 * still being called back by it afterwards.
 * */
NYA_API void nya_http_websocket_route_remove(const NYA_HttpWebSocketRoute* route);

/*
 * ─────────────────────────────────────────────────────────
 * MESSAGES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Queues `text` as one message for `socket`, to go out on the next drain.
 *
 * NYA_ERROR_IO once the connection is closing or closed, NYA_ERROR_INVALID_ARGUMENT for text that is
 * not UTF-8, and NYA_ERROR_OUT_OF_MEMORY when NYA_HTTP_WEBSOCKET_SEND_BYTES cannot hold it, which
 * means the peer has stopped reading and is about to be dropped.
 * */
NYA_API NYA_Error nya_http_websocket_send_text(NYA_HttpWebSocket* socket, NYA_ConstCString text) __attr_no_discard;

/**
 * The same message to every connection on `path`, and how many of them took it.
 *
 * What a push is: a program with something to say says it once. A connection whose queue is full is
 * skipped rather than failing the call, since one slow peer is not a reason for the others to miss a
 * tick; it is dropped by the pending-write bound soon enough.
 * */
NYA_API u32 nya_http_websocket_broadcast_text(NYA_ConstCString path, NYA_ConstCString text);

/**
 * The connection's framing, which is where binary messages, pings and the closing handshake live:
 * nya_websocket_protocol_send and nya_websocket_protocol_close, the same calls the client end makes.
 *
 * Whatever it queues goes out on the next drain. Null for a connection that is no longer open.
 * */
NYA_API NYA_WebSocketProtocol* nya_http_websocket_protocol(NYA_HttpWebSocket* socket) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

/** How many connections are open. Zero when the server is not running. */
NYA_API u32 nya_http_websocket_count(void) __attr_no_discard;

/** The `index`th open connection, or null. The order is the table's and is not stable across drains. */
NYA_API NYA_HttpWebSocket* nya_http_websocket_at(u32 index) __attr_no_discard;

/** The path it upgraded on, which is its route's. Never null. */
NYA_API NYA_ConstCString nya_http_websocket_path(const NYA_HttpWebSocket* socket) __attr_no_discard;

/** The peer as the socket reports it, in full, never a forwarded header. Never null. */
NYA_API NYA_ConstCString nya_http_websocket_address(const NYA_HttpWebSocket* socket) __attr_no_discard;

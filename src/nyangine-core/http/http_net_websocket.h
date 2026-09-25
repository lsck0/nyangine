/**
 * @file http_net_websocket.h
 *
 * A net transport over a WebSocket, so a browser can be a peer of a native server.
 *
 * ```
 * nya_net_transport_ws_create   a server-side transport that mounts a route browsers dial into
 * nya_net_transport_ws_allow    a player key the server will accept; a join off the list is refused
 * nya_net_transport_ws_disallow the pair
 * nya_net_ws_join_encode        the join frame a client sends first, for a browser or a test
 * ```
 *
 * ── where this sits ──
 *
 * It is a net transport (net_transport.h) that a server enables instead of, or beside, the UDP one, and
 * it is here rather than in `net` because it is built on http_websocket_server.h: the RFC 6455 upgrade,
 * the framing and the connection table are the http module's, and this file wraps whole WebSocket
 * messages as net messages rather than writing a second WebSocket. `http` may name `net` — net is
 * compiled before it — which is the only direction this dependency runs.
 *
 * A browser dials in, so this transport listens and never connects out: `connect` is null the way it is
 * for a loopback pair. The host owns the HTTP server (nya_system_http_init, nya_system_http_tick); this
 * transport mounts one route on it and turns its callbacks into transport events, which the host drains
 * with nya_net_transport_poll after each tick.
 *
 * ── the transport's own handshake ──
 *
 * The first WebSocket message a peer sends is a join frame (nya_net_ws_join_encode): a tag, the version
 * the client speaks, and the player key it presents. The server accepts the peer, and reports it as
 * NYA_NET_TRANSPORT_EVENT_CONNECTED, only when the version equals this server's and the key is one
 * nya_net_transport_ws_allow was given. A version that does not match is closed with
 * NYA_WEBSOCKET_CLOSE_POLICY and the reason "protocol version mismatch"; a key that is not on the list
 * with the reason "player key not on allowlist"; a malformed join with NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR.
 * A refused peer is dropped at the join, before it is ever an established peer, so it is a close on the
 * wire the client reads and not a silent drop — and no CONNECTED or DISCONNECTED event above.
 *
 * Every WebSocket message is binary. A join is tagged NYA_NET_WS_TAG_JOIN, the server's acceptance
 * NYA_NET_WS_TAG_ACCEPT, and a carried net message NYA_NET_WS_TAG_DATA, so control and data share one
 * ordered stream without a caller having to tell them apart.
 *
 * ── what is not here ──
 *
 * Only one WebSocket net transport may exist in a process, since the HTTP server it mounts on is a
 * singleton and a route's callbacks carry no user pointer. There is one wire, and it is reliable and
 * ordered because TCP is: both net channels ride it, so NYA_NET_CHANNEL_UNRELIABLE is delivered rather
 * than dropped, which is a correctness-over-latency choice a browser link makes anyway. Encryption is
 * the transport's (wss/TLS), not this file's; the UDP transport's per-packet sealing has no equivalent
 * here. The fuller multiplayer work — fragmentation past a message, lag compensation, a jitter buffer —
 * is the net layer's above this and is unchanged by this transport.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/net/net_crypto.h"
#include "nyangine-core/net/net_transport.h"

// CONSTANTS

/** The route mounted when NYA_NetWsOptions.path is null. */
#define NYA_NET_WS_DEFAULT_PATH "/ws/net"

/** Player keys one server holds on its allowlist. A join off the list is refused. */
#define NYA_NET_WS_MAX_ALLOWED NYA_NET_MAX_PEERS

/** The first byte of every transport frame, telling control from data on one stream. */
#define NYA_NET_WS_TAG_JOIN   0x01U /* client → server: version and player key, sent first */
#define NYA_NET_WS_TAG_ACCEPT 0x02U /* server → client: the join was accepted */
#define NYA_NET_WS_TAG_DATA   0x03U /* either way: a net message follows the tag */

/** A join frame: the tag, the version as four little-endian bytes, and the key. */
#define NYA_NET_WS_JOIN_SIZE (1 + 4 + NYA_NET_KEY_SIZE)

/** An accept frame: the tag and the server's version, so a client learns it is in. */
#define NYA_NET_WS_ACCEPT_SIZE (1 + 4)

// TYPES

typedef struct NYA_NetWsOptions NYA_NetWsOptions;

/** How a WebSocket net transport is configured. Zero mounts NYA_NET_WS_DEFAULT_PATH and speaks version 0. */
struct NYA_NetWsOptions {
    /**
     * The path a client upgrades on, absolute and matched exactly, e.g. "/ws/net". Null is the default.
     * The string is not copied and must outlive the transport.
     * */
    NYA_ConstCString path;

    /**
     * The protocol or application version this server speaks. A peer presenting any other value in its
     * join is refused with NYA_NET_DISCONNECT_VERSION before any state is exchanged.
     * */
    u32 version;
};

// FUNCTIONS

/**
 * A server-side net transport over a WebSocket. The HTTP server must already be running
 * (nya_system_http_init); nya_net_transport_listen mounts the route on it, and the host ticks the
 * server and then polls this transport.
 *
 * NYA_ERROR_ALREADY_EXISTS when a WebSocket net transport already exists in this process.
 * */
NYA_API NYA_Error nya_net_transport_ws_create(NYA_Arena* arena, NYA_NetWsOptions options, OUT NYA_NetTransport** out_transport) __attr_no_discard;

/**
 * Adds a player key to the allowlist, so a peer presenting it in its join is accepted. Adding the same
 * key twice is a no-op.
 *
 * NYA_ERROR_INVALID_ARGUMENT for an all-zero key, and NYA_ERROR_OUT_OF_MEMORY past NYA_NET_WS_MAX_ALLOWED.
 * */
NYA_API NYA_Error nya_net_transport_ws_allow(NYA_NetTransport* transport, const u8 key[NYA_NET_KEY_SIZE]) __attr_no_discard;

/** Removes a key from the allowlist. Already-connected peers keep their connection; the next join is checked afresh. A key that was never on it is a no-op. */
NYA_API void nya_net_transport_ws_disallow(NYA_NetTransport* transport, const u8 key[NYA_NET_KEY_SIZE]);

/** Whether a key is on the allowlist right now. */
NYA_API b8 nya_net_transport_ws_is_allowed(NYA_NetTransport* transport, const u8 key[NYA_NET_KEY_SIZE]) __attr_no_discard;

/**
 * Writes the join frame a client sends first: NYA_NET_WS_TAG_JOIN, `version` little-endian, then `key`.
 * A browser builds the same bytes and sends them as one binary WebSocket message.
 * */
NYA_API void nya_net_ws_join_encode(u32 version, const u8 key[NYA_NET_KEY_SIZE], OUT u8 out_frame[NYA_NET_WS_JOIN_SIZE]);

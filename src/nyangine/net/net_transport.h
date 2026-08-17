/**
 * @file net_transport.h
 *
 * How bytes get from one process to another, behind one interface with three implementations.
 *
 * The server and the client above this file know about peers, ticks and snapshots. They do not know
 * whether a peer is across the room, across the internet, or in the same process — and that is the
 * point. "Open to LAN" changes which transport the server holds and nothing else; a Steam lobby is a
 * fourth line in a switch rather than a second netcode.
 *
 * ## The three
 *
 * - **Loopback** (net_loopback.c) hands buffers between two endpoints in one process. Never drops,
 *   never reorders, zero latency. What a listen server's own local client uses, and what every test
 *   in tests/nyangine/net uses to exercise the layers above without a socket.
 * - **UDP** (net_udp.c) over SDL_net datagrams, with the reliability, ordering and fragmentation
 *   this file's contract promises built on top. LAN and internet alike.
 * - **Steam** (net_steam.c) over Steam's relayed peer-to-peer sockets, which is how a player behind
 *   a NAT is reachable without port forwarding. Behind NYA_PLUGIN_STEAM, and presently a stub that
 *   reports itself unavailable — the interface exists so the rest of the engine is already written
 *   against it.
 *
 * ## What a transport must guarantee
 *
 * - `NYA_NET_CHANNEL_RELIABLE` messages arrive, in order, exactly once, or the peer is disconnected.
 * - `NYA_NET_CHANNEL_UNRELIABLE` messages may be dropped or reordered, but never *duplicated* and
 *   never *truncated*: a message that arrives is the whole message. Duplicate suppression matters
 *   because a snapshot applied twice is harmless while a command applied twice is a double jump.
 * - A message of any size may be sent. Splitting it to fit the path is the transport's problem.
 * - Nothing blocks. Every call returns immediately; `nya_net_transport_poll` is where work happens.
 *
 * A transport does *not* interpret payloads. It does not know what a snapshot is.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/net/net_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_NetTransport      NYA_NetTransport;
typedef struct NYA_NetTransportVTable NYA_NetTransportVTable;
typedef struct NYA_NetTransportEvent NYA_NetTransportEvent;
typedef struct NYA_NetPeerStats      NYA_NetPeerStats;
typedef enum NYA_NetTransportKind    NYA_NetTransportKind;
typedef enum NYA_NetTransportEventKind NYA_NetTransportEventKind;

enum NYA_NetTransportKind {
    NYA_NET_TRANSPORT_LOOPBACK = 0,
    NYA_NET_TRANSPORT_UDP,
    NYA_NET_TRANSPORT_STEAM,

    NYA_NET_TRANSPORT_KIND_COUNT,
};

enum NYA_NetTransportEventKind {
    NYA_NET_TRANSPORT_EVENT_NONE = 0,

    /** A peer completed the transport's own handshake. The layer above may now send to it. */
    NYA_NET_TRANSPORT_EVENT_CONNECTED,

    /** A peer is gone. Its id will never resolve again. */
    NYA_NET_TRANSPORT_EVENT_DISCONNECTED,

    /** A whole message arrived. See NYA_NetTransportEvent.data. */
    NYA_NET_TRANSPORT_EVENT_MESSAGE,

    NYA_NET_TRANSPORT_EVENT_KIND_COUNT,
};

/**
 * One thing that happened, drained by nya_net_transport_poll.
 *
 * A queue rather than callbacks, because the layers above run inside a fixed tick and want to
 * process everything that arrived *between* ticks at a known point — not at whatever moment a
 * datagram landed. It also means a transport never calls into game code, which is what keeps a
 * malformed packet from unwinding through a callback into the middle of a simulation step.
 * */
struct NYA_NetTransportEvent {
    NYA_NetTransportEventKind kind;
    NYA_NetPeerId             peer;

    /**
     * The message, for a MESSAGE event.
     *
     * Owned by the transport and valid only until the next nya_net_transport_poll — copy anything
     * that has to outlive the call. The loopback transport in particular hands back a pointer into
     * the sender's own buffer, which is exactly why single player costs no copy.
     * */
    const u8* data;
    u64       size;

    NYA_NetChannel channel;

    /** Why, for a DISCONNECTED event. */
    NYA_NetDisconnect reason;
};

/** What a connection is currently costing, for a debug overlay and for the client's clock sync. */
struct NYA_NetPeerStats {
    /** Smoothed round trip time. Zero for a loopback peer, which is not a rounding of a small number. */
    f32 rtt_ms;

    /** Variation in the round trip, which is what a jitter buffer is sized from. */
    f32 jitter_ms;

    /** Fraction of sent packets never acknowledged, 0..1, over a recent window. */
    f32 packet_loss;

    u64 bytes_sent;
    u64 bytes_received;
    u64 packets_sent;
    u64 packets_received;

    /** Reliable messages resent because they were not acknowledged in time. */
    u64 retransmits;
};

/**
 * What every transport implements. See the contract at the top of this file.
 *
 * A vtable rather than a compile-time switch, because a listen server holds *two at once*: a
 * loopback for its own player and a UDP socket for everyone else. Selecting one at build time would
 * make hosting and playing mutually exclusive.
 * */
struct NYA_NetTransportVTable {
    NYA_ConstCString name;
    NYA_NetTransportKind kind;

    /** Starts accepting peers on `port`. Null for a transport that cannot listen. */
    NYA_Error (*listen)(NYA_NetTransport* transport, u16 port);

    /** Starts connecting to `address`. Completion arrives as a CONNECTED event, or a timeout. */
    NYA_Error (*connect)(NYA_NetTransport* transport, NYA_ConstCString address, u16 port);

    /** Queues a message. Returning an error means the peer is unusable, not that the message was refused. */
    NYA_Error (*send)(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size);

    /**
     * Drains one event. False when there are none left.
     *
     * Also where a transport does its own work — retransmits, timeouts, fragment reassembly — so it
     * must be called every frame even when nothing is expected.
     * */
    b8 (*poll)(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event);

    /** Drops a peer, sending a disconnect where the transport has a way to. */
    void (*disconnect)(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason);

    NYA_NetPeerStats (*stats)(NYA_NetTransport* transport, NYA_NetPeerId peer);

    /** Human readable, for a server browser or a log line. */
    NYA_ConstCString (*peer_address)(NYA_NetTransport* transport, NYA_NetPeerId peer);

    /**
     * Drops `percent` of outgoing datagrams on purpose. Null for a transport that cannot lie.
     *
     * For tests and for a "bad connection" developer toggle. The reliability layer's whole job is
     * only exercised by loss, and a loopback interface never provides any.
     * */
    void (*simulate_packet_loss)(NYA_NetTransport* transport, u32 percent);

    void (*destroy)(NYA_NetTransport* transport);
};

/**
 * One transport instance.
 *
 * The vtable plus an implementation-owned pointer, rather than an inheritance hierarchy: there are
 * three of these and they share no state, so a base struct would be a base struct with nothing in it.
 * */
struct NYA_NetTransport {
    const NYA_NetTransportVTable* vtable;

    NYA_Arena* allocator;

    /** The implementation's own state. Meaningless to everything above this file. */
    void* state;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * CONSTRUCTION
 * ─────────────────────────────────────────────────────────
 */

/**
 * A transport that carries messages between two endpoints in the same process.
 *
 * Returns the pair already joined: `out_a` and `out_b` are each other's only peer, and each sees the
 * other as peer index zero. That is what a listen server's local client is — the server holds one
 * end and the client the other, and neither knows the difference from a socket.
 *
 * Nothing is serialised twice and nothing is copied: a send hands the receiving end a pointer into
 * the sender's arena. This is the whole reason single player costs what it costs.
 * */
NYA_API NYA_Error nya_net_transport_loopback_create(NYA_Arena* arena, OUT NYA_NetTransport** out_a, OUT NYA_NetTransport** out_b) __attr_no_discard;

/**
 * A transport over UDP datagrams, with reliability and fragmentation on top. See net_udp.c.
 *
 * Created unbound: call listen or connect. SDL_net is brought up on first use and taken down with
 * the last transport, so nothing else has to know it exists.
 * */
NYA_API NYA_Error nya_net_transport_udp_create(NYA_Arena* arena, OUT NYA_NetTransport** out_transport) __attr_no_discard;

/**
 * A transport over Steam's relayed peer-to-peer sockets.
 *
 * NYA_ERROR_NOT_SUPPORTED when the Steam plugin is not compiled in, which is the default — see
 * plugins/steam/steam.h. Presently a stub even when it is; the interface exists so the server and
 * the client are already written against it.
 * */
NYA_API NYA_Error nya_net_transport_steam_create(NYA_Arena* arena, OUT NYA_NetTransport** out_transport) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * OPERATIONS
 * ─────────────────────────────────────────────────────────
 */

/*
 * Thin forwarders over the vtable. They exist so a caller writes nya_net_transport_send rather than
 * transport->vtable->send(transport, ...), and so a null vtable entry is one assertion here instead
 * of a fault at every call site.
 */

NYA_API NYA_Error nya_net_transport_listen(NYA_NetTransport* transport, u16 port) __attr_no_discard;
NYA_API NYA_Error nya_net_transport_connect(NYA_NetTransport* transport, NYA_ConstCString address, u16 port) __attr_no_discard;
NYA_API NYA_Error nya_net_transport_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size);
NYA_API b8        nya_net_transport_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event);
NYA_API void      nya_net_transport_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason);
NYA_API NYA_NetPeerStats nya_net_transport_stats(NYA_NetTransport* transport, NYA_NetPeerId peer) __attr_no_discard;
NYA_API NYA_ConstCString nya_net_transport_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer) __attr_no_discard;
NYA_API void      nya_net_transport_destroy(NYA_NetTransport* transport);

/**
 * Deliberately drops `percent` of outgoing datagrams, 0..100.
 *
 * Does nothing on a transport with no wire to lose packets on, which is the loopback pair. Not a
 * debug-only function: shipping it means a developer can reproduce a player's bad connection without
 * a special build, and the reliability layer is only ever exercised by loss.
 * */
NYA_API void nya_net_simulate_packet_loss(NYA_NetTransport* transport, u32 percent);

/**
 * Whether this transport's peers are in the same process.
 *
 * What lets the client skip prediction entirely on a listen server: there is no latency to hide, so
 * predicting and reconciling would be pure cost for an answer that is already exact.
 * */
NYA_API b8 nya_net_transport_is_local(const NYA_NetTransport* transport) __attr_no_discard;

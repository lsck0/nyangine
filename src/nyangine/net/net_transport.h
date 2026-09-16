/**
 * @file net_transport.h
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
 * */
struct NYA_NetTransportEvent {
    NYA_NetTransportEventKind kind;
    NYA_NetPeerId             peer;

    /**
     * The message, for a MESSAGE event.
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
     * */
    b8 (*poll)(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event);

    /** Drops a peer, sending a disconnect where the transport has a way to. */
    void (*disconnect)(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason);

    NYA_NetPeerStats (*stats)(NYA_NetTransport* transport, NYA_NetPeerId peer);

    /** Human readable, for a server browser or a log line. */
    NYA_ConstCString (*peer_address)(NYA_NetTransport* transport, NYA_NetPeerId peer);

    /**
     * Drops `percent` of outgoing datagrams on purpose. Null for a transport that cannot lie.
     * */
    void (*simulate_packet_loss)(NYA_NetTransport* transport, u32 percent);

    void (*destroy)(NYA_NetTransport* transport);
};

/**
 * One transport instance.
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
 * */
NYA_API NYA_Error nya_net_transport_loopback_create(NYA_Arena* arena, OUT NYA_NetTransport** out_a, OUT NYA_NetTransport** out_b) __attr_no_discard;

/**
 * A transport over UDP datagrams, with reliability and fragmentation on top. See net_udp.c.
 * */
NYA_API NYA_Error nya_net_transport_udp_create(NYA_Arena* arena, OUT NYA_NetTransport** out_transport) __attr_no_discard;

/**
 * A transport over Steam's relayed peer-to-peer sockets.
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
NYA_API NYA_Error nya_net_transport_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size) __attr_no_discard;
NYA_API b8        nya_net_transport_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event);
NYA_API void      nya_net_transport_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason);
NYA_API NYA_NetPeerStats nya_net_transport_stats(NYA_NetTransport* transport, NYA_NetPeerId peer) __attr_no_discard;
NYA_API NYA_ConstCString nya_net_transport_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer) __attr_no_discard;
NYA_API void      nya_net_transport_destroy(NYA_NetTransport* transport);

/**
 * Deliberately drops `percent` of outgoing datagrams, 0..100.
 * */
NYA_API void nya_net_simulate_packet_loss(NYA_NetTransport* transport, u32 percent);

/**
 * Whether this transport's peers are in the same process.
 * */
NYA_API b8 nya_net_transport_is_local(const NYA_NetTransport* transport) __attr_no_discard;

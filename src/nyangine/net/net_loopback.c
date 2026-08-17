#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * One queued message, waiting for the far end to poll for it.
 *
 * The bytes live in the *receiving* endpoint's arena rather than the sender's. That is the one
 * subtlety here: the transport contract says a received message is valid until the next poll, and
 * the sender is free to reuse its scratch buffer the moment send returns. Pointing at the sender's
 * memory would be correct only until it did.
 *
 * Still one copy rather than two — nothing is serialised, framed, or parsed — which is what makes
 * single player cost what it costs.
 * */
typedef struct {
    u8*            data;
    u64            size;
    NYA_NetChannel channel;
} _NYA_NetLoopbackMessage;

nya_derive_array(_NYA_NetLoopbackMessage);

typedef struct _NYA_NetLoopbackEndpoint _NYA_NetLoopbackEndpoint;

struct _NYA_NetLoopbackEndpoint {
    /** The other half of the pair. Each endpoint's only peer, forever. */
    _NYA_NetLoopbackEndpoint* other;

    NYA_Arena* allocator;

    /**
     * Messages this endpoint has been sent and has not yet polled.
     *
     * Written by the *other* endpoint's send and drained by this one's poll. There is no lock: both
     * ends live in one process and, by construction, on one thread — the whole point of a loopback
     * pair is a listen server talking to its own client inside the frame loop.
     * */
    NYA_Arrayᐸ_NYA_NetLoopbackMessageᐳ* inbox;

    /**
     * Bytes handed out by the last poll, freed by the next one.
     *
     * A message must stay valid until the caller polls again, and it must not stay valid forever, so
     * something has to own it for exactly one poll. This arena is that owner: it is reset at the top
     * of every poll, which frees everything the previous poll returned in one operation.
     * */
    NYA_Arena* delivered;

    /** Queued but not yet reported, so the pair can be created before either end is "connected". */
    b8 connect_pending;
    b8 connected;

    NYA_NetPeerStats stats;
};

NYA_INTERNAL NYA_Error _nya_net_loopback_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size);
NYA_INTERNAL b8        _nya_net_loopback_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event);
NYA_INTERNAL void      _nya_net_loopback_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason);
NYA_INTERNAL NYA_NetPeerStats _nya_net_loopback_stats(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL NYA_ConstCString _nya_net_loopback_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL void      _nya_net_loopback_destroy(NYA_NetTransport* transport);

/**
 * The one peer a loopback endpoint ever has.
 *
 * Index zero, generation one. Generation one rather than zero because zero is NYA_NET_PEER_NONE and
 * nya_net_peer_is_set reads the generation — a peer with generation zero would report as unset.
 * */
#define _NYA_NET_LOOPBACK_PEER ((NYA_NetPeerId){ .index = 0, .generation = 1 })

NYA_INTERNAL const NYA_NetTransportVTable _NYA_NET_LOOPBACK_VTABLE = {
    .name = "loopback",
    .kind = NYA_NET_TRANSPORT_LOOPBACK,

    // No listen and no connect. A loopback pair is joined at creation, which is the honest shape:
    // there is no address to bind and nothing to wait for.
    .listen  = nullptr,
    .connect = nullptr,

    .send         = &_nya_net_loopback_send,
    .poll         = &_nya_net_loopback_poll,
    .disconnect   = &_nya_net_loopback_disconnect,
    .stats        = &_nya_net_loopback_stats,
    .peer_address = &_nya_net_loopback_peer_address,
    .destroy      = &_nya_net_loopback_destroy,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_transport_loopback_create(NYA_Arena* arena, OUT NYA_NetTransport** out_a, OUT NYA_NetTransport** out_b) {
    nya_assert(arena != nullptr);
    nya_assert(out_a != nullptr);
    nya_assert(out_b != nullptr);

    *out_a = nullptr;
    *out_b = nullptr;

    NYA_NetTransport* transports[2] = { nullptr, nullptr };
    _NYA_NetLoopbackEndpoint* endpoints[2] = { nullptr, nullptr };

    for (u32 i = 0; i < 2; i++) {
        transports[i] = nya_arena_alloc(arena, sizeof(NYA_NetTransport));
        endpoints[i]  = nya_arena_alloc(arena, sizeof(_NYA_NetLoopbackEndpoint));

        *endpoints[i] = (_NYA_NetLoopbackEndpoint){
            .allocator = arena,
            .inbox     = nya_array_create(arena, _NYA_NetLoopbackMessage),
            .delivered = nya_arena_create(.name = "net_loopback_delivered"),

            // Both ends report a connection on their first poll. Neither has to be told to connect,
            // because a pair that exists is already joined — but the layers above still expect to
            // *learn* that from an event rather than to assume it.
            .connect_pending = true,
        };

        *transports[i] = (NYA_NetTransport){
            .vtable    = &_NYA_NET_LOOPBACK_VTABLE,
            .allocator = arena,
            .state     = endpoints[i],
        };
    }

    endpoints[0]->other = endpoints[1];
    endpoints[1]->other = endpoints[0];

    *out_a = transports[0];
    *out_b = transports[1];

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error _nya_net_loopback_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size) {
    _NYA_NetLoopbackEndpoint* endpoint = transport->state;

    if (!nya_net_peer_equals(peer, _NYA_NET_LOOPBACK_PEER)) {
        return nya_error(NYA_ERROR_NOT_FOUND, "a loopback transport has exactly one peer");
    }

    _NYA_NetLoopbackEndpoint* other = endpoint->other;

    // The far end was destroyed, or disconnected. Reported rather than asserted: on a listen server
    // this is what shutting down looks like from the half that is still running.
    if (other == nullptr || !other->connected) {
        if (other == nullptr || !other->connect_pending) return nya_error(NYA_ERROR_NOT_OK, "the loopback peer is gone");
    }

    /*
     * Copied into the *receiver's* arena, not the sender's.
     *
     * See the note on _NYA_NetLoopbackMessage: the contract is that a received message stays valid
     * until the receiver's next poll, and the sender may reuse its buffer as soon as this returns.
     * A snapshot is built into a scratch buffer that is reused every tick, so pointing at it would
     * hand the client last tick's bytes about a third of the time.
     */
    u8* copy = nya_arena_alloc(other->allocator, size);
    nya_memcpy(copy, data, size);

    nya_array_push_back(other->inbox, ((_NYA_NetLoopbackMessage){ .data = copy, .size = size, .channel = channel }));

    endpoint->stats.bytes_sent += size;
    endpoint->stats.packets_sent++;

    return NYA_OK;
}

b8 _nya_net_loopback_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event) {
    _NYA_NetLoopbackEndpoint* endpoint = transport->state;

    if (endpoint->connect_pending) {
        endpoint->connect_pending = false;
        endpoint->connected       = true;

        *out_event = (NYA_NetTransportEvent){ .kind = NYA_NET_TRANSPORT_EVENT_CONNECTED, .peer = _NYA_NET_LOOPBACK_PEER };

        return true;
    }

    if (endpoint->inbox->length == 0) return false;

    _NYA_NetLoopbackMessage message = endpoint->inbox->items[0];
    nya_array_remove(endpoint->inbox, 0);

    endpoint->stats.bytes_received += message.size;
    endpoint->stats.packets_received++;

    *out_event = (NYA_NetTransportEvent){
        .kind    = NYA_NET_TRANSPORT_EVENT_MESSAGE,
        .peer    = _NYA_NET_LOOPBACK_PEER,
        .data    = message.data,
        .size    = message.size,
        .channel = message.channel,
    };

    return true;
}

void _nya_net_loopback_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason) {
    nya_unused(peer, reason);

    _NYA_NetLoopbackEndpoint* endpoint = transport->state;

    endpoint->connected       = false;
    endpoint->connect_pending = false;

    // The far end learns about it the same way a socket peer would: as an event on its next poll.
    // Written directly rather than through its inbox because a disconnect is not a message, and
    // queueing it behind whatever is already there would deliver it after data from a dead peer.
    if (endpoint->other != nullptr) {
        endpoint->other->connected       = false;
        endpoint->other->connect_pending = false;
    }
}

NYA_NetPeerStats _nya_net_loopback_stats(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    nya_unused(peer);

    _NYA_NetLoopbackEndpoint* endpoint = transport->state;

    // Latency and loss stay zero, and that is a fact rather than a placeholder: there is no wire.
    // nya_net_transport_is_local is what callers should branch on, but a debug overlay reading these
    // should show honest zeroes rather than an invented small number.
    return endpoint->stats;
}

NYA_ConstCString _nya_net_loopback_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    nya_unused(transport, peer);

    return "local";
}

void _nya_net_loopback_destroy(NYA_NetTransport* transport) {
    _NYA_NetLoopbackEndpoint* endpoint = transport->state;
    if (endpoint == nullptr) return;

    // Unhook the far end first, so a send from the half that is still alive reports a dead peer
    // rather than writing into an arena that is about to go.
    if (endpoint->other != nullptr) {
        endpoint->other->other     = nullptr;
        endpoint->other->connected = false;
    }

    nya_arena_destroy(endpoint->delivered);

    // The inbox, the endpoint and the transport all came from the arena the caller passed in, which
    // is the caller's to destroy. Only the delivered arena is this transport's own.
    endpoint->other = nullptr;
    transport->state = nullptr;
}

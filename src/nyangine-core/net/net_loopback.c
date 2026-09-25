#include "nyangine-core/nyangine.h"

// PRIVATE API DECLARATION

/** One queued message, waiting for the far end to poll for it. */
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

    /** Messages this endpoint has been sent and has not yet polled. */
    NYA_Arrayᐸ_NYA_NetLoopbackMessageᐳ* inbox;

    /** Bytes handed out by the last poll, freed by the next one. */
    NYA_Arena* delivered;

    /** Queued but not yet reported, so the pair can be created before either end is "connected". */
    b8 connect_pending;
    b8 connected;

    /** The far end let go, and why. Reported once the messages it sent before that have been polled. */
    b8                disconnect_pending;
    NYA_NetDisconnect disconnect_reason;

    NYA_NetPeerStats stats;
};

NYA_INTERNAL NYA_Error _nya_net_loopback_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size);
NYA_INTERNAL b8        _nya_net_loopback_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event);
NYA_INTERNAL void      _nya_net_loopback_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason);
NYA_INTERNAL NYA_NetPeerStats _nya_net_loopback_stats(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL NYA_ConstCString _nya_net_loopback_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL void      _nya_net_loopback_destroy(NYA_NetTransport* transport);

/** The one peer a loopback endpoint ever has. */
#define _NYA_NET_LOOPBACK_PEER ((NYA_NetPeerId){ .index = 0, .generation = 1 })

NYA_INTERNAL const NYA_NetTransportVTable _NYA_NET_LOOPBACK_VTABLE = {
    .name = "loopback",
    .kind = NYA_NET_TRANSPORT_LOOPBACK,

    // No listen and no connect: a loopback pair is joined at creation, with no address to bind.
    .listen  = nullptr,
    .connect = nullptr,

    .send         = &_nya_net_loopback_send,
    .poll         = &_nya_net_loopback_poll,
    .disconnect   = &_nya_net_loopback_disconnect,
    .stats        = &_nya_net_loopback_stats,
    .peer_address = &_nya_net_loopback_peer_address,
    .destroy      = &_nya_net_loopback_destroy,
};

// PUBLIC API IMPLEMENTATION

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

            // both ends report a connection on their first poll, since the layers above learn it from an event.
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

// PRIVATE API IMPLEMENTATION

NYA_Error _nya_net_loopback_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size) {
    _NYA_NetLoopbackEndpoint* endpoint = transport->state;

    if (!nya_net_peer_equals(peer, _NYA_NET_LOOPBACK_PEER)) {
        return nya_error(NYA_ERROR_NOT_FOUND, "a loopback transport has exactly one peer");
    }

    _NYA_NetLoopbackEndpoint* other = endpoint->other;

    // The far end was destroyed or disconnected; reported, not asserted (a listen server shutting down).
    if (other == nullptr || !other->connected) {
        if (other == nullptr || !other->connect_pending) return nya_error(NYA_ERROR_NOT_OK, "the loopback peer is gone");
    }

    // Copied into the *receiver's* arena, not the sender's.
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

    // what the far end sent before it let go comes first, as it would off a wire, and then the disconnect.
    if (endpoint->inbox->length == 0) {
        if (!endpoint->disconnect_pending) return false;

        endpoint->disconnect_pending = false;
        *out_event = (NYA_NetTransportEvent){ .kind = NYA_NET_TRANSPORT_EVENT_DISCONNECTED, .peer = _NYA_NET_LOOPBACK_PEER, .reason = endpoint->disconnect_reason };

        return true;
    }

    _NYA_NetLoopbackMessage message = endpoint->inbox->items[0];
    nya_array_remove(endpoint->inbox, 0);

    // the previous poll's bytes are done with; this poll's move out of the long-lived arena the send copied into.
    nya_arena_free_all(endpoint->delivered);

    if (message.size > 0) {
        u8* delivered = nya_arena_alloc(endpoint->delivered, message.size);
        nya_memcpy(delivered, message.data, message.size);
        nya_arena_free(endpoint->allocator, message.data, message.size);
        message.data = delivered;
    }

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
    nya_unused(peer);
    nya_assert((u32)reason < NYA_NET_DISCONNECT_COUNT);

    _NYA_NetLoopbackEndpoint* endpoint = transport->state;

    endpoint->connected       = false;
    endpoint->connect_pending = false;

    // the far end learns it as a socket peer would: a DISCONNECTED event with the reason, after what was already sent.
    if (endpoint->other != nullptr && (endpoint->other->connected || endpoint->other->connect_pending)) {
        endpoint->other->connected          = false;
        endpoint->other->connect_pending    = false;
        endpoint->other->disconnect_pending = true;
        endpoint->other->disconnect_reason  = reason;
    }
}

NYA_NetPeerStats _nya_net_loopback_stats(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    nya_unused(peer);

    _NYA_NetLoopbackEndpoint* endpoint = transport->state;

    // Latency and loss stay zero, a fact not a placeholder: there is no wire (branch on nya_net_transport_is_local).
    return endpoint->stats;
}

NYA_ConstCString _nya_net_loopback_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    nya_unused(transport, peer);

    return "local";
}

void _nya_net_loopback_destroy(NYA_NetTransport* transport) {
    _NYA_NetLoopbackEndpoint* endpoint = transport->state;
    if (endpoint == nullptr) return;

    // Unhook the far end first, so a send from the surviving half reports a dead peer, not a freed arena.
    if (endpoint->other != nullptr) {
        endpoint->other->other     = nullptr;
        endpoint->other->connected = false;
    }

    nya_arena_destroy(endpoint->delivered);

    // The inbox, endpoint and transport are the caller's arena to destroy; only the delivered arena is this transport's own.
    endpoint->other = nullptr;
    transport->state = nullptr;
}

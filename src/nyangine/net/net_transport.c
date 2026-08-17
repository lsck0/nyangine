#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_net_peer_equals(NYA_NetPeerId a, NYA_NetPeerId b) {
    return a.index == b.index && a.generation == b.generation;
}

b8 nya_net_peer_is_set(NYA_NetPeerId peer) {
    // The generation, not the index. Slot zero is a perfectly ordinary peer — on a listen server it
    // is the host — and only generation zero means "never assigned".
    return peer.generation != 0;
}

/*
 * ─────────────────────────────────────────────────────────
 * OPERATIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_net_transport_listen(NYA_NetTransport* transport, u16 port) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);

    // Not an assertion: "this transport cannot accept connections" is a real answer for the loopback
    // pair and for a Steam client socket, and a caller offering to open a game to the LAN should get
    // an error it can show rather than a panic.
    if (transport->vtable->listen == nullptr) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "the %s transport cannot listen", transport->vtable->name);
    }

    return transport->vtable->listen(transport, port);
}

NYA_Error nya_net_transport_connect(NYA_NetTransport* transport, NYA_ConstCString address, u16 port) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);

    if (transport->vtable->connect == nullptr) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "the %s transport cannot connect out", transport->vtable->name);
    }

    return transport->vtable->connect(transport, address, port);
}

NYA_Error nya_net_transport_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);
    nya_assert(transport->vtable->send != nullptr, "the %s transport has no send", transport->vtable->name);
    nya_assert(channel < NYA_NET_CHANNEL_COUNT);

    // A zero length message is a caller bug rather than a wire condition: every receiver here
    // switches on a message id in the first byte, so an empty payload has nowhere to carry one.
    if (data == nullptr || size == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an empty network message");

    return transport->vtable->send(transport, peer, channel, data, size);
}

b8 nya_net_transport_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);
    nya_assert(transport->vtable->poll != nullptr, "the %s transport has no poll", transport->vtable->name);
    nya_assert(out_event != nullptr);

    *out_event = (NYA_NetTransportEvent){ 0 };

    return transport->vtable->poll(transport, out_event);
}

void nya_net_transport_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);

    if (transport->vtable->disconnect == nullptr) return;

    transport->vtable->disconnect(transport, peer, reason);
}

NYA_NetPeerStats nya_net_transport_stats(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);

    if (transport->vtable->stats == nullptr) return (NYA_NetPeerStats){ 0 };

    return transport->vtable->stats(transport, peer);
}

NYA_ConstCString nya_net_transport_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);

    if (transport->vtable->peer_address == nullptr) return "(unknown)";

    return transport->vtable->peer_address(transport, peer);
}

void nya_net_transport_destroy(NYA_NetTransport* transport) {
    // Null tolerated: a teardown path runs over transports that may never have been created, and
    // making every one of those check first is how one gets missed.
    if (transport == nullptr) return;

    nya_assert(transport->vtable != nullptr);

    if (transport->vtable->destroy != nullptr) transport->vtable->destroy(transport);
}

void nya_net_simulate_packet_loss(NYA_NetTransport* transport, u32 percent) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);
    nya_assert(percent <= 100, "packet loss is a percentage, got %u", percent);

    // Silently nothing for a transport with no wire. A test that turns loss on for a loopback pair
    // is asking for something meaningless rather than something wrong.
    if (transport->vtable->simulate_packet_loss == nullptr) return;

    transport->vtable->simulate_packet_loss(transport, percent);
}

b8 nya_net_transport_is_local(const NYA_NetTransport* transport) {
    if (transport == nullptr) return false;
    nya_assert(transport->vtable != nullptr);

    return transport->vtable->kind == NYA_NET_TRANSPORT_LOOPBACK;
}

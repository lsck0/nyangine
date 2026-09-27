#include "nyangine-core/nyangine.h"

// PUBLIC API IMPLEMENTATION

b8 nya_net_peer_equals(NYA_NetPeerId a, NYA_NetPeerId b) {
    return a.index == b.index && a.generation == b.generation;
}

b8 nya_net_peer_is_set(NYA_NetPeerId peer) {
    // the generation, not the index: slot zero is an ordinary peer, only generation zero means never assigned.
    return peer.generation != 0;
}

// OPERATIONS

NYA_Error nya_net_transport_listen(NYA_NetTransport* transport, u16 port) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);

    // Not an assertion: "cannot accept connections" is a real answer (loopback, Steam client), so return an error, not a panic.
    if (transport->vtable->listen == nullptr) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "the %s transport cannot listen", transport->vtable->name);
    }

    return transport->vtable->listen(transport, port);
}

u16 nya_net_transport_port(NYA_NetTransport* transport) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);

    // zero, not an assertion: "no port" is the honest answer for a loopback pair and for Steam's relay.
    if (transport->vtable->port == nullptr) return 0;

    return transport->vtable->port(transport);
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

    // A zero length message is a caller bug: every receiver switches on a message id in the first byte.
    if (data == nullptr || size == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an empty network message");

    return transport->vtable->send(transport, peer, channel, data, size);
}

void nya_net_transport_flush(NYA_NetTransport* transport) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);

    if (transport->vtable->flush != nullptr) transport->vtable->flush(transport);
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
    // Null tolerated: a teardown path runs over transports that may never have been created.
    if (transport == nullptr) return;

    nya_assert(transport->vtable != nullptr);

    if (transport->vtable->destroy != nullptr) transport->vtable->destroy(transport);
}

void nya_net_transport_condition(NYA_NetTransport* transport, NYA_NetConditions conditions) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);
    nya_assert(conditions.loss_percent >= 0.0F && conditions.loss_percent <= 100.0F, "loss is a percentage, got %f", (f64)conditions.loss_percent);
    nya_assert(conditions.duplicate_percent >= 0.0F && conditions.duplicate_percent <= 100.0F);
    nya_assert(conditions.reorder_percent >= 0.0F && conditions.reorder_percent <= 100.0F);

    // silently nothing for a transport with no wire: a test degrading a loopback pair asks for something meaningless, not wrong.
    if (transport->vtable->condition == nullptr) return;

    transport->vtable->condition(transport, conditions);
}

b8 nya_net_conditions_active(NYA_NetConditions conditions) {
    return conditions.latency_ms > 0 || conditions.jitter_ms > 0 || conditions.loss_percent > 0.0F || conditions.duplicate_percent > 0.0F
        || conditions.reorder_percent > 0.0F;
}

const u8* nya_net_transport_public_key(NYA_NetTransport* transport) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);

    if (transport->vtable->public_key == nullptr) return nullptr;

    return transport->vtable->public_key(transport);
}

const u8* nya_net_transport_peer_key(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    nya_assert(transport != nullptr);
    nya_assert(transport->vtable != nullptr);

    if (transport->vtable->peer_key == nullptr) return nullptr;

    return transport->vtable->peer_key(transport, peer);
}

b8 nya_net_transport_is_local(const NYA_NetTransport* transport) {
    if (transport == nullptr) return false;
    nya_assert(transport->vtable != nullptr);

    return transport->vtable->kind == NYA_NET_TRANSPORT_LOOPBACK;
}

// ALLOWLIST

/** The fullest any allowlist has been. NYA_NET_ALLOWLIST_MAX is per list, so the ceiling registry watches the fullest. */
NYA_INTERNAL u32 _nya_net_allowlist_count_worst = 0;

NYA_Error nya_net_allowlist_add(NYA_NetAllowlist* allowlist, const u8 key[NYA_NET_KEY_SIZE]) {
    nya_assert(allowlist != nullptr);
    nya_assert(key != nullptr);
    nya_assert(allowlist->count <= NYA_NET_ALLOWLIST_MAX);

    if (!nya_net_key_is_set(key)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an all-zero player key");

    allowlist->closed = true;

    if (nya_net_allowlist_contains(allowlist, key)) return NYA_OK;
    if (allowlist->count == NYA_NET_ALLOWLIST_MAX) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the allowlist holds at most %d keys", NYA_NET_ALLOWLIST_MAX);

    nya_memcpy(allowlist->keys[allowlist->count], key, NYA_NET_KEY_SIZE);
    allowlist->count++;

    if (allowlist->count > _nya_net_allowlist_count_worst) _nya_net_allowlist_count_worst = allowlist->count;

    // registered once, on the first key any list is given.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("net_allowlist", NYA_NET_ALLOWLIST_MAX, &_nya_net_allowlist_count_worst);
        ceiling_registered = true;
    }

    return NYA_OK;
}

void nya_net_allowlist_remove(NYA_NetAllowlist* allowlist, const u8 key[NYA_NET_KEY_SIZE]) {
    nya_assert(allowlist != nullptr);
    nya_assert(key != nullptr);
    nya_assert(allowlist->count <= NYA_NET_ALLOWLIST_MAX);

    for (u32 i = 0; i < allowlist->count; i++) {
        if (nya_memcmp(allowlist->keys[i], key, NYA_NET_KEY_SIZE) != 0) continue;

        // the last entry fills the hole, so the list stays dense and a scan stops at count.
        allowlist->count--;
        if (i != allowlist->count) nya_memcpy(allowlist->keys[i], allowlist->keys[allowlist->count], NYA_NET_KEY_SIZE);
        nya_memset(allowlist->keys[allowlist->count], 0, NYA_NET_KEY_SIZE);

        nya_assert(!nya_net_allowlist_contains(allowlist, key), "a key was listed twice");
        return;
    }
}

b8 nya_net_allowlist_contains(const NYA_NetAllowlist* allowlist, const u8 key[NYA_NET_KEY_SIZE]) {
    nya_assert(allowlist != nullptr);
    nya_assert(key != nullptr);
    nya_assert(allowlist->count <= NYA_NET_ALLOWLIST_MAX);

    for (u32 i = 0; i < allowlist->count; i++) {
        if (nya_memcmp(allowlist->keys[i], key, NYA_NET_KEY_SIZE) == 0) return true;
    }

    return false;
}

b8 nya_net_allowlist_admits(const NYA_NetAllowlist* allowlist, const u8* key) {
    nya_assert(allowlist != nullptr);

    if (!allowlist->closed) return true;

    // a keyless peer has nothing to be listed under.
    return key != nullptr && nya_net_key_is_set(key) && nya_net_allowlist_contains(allowlist, key);
}

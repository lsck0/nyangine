#include "nyangine/nyangine.h"

#include "nyangine/net/net_bytes.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How far a prediction may be wrong before it is corrected, when the config does not say.
 *
 * Not zero. The server and the client run the same movement function in floating point, possibly on
 * different hardware with different rounding of the same expression, so tiny disagreements are
 * constant. Correcting for them would replay every command every tick for no visible benefit.
 *
 * A hundredth of a world unit: far below anything a player can see, far above float noise.
 * */
#define _NYA_NET_CLIENT_DEFAULT_THRESHOLD 0.01F

typedef struct {
    b8 active;

    NYA_NetClientState state;
    NYA_NetDisconnect  disconnect_reason;

    NYA_Arena* allocator;

    /** Reset every tick. Holds the decoded snapshot and the outgoing payloads. */
    NYA_Arena* tick_arena;

    NYA_NetClientConfig config;

    NYA_NetTransport* transport;

    /** True when this client created the transport and must therefore destroy it. */
    b8 owns_transport;

    char name[NYA_NET_MAX_NAME];

    NYA_NetPeerId peer;

    /**
     * What this client controls, as the *server* names it. What WELCOME carried.
     *
     * The only name the two processes agree on, and therefore what a snapshot has to be matched
     * against. Not usable with nya_entity_get: it indexes the server's table, not this one.
     * */
    NYA_EntityHandle entity_remote;

    /**
     * The same entity, as *this* process names it.
     *
     * Resolved through the replica map once the first snapshot has spawned it, so it is
     * NYA_ENTITY_HANDLE_NONE for a tick or two after WELCOME. On a listen server the two handles are
     * the same, because there is one table.
     * */
    NYA_EntityHandle entity_local;

    /**
     * Which local entity stands for which server entity. See NYA_NetReplicaMap.
     *
     * Unused on a listen server, where the client shares the server's world and applies no snapshots
     * at all.
     * */
    NYA_NetReplicaMap replicas;

    /** The transport peer the server is, from this client's side. Always index zero, one peer. */
    NYA_NetPeerId server_peer;

    /*
     * ── snapshots ──
     */

    /** The newest snapshot applied, kept as the baseline the next delta is decoded against. */
    NYA_NetSnapshot baseline;
    b8              has_baseline;

    /** Which arena `baseline` lives in. Swapped between two, so decoding does not free what it reads. */
    NYA_Arena* baseline_arena;
    NYA_Arena* baseline_spare;

    u64 server_tick;

    /*
     * ── prediction ──
     */

    /**
     * Commands sent but not yet confirmed, by tick.
     *
     * A ring indexed by `tick % NYA_NET_COMMAND_HISTORY`, so finding the command for a tick is a
     * modulo. Replayed after a correction, oldest first.
     * */
    NYA_NetCommand history[NYA_NET_COMMAND_HISTORY];

    u64 local_tick;

    u64 correction_count;
} _NYA_NetClientState;

/**
 * The one client, as a file scope static. Same reasoning as the server's.
 * */
NYA_INTERNAL _NYA_NetClientState _NYA_NET_CLIENT = { 0 };

NYA_INTERNAL void _nya_net_client_drain(f32 delta_time_s);
NYA_INTERNAL void _nya_net_client_handle_message(const u8* data, u64 size, f32 delta_time_s);
NYA_INTERNAL void _nya_net_client_handle_welcome(const u8* body, u64 size);
NYA_INTERNAL void _nya_net_client_handle_snapshot(const u8* body, u64 size, f32 delta_time_s);

/** Sends HELLO. Called once the transport reports a connection. */
NYA_INTERNAL void _nya_net_client_send_hello(void);

/** Samples input, predicts locally and sends the command run. */
NYA_INTERNAL void _nya_net_client_send_command(u64 tick, f32 delta_time_s);

/**
 * Compares the server's answer for the predicted entity against what was predicted, and replays if
 * they differ. See the long note at the definition.
 * */
NYA_INTERNAL void _nya_net_client_reconcile(const NYA_NetSnapshot* snapshot, f32 delta_time_s);

NYA_INTERNAL void _nya_net_client_reset(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_client_connect(NYA_ConstCString address, u16 port, NYA_ConstCString name, NYA_NetClientConfig config) {
    if (_NYA_NET_CLIENT.active) return nya_error(NYA_ERROR_NOT_OK, "a client is already connected");
    if (address == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no address to connect to");

    NYA_Arena* allocator = nya_arena_create(.name = "net_client");

    NYA_NetTransport* transport = nullptr;

    NYA_Error created = nya_net_transport_udp_create(allocator, &transport);
    if (!created.ok) {
        nya_arena_destroy(allocator);
        return created;
    }

    NYA_Error connecting = nya_net_transport_connect(transport, address, port);
    if (!connecting.ok) {
        nya_net_transport_destroy(transport);
        nya_arena_destroy(allocator);
        return connecting;
    }

    NYA_Error attached = nya_net_client_attach(transport, name, config);
    if (!attached.ok) {
        nya_net_transport_destroy(transport);
        nya_arena_destroy(allocator);
        return attached;
    }

    // Attach made its own allocator; this one existed only to hold the transport while it was being
    // set up, and the transport itself was allocated from it.
    _NYA_NET_CLIENT.owns_transport = true;

    return NYA_OK;
}

NYA_Error nya_net_client_attach(NYA_NetTransport* transport, NYA_ConstCString name, NYA_NetClientConfig config) {
    if (_NYA_NET_CLIENT.active) return nya_error(NYA_ERROR_NOT_OK, "a client is already connected");
    if (transport == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no transport");
    // Zero rather than nullptr: these are nya_callback handles now, and an unset one is a zero handle.
    // What cannot be checked here is whether the handle *resolves* — under hot reload it is looked up
    // per call, so a name that no longer exists in the DLL is a null at the call site rather than here.
    if (config.on_sample_command == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a client needs on_sample_command");
    if (config.on_apply_command == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a client needs on_apply_command");

    if (config.correction_threshold <= 0.0F) config.correction_threshold = _NYA_NET_CLIENT_DEFAULT_THRESHOLD;

    _NYA_NET_CLIENT = (_NYA_NetClientState){
        .active    = true,
        .state     = NYA_NET_CLIENT_CONNECTING,
        .config    = config,
        .transport = transport,
        .entity_remote = NYA_ENTITY_HANDLE_NONE,
        .entity_local  = NYA_ENTITY_HANDLE_NONE,
        .peer          = NYA_NET_PEER_NONE,

        .allocator  = nya_arena_create(.name = "net_client"),
        .tick_arena = nya_arena_create(.name = "net_client_tick"),

        /*
         * Two arenas for the baseline, used alternately.
         *
         * Decoding a delta *reads* the current baseline and *produces* the next one. With one arena
         * the reset that frees the old would invalidate what the decode is reading. Alternating means
         * the previous baseline stays intact for exactly as long as it is needed.
         */
        .baseline_arena = nya_arena_create(.name = "net_client_baseline_a"),
        .baseline_spare = nya_arena_create(.name = "net_client_baseline_b"),
    };

    (void)snprintf(_NYA_NET_CLIENT.name, sizeof(_NYA_NET_CLIENT.name), "%s", name != nullptr ? name : "player");

    return NYA_OK;
}

void nya_net_client_disconnect(void) {
    if (!_NYA_NET_CLIENT.active) return;

    if (_NYA_NET_CLIENT.transport != nullptr && nya_net_peer_is_set(_NYA_NET_CLIENT.server_peer)) {
        nya_net_transport_disconnect(_NYA_NET_CLIENT.transport, _NYA_NET_CLIENT.server_peer, NYA_NET_DISCONNECT_REQUESTED);
    }

    // Only if this client created it. A listen server's loopback end belongs to the server, which
    // destroys it — freeing it here would leave the server holding a dangling pointer.
    if (_NYA_NET_CLIENT.owns_transport) nya_net_transport_destroy(_NYA_NET_CLIENT.transport);

    _nya_net_client_reset();
}

NYA_NetClientState nya_net_client_state(void) {
    return _NYA_NET_CLIENT.state;
}

NYA_NetDisconnect nya_net_client_disconnect_reason(void) {
    return _NYA_NET_CLIENT.disconnect_reason;
}

void nya_net_client_tick(u64 tick, f32 delta_time_s) {
    if (!_NYA_NET_CLIENT.active) return;

    nya_arena_free_all(_NYA_NET_CLIENT.tick_arena);

    _NYA_NET_CLIENT.local_tick = tick;

    _nya_net_client_drain(delta_time_s);

    if (_NYA_NET_CLIENT.state == NYA_NET_CLIENT_PLAYING) _nya_net_client_send_command(tick, delta_time_s);
}

NYA_EntityHandle nya_net_client_entity(void) {
    /*
     * The *local* handle, because that is the one a caller can do anything with.
     *
     * The server's handle is on the wire and is what snapshots are matched against, but handing it to
     * nya_entity_get would index this process's table with the server's number and return whatever
     * happened to be in that slot. See nya_net_client_entity_remote.
     */
    return _NYA_NET_CLIENT.entity_local;
}

NYA_EntityHandle nya_net_client_entity_remote(void) {
    return _NYA_NET_CLIENT.entity_remote;
}

NYA_EntityHandle nya_net_client_local_entity(NYA_EntityHandle remote) {
    // On a listen server there is one table and one world, so a server handle is already local.
    if (nya_net_transport_is_local(_NYA_NET_CLIENT.transport)) return remote;

    return nya_net_replica_local(&_NYA_NET_CLIENT.replicas, remote);
}

NYA_NetPeerId nya_net_client_peer(void) {
    return _NYA_NET_CLIENT.peer;
}

NYA_NetPeerStats nya_net_client_stats(void) {
    if (!_NYA_NET_CLIENT.active || _NYA_NET_CLIENT.transport == nullptr) return (NYA_NetPeerStats){ 0 };

    return nya_net_transport_stats(_NYA_NET_CLIENT.transport, _NYA_NET_CLIENT.server_peer);
}

u64 nya_net_client_server_tick(void) {
    return _NYA_NET_CLIENT.server_tick;
}

u64 nya_net_client_correction_count(void) {
    return _NYA_NET_CLIENT.correction_count;
}

NYA_Error nya_net_client_send_event(const NYA_Object* event) {
    if (_NYA_NET_CLIENT.state != NYA_NET_CLIENT_PLAYING) return nya_error(NYA_ERROR_NOT_OK, "not in a game");
    if (event == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no event to send");

    NYA_Arena* scratch = nya_arena_create(.name = "net_client_event");
    defer      nya_arena_destroy(scratch);

    NYA_String* payload = nya_string_create(scratch);

    nya_net_message_begin(payload, NYA_NET_MSG_GAME_EVENT);
    NYA_TRY(nya_net_message_write_object(scratch, payload, event));

    return nya_net_transport_send(_NYA_NET_CLIENT.transport, _NYA_NET_CLIENT.server_peer, NYA_NET_CHANNEL_RELIABLE, payload->items, payload->length);
}

void nya_net_client_interpolate(f32 delta_time_s) {
    if (!_NYA_NET_CLIENT.active) return;

    // Nothing to smooth on a listen server: no snapshots were applied, so the map is empty and the
    // entities are the server's own.
    if (nya_net_transport_is_local(_NYA_NET_CLIENT.transport)) return;

    /*
     * The interval is measured rather than configured.
     *
     * The server's snapshot rate is its own business and may change — it is a field on
     * NYA_NetServerConfig and nothing sends it over. What the client *can* see is how far apart the
     * ticks of the last two snapshots it received were, which is the same number and is correct even if
     * the server changes its mind.
     */
    u64 tick_gap = 1;

    for (u32 i = 0; i < _NYA_NET_CLIENT.replicas.count; i++) {
        const NYA_NetReplica* replica = &_NYA_NET_CLIENT.replicas.entries[i];

        if (!replica->can_interpolate) continue;
        if (replica->to_tick <= replica->from_tick) continue;

        tick_gap = replica->to_tick - replica->from_tick;
        break;
    }

    /*
     * The tick length, from the app rather than assumed.
     *
     * Zero before the app is up, which a test can reach — treated as no smoothing rather than as a
     * division by zero.
     */
    f64 tick_seconds = nya_time_ns_to_s(nya_app_get()->options.time_step_ns);
    if (tick_seconds <= 0.0) return;

    f32 interval = (f32)(tick_seconds * (f64)tick_gap);

    nya_net_replica_interpolate(&_NYA_NET_CLIENT.replicas, delta_time_s, interval, _NYA_NET_CLIENT.entity_remote);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_net_client_drain(f32 delta_time_s) {
    NYA_NetTransportEvent event = { 0 };

    while (nya_net_transport_poll(_NYA_NET_CLIENT.transport, &event)) {
        switch (event.kind) {
            case NYA_NET_TRANSPORT_EVENT_CONNECTED: {
                _NYA_NET_CLIENT.server_peer = event.peer;
                _NYA_NET_CLIENT.state       = NYA_NET_CLIENT_HANDSHAKING;

                _nya_net_client_send_hello();
            } break;

            case NYA_NET_TRANSPORT_EVENT_DISCONNECTED: {
                _NYA_NET_CLIENT.disconnect_reason = event.reason;

                nya_info("Disconnected (%d).", (int)event.reason);

                // The transport is left alone: it belongs to whoever created it, and a disconnect is not
                // a reason to destroy it — a caller may want to read the reason and then reconnect.
                NYA_NetDisconnect reason = event.reason;
                _nya_net_client_reset();
                _NYA_NET_CLIENT.disconnect_reason = reason;

                return;
            }

            case NYA_NET_TRANSPORT_EVENT_MESSAGE: {
                _nya_net_client_handle_message(event.data, event.size, delta_time_s);

                /*
                 * A message can end the connection, and the loop must stop when it does.
                 *
                 * REJECT tears the client down through _nya_net_client_reset, which nulls the transport —
                 * so continuing to poll dereferences it. Reachable from any server that refuses a
                 * connection, which is the ordinary version-mismatch path rather than an exotic one.
                 *
                 * Checked after every message rather than only after REJECT, because "handling a message
                 * may have disconnected us" is a property of the handler, not of one message kind.
                 */
                if (!_NYA_NET_CLIENT.active) return;
            } break;

            default: break;
        }
    }
}

void _nya_net_client_send_hello(void) {
    NYA_Arena* scratch = nya_arena_create(.name = "net_client_hello");
    defer      nya_arena_destroy(scratch);

    NYA_String* payload = nya_string_create(scratch);

    nya_net_message_begin(payload, NYA_NET_MSG_HELLO);

    NYA_Object* hello = nya_object_create(scratch);
    nya_object_set(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
    nya_object_set(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
    nya_object_set(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = _NYA_NET_CLIENT.name });

    if (!nya_net_message_write_object(scratch, payload, hello).ok) return;

    (void)nya_net_transport_send(_NYA_NET_CLIENT.transport, _NYA_NET_CLIENT.server_peer, NYA_NET_CHANNEL_RELIABLE, payload->items, payload->length);
}

void _nya_net_client_handle_message(const u8* data, u64 size, f32 delta_time_s) {
    u64                body_offset = 0;
    NYA_NetMessageKind kind        = nya_net_message_kind(data, size, &body_offset);

    if (kind == NYA_NET_MSG_COUNT) return;

    const u8* body      = data + body_offset;
    u64       body_size = size - body_offset;

    switch (kind) {
        case NYA_NET_MSG_WELCOME:  _nya_net_client_handle_welcome(body, body_size); break;
        case NYA_NET_MSG_SNAPSHOT: _nya_net_client_handle_snapshot(body, body_size, delta_time_s); break;

        case NYA_NET_MSG_REJECT: {
            NYA_Arena* scratch = nya_arena_create(.name = "net_client_reject");
            defer      nya_arena_destroy(scratch);

            NYA_Object* reject = nullptr;

            NYA_NetDisconnect reason = NYA_NET_DISCONNECT_PROTOCOL;

            if (nya_net_message_read_object(scratch, body, body_size, &reject).ok && reject != nullptr) {
                NYA_Value* value = nya_object_get(reject, "reason");
                if (value != nullptr && value->type == NYA_TYPE_U64 && value->as_u64 < NYA_NET_DISCONNECT_COUNT) {
                    reason = (NYA_NetDisconnect)value->as_u64;
                }
            }

            nya_log_error("The server refused the connection (%d).", (int)reason);

            _nya_net_client_reset();
            _NYA_NET_CLIENT.disconnect_reason = reason;
        } break;

        case NYA_NET_MSG_PEER_JOINED:
        case NYA_NET_MSG_PEER_LEFT: {
            NYA_Arena* scratch = nya_arena_create(.name = "net_client_roster");
            defer      nya_arena_destroy(scratch);

            NYA_Object* object = nullptr;
            if (!nya_net_message_read_object(scratch, body, body_size, &object).ok || object == nullptr) break;

            NYA_Value* name       = nya_object_get(object, "name");
            NYA_Value* index      = nya_object_get(object, "peer_index");
            NYA_Value* generation = nya_object_get(object, "peer_generation");

            if (name == nullptr || name->type != NYA_TYPE_STRING) break;
            if (index == nullptr || index->type != NYA_TYPE_U64) break;
            if (generation == nullptr || generation->type != NYA_TYPE_U64) break;

            NYA_NetPeerId who = { .index = (u32)index->as_u64, .generation = (u32)generation->as_u64 };

            b8 joined = kind == NYA_NET_MSG_PEER_JOINED;

            nya_info("Player '%s' %s.", name->as_string, joined ? "joined" : "left");

            NYA_NetPeerChangeFn on_peer_change = nya_callback_get(_NYA_NET_CLIENT.config.on_peer_change);

            if (on_peer_change != nullptr) on_peer_change(who, name->as_string, joined);
        } break;

        case NYA_NET_MSG_GAME_EVENT: {
            NYA_NetGameEventFn on_game_event = nya_callback_get(_NYA_NET_CLIENT.config.on_game_event);

            if (on_game_event == nullptr) break;

            NYA_Arena* scratch = nya_arena_create(.name = "net_client_game_event");
            defer      nya_arena_destroy(scratch);

            NYA_Object* object = nullptr;
            if (!nya_net_message_read_object(scratch, body, body_size, &object).ok || object == nullptr) break;

            on_game_event(object);
        } break;

        default: break;
    }
}

void _nya_net_client_handle_welcome(const u8* body, u64 size) {
    NYA_Arena* scratch = nya_arena_create(.name = "net_client_welcome");
    defer      nya_arena_destroy(scratch);

    NYA_Object* welcome = nullptr;

    if (!nya_net_message_read_object(scratch, body, size, &welcome).ok || welcome == nullptr) return;

    NYA_Value* peer_index        = nya_object_get(welcome, "peer_index");
    NYA_Value* peer_generation   = nya_object_get(welcome, "peer_generation");
    NYA_Value* entity_index      = nya_object_get(welcome, "entity_index");
    NYA_Value* entity_generation = nya_object_get(welcome, "entity_generation");
    NYA_Value* replicated_flag   = nya_object_get(welcome, "replicated_flag");

    if (peer_index == nullptr || peer_index->type != NYA_TYPE_U64) return;
    if (peer_generation == nullptr || peer_generation->type != NYA_TYPE_U64) return;

    _NYA_NET_CLIENT.peer = (NYA_NetPeerId){ .index = (u32)peer_index->as_u64, .generation = (u32)peer_generation->as_u64 };

    if (entity_index != nullptr && entity_index->type == NYA_TYPE_U64 && entity_generation != nullptr && entity_generation->type == NYA_TYPE_U64) {
        _NYA_NET_CLIENT.entity_remote = (NYA_EntityHandle){ .index = (u32)entity_index->as_u64, .generation = (u32)entity_generation->as_u64 };

        /*
         * On a listen server the two handle spaces are one, so the local name is known immediately.
         *
         * A remote client has to wait for the first snapshot to spawn the entity before it has a local
         * handle at all — which is why nya_net_client_entity can be NONE for a tick or two after the
         * handshake, and why a game must not assume otherwise.
         */
        if (nya_net_transport_is_local(_NYA_NET_CLIENT.transport)) _NYA_NET_CLIENT.entity_local = _NYA_NET_CLIENT.entity_remote;
    }

    /*
     * The server's replicated flag wins over the config's.
     *
     * They should agree, and a mismatch is a game's bug — but if they disagree, the server is the one
     * whose snapshots are being applied, so its answer is the one that makes them apply correctly.
     */
    if (replicated_flag != nullptr && replicated_flag->type == NYA_TYPE_U64) {
        _NYA_NET_CLIENT.config.replicated_flag = replicated_flag->as_u64;
    }

    _NYA_NET_CLIENT.state = NYA_NET_CLIENT_PLAYING;

    nya_info("Joined as peer %u, controlling server entity %u.", _NYA_NET_CLIENT.peer.index, _NYA_NET_CLIENT.entity_remote.index);
}

void _nya_net_client_handle_snapshot(const u8* body, u64 size, f32 delta_time_s) {
    // A snapshot before the handshake is either a stray packet or a confused server; either way there
    // is no predicted entity to reconcile against yet.
    if (_NYA_NET_CLIENT.state != NYA_NET_CLIENT_PLAYING) return;

    const NYA_NetSnapshot* baseline = _NYA_NET_CLIENT.has_baseline ? &_NYA_NET_CLIENT.baseline : nullptr;

    /*
     * Decoded into the *spare* arena, never the one holding the current baseline.
     *
     * The decode reads the baseline to fill in unchanged fields while producing the snapshot that will
     * become the next baseline. Into one arena, resetting first would free what it is about to read.
     */
    nya_arena_free_all(_NYA_NET_CLIENT.baseline_spare);

    NYA_NetSnapshot snapshot = { 0 };

    NYA_Error decoded = nya_net_snapshot_decode(_NYA_NET_CLIENT.baseline_spare, body, size, baseline, &snapshot);

    if (!decoded.ok) {
        // Dropped, not fatal. A malformed snapshot from a peer is data, and the next one arrives in a
        // sixteenth of a second — this is exactly the case an unreliable channel is built for.
        nya_debug("Discarding a malformed snapshot: %s", (NYA_ConstCString)decoded.message);
        return;
    }

    /*
     * Older than what has already been applied. Discarded.
     *
     * Snapshots travel unreliably and therefore out of order. Applying an older one over a newer one
     * would move the whole world backwards for a tick — and its delta was computed against a baseline
     * this client may have already replaced, so its unchanged fields are not even the right ones.
     */
    if (_NYA_NET_CLIENT.has_baseline && snapshot.tick <= _NYA_NET_CLIENT.server_tick) return;

    /*
     * A listen server applies nothing.
     *
     * The client and the server share one entity table and one world here — the snapshot describes what
     * the server has *already written*, to entities that already exist. Applying it would be at best a
     * no-op and at worst a duplicate spawn: the replica map has never seen these handles, so every
     * entity in the first snapshot would be spawned a second time and the world would double every
     * tick.
     *
     * The tick is still tracked and acknowledged below, because the server's baseline bookkeeping runs
     * the same way for every peer and a local one that never acknowledged would be sent full snapshots
     * forever.
     */
    if (!nya_net_transport_is_local(_NYA_NET_CLIENT.transport)) {
        nya_net_snapshot_apply(&snapshot, _NYA_NET_CLIENT.config.replicated_flag, &_NYA_NET_CLIENT.replicas, _NYA_NET_CLIENT.entity_remote);

        /*
         * The local name for the player, now that a snapshot may have spawned it.
         *
         * WELCOME gave the server's handle, which this process cannot use. Resolved every snapshot
         * rather than once, because the entity is spawned by whichever snapshot first mentions it — and
         * because a despawn and respawn on the server produces a new pairing.
         */
        _NYA_NET_CLIENT.entity_local = nya_net_replica_local(&_NYA_NET_CLIENT.replicas, _NYA_NET_CLIENT.entity_remote);
    }

    _nya_net_client_reconcile(&snapshot, delta_time_s);

    // The spare becomes current and the old current becomes spare. One swap, no copy.
    NYA_Arena* previous = _NYA_NET_CLIENT.baseline_arena;

    _NYA_NET_CLIENT.baseline_arena = _NYA_NET_CLIENT.baseline_spare;
    _NYA_NET_CLIENT.baseline_spare = previous;

    _NYA_NET_CLIENT.baseline     = snapshot;
    _NYA_NET_CLIENT.has_baseline = true;
    _NYA_NET_CLIENT.server_tick  = snapshot.tick;

    // Acknowledged so the server can delta against this. Unreliable: losing one costs nothing, because
    // the next names a tick at least as high.
    NYA_String* ack = nya_string_create(_NYA_NET_CLIENT.tick_arena);

    nya_net_message_begin(ack, NYA_NET_MSG_SNAPSHOT_ACK);
    _nya_net_write_u64(ack, snapshot.tick);

    (void)nya_net_transport_send(_NYA_NET_CLIENT.transport, _NYA_NET_CLIENT.server_peer, NYA_NET_CHANNEL_UNRELIABLE, ack->items, ack->length);
}

void _nya_net_client_reconcile(const NYA_NetSnapshot* snapshot, f32 delta_time_s) {
    /*
     * On a listen server there is nothing to reconcile.
     *
     * The client's world *is* the server's world — the same entity table, written by the same
     * simulation — so the snapshot describes what already happened. There is no latency to hide and no
     * prediction that could disagree.
     */
    if (nya_net_transport_is_local(_NYA_NET_CLIENT.transport)) return;

    if (!nya_entity_is_valid(_NYA_NET_CLIENT.entity_local)) return;

    // Looked up by the *server's* handle, because that is the name a snapshot uses. Searching for the
    // local handle would match nothing, or — worse — match whichever server entity happens to share
    // the number.
    const NYA_NetEntityState* authoritative = nya_net_snapshot_find(snapshot, _NYA_NET_CLIENT.entity_remote);
    if (authoritative == nullptr) return;

    // And applied to the *local* entity, which is the one in this process's table.
    NYA_Entity* entity = nya_entity_get(_NYA_NET_CLIENT.entity_local);
    if (entity == nullptr) return;

    /*
     * How far the prediction was wrong.
     *
     * Compared as a squared distance against a squared threshold, so there is no square root on a path
     * that runs every snapshot. Position only: it is what a player sees, and a rotation or velocity
     * disagreement that does not move anything is not worth replaying for.
     */
    f32x3 error = entity->position - authoritative->position;

    f32 distance_squared = (error.x * error.x) + (error.y * error.y) + (error.z * error.z);
    f32 threshold        = _NYA_NET_CLIENT.config.correction_threshold;

    if (distance_squared <= threshold * threshold) return;

    _NYA_NET_CLIENT.correction_count++;

    /*
     * Snap to the server's answer, then replay everything since.
     *
     * The correction is applied to the *past* — to the tick the snapshot describes — and the player's
     * current position is then recomputed by re-running every command from there to now. Without the
     * replay the player would be yanked back to where they were a round trip ago and would have to
     * walk the distance again, every time a correction happened.
     *
     * This is the entire reason commands are kept in a ring: the client has to be able to answer "what
     * was I trying to do at tick T" for every T between the server's answer and the present.
     */
    nya_net_entity_state_apply(entity, authoritative);

    NYA_NetApplyCommandFn apply_command = nya_callback_get(_NYA_NET_CLIENT.config.on_apply_command);
    if (apply_command == nullptr) return;

    u64 from = snapshot->tick + 1;

    // Bounded by the ring: a command older than NYA_NET_COMMAND_HISTORY ticks has been overwritten, so
    // replaying from there would replay whatever now occupies the slot. A client this far behind has
    // bigger problems than a smooth correction.
    if (_NYA_NET_CLIENT.local_tick >= NYA_NET_COMMAND_HISTORY && from < _NYA_NET_CLIENT.local_tick - NYA_NET_COMMAND_HISTORY + 1) {
        from = _NYA_NET_CLIENT.local_tick - NYA_NET_COMMAND_HISTORY + 1;
    }

    for (u64 replay = from; replay <= _NYA_NET_CLIENT.local_tick; replay++) {
        const NYA_NetCommand* command = &_NYA_NET_CLIENT.history[replay % NYA_NET_COMMAND_HISTORY];

        // A slot whose tick does not match was never filled, or has been overwritten. Skipped rather
        // than applied: replaying a stale command is worse than replaying nothing.
        if (command->tick != replay) continue;

        apply_command(entity, command, delta_time_s);
    }
}

void _nya_net_client_send_command(u64 tick, f32 delta_time_s) {
    NYA_NetCommand command = { .tick = tick };

    NYA_NetSampleCommandFn sample_command = nya_callback_get(_NYA_NET_CLIENT.config.on_sample_command);

    /*
     * A handle that no longer resolves is a reload that renamed or removed the game's sampler.
     *
     * Returning rather than asserting: the client is otherwise healthy, and the honest behaviour is a
     * player who stops moving until the next reload fixes it, not a crash that takes the session down.
     */
    if (sample_command == nullptr) {
        nya_log_error("The client's on_sample_command no longer resolves; sending no commands.");
        return;
    }

    sample_command(&command);

    // The tick is the engine's, not the game's, so it is stamped after sampling — a game filling it in
    // would have two sources of truth for which tick this is.
    command.tick = tick;

    _NYA_NET_CLIENT.history[tick % NYA_NET_COMMAND_HISTORY] = command;

    /*
     * Applied locally before it is sent, which is what prediction *is*.
     *
     * Skipped on a listen server: the server applies this same command to this same entity a moment
     * later, and doing it here as well would move the player twice per tick.
     */
    if (!nya_net_transport_is_local(_NYA_NET_CLIENT.transport)) {
        NYA_Entity* entity = nya_entity_get(_NYA_NET_CLIENT.entity_local);

        // Null until the first snapshot has spawned the player locally, which is a tick or two after
        // WELCOME. Nothing to predict until then, and the commands are still sent and still counted —
        // so the server moves the player and the next snapshot brings the client into step.
        NYA_NetApplyCommandFn apply_command = nya_callback_get(_NYA_NET_CLIENT.config.on_apply_command);

        if (entity != nullptr && apply_command != nullptr) apply_command(entity, &command, delta_time_s);
    }

    /*
     * The last several ticks go out together.
     *
     * Redundancy instead of reliability: a command is only useful for its own tick, so a retransmit
     * would arrive after the server had simulated past it. Sending four means one lost packet costs
     * nothing at all.
     */
    NYA_NetCommand run[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
    u32            count                          = 0;

    u64 oldest = tick >= NYA_NET_COMMAND_REDUNDANCY - 1 ? tick - (NYA_NET_COMMAND_REDUNDANCY - 1) : 0;

    for (u64 at = oldest; at <= tick; at++) {
        const NYA_NetCommand* historic = &_NYA_NET_CLIENT.history[at % NYA_NET_COMMAND_HISTORY];

        if (historic->tick != at) continue;

        run[count++] = *historic;
    }

    if (count == 0) return;

    NYA_String* payload = nya_string_create(_NYA_NET_CLIENT.tick_arena);

    nya_net_message_begin(payload, NYA_NET_MSG_COMMAND);

    if (!nya_net_command_encode(payload, run, count).ok) return;

    (void)nya_net_transport_send(_NYA_NET_CLIENT.transport, _NYA_NET_CLIENT.server_peer, NYA_NET_CHANNEL_UNRELIABLE, payload->items, payload->length);
}

void _nya_net_client_reset(void) {
    /*
     * The replicated world goes with the connection.
     *
     * These entities exist only because a server said so, and no server is saying so any more. Leaving
     * them would show a player a frozen snapshot of a session that has ended — and on reconnect the map
     * would be empty, so every one of them would be spawned a second time.
     *
     * Only on a remote connection: a listen server's client never populated the map, and the entities
     * in question are the server's own.
     */
    if (!nya_net_transport_is_local(_NYA_NET_CLIENT.transport)) nya_net_replica_map_despawn_all(&_NYA_NET_CLIENT.replicas);

    if (_NYA_NET_CLIENT.tick_arena != nullptr) nya_arena_destroy(_NYA_NET_CLIENT.tick_arena);
    if (_NYA_NET_CLIENT.baseline_arena != nullptr) nya_arena_destroy(_NYA_NET_CLIENT.baseline_arena);
    if (_NYA_NET_CLIENT.baseline_spare != nullptr) nya_arena_destroy(_NYA_NET_CLIENT.baseline_spare);
    if (_NYA_NET_CLIENT.allocator != nullptr) nya_arena_destroy(_NYA_NET_CLIENT.allocator);

    _NYA_NET_CLIENT = (_NYA_NetClientState){
        .entity_remote = NYA_ENTITY_HANDLE_NONE,
        .entity_local  = NYA_ENTITY_HANDLE_NONE,
        .peer          = NYA_NET_PEER_NONE,
    };
}

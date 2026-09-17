#include "nyangine/nyangine.h"

#include "nyangine/net/net_bytes.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One entry of a peer's snapshot ring: what was sent, so a later delta can be built against it. */
typedef struct {
    b8              used;
    NYA_NetSnapshot snapshot;

    /** Capacity of `snapshot.entities`, so a ring slot reuses its allocation instead of cloning on every send. */
    u32 capacity;
} _NYA_NetServerBaseline;

typedef struct {
    NYA_NetServerPeer public_state;

    /** Which transport this peer arrived on. A listen server has two, and a peer belongs to one. */
    NYA_NetTransport* transport;

    /**
     * Snapshots sent to this peer, indexed by `tick % NYA_NET_SNAPSHOT_HISTORY`. A slot whose tick does not match
     * means the acknowledged tick is too old for a delta.
     * */
    _NYA_NetServerBaseline baselines[NYA_NET_SNAPSHOT_HISTORY];

    /** The newest tick this peer says it has applied. Zero until it says anything. */
    u64 acknowledged_tick;

    /** The newest command tick applied, so a duplicate or a late one is ignored. */
    u64 last_command_tick;

    /**
     * The server tick a command was last applied to this peer. Not comparable with `last_command_tick`, which is
     * the client's numbering: a command for client tick T is applied during server tick T+1.
     * */
    u64 last_applied_server_tick;

    /**
     * The last applied command, repeated on ticks without one, so a player whose packets stall keeps walking as
     * their client predicts.
     * */
    NYA_NetCommand last_command;

    /* bandwidth */

    /**
     * Bytes this peer may still be sent, as a token bucket rather than a per-tick quota, so a full snapshot after
     * a reconnect can spend what idle time saved.
     * */
    s64 budget_bytes;

    /** Monotonic ms the bucket was last topped up. */
    u64 budget_refilled_ms;

    /* interest management */

    /**
     * Which entities are being sent to this peer, a bit per slot. Keeping an entity uses a looser threshold than
     * starting to send it, so boundary entities do not flicker.
     * */
    u8 relevant[NYA_ENTITY_MAX / 8];
} _NYA_NetServerPeerState;

typedef struct {
    b8 running;

    NYA_Arena* allocator;

    NYA_NetServerConfig config;

    /** Opened by nya_net_server_listen. Null for single player. */
    NYA_NetTransport* udp;

    /** The listen server's own player. Null on a dedicated server. */
    NYA_NetTransport* loopback_server_end;

    /**
     * The client half of the loopback pair, kept so the server that created it can destroy it. See the ordering
     * note in nya_net_server_stop.
     * */
    NYA_NetTransport* loopback_client_end;

    NYA_NetPeerId local_peer;

    /**
     * Indexed like the transport's peers. Allocated when a peer first takes the slot and reused after it leaves, so a
     * single player server holds one peer's state rather than NYA_NET_MAX_PEERS.
     * */
    _NYA_NetServerPeerState* peers[NYA_NET_MAX_PEERS];

    /** Peers connected on a transport that is not the loopback. */
    u32 remote_peer_count;

    u32 peer_count;

    /* lag compensation */

    /**
     * The world at each of the last NYA_NET_LAG_HISTORY ticks, for rewinding.
     * */
    _NYA_NetServerBaseline history[NYA_NET_LAG_HISTORY];

    /**
     * Where every rewound entity actually was, so nya_net_server_rewind_end can put it back.
     * */
    NYA_NetSnapshot rewind_restore;
    u32             rewind_restore_capacity;

    b8  rewind_active;
    u64 rewind_ticks;

    /**
     * The tick nya_net_server_tick was last called with.
     * */
    u64 current_tick;

    /**
     * Scratch for one tick's snapshot and its encodings.
     * */
    NYA_Arena* tick_arena;
} _NYA_NetServerState;

/** The one server. */
NYA_INTERNAL _NYA_NetServerState _NYA_NET_SERVER = { 0 };

NYA_INTERNAL void _nya_net_server_drain(NYA_NetTransport* transport, u64 tick, f32 delta_time_s);
NYA_INTERNAL void _nya_net_server_handle_message(
    NYA_NetTransport* transport, NYA_NetPeerId peer, const u8* data, u64 size, u64 tick, f32 delta_time_s
);
NYA_INTERNAL void _nya_net_server_handle_hello(NYA_NetTransport* transport, NYA_NetPeerId peer, const u8* body, u64 size);
NYA_INTERNAL void _nya_net_server_send_snapshots(u64 tick);

/**
 * The subset of `snapshot` that `peer` is entitled to see, in `arena`.
 * */
NYA_INTERNAL NYA_NetSnapshot _nya_net_server_relevant(
    NYA_Arena* arena, const NYA_NetSnapshot* snapshot, _NYA_NetServerPeerState* state
) __attr_no_discard;

/** Tops a peer's bandwidth bucket up for however long has passed, and whether `bytes` fit in it. */
NYA_INTERNAL b8 _nya_net_server_afford(_NYA_NetServerPeerState* state, u64 bytes) __attr_no_discard;

/** Copies `snapshot` into `slot`, growing its buffer only when needed. */
NYA_INTERNAL void _nya_net_server_store(_NYA_NetServerBaseline* slot, const NYA_NetSnapshot* snapshot);

/** Hands a stored snapshot's buffer back to the arena. */
NYA_INTERNAL void _nya_net_server_release(_NYA_NetServerBaseline* slot);

/** The peer slot for an id, or null when it names nothing live. */
NYA_INTERNAL _NYA_NetServerPeerState* _nya_net_server_find(NYA_NetPeerId peer) __attr_no_discard;

/** Takes a slot for a newly connected peer, or null when the server is full. */
NYA_INTERNAL _NYA_NetServerPeerState* _nya_net_server_admit(NYA_NetTransport* transport, NYA_NetPeerId peer) __attr_no_discard;

NYA_INTERNAL void _nya_net_server_remove(NYA_NetPeerId peer, NYA_NetDisconnect reason);

/** Tells everyone but `about` that a peer joined or left. */
NYA_INTERNAL void _nya_net_server_broadcast_roster(NYA_NetPeerId about, NYA_ConstCString name, b8 joined);

/** Sends `payload` to one peer on whichever transport it arrived on. */
NYA_INTERNAL void _nya_net_server_send(_NYA_NetServerPeerState* state, NYA_NetChannel channel, const NYA_String* payload);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_server_start(NYA_NetServerConfig config) {
    if (_NYA_NET_SERVER.running) return nya_error(NYA_ERROR_NOT_OK, "a server is already running");

    if (config.max_players == 0 || config.max_players > NYA_NET_MAX_PEERS) config.max_players = NYA_NET_MAX_PEERS;
    if (config.snapshot_interval_ticks == 0) config.snapshot_interval_ticks = 1;

    // clamped: asking for more history than the ring holds means as much as possible.
    if (config.lag_history_ticks > NYA_NET_LAG_HISTORY) {
        nya_log_warn("lag_history_ticks %u is past the %d the ring holds; using %d.", config.lag_history_ticks, NYA_NET_LAG_HISTORY, NYA_NET_LAG_HISTORY);
        config.lag_history_ticks = NYA_NET_LAG_HISTORY;
    }

    _NYA_NET_SERVER = (_NYA_NetServerState){
        .running    = true,
        .config     = config,
        .allocator  = nya_arena_create(.name = "net_server"),
        .tick_arena = nya_arena_create(.name = "net_server_tick"),
        .local_peer = NYA_NET_PEER_NONE,
    };

    // no socket: a server nobody listens to is single player.
    nya_log_info("Server started (replicating flag 0x%llx, up to %u players).", (unsigned long long)config.replicated_flag, config.max_players);

    // registered once, so restarting the server does not add duplicates.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("net_peers", NYA_NET_MAX_PEERS, &_NYA_NET_SERVER.peer_count);
        ceiling_registered = true;
    }

    return NYA_OK;
}

void nya_net_server_stop(void) {
    if (!_NYA_NET_SERVER.running) return;

    // each peer is told, so a client says "the server closed" instead of timing out.
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        _NYA_NetServerPeerState* state = _NYA_NET_SERVER.peers[i];
        if (state == nullptr || !nya_net_peer_is_set(state->public_state.peer)) continue;

        nya_net_transport_disconnect(state->transport, state->public_state.peer, NYA_NET_DISCONNECT_SERVER_CLOSED);
    }

    // released explicitly for readability; the arena destroy below reclaims it anyway.
    for (u32 i = 0; i < NYA_NET_LAG_HISTORY; i++) _nya_net_server_release(&_NYA_NET_SERVER.history[i]);

    nya_net_transport_destroy(_NYA_NET_SERVER.udp);

    /* Both halves of the loopback pair, since this server created both. */
    nya_net_transport_destroy(_NYA_NET_SERVER.loopback_server_end);
    nya_net_transport_destroy(_NYA_NET_SERVER.loopback_client_end);

    if (_NYA_NET_SERVER.tick_arena != nullptr) nya_arena_destroy(_NYA_NET_SERVER.tick_arena);
    if (_NYA_NET_SERVER.allocator != nullptr) nya_arena_destroy(_NYA_NET_SERVER.allocator);

    _NYA_NET_SERVER = (_NYA_NetServerState){ 0 };

    nya_log_info("Server stopped.");
}

b8 nya_net_server_running(void) {
    return _NYA_NET_SERVER.running;
}

NYA_Error nya_net_server_listen(u16 port) {
    if (!_NYA_NET_SERVER.running) return nya_error(NYA_ERROR_NOT_OK, "no server is running");
    if (_NYA_NET_SERVER.udp != nullptr) return nya_error(NYA_ERROR_NOT_OK, "the server is already listening");

    NYA_NetTransport* transport = nullptr;
    NYA_TRY(nya_net_transport_udp_create(_NYA_NET_SERVER.allocator, &transport));

    NYA_Error listening = nya_net_transport_listen(transport, port);

    if (!listening.ok) {
        // destroyed, so a retry on another port is not refused as already listening.
        nya_net_transport_destroy(transport);
        return listening;
    }

    _NYA_NET_SERVER.udp = transport;

    return NYA_OK;
}

b8 nya_net_server_is_listening(void) {
    return _NYA_NET_SERVER.udp != nullptr;
}

NYA_Error nya_net_server_attach_local(OUT NYA_NetTransport** out_client_transport) {
    nya_assert(out_client_transport != nullptr);

    *out_client_transport = nullptr;

    if (!_NYA_NET_SERVER.running) return nya_error(NYA_ERROR_NOT_OK, "no server is running");
    if (_NYA_NET_SERVER.loopback_server_end != nullptr) return nya_error(NYA_ERROR_NOT_OK, "a local player is already attached");

    NYA_NetTransport* server_end = nullptr;
    NYA_NetTransport* client_end = nullptr;

    NYA_TRY(nya_net_transport_loopback_create(_NYA_NET_SERVER.allocator, &server_end, &client_end));

    _NYA_NET_SERVER.loopback_server_end = server_end;
    _NYA_NET_SERVER.loopback_client_end = client_end;

    *out_client_transport = client_end;

    return NYA_OK;
}

NYA_NetPeerId nya_net_server_local_peer(void) {
    return _NYA_NET_SERVER.local_peer;
}

b8 nya_net_server_is_dedicated(void) {
    return _NYA_NET_SERVER.running && _NYA_NET_SERVER.loopback_server_end == nullptr;
}

void nya_net_server_tick(u64 tick, f32 delta_time_s) {
    if (!_NYA_NET_SERVER.running) return;

    /* The single player fast path. */
    if (_NYA_NET_SERVER.udp == nullptr && _NYA_NET_SERVER.loopback_server_end == nullptr) return;

    _NYA_NET_SERVER.current_tick = tick;

    nya_arena_free_all(_NYA_NET_SERVER.tick_arena);

    if (_NYA_NET_SERVER.udp != nullptr) _nya_net_server_drain(_NYA_NET_SERVER.udp, tick, delta_time_s);
    if (_NYA_NET_SERVER.loopback_server_end != nullptr) _nya_net_server_drain(_NYA_NET_SERVER.loopback_server_end, tick, delta_time_s);

    /* A tick with no command from a peer repeats the last one. */
    // resolved once for the loop: under hot reload it is a registry lookup.
    NYA_NetApplyCommandFn apply_command = nya_callback_get(_NYA_NET_SERVER.config.on_apply_command);

    if (apply_command != nullptr) {
        for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
            _NYA_NetServerPeerState* state = _NYA_NET_SERVER.peers[i];

            if (state == nullptr || !state->public_state.accepted) continue;

            // applied this server tick already. compared with the server's tick, never the client's.
            if (state->last_applied_server_tick == tick) continue;

            if (state->last_command.tick == 0) continue; // nothing to repeat yet

            NYA_Entity* entity = nya_entity_get(state->public_state.entity);
            if (entity == nullptr) continue;

            apply_command(entity, &state->last_command, delta_time_s);

            state->last_applied_server_tick = tick;
        }
    }

    if (tick % _NYA_NET_SERVER.config.snapshot_interval_ticks == 0) _nya_net_server_send_snapshots(tick);
}

u32 nya_net_server_peer_count(void) {
    return _NYA_NET_SERVER.peer_count;
}

const NYA_NetServerPeer* nya_net_server_peer_at(u32 index) {
    if (index >= NYA_NET_MAX_PEERS) return nullptr;
    const _NYA_NetServerPeerState* state = _NYA_NET_SERVER.peers[index];
    if (state == nullptr || !nya_net_peer_is_set(state->public_state.peer)) return nullptr;

    return &state->public_state;
}

const NYA_NetServerPeer* nya_net_server_peer(NYA_NetPeerId peer) {
    _NYA_NetServerPeerState* state = _nya_net_server_find(peer);

    return state == nullptr ? nullptr : &state->public_state;
}

void nya_net_server_kick(NYA_NetPeerId peer, NYA_NetDisconnect reason) {
    _NYA_NetServerPeerState* state = _nya_net_server_find(peer);
    if (state == nullptr) return;

    nya_net_transport_disconnect(state->transport, peer, reason);

    // removed now: the transport raises no event for a disconnect the caller requested.
    _nya_net_server_remove(peer, reason);
}

NYA_Error nya_net_server_send_event(NYA_NetPeerId peer, const NYA_Object* event) {
    if (!_NYA_NET_SERVER.running) return nya_error(NYA_ERROR_NOT_OK, "no server is running");
    if (event == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no event to send");

    NYA_Arena* scratch = nya_arena_create(.name = "net_server_event");
    defer      nya_arena_destroy(scratch);

    NYA_String* payload = nya_string_create(scratch);

    nya_net_message_begin(payload, NYA_NET_MSG_GAME_EVENT);
    NYA_TRY(nya_net_message_write_object(scratch, payload, event));

    // NYA_NET_PEER_NONE means everyone.
    if (!nya_net_peer_is_set(peer)) {
        for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
            _NYA_NetServerPeerState* state = _NYA_NET_SERVER.peers[i];
            if (state == nullptr || !state->public_state.accepted) continue;

            _nya_net_server_send(state, NYA_NET_CHANNEL_RELIABLE, payload);
        }

        return NYA_OK;
    }

    _NYA_NetServerPeerState* state = _nya_net_server_find(peer);
    if (state == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "no such peer");

    _nya_net_server_send(state, NYA_NET_CHANNEL_RELIABLE, payload);

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────
 * LAG COMPENSATION
 * ─────────────────────────────────────────────────────────
 */

b8 nya_net_server_rewind_begin(NYA_NetPeerId peer) {
    if (!_NYA_NET_SERVER.running) return false;
    if (_NYA_NET_SERVER.config.lag_history_ticks == 0) return false;

    // refused, not asserted: two systems rewinding is a plausible mistake, and nesting would restore to the past.
    if (_NYA_NET_SERVER.rewind_active) {
        nya_log_warn("nya_net_server_rewind_begin was called while already rewound; refusing to nest.");
        return false;
    }

    _NYA_NetServerPeerState* state = _nya_net_server_find(peer);
    if (state == nullptr) return false;

    /* The tick this peer had applied. */
    if (state->acknowledged_tick == 0) return false;

    _NYA_NetServerBaseline* past = &_NYA_NET_SERVER.history[state->acknowledged_tick % _NYA_NET_SERVER.config.lag_history_ticks];

    // overwritten by a newer tick: further back than the history reaches.
    if (!past->used || past->snapshot.tick != state->acknowledged_tick) return false;

    // the tick the server was last driven with. see `current_tick`.
    u64 now = _NYA_NET_SERVER.current_tick;

    /* Where everything is now. */
    if (_NYA_NET_SERVER.rewind_restore_capacity < past->snapshot.entity_count) {
        if (_NYA_NET_SERVER.rewind_restore.entities != nullptr) {
            nya_arena_free(_NYA_NET_SERVER.allocator, _NYA_NET_SERVER.rewind_restore.entities,
                           (u64)_NYA_NET_SERVER.rewind_restore_capacity * sizeof(NYA_NetEntityState));
        }

        _NYA_NET_SERVER.rewind_restore.entities = nya_arena_alloc(_NYA_NET_SERVER.allocator,
                                                                 (u64)past->snapshot.entity_count * sizeof(NYA_NetEntityState));
        _NYA_NET_SERVER.rewind_restore_capacity = past->snapshot.entity_count;
    }

    u32 restored = 0;

    for (u32 i = 0; i < past->snapshot.entity_count; i++) {
        const NYA_NetEntityState* historical = &past->snapshot.entities[i];

        /* The shooter is not moved. */
        if (historical->handle.index == state->public_state.entity.index
            && historical->handle.generation == state->public_state.entity.generation) {
            continue;
        }

        NYA_Entity* entity = nya_entity_get(historical->handle);
        if (entity == nullptr) continue;

        /* Anything the solver owns is left alone. */
        if (nya_physics2d_body_attached(entity) || nya_physics3d_body_attached(entity)) continue;

        // the present, kept to be put back.
        _NYA_NET_SERVER.rewind_restore.entities[restored++] = (NYA_NetEntityState){
            .handle   = entity->handle,
            .position = entity->position,
            .rotation = entity->rotation,
        };

        entity->position = historical->position;
        entity->rotation = historical->rotation;
    }

    _NYA_NET_SERVER.rewind_restore.entity_count = restored;
    _NYA_NET_SERVER.rewind_active               = true;
    _NYA_NET_SERVER.rewind_ticks                = now > state->acknowledged_tick ? now - state->acknowledged_tick : 0;

    return true;
}

void nya_net_server_rewind_end(void) {
    if (!_NYA_NET_SERVER.rewind_active) return;

    for (u32 i = 0; i < _NYA_NET_SERVER.rewind_restore.entity_count; i++) {
        const NYA_NetEntityState* saved = &_NYA_NET_SERVER.rewind_restore.entities[i];

        NYA_Entity* entity = nya_entity_get(saved->handle);

        // despawned in between, likely by the hit test. nothing to restore.
        if (entity == nullptr) continue;

        entity->position = saved->position;
        entity->rotation = saved->rotation;
    }

    _NYA_NET_SERVER.rewind_restore.entity_count = 0;
    _NYA_NET_SERVER.rewind_active               = false;
}

u64 nya_net_server_rewind_ticks(void) {
    return _NYA_NET_SERVER.rewind_active ? _NYA_NET_SERVER.rewind_ticks : 0;
}

NYA_NetCommand nya_net_server_last_command(NYA_NetPeerId peer) {
    _NYA_NetServerPeerState* state = _nya_net_server_find(peer);

    return state == nullptr ? (NYA_NetCommand){ 0 } : state->last_command;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_net_server_drain(NYA_NetTransport* transport, u64 tick, f32 delta_time_s) {
    NYA_NetTransportEvent event = { 0 };

    while (nya_net_transport_poll(transport, &event)) {
        switch (event.kind) {
            case NYA_NET_TRANSPORT_EVENT_CONNECTED: {
                /* A transport connection is not a player yet. The peer gets nothing until its HELLO passes the version check. */
                nya_log_debug("Transport connection from %s.", nya_net_transport_peer_address(transport, event.peer));
            } break;

            case NYA_NET_TRANSPORT_EVENT_DISCONNECTED: _nya_net_server_remove(event.peer, event.reason); break;

            case NYA_NET_TRANSPORT_EVENT_MESSAGE: {
                _nya_net_server_handle_message(transport, event.peer, event.data, event.size, tick, delta_time_s);
            } break;

            default: break;
        }
    }
}

void _nya_net_server_handle_message(NYA_NetTransport* transport, NYA_NetPeerId peer, const u8* data, u64 size, u64 tick, f32 delta_time_s) {
    u64                body_offset = 0;
    NYA_NetMessageKind kind        = nya_net_message_kind(data, size, &body_offset);

    // unknown kind: ignored, since a newer client may send one and incompatible peers were refused already.
    if (kind == NYA_NET_MSG_COUNT) return;

    const u8* body      = data + body_offset;
    u64       body_size = size - body_offset;

    if (kind == NYA_NET_MSG_HELLO) {
        _nya_net_server_handle_hello(transport, peer, body, body_size);
        return;
    }

    _NYA_NetServerPeerState* state = _nya_net_server_find(peer);

    // everything but HELLO requires an accepted peer.
    if (state == nullptr || !state->public_state.accepted) return;

    switch (kind) {
        case NYA_NET_MSG_COMMAND: {
            NYA_NetCommand commands[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
            u32            count                                = 0;

            NYA_Error decoded = nya_net_command_decode(body, body_size, commands, &count);

            if (!decoded.ok) {
                /* Malformed: the peer is dropped. */
                nya_log_warn("Dropping a peer that sent a malformed command: %s", (NYA_ConstCString)decoded.message);
                nya_net_server_kick(peer, NYA_NET_DISCONNECT_PROTOCOL);
                return;
            }

            NYA_Entity* entity = nya_entity_get(state->public_state.entity);

            /* How far ahead of the server a command may claim to be. */
            const u64 command_slack = 256;

            for (u32 i = 0; i < count; i++) {
                /* A command from the far future is discarded. */
                if (commands[i].tick > tick + command_slack) continue;

                // ordered and de-duplicated by tick; the unreliable channel sends each command several times.
                if (commands[i].tick <= state->last_command_tick) continue;

                state->last_command      = commands[i];
                state->last_command_tick = commands[i].tick;

                NYA_NetApplyCommandFn apply_command = nya_callback_get(_NYA_NET_SERVER.config.on_apply_command);

                if (entity != nullptr && apply_command != nullptr) {
                    apply_command(entity, &commands[i], delta_time_s);

                    // so the repeat pass knows this peer already moved.
                    state->last_applied_server_tick = tick;
                }
            }
        } break;

        case NYA_NET_MSG_SNAPSHOT_ACK: {
            if (body_size < 8) return;

            _NYA_NetReader reader = { .data = body, .size = body_size };
            u64            acked  = _nya_net_read_u64(&reader);

            /* A client cannot have applied a snapshot that was not sent. */
            if (acked > _NYA_NET_SERVER.current_tick) {
                nya_log_warn("Dropping a peer that acknowledged tick %llu when the server is at %llu.", (unsigned long long)acked,
                         (unsigned long long)_NYA_NET_SERVER.current_tick);
                nya_net_server_kick(peer, NYA_NET_DISCONNECT_PROTOCOL);
                return;
            }

            // monotonic: an old acknowledgement must not move the baseline backwards.
            if (acked > state->acknowledged_tick) state->acknowledged_tick = acked;
        } break;

        case NYA_NET_MSG_GAME_EVENT: {
            NYA_NetServerEventFn on_client_event = nya_callback_get(_NYA_NET_SERVER.config.on_client_event);

            if (on_client_event == nullptr) {
                nya_log_debug("A client sent a game event and no on_client_event is registered; ignoring it.");
                break;
            }

            NYA_Arena* scratch = nya_arena_create(.name = "net_server_client_event");
            defer      nya_arena_destroy(scratch);

            NYA_Object* object = nullptr;

            /* A malformed event is ignored. */
            if (!nya_net_message_read_object(scratch, body, body_size, &object).ok || object == nullptr) {
                nya_log_debug("Ignoring an unreadable game event from '%s'.", state->public_state.name);
                break;
            }

            // the peer is passed so the handler can decide whether it is allowed. see NYA_NetServerEventFn.
            on_client_event(peer, object);
        } break;

        default: break;
    }
}

/**
 * The most a HELLO may be, in bytes.
 * */
#define _NYA_NET_SERVER_MAX_HELLO 512

void _nya_net_server_handle_hello(NYA_NetTransport* transport, NYA_NetPeerId peer, const u8* body, u64 size) {
    /* Refused on size before parsing, since parsing is what is being protected. */
    if (size > _NYA_NET_SERVER_MAX_HELLO) {
        nya_log_warn("Refusing a %llu byte HELLO; the limit is %d.", (unsigned long long)size, _NYA_NET_SERVER_MAX_HELLO);
        nya_net_transport_disconnect(transport, peer, NYA_NET_DISCONNECT_PROTOCOL);
        return;
    }

    NYA_Arena* scratch = nya_arena_create(.name = "net_server_hello");
    defer      nya_arena_destroy(scratch);

    NYA_Object* hello = nullptr;

    if (!nya_net_message_read_object(scratch, body, size, &hello).ok || hello == nullptr) {
        nya_net_transport_disconnect(transport, peer, NYA_NET_DISCONNECT_PROTOCOL);
        return;
    }

    NYA_Value* protocol = nya_object_get(hello, "protocol");
    NYA_Value* snapshot = nya_object_get(hello, "snapshot");
    NYA_Value* name     = nya_object_get(hello, "name");

    u64 their_protocol = protocol != nullptr && protocol->type == NYA_TYPE_U64 ? protocol->as_u64 : 0;
    u64 their_snapshot = snapshot != nullptr && snapshot->type == NYA_TYPE_U64 ? snapshot->as_u64 : 0;

    /* Refused before any state is exchanged. */
    if (their_protocol != NYA_NET_PROTOCOL_VERSION || their_snapshot != NYA_NET_SNAPSHOT_VERSION) {
        NYA_String* payload = nya_string_create(scratch);

        nya_net_message_begin(payload, NYA_NET_MSG_REJECT);

        NYA_Object* reject = nya_object_create(scratch);
        nya_object_set(reject, "reason", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_DISCONNECT_VERSION });
        nya_object_set(reject, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION });
        nya_object_set(reject, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });

        NYA_EXPECT(nya_net_message_write_object(scratch, payload, reject));

        (void)nya_net_transport_send(transport, peer, NYA_NET_CHANNEL_RELIABLE, payload->items, payload->length);

        nya_log_warn("Refused a peer speaking protocol %llu/%llu; this build speaks %d/%d.", (unsigned long long)their_protocol,
                 (unsigned long long)their_snapshot, NYA_NET_PROTOCOL_VERSION, NYA_NET_SNAPSHOT_VERSION);
        return;
    }

    // a second HELLO is ignored, or one player would get two entities.
    _NYA_NetServerPeerState* existing = _nya_net_server_find(peer);
    if (existing != nullptr && existing->public_state.accepted) return;

    _NYA_NetServerPeerState* state = existing != nullptr ? existing : _nya_net_server_admit(transport, peer);

    if (state == nullptr) {
        NYA_String* payload = nya_string_create(scratch);
        nya_net_message_begin(payload, NYA_NET_MSG_REJECT);

        NYA_Object* reject = nya_object_create(scratch);
        nya_object_set(reject, "reason", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_DISCONNECT_FULL });

        NYA_EXPECT(nya_net_message_write_object(scratch, payload, reject));
        (void)nya_net_transport_send(transport, peer, NYA_NET_CHANNEL_RELIABLE, payload->items, payload->length);

        return;
    }

    NYA_ConstCString their_name = name != nullptr && name->type == NYA_TYPE_STRING ? name->as_string : "player";

    // copied and truncated: the length came from a peer, and the scratch arena dies with this function.
    (void)snprintf(state->public_state.name, sizeof(state->public_state.name), "%s", their_name);

    state->public_state.accepted = true;
    state->public_state.is_local = nya_net_transport_is_local(transport);

    if (state->public_state.is_local) _NYA_NET_SERVER.local_peer = peer;
    else _NYA_NET_SERVER.remote_peer_count++;

    NYA_NetSpawnPlayerFn on_spawn_player = nya_callback_get(_NYA_NET_SERVER.config.on_spawn_player);

    if (on_spawn_player != nullptr) {
        state->public_state.entity = on_spawn_player(peer, state->public_state.name);
    }

    NYA_String* welcome = nya_string_create(scratch);
    nya_net_message_begin(welcome, NYA_NET_MSG_WELCOME);

    NYA_Object* body_object = nya_object_create(scratch);
    nya_object_set(body_object, "peer_index", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = peer.index });
    nya_object_set(body_object, "peer_generation", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = peer.generation });
    nya_object_set(body_object, "entity_index", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = state->public_state.entity.index });
    nya_object_set(body_object, "entity_generation", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = state->public_state.entity.generation });
    nya_object_set(body_object, "replicated_flag", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = _NYA_NET_SERVER.config.replicated_flag });

    NYA_EXPECT(nya_net_message_write_object(scratch, welcome, body_object));

    _nya_net_server_send(state, NYA_NET_CHANNEL_RELIABLE, welcome);

    nya_log_info("Player '%s' joined from %s.", state->public_state.name, nya_net_transport_peer_address(transport, peer));

    _nya_net_server_broadcast_roster(peer, state->public_state.name, true);
}

void _nya_net_server_send_snapshots(u64 tick) {
    if (_NYA_NET_SERVER.peer_count == 0) return;

    NYA_Arena* arena = _NYA_NET_SERVER.tick_arena;

    NYA_NetSnapshot snapshot = { 0 };

    NYA_Error captured = nya_net_snapshot_capture(arena, _NYA_NET_SERVER.config.replicated_flag, tick, &snapshot);
    if (!captured.ok) return;

    /* The unfiltered world goes into the history ring first. */
    if (_NYA_NET_SERVER.config.lag_history_ticks > 0) {
        _nya_net_server_store(&_NYA_NET_SERVER.history[tick % _NYA_NET_SERVER.config.lag_history_ticks], &snapshot);
    }

    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        _NYA_NetServerPeerState* state = _NYA_NET_SERVER.peers[i];
        if (state == nullptr || !state->public_state.accepted) continue;

        /* The baseline is what this peer acknowledged, if still in the ring. */
        const NYA_NetSnapshot* baseline = nullptr;

        if (state->acknowledged_tick != 0) {
            _NYA_NetServerBaseline* candidate = &state->baselines[state->acknowledged_tick % NYA_NET_SNAPSHOT_HISTORY];

            if (candidate->used && candidate->snapshot.tick == state->acknowledged_tick) baseline = &candidate->snapshot;
        }

        // only what this peer may see. a subset of a sorted list stays sorted. see NYA_NetRelevanceFn.
        NYA_NetSnapshot relevant = _nya_net_server_relevant(arena, &snapshot, state);

        NYA_String* payload = nya_string_create(arena);

        nya_net_message_begin(payload, NYA_NET_MSG_SNAPSHOT);

        if (!nya_net_snapshot_encode(arena, &relevant, baseline, payload).ok) continue;

        /* Skipped when it exceeds the peer's budget. */
        if (!_nya_net_server_afford(state, payload->length)) continue;

        _nya_net_server_send(state, NYA_NET_CHANNEL_UNRELIABLE, payload);

        /* Kept as a future baseline; the filtered snapshot is what the peer has. */
        _nya_net_server_store(&state->baselines[tick % NYA_NET_SNAPSHOT_HISTORY], &relevant);
    }
}

NYA_NetSnapshot _nya_net_server_relevant(NYA_Arena* arena, const NYA_NetSnapshot* snapshot, _NYA_NetServerPeerState* state) {
    NYA_NetRelevanceFn on_relevance  = nya_callback_get(_NYA_NET_SERVER.config.on_relevance);
    b8                 has_callback  = on_relevance != nullptr;
    b8 has_radius   = _NYA_NET_SERVER.config.relevance_radius > 0.0F;

    // nothing filters, so nothing is copied. the common configuration.
    if (!has_callback && !has_radius) return *snapshot;

    const NYA_Entity* peer_entity = nya_entity_get(state->public_state.entity);

    /* A spectator is sent everything. */
    if (peer_entity == nullptr && !has_callback) return *snapshot;

    NYA_NetEntityState* kept  = nya_arena_alloc(arena, (u64)snapshot->entity_count * sizeof(NYA_NetEntityState));
    u32                 count = 0;

    /* Two thresholds: enter at the radius, leave past the radius plus the band. */
    f32 band = _NYA_NET_SERVER.config.relevance_hysteresis > 0.0F
                 ? _NYA_NET_SERVER.config.relevance_hysteresis
                 : _NYA_NET_SERVER.config.relevance_radius * NYA_NET_RELEVANCE_HYSTERESIS;

    f32 enter_squared = _NYA_NET_SERVER.config.relevance_radius * _NYA_NET_SERVER.config.relevance_radius;
    f32 leave         = _NYA_NET_SERVER.config.relevance_radius + band;
    f32 leave_squared = leave * leave;

    for (u32 i = 0; i < snapshot->entity_count; i++) {
        const NYA_NetEntityState* candidate = &snapshot->entities[i];

        u32 slot = candidate->handle.index;

        // bounded before indexing the bitset, even though the handle is the server's own.
        b8 was_relevant = slot < NYA_ENTITY_MAX && (state->relevant[slot / 8] & (u8)(1U << (slot % 8))) != 0;

        /* The peer's own entity is always relevant. */
        b8 is_own = candidate->handle.index == state->public_state.entity.index
                 && candidate->handle.generation == state->public_state.entity.generation;

        b8 relevant = true;

        if (!is_own) {
            if (has_callback) {
                const NYA_Entity* entity = nya_entity_get(candidate->handle);

                // no longer resolves: treated as gone, and its bit cleared so a reused slot does not inherit it.
                relevant = entity != nullptr && on_relevance(state->public_state.peer, peer_entity, entity, was_relevant);
            } else {
                f32x3 offset = candidate->position - peer_entity->position;

                // squared: this runs per entity per peer per tick.
                f32 distance_squared = (offset.x * offset.x) + (offset.y * offset.y) + (offset.z * offset.z);

                relevant = distance_squared <= (was_relevant ? leave_squared : enter_squared);
            }
        }

        if (slot < NYA_ENTITY_MAX) {
            if (relevant) state->relevant[slot / 8] |= (u8)(1U << (slot % 8));
            else state->relevant[slot / 8] &= (u8)~(1U << (slot % 8));
        }

        if (!relevant) continue;

        kept[count++] = *candidate;
    }

    // still in ascending handle order, which the delta encoder and decoder depend on.
    return (NYA_NetSnapshot){ .tick = snapshot->tick, .entities = kept, .entity_count = count };
}

b8 _nya_net_server_afford(_NYA_NetServerPeerState* state, u64 bytes) {
    u32 limit = _NYA_NET_SERVER.config.bandwidth_bytes_per_second;

    if (limit == 0) return true;

    u64 now_ms = nya_clock_get_monotonic_ms();

    if (state->budget_refilled_ms == 0) {
        state->budget_refilled_ms = now_ms;
        state->budget_bytes       = (s64)limit;
    }

    u64 elapsed_ms = _nya_net_elapsed_ms(now_ms, state->budget_refilled_ms);

    if (elapsed_ms > 0) {
        state->budget_bytes += (s64)((u64)limit * elapsed_ms / 1000ULL);
        state->budget_refilled_ms = now_ms;

        /* The bucket holds at most one second's worth. */
        if (state->budget_bytes > (s64)limit) state->budget_bytes = (s64)limit;
    }

    /* The gate is "anything left", not "does this fit". */
    if (state->budget_bytes <= 0) return false;

    state->budget_bytes -= (s64)bytes;

    /* The debt is bounded, so one huge snapshot cannot silence a peer for minutes. */
    if (state->budget_bytes < -(s64)limit) state->budget_bytes = -(s64)limit;

    return true;
}

void _nya_net_server_store(_NYA_NetServerBaseline* slot, const NYA_NetSnapshot* snapshot) {
    nya_assert(slot != nullptr);
    nya_assert(snapshot != nullptr);

    /* The slot's buffer is reused unless too small. */
    if (slot->capacity < snapshot->entity_count) {
        _nya_net_server_release(slot);

        slot->snapshot.entities = nya_arena_alloc(_NYA_NET_SERVER.allocator, (u64)snapshot->entity_count * sizeof(NYA_NetEntityState));
        slot->capacity          = snapshot->entity_count;
    }

    if (snapshot->entity_count > 0) {
        nya_memcpy(slot->snapshot.entities, snapshot->entities, (u64)snapshot->entity_count * sizeof(NYA_NetEntityState));
    }

    slot->snapshot.tick         = snapshot->tick;
    slot->snapshot.entity_count = snapshot->entity_count;
    slot->used                  = true;
}

void _nya_net_server_release(_NYA_NetServerBaseline* slot) {
    nya_assert(slot != nullptr);

    if (slot->snapshot.entities == nullptr) return;

    nya_arena_free(_NYA_NET_SERVER.allocator, slot->snapshot.entities, (u64)slot->capacity * sizeof(NYA_NetEntityState));

    *slot = (_NYA_NetServerBaseline){ 0 };
}

_NYA_NetServerPeerState* _nya_net_server_find(NYA_NetPeerId peer) {
    if (!nya_net_peer_is_set(peer)) return nullptr;
    if (peer.index >= NYA_NET_MAX_PEERS) return nullptr;

    _NYA_NetServerPeerState* state = _NYA_NET_SERVER.peers[peer.index];
    if (state == nullptr) return nullptr;

    // the generation, so a stale handle does not address the slot's next peer.
    if (!nya_net_peer_equals(state->public_state.peer, peer)) return nullptr;

    return state;
}

_NYA_NetServerPeerState* _nya_net_server_admit(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    if (peer.index >= NYA_NET_MAX_PEERS) return nullptr;
    if (_NYA_NET_SERVER.peer_count >= _NYA_NET_SERVER.config.max_players) return nullptr;

    /* The transport's peer index is the server's slot. */
    _NYA_NetServerPeerState* state = _NYA_NET_SERVER.peers[peer.index];

    if (state == nullptr) {
        state                             = nya_arena_alloc(_NYA_NET_SERVER.allocator, sizeof(_NYA_NetServerPeerState));
        _NYA_NET_SERVER.peers[peer.index] = state;
    } else if (nya_net_peer_is_set(state->public_state.peer)) {
        return nullptr;
    }

    *state = (_NYA_NetServerPeerState){
        .transport = transport,
        .public_state = { .peer = peer, .entity = NYA_ENTITY_HANDLE_NONE },
    };

    _NYA_NET_SERVER.peer_count++;

    return state;
}

void _nya_net_server_remove(NYA_NetPeerId peer, NYA_NetDisconnect reason) {
    _NYA_NetServerPeerState* state = _nya_net_server_find(peer);
    if (state == nullptr) return;

    b8 was_accepted = state->public_state.accepted;

    if (was_accepted) {
        NYA_NetDespawnPlayerFn on_despawn_player = nya_callback_get(_NYA_NET_SERVER.config.on_despawn_player);

        if (on_despawn_player != nullptr) {
            on_despawn_player(peer, state->public_state.entity);
        }

        // deferred: this can run inside the drain loop, and despawning mid-iteration is what the barrier prevents.
        if (nya_entity_is_valid(state->public_state.entity)) nya_entity_despawn_deferred(state->public_state.entity);

        nya_log_info("Player '%s' left (%d).", state->public_state.name, (int)reason);

        if (state->public_state.is_local) _NYA_NET_SERVER.local_peer = NYA_NET_PEER_NONE;
        else if (_NYA_NET_SERVER.remote_peer_count > 0) _NYA_NET_SERVER.remote_peer_count--;
    }

    if (_NYA_NET_SERVER.peer_count > 0) _NYA_NET_SERVER.peer_count--;

    /* The peer's snapshot ring goes back to the arena. */
    for (u32 i = 0; i < NYA_NET_SNAPSHOT_HISTORY; i++) _nya_net_server_release(&state->baselines[i]);

    char departed[NYA_NET_MAX_NAME];
    (void)snprintf(departed, sizeof(departed), "%s", state->public_state.name);

    // zeroed, freeing the slot and its baselines, which a new peer must not inherit.
    *state = (_NYA_NetServerPeerState){ 0 };

    if (was_accepted) _nya_net_server_broadcast_roster(peer, departed, false);
}

void _nya_net_server_broadcast_roster(NYA_NetPeerId about, NYA_ConstCString name, b8 joined) {
    NYA_Arena* scratch = nya_arena_create(.name = "net_server_roster");
    defer      nya_arena_destroy(scratch);

    NYA_String* payload = nya_string_create(scratch);

    nya_net_message_begin(payload, joined ? NYA_NET_MSG_PEER_JOINED : NYA_NET_MSG_PEER_LEFT);

    NYA_Object* object = nya_object_create(scratch);
    nya_object_set(object, "peer_index", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = about.index });
    nya_object_set(object, "peer_generation", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = about.generation });
    nya_object_set(object, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)name });

    if (!nya_net_message_write_object(scratch, payload, object).ok) return;

    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        _NYA_NetServerPeerState* state = _NYA_NET_SERVER.peers[i];

        if (state == nullptr || !state->public_state.accepted) continue;

        // not to the peer itself: it learns its arrival from WELCOME.
        if (nya_net_peer_equals(state->public_state.peer, about)) continue;

        _nya_net_server_send(state, NYA_NET_CHANNEL_RELIABLE, payload);
    }
}

void _nya_net_server_send(_NYA_NetServerPeerState* state, NYA_NetChannel channel, const NYA_String* payload) {
    nya_assert(state != nullptr);
    nya_assert(payload != nullptr);

    if (state->transport == nullptr || payload->length == 0) return;

    NYA_Error sent = nya_net_transport_send(state->transport, state->public_state.peer, channel, payload->items, payload->length);

    // a failed send is not a dead peer; the timeout decides. debug level, since bad connections do this often.
    if (!sent.ok) nya_log_debug("Could not send to '%s': %s", state->public_state.name, (NYA_ConstCString)sent.message);
}

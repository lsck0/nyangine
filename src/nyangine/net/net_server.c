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

    /**
     * How many entities `snapshot.entities` has room for, versus how many it holds — lets a ring slot
     * reuse its allocation instead of cloning fresh every send. Without it, a slot overwritten every
     * NYA_NET_SNAPSHOT_HISTORY ticks leaked one snapshot per peer per wrap: on the order of a hundred
     * kilobytes/sec/player at sixty hertz, enough to take a long-running server down.
     * */
    u32 capacity;
} _NYA_NetServerBaseline;

typedef struct {
    NYA_NetServerPeer public_state;

    /** Which transport this peer arrived on. A listen server has two, and a peer belongs to one. */
    NYA_NetTransport* transport;

    /**
     * Snapshots this peer has been sent, newest anywhere in the ring. Indexed by `tick %
     * NYA_NET_SNAPSHOT_HISTORY`, so finding the acknowledged one is a modulo, not a search — a tick too
     * old lands on an entry whose own tick doesn't match, which is how "too far behind for a delta" is
     * detected.
     * */
    _NYA_NetServerBaseline baselines[NYA_NET_SNAPSHOT_HISTORY];

    /** The newest tick this peer says it has applied. Zero until it says anything. */
    u64 acknowledged_tick;

    /** The newest command tick applied, so a duplicate or a late one is ignored. */
    u64 last_command_tick;

    /**
     * The *server* tick a command was last applied to this peer — distinct from `last_command_tick`
     * (the client's numbering); the two must not be compared. A client's command for tick T is drained
     * during server tick T+1, so `last_command_tick < tick` is true the very tick it was applied — using
     * that as the "did anything arrive" test made the repeat below fire on a fresh command too, moving
     * every player twice per tick.
     * */
    u64 last_applied_server_tick;

    /**
     * The command applied most recently — kept so a tick with no command can repeat it. A player
     * mid-stride whose packets stop should keep walking, not stop dead, which also agrees with what
     * their own client is predicting.
     * */
    NYA_NetCommand last_command;

    /*
     * ── bandwidth ──
     */

    /**
     * Bytes this peer may still be sent this second, as a token bucket — not a per-tick quota, so a
     * burst (a full snapshot after a reconnect) can spend what an idle second accumulated. A flat
     * per-tick limit would refuse exactly the snapshot a recovering peer most needs.
     * */
    s64 budget_bytes;

    /** Monotonic ms the bucket was last topped up. */
    u64 budget_refilled_ms;

    /*
     * ── interest management ──
     */

    /**
     * Which entities are currently being sent to this peer, one bit per entity slot — what makes
     * relevance hysteretic: the *keep* threshold is looser than the *start* threshold, and this answers
     * "am I already sending it". Without it, relevance is re-decided from scratch every snapshot and
     * boundary entities flicker.
     *
     * Keyed on slot index alone, not generation — a full generation per slot would be four kilobytes per
     * peer for a benign case: a reused slot's newcomer inherits the departed entity's bit for one
     * snapshot, judged against the *leave* radius instead of *enter* and possibly sent one tick early,
     * which is the safe direction.
     * */
    u8 relevant[NYA_ENTITY_MAX / 8];
} _NYA_NetServerPeerState;

typedef struct {
    b8 running;

    NYA_Arena* allocator;

    NYA_NetServerConfig config;

    /** Opened by nya_net_server_listen. Null for single player, which is the whole point. */
    NYA_NetTransport* udp;

    /** The listen server's own player. Null on a dedicated server. */
    NYA_NetTransport* loopback_server_end;

    /**
     * The other half of that pair, handed to the local client — kept here so it can be destroyed. The
     * server created the pair and owns both ends; the client end used to leak its delivered arena every
     * session, since nya_net_client_attach doesn't take ownership of a transport it didn't create. A
     * borrowed pointer from the client's view, destroyed here after the client disconnects; see the
     * ordering note in nya_net_server_stop.
     * */
    NYA_NetTransport* loopback_client_end;

    NYA_NetPeerId local_peer;

    _NYA_NetServerPeerState peers[NYA_NET_MAX_PEERS];

    /**
     * How many peers are connected on a transport that is not the loopback.
     *
     * The number nya_net_server_tick branches on. Counted rather than derived, because it is read
     * every tick and deriving it would mean walking the peer table to decide whether to do nothing.
     * */
    u32 remote_peer_count;

    u32 peer_count;

    /*
     * ── lag compensation ──
     */

    /**
     * The world at each of the last NYA_NET_LAG_HISTORY ticks, for rewinding.
     *
     * Indexed by `tick % lag_history_ticks`, so finding a tick is a modulo and a tick too old lands on
     * an entry whose own tick no longer matches — which is how "further back than the history reaches"
     * is detected without tracking it separately.
     *
     * One ring for the whole server rather than per peer: everyone is rewound against the same history,
     * so this is a fixed cost.
     * */
    _NYA_NetServerBaseline history[NYA_NET_LAG_HISTORY];

    /**
     * Where every rewound entity actually was, so nya_net_server_rewind_end can put it back.
     *
     * Captured at rewind_begin from the *live* world rather than reconstructed from history, because the
     * present is the thing that has to be restored exactly — history is only an approximation of the
     * past and would quietly become the present if it were used to restore.
     * */
    NYA_NetSnapshot rewind_restore;
    u32             rewind_restore_capacity;

    b8  rewind_active;
    u64 rewind_ticks;

    /**
     * The tick nya_net_server_tick was last called with.
     *
     * Not read from nya_world()->sim_system.tick, even though the app loop passes exactly that. The tick
     * is a *parameter*, so a caller driving the server itself — a test, a replay tool, a game with its own
     * loop — can pass whatever it likes, and reaching for the world's counter instead would silently
     * disagree with it. That made nya_net_server_rewind_ticks report zero for a genuine rewind.
     * */
    u64 current_tick;

    /**
     * Scratch for one tick's snapshot and its encodings.
     *
     * Reset at the top of every tick rather than freed piecemeal: a snapshot plus one encoded payload
     * per peer is a dozen allocations a tick, and an arena reset is one operation for all of them.
     * */
    NYA_Arena* tick_arena;
} _NYA_NetServerState;

/**
 * The one server, as a file scope static.
 *
 * Not on NYA_App, for the reason the i18n system is not: net sits above core in the include order but
 * is not owned by it, and a headless tool compiles core without ever starting a server. Safe across a
 * hot reload because this is compiled into the executable rather than the game's library.
 * */
NYA_INTERNAL _NYA_NetServerState _NYA_NET_SERVER = { 0 };

NYA_INTERNAL void _nya_net_server_drain(NYA_NetTransport* transport, u64 tick, f32 delta_time_s);
NYA_INTERNAL void _nya_net_server_handle_message(
    NYA_NetTransport* transport, NYA_NetPeerId peer, const u8* data, u64 size, u64 tick, f32 delta_time_s
);
NYA_INTERNAL void _nya_net_server_handle_hello(NYA_NetTransport* transport, NYA_NetPeerId peer, const u8* body, u64 size);
NYA_INTERNAL void _nya_net_server_send_snapshots(u64 tick);

/**
 * The subset of `snapshot` that `peer` is entitled to see, in `arena`.
 *
 * A subset of a sorted list is sorted, so the result feeds the delta encoder unchanged. Returns the
 * snapshot itself when nothing filters, which is the common case and costs no copy.
 * */
NYA_INTERNAL NYA_NetSnapshot _nya_net_server_relevant(
    NYA_Arena* arena, const NYA_NetSnapshot* snapshot, _NYA_NetServerPeerState* state
) __attr_no_discard;

/** Tops a peer's bandwidth bucket up for however long has passed, and whether `bytes` fit in it. */
NYA_INTERNAL b8 _nya_net_server_afford(_NYA_NetServerPeerState* state, u64 bytes) __attr_no_discard;

/** Copies `snapshot` into `slot`, growing its buffer only when it has to. Shared by the baselines and the history. */
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

    // Clamped rather than refused: a game asking for more history than the ring holds is asking for as
    // much as it can have, and failing to start over it would be absurd.
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

    // No socket. That is what makes this the single player path as well as the hosting path: a server
    // with nobody listening simulates a world and sends nothing.
    nya_log_info("Server started (replicating flag 0x%llx, up to %u players).", (unsigned long long)config.replicated_flag, config.max_players);

    // Guarded so a game (or a test) that starts and stops a server repeatedly in one process does
    // not add a copy of itself to the ceiling registry on every restart.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("net_peers", NYA_NET_MAX_PEERS, &_NYA_NET_SERVER.peer_count);
        ceiling_registered = true;
    }

    return NYA_OK;
}

void nya_net_server_stop(void) {
    if (!_NYA_NET_SERVER.running) return;

    // Each peer is told, so a client shows "the server closed" rather than waiting out a timeout.
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        _NYA_NetServerPeerState* state = &_NYA_NET_SERVER.peers[i];
        if (!nya_net_peer_is_set(state->public_state.peer)) continue;

        nya_net_transport_disconnect(state->transport, state->public_state.peer, NYA_NET_DISCONNECT_SERVER_CLOSED);
    }

    // The history ring and the restore buffer, before the arena goes. Belt and braces: the arena destroy
    // below reclaims everything anyway, and doing it explicitly keeps the ownership readable.
    for (u32 i = 0; i < NYA_NET_LAG_HISTORY; i++) _nya_net_server_release(&_NYA_NET_SERVER.history[i]);

    nya_net_transport_destroy(_NYA_NET_SERVER.udp);

    /*
     * Both halves of the loopback pair, because this server created both.
     *
     * The client end was handed to nya_net_client_attach, which deliberately does not take ownership of a
     * transport it did not create — so without this its delivered arena leaked once per session. Whoever
     * makes a pair destroys a pair.
     *
     * Safe in this order because a client is expected to have disconnected first; a loopback destroy
     * unhooks the far end before freeing anything, so even out of order it reports a dead peer rather than
     * touching freed memory.
     */
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
        // Destroyed rather than kept, so a second attempt on another port is not refused by the
        // "already listening" check above for a transport that never bound anything.
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

    /*
     * The single player fast path, and the reason the architecture is arranged this way.
     *
     * With no transports there is nothing to drain, nothing to apply and nobody to send to — so no
     * snapshot is captured, nothing is encoded, and no baseline is kept. A game that never opens to
     * the LAN pays for these two comparisons per tick and nothing else.
     */
    if (_NYA_NET_SERVER.udp == nullptr && _NYA_NET_SERVER.loopback_server_end == nullptr) return;

    _NYA_NET_SERVER.current_tick = tick;

    nya_arena_free_all(_NYA_NET_SERVER.tick_arena);

    if (_NYA_NET_SERVER.udp != nullptr) _nya_net_server_drain(_NYA_NET_SERVER.udp, tick, delta_time_s);
    if (_NYA_NET_SERVER.loopback_server_end != nullptr) _nya_net_server_drain(_NYA_NET_SERVER.loopback_server_end, tick, delta_time_s);

    /*
     * A tick with no command from a peer repeats its last one.
     *
     * Done after draining, so a command that did arrive wins. What this covers is the gap: several
     * consecutive lost packets, or a client whose frame ran long. The alternative — applying nothing —
     * stops the player dead for a tick and then resumes, which is both visibly wrong and disagrees
     * with what that client is predicting.
     */
    // Resolved once for the whole loop rather than per peer: under hot reload this is a registry
    // lookup, and it cannot change part way through a tick.
    NYA_NetApplyCommandFn apply_command = nya_callback_get(_NYA_NET_SERVER.config.on_apply_command);

    if (apply_command != nullptr) {
        for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
            _NYA_NetServerPeerState* state = &_NYA_NET_SERVER.peers[i];

            if (!state->public_state.accepted) continue;

            // Something arrived this server tick and has already been applied. Compared against the
            // server's own tick, never against the client's — see last_applied_server_tick.
            if (state->last_applied_server_tick == tick) continue;

            if (state->last_command.tick == 0) continue; // never sent anything to repeat

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
    if (!nya_net_peer_is_set(_NYA_NET_SERVER.peers[index].public_state.peer)) return nullptr;

    return &_NYA_NET_SERVER.peers[index].public_state;
}

const NYA_NetServerPeer* nya_net_server_peer(NYA_NetPeerId peer) {
    _NYA_NetServerPeerState* state = _nya_net_server_find(peer);

    return state == nullptr ? nullptr : &state->public_state;
}

void nya_net_server_kick(NYA_NetPeerId peer, NYA_NetDisconnect reason) {
    _NYA_NetServerPeerState* state = _nya_net_server_find(peer);
    if (state == nullptr) return;

    nya_net_transport_disconnect(state->transport, peer, reason);

    // Removed here rather than waiting for a DISCONNECTED event: the transport does not raise one for
    // a disconnect the caller asked for, so nothing else would clean the slot up.
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

    // NYA_NET_PEER_NONE means everyone, which is what a chat line or a world event wants and saves a
    // caller writing the same loop.
    if (!nya_net_peer_is_set(peer)) {
        for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
            _NYA_NetServerPeerState* state = &_NYA_NET_SERVER.peers[i];
            if (!state->public_state.accepted) continue;

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

    // Nesting would restore to whatever the inner rewind captured, which is already the past. Refused
    // rather than asserted, because a game calling this from two systems is a plausible mistake.
    if (_NYA_NET_SERVER.rewind_active) {
        nya_log_warn("nya_net_server_rewind_begin was called while already rewound; refusing to nest.");
        return false;
    }

    _NYA_NetServerPeerState* state = _nya_net_server_find(peer);
    if (state == nullptr) return false;

    /*
     * The tick this peer had actually applied.
     *
     * Its own acknowledgement, not an estimate from the round trip — the client says which snapshot it
     * is standing on, and that is a better answer than anything derived from a smoothed RTT. It is also
     * a number the client controls, which is why nya_net_server_rewind_ticks exists for a game that
     * wants to refuse an implausible one.
     */
    if (state->acknowledged_tick == 0) return false;

    _NYA_NetServerBaseline* past = &_NYA_NET_SERVER.history[state->acknowledged_tick % _NYA_NET_SERVER.config.lag_history_ticks];

    // The slot has been overwritten by a newer tick, so that moment is gone. The ring index is a modulo,
    // so this comparison is what detects "further back than the history reaches".
    if (!past->used || past->snapshot.tick != state->acknowledged_tick) return false;

    // The tick the server was last driven with, not the world's counter. See `current_tick`.
    u64 now = _NYA_NET_SERVER.current_tick;

    /*
     * Where everything is *now*, captured from the live world.
     *
     * From the world rather than from the newest history entry, because this is what has to be restored
     * exactly. History is a per-snapshot sample and the world has moved on within the tick since — using
     * it to restore would quietly make an approximation of the past into the present.
     */
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

        /*
         * The shooter is not moved.
         *
         * They predicted themselves and are already where they believe they are; rewinding them would
         * resolve the shot from where they used to stand, which is the one position the client did *not*
         * aim from.
         */
        if (historical->handle.index == state->public_state.entity.index
            && historical->handle.generation == state->public_state.entity.generation) {
            continue;
        }

        NYA_Entity* entity = nya_entity_get(historical->handle);
        if (entity == nullptr) continue;

        /*
         * Anything the solver owns is left alone.
         *
         * A body's transform is rewritten every step from the solver's own state, so moving the entity
         * here would be undone — and moving the *body* would corrupt the simulation for everyone else.
         * See the note in net_server.h; a game that needs its players compensated must not give them
         * bodies.
         */
        if (nya_physics2d_body_attached(entity) || nya_physics3d_body_attached(entity)) continue;

        // The present, kept so it can be put back exactly.
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

        // Despawned between the two calls — a hit test that removed something, most likely. Nothing to
        // put back, and the handle no longer resolving is exactly how that is detected.
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
                /*
                 * Nothing is admitted here. A transport-level connection is not a player yet — the
                 * peer has to say HELLO and pass the version check first, and until it does it gets
                 * no entity, no snapshots and no place in the roster.
                 *
                 * That ordering is what stops an unauthenticated peer costing the server a spawned
                 * entity and a snapshot baseline.
                 */
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

    // A kind this build does not know. Ignored rather than punished — a newer client may send one, and
    // the handshake has already refused a genuinely incompatible peer.
    if (kind == NYA_NET_MSG_COUNT) return;

    const u8* body      = data + body_offset;
    u64       body_size = size - body_offset;

    if (kind == NYA_NET_MSG_HELLO) {
        _nya_net_server_handle_hello(transport, peer, body, body_size);
        return;
    }

    _NYA_NetServerPeerState* state = _nya_net_server_find(peer);

    // Everything but HELLO requires an accepted peer. A peer sending commands before it has been
    // admitted is either confused or probing; either way it gets nothing.
    if (state == nullptr || !state->public_state.accepted) return;

    switch (kind) {
        case NYA_NET_MSG_COMMAND: {
            NYA_NetCommand commands[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
            u32            count                                = 0;

            NYA_Error decoded = nya_net_command_decode(body, body_size, commands, &count);

            if (!decoded.ok) {
                /*
                 * Malformed. The peer is dropped rather than the message ignored.
                 *
                 * Unlike an unknown message kind, this is a payload claiming to be something it is not
                 * — a length that does not match, a count past the limit. A well behaved client of any
                 * version cannot produce it, so it is either a broken client or a probe.
                 */
                nya_log_warn("Dropping a peer that sent a malformed command: %s", (NYA_ConstCString)decoded.message);
                nya_net_server_kick(peer, NYA_NET_DISCONNECT_PROTOCOL);
                return;
            }

            NYA_Entity* entity = nya_entity_get(state->public_state.entity);

            /*
             * How far ahead of the server a command may legitimately claim to be.
             *
             * A client runs ahead by roughly its own latency, and the redundancy window adds a few more —
             * so a couple of hundred ticks is generous at any playable ping. Beyond that the number is
             * not a clock skew, it is wrong.
             */
            const u64 command_slack = 256;

            for (u32 i = 0; i < count; i++) {
                /*
                 * A command from the far future is discarded rather than recorded.
                 *
                 * `tick` is an unvalidated u64. One command claiming U64_MAX sets `last_command_tick` to
                 * it, and every subsequent command from that client is then discarded by the comparison
                 * below — while the repeat pass keeps re-applying the frozen last one forever. The player
                 * is stuck walking in one direction and it looks like a server bug.
                 *
                 * Self-inflicted, so it is dropped rather than a disconnection: a client with a broken
                 * clock deserves to be ignored, not kicked.
                 */
                if (commands[i].tick > tick + command_slack) continue;

                // Ordered and de-duplicated by tick. The channel is unreliable and redundant, so the
                // same command arrives several times and out of order as a matter of course.
                if (commands[i].tick <= state->last_command_tick) continue;

                state->last_command      = commands[i];
                state->last_command_tick = commands[i].tick;

                NYA_NetApplyCommandFn apply_command = nya_callback_get(_NYA_NET_SERVER.config.on_apply_command);

                if (entity != nullptr && apply_command != nullptr) {
                    apply_command(entity, &commands[i], delta_time_s);

                    // So the repeat pass in nya_net_server_tick knows this peer already moved.
                    state->last_applied_server_tick = tick;
                }
            }
        } break;

        case NYA_NET_MSG_SNAPSHOT_ACK: {
            if (body_size < 8) return;

            _NYA_NetReader reader = { .data = body, .size = body_size };
            u64            acked  = _nya_net_read_u64(&reader);

            /*
             * A client cannot have applied a snapshot that has not been sent yet.
             *
             * The field is a u64 the client chooses. Taken at face value, a single message naming
             * U64_MAX sets `acknowledged_tick` permanently — and because the assignment below is
             * monotonic, no baseline ever matches again, so that peer is sent a **full** snapshot every
             * tick for the rest of the session. A per-peer bandwidth multiplier the client picks, times
             * however many peers it can occupy.
             *
             * It also feeds lag compensation, where a client claiming to be further behind than it is
             * has its shots resolved against a staler world. The ring lookup there already refuses a
             * tick it does not hold, but there is no reason to let an impossible number in at all.
             */
            if (acked > _NYA_NET_SERVER.current_tick) {
                nya_log_warn("Dropping a peer that acknowledged tick %llu when the server is at %llu.", (unsigned long long)acked,
                         (unsigned long long)_NYA_NET_SERVER.current_tick);
                nya_net_server_kick(peer, NYA_NET_DISCONNECT_PROTOCOL);
                return;
            }

            // Monotonic: a late acknowledgement naming an older tick must not move the baseline
            // backwards, or the next delta would be built against something the peer has since
            // replaced.
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

            /*
             * A malformed event is ignored rather than fatal to the peer.
             *
             * Unlike a malformed *command*, which cannot be produced by a well behaved client of any
             * version, a game event is a game-defined document — a newer client may send one shaped in a
             * way this build's serde cannot read. Dropping the connection over that would make every
             * addition to a game's event vocabulary a hard compatibility break.
             */
            if (!nya_net_message_read_object(scratch, body, body_size, &object).ok || object == nullptr) {
                nya_log_debug("Ignoring an unreadable game event from '%s'.", state->public_state.name);
                break;
            }

            // The peer is passed so the handler knows who is asking, which is the whole basis on which it
            // can decide whether they are allowed to. See NYA_NetServerEventFn.
            on_client_event(peer, object);
        } break;

        default: break;
    }
}

/**
 * The most a HELLO may be, in bytes.
 *
 * A HELLO carries three fields and is dispatched *before* the peer is accepted, so it is the one message
 * an unauthenticated address can put in front of the deserializer. A reassembled reliable message can be
 * hundreds of kilobytes, which would make the whole nya parser reachable from an unverified source with
 * a large block of chosen input — the biggest untrusted surface in the process, for a message that needs
 * a couple of hundred bytes.
 *
 * Generous enough for a long player name and a few added fields later; small enough that whatever the
 * parser allocates per byte of nesting cannot matter.
 * */
#define _NYA_NET_SERVER_MAX_HELLO 512

void _nya_net_server_handle_hello(NYA_NetTransport* transport, NYA_NetPeerId peer, const u8* body, u64 size) {
    /*
     * Refused on size before it is parsed at all.
     *
     * Checking after parsing is not checking: the parse is the thing being protected against.
     */
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

    /*
     * Refused before any state is exchanged.
     *
     * A peer that misparses a snapshot does not fail cleanly — it plays a subtly different game, and
     * neither side can tell. Refusing a connection a player can understand beats allowing one nobody
     * can debug.
     */
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

    // A second HELLO from an accepted peer is ignored rather than re-admitting it, which would spawn
    // a second entity for one player.
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

    // Copied into a fixed buffer, truncating. The name came from a peer, so its length is not ours to
    // trust and holding a pointer into a scratch arena that dies at the end of this function is worse.
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

    /*
     * The unfiltered world goes into the history ring, before anything is filtered for anyone.
     *
     * Lag compensation rewinds to what a client *saw*, and what it saw is a subset — but the rewind has
     * to place entities the shooter could see and the shot might hit, which is not the same set and can
     * be larger. Storing the whole world means a rewind is never limited by somebody's interest radius.
     */
    if (_NYA_NET_SERVER.config.lag_history_ticks > 0) {
        _nya_net_server_store(&_NYA_NET_SERVER.history[tick % _NYA_NET_SERVER.config.lag_history_ticks], &snapshot);
    }

    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        _NYA_NetServerPeerState* state = &_NYA_NET_SERVER.peers[i];
        if (!state->public_state.accepted) continue;

        /*
         * The baseline is whatever *this* peer has acknowledged, and only if it is still in the ring.
         *
         * A peer that has acknowledged nothing, or that has fallen further behind than
         * NYA_NET_SNAPSHOT_HISTORY ticks, gets a full snapshot. The ring index is a modulo, so a tick
         * too old lands on an entry whose own tick no longer matches — which is how "too far behind"
         * is detected without tracking it separately.
         */
        const NYA_NetSnapshot* baseline = nullptr;

        if (state->acknowledged_tick != 0) {
            _NYA_NetServerBaseline* candidate = &state->baselines[state->acknowledged_tick % NYA_NET_SNAPSHOT_HISTORY];

            if (candidate->used && candidate->snapshot.tick == state->acknowledged_tick) baseline = &candidate->snapshot;
        }

        // Only what this peer is entitled to see. A subset of a sorted list is sorted, so the encoder is
        // unaffected. See NYA_NetRelevanceFn.
        NYA_NetSnapshot relevant = _nya_net_server_relevant(arena, &snapshot, state);

        NYA_String* payload = nya_string_create(arena);

        nya_net_message_begin(payload, NYA_NET_MSG_SNAPSHOT);

        if (!nya_net_snapshot_encode(arena, &relevant, baseline, payload).ok) continue;

        /*
         * Skipped rather than queued when it does not fit the peer's budget.
         *
         * The next snapshot supersedes this one, so dropping it costs a little staleness — where queueing
         * would build a backlog that only grows for a peer already unable to keep up, and deliver every
         * one of those snapshots late.
         *
         * Crucially the baseline is **not** updated either. Recording a snapshot the peer never received
         * as its baseline would have every subsequent delta computed against a state it does not have,
         * and the client would apply changes on top of the wrong world with nothing detecting it.
         */
        if (!_nya_net_server_afford(state, payload->length)) continue;

        _nya_net_server_send(state, NYA_NET_CHANNEL_UNRELIABLE, payload);

        /*
         * Kept as a possible future baseline — and it is the *filtered* snapshot that is kept.
         *
         * The baseline must be what the peer actually received. Storing the unfiltered world would have
         * a delta claim "unchanged" about entities that peer has never been sent, so they would never
         * arrive at all.
         *
         * In the server's long-lived arena rather than the tick arena, since a baseline has to survive
         * until the peer acknowledges it.
         */
        _nya_net_server_store(&state->baselines[tick % NYA_NET_SNAPSHOT_HISTORY], &relevant);
    }
}

NYA_NetSnapshot _nya_net_server_relevant(NYA_Arena* arena, const NYA_NetSnapshot* snapshot, _NYA_NetServerPeerState* state) {
    NYA_NetRelevanceFn on_relevance  = nya_callback_get(_NYA_NET_SERVER.config.on_relevance);
    b8                 has_callback  = on_relevance != nullptr;
    b8 has_radius   = _NYA_NET_SERVER.config.relevance_radius > 0.0F;

    // Nothing filters, so nothing is copied. The overwhelmingly common configuration.
    if (!has_callback && !has_radius) return *snapshot;

    const NYA_Entity* peer_entity = nya_entity_get(state->public_state.entity);

    /*
     * A spectator is sent everything.
     *
     * There is no centre to measure a radius from, and a game's own rule is welcome to decide otherwise —
     * but the built-in one showing a bodiless observer nothing at all would be a worse default than
     * showing them too much.
     */
    if (peer_entity == nullptr && !has_callback) return *snapshot;

    NYA_NetEntityState* kept  = nya_arena_alloc(arena, (u64)snapshot->entity_count * sizeof(NYA_NetEntityState));
    u32                 count = 0;

    /*
     * Two thresholds, not one: enter at the radius, leave only past the radius plus the band.
     *
     * With one threshold an entity sitting on it flips every snapshot, and each flip is a spawn and a
     * despawn on the client. The band is what stops that, and zero means the default fraction rather than
     * none — see NYA_NetServerConfig.relevance_hysteresis.
     */
    f32 band = _NYA_NET_SERVER.config.relevance_hysteresis > 0.0F
                 ? _NYA_NET_SERVER.config.relevance_hysteresis
                 : _NYA_NET_SERVER.config.relevance_radius * NYA_NET_RELEVANCE_HYSTERESIS;

    f32 enter_squared = _NYA_NET_SERVER.config.relevance_radius * _NYA_NET_SERVER.config.relevance_radius;
    f32 leave         = _NYA_NET_SERVER.config.relevance_radius + band;
    f32 leave_squared = leave * leave;

    for (u32 i = 0; i < snapshot->entity_count; i++) {
        const NYA_NetEntityState* candidate = &snapshot->entities[i];

        u32 slot = candidate->handle.index;

        // Bounded before it indexes the bitset. The handle came from this server's own capture, so this is
        // a guard against a future change rather than against a peer, but an unbounded shift is not the
        // kind of thing to leave to trust.
        b8 was_relevant = slot < NYA_ENTITY_MAX && (state->relevant[slot / 8] & (u8)(1U << (slot % 8))) != 0;

        /*
         * The peer's own entity is always relevant.
         *
         * It is what they are predicting, and reconciliation needs the server's answer for it every
         * snapshot. A radius centred on it would include it anyway; a game's own rule might not, and
         * excluding it would leave that player unable to be corrected.
         */
        b8 is_own = candidate->handle.index == state->public_state.entity.index
                 && candidate->handle.generation == state->public_state.entity.generation;

        b8 relevant = true;

        if (!is_own) {
            if (has_callback) {
                const NYA_Entity* entity = nya_entity_get(candidate->handle);

                // A handle in the snapshot that no longer resolves. Treated as gone rather than relevant,
                // and its bit cleared below so a reused slot does not inherit it for longer than a tick.
                relevant = entity != nullptr && on_relevance(state->public_state.peer, peer_entity, entity, was_relevant);
            } else {
                f32x3 offset = candidate->position - peer_entity->position;

                // Squared, so there is no square root on a path that runs per entity per peer per tick.
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

    // Still in ascending handle order, because a subset of an ordered list is ordered — which the delta
    // encoder and the decoder's order check both depend on.
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

        /*
         * The bucket holds at most one second's worth.
         *
         * Without a ceiling an idle peer accumulates an unbounded allowance and then spends it all in one
         * frame the moment something happens — which is a burst the connection cannot absorb and is
         * exactly the shape of traffic a cap exists to prevent.
         */
        if (state->budget_bytes > (s64)limit) state->budget_bytes = (s64)limit;
    }

    /*
     * The gate is "is there anything left", not "does this fit".
     *
     * Requiring it to fit means a snapshot larger than one second's budget can **never** be sent, because
     * the bucket is capped at exactly that — so the peer is starved of state permanently and silently.
     * A 40-entity world at a modest cap reaches this immediately, which is not an exotic configuration.
     *
     * So a send is allowed whenever the bucket is not empty, and the cost is taken even if it goes
     * negative. The debt is then paid off by the refill above before anything else goes out, which makes
     * the *average* rate exactly the cap while guaranteeing forward progress. That is the difference
     * between a rate limit and a mute button.
     */
    if (state->budget_bytes <= 0) return false;

    state->budget_bytes -= (s64)bytes;

    /*
     * The debt is bounded, so one enormous snapshot cannot silence a peer for minutes.
     *
     * Without a floor, a burst leaves a deficit that takes proportionally long to clear — and the peer
     * receives nothing at all in the meantime, which is the starvation this whole branch exists to avoid.
     * One second of debt means the worst case is one second of silence.
     */
    if (state->budget_bytes < -(s64)limit) state->budget_bytes = -(s64)limit;

    return true;
}

void _nya_net_server_store(_NYA_NetServerBaseline* slot, const NYA_NetSnapshot* snapshot) {
    nya_assert(slot != nullptr);
    nya_assert(snapshot != nullptr);

    /*
     * The slot's buffer is reused unless it is too small.
     *
     * nya_net_snapshot_clone allocates a fresh array every time, and a ring slot is overwritten every
     * time it comes round — so cloning leaked one snapshot per wrap per peer. At sixty hertz that is on
     * the order of a hundred kilobytes a second per player, which takes a long-running server down.
     *
     * Reusing means a slot converges on the largest count it has seen and then allocates nothing.
     */
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

    _NYA_NetServerPeerState* state = &_NYA_NET_SERVER.peers[peer.index];

    // The generation, so a handle held across a disconnect does not address whoever took the slot.
    if (!nya_net_peer_equals(state->public_state.peer, peer)) return nullptr;

    return state;
}

_NYA_NetServerPeerState* _nya_net_server_admit(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    if (peer.index >= NYA_NET_MAX_PEERS) return nullptr;
    if (_NYA_NET_SERVER.peer_count >= _NYA_NET_SERVER.config.max_players) return nullptr;

    /*
     * The transport's peer index is reused as the server's slot.
     *
     * They are bounded by the same constant and the transport already guarantees uniqueness among live
     * peers, so a second independent allocation would be a second thing to keep in step. The catch is
     * that a listen server has *two* transports whose indices both start at zero — which is why the
     * loopback peer occupies a slot the UDP transport can also hand out.
     *
     * Handled by refusing the collision: the loopback player connects first, at start-up, so the slot
     * is taken before any remote peer can arrive. A remote peer landing on it is turned away and
     * reconnects onto another index.
     */
    _NYA_NetServerPeerState* state = &_NYA_NET_SERVER.peers[peer.index];

    if (nya_net_peer_is_set(state->public_state.peer)) return nullptr;

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

        // Deferred, because this can run from inside the drain loop and a despawn during iteration is
        // what the barrier exists to make safe.
        if (nya_entity_is_valid(state->public_state.entity)) nya_entity_despawn_deferred(state->public_state.entity);

        nya_log_info("Player '%s' left (%d).", state->public_state.name, (int)reason);

        if (state->public_state.is_local) _NYA_NET_SERVER.local_peer = NYA_NET_PEER_NONE;
        else if (_NYA_NET_SERVER.remote_peer_count > 0) _NYA_NET_SERVER.remote_peer_count--;
    }

    if (_NYA_NET_SERVER.peer_count > 0) _NYA_NET_SERVER.peer_count--;

    /*
     * The peer's snapshot ring goes back to the arena.
     *
     * These live in the server's long-lived allocator because a baseline has to outlive the tick that
     * produced it, so nothing else would ever reclaim them — and a server that players join and leave
     * all day would accumulate one full ring per departure.
     */
    for (u32 i = 0; i < NYA_NET_SNAPSHOT_HISTORY; i++) _nya_net_server_release(&state->baselines[i]);

    char departed[NYA_NET_MAX_NAME];
    (void)snprintf(departed, sizeof(departed), "%s", state->public_state.name);

    // Zeroed, which clears the peer id and so makes the slot free again. The baselines go with it,
    // which matters: they belong to the long-lived arena and a new peer must not inherit them.
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
        _NYA_NetServerPeerState* state = &_NYA_NET_SERVER.peers[i];

        if (!state->public_state.accepted) continue;

        // Not to the peer it is about: a client learns its own arrival from WELCOME, and telling it
        // again would have it add itself to its own roster twice.
        if (nya_net_peer_equals(state->public_state.peer, about)) continue;

        _nya_net_server_send(state, NYA_NET_CHANNEL_RELIABLE, payload);
    }
}

void _nya_net_server_send(_NYA_NetServerPeerState* state, NYA_NetChannel channel, const NYA_String* payload) {
    nya_assert(state != nullptr);
    nya_assert(payload != nullptr);

    if (state->transport == nullptr || payload->length == 0) return;

    NYA_Error sent = nya_net_transport_send(state->transport, state->public_state.peer, channel, payload->items, payload->length);

    // A failed send is not a dead peer — a full buffer, a momentary route problem — so the timeout
    // decides rather than this. Logged at debug because on a bad connection it is not rare.
    if (!sent.ok) nya_log_debug("Could not send to '%s': %s", state->public_state.name, (NYA_ConstCString)sent.message);
}

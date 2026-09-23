#include "nyangine/nyangine.h"

#include "nyangine/net/net_bytes.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How far a prediction may be wrong before it is corrected, when the config does not say.
 * */
#define _NYA_NET_CLIENT_DEFAULT_THRESHOLD 0.01F

/** The range the interpolation delay stays in, in ticks, and how far the timeline may drift before it jumps rather than eases. */
#define _NYA_NET_CLIENT_DELAY_MIN_TICKS 1.0F
#define _NYA_NET_CLIENT_DELAY_MAX_TICKS 30.0F
#define _NYA_NET_CLIENT_RESYNC_TICKS    8.0

/** A snapshot the client decoded, kept so the server may send deltas against it. */
typedef struct {
    NYA_NetSnapshot snapshot;

    /** How many entities `snapshot.entities` has room for, so a slot reuses its buffer. */
    u32 capacity;
} _NYA_NetClientBaseline;

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
     * */
    NYA_EntityHandle entity_remote;

    /**
     * The same entity, as *this* process names it.
     * */
    NYA_EntityHandle entity_local;

    /**
     * Which local entity stands for which server entity. See NYA_NetReplicaMap. Null on a listen server's own
     * client, which shares the server's world and replicates nothing, so single player carries no map.
     * */
    NYA_NetReplicaMap* replicas;

    /** The transport peer the server is, from this client's side. Always index zero, one peer. */
    NYA_NetPeerId server_peer;

    /* snapshots */

    /**
     * Recently decoded snapshots by `tick % NYA_NET_SNAPSHOT_HISTORY`. The server deltas against whichever one it
     * last heard acknowledged, which is rarely the newest, so the ring is as long as the server's.
     * */
    _NYA_NetClientBaseline baselines[NYA_NET_SNAPSHOT_HISTORY];

    /** The newest snapshot applied, and what every command acknowledges. */
    u64 server_tick;

    /** The newest snapshot's payload size, for the stats. */
    u32 snapshot_bytes;

    /* prediction */

    /**
     * Commands sent but not yet confirmed, by tick.
     * */
    NYA_NetCommand history[NYA_NET_COMMAND_HISTORY];

    /** Where prediction put the player after each command in `history`, to compare with the server's answer for it. */
    f32x3 predicted[NYA_NET_COMMAND_HISTORY];

    u64 local_tick;

    u64 correction_count;

    /*
     * the timeline replicas are drawn on
     *
     * Remote entities are drawn a little in the past, at `render_tick`, so there are nearly always two snapshots
     * around the moment drawn. How far in the past follows the link: the gap between snapshots plus twice how
     * irregularly they arrive.
     */

    f64 render_tick;
    b8  render_started;

    /** How far behind the newest snapshot replicas are drawn, in ticks, easing toward what the link needs. */
    f32 delay_ticks;

    /** Moving averages of the tick gap between applied snapshots and of how far each arrived from when it was due. */
    f32 arrival_gap_ticks;
    f32 arrival_jitter_ticks;

    /** When the newest snapshot arrived, by the monotonic clock. */
    u64 arrival_ns;
} _NYA_NetClientState;

/** The one client, as a file scope static, like the server. */
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
 * Compares the server's state for the predicted entity with the prediction and replays on a
 * mismatch.
 * */
NYA_INTERNAL void _nya_net_client_reconcile(const NYA_NetSnapshot* snapshot, f32 delta_time_s);

NYA_INTERNAL void _nya_net_client_reset(void);

/** nya_net_client_tick behind the system registry's signature, reading the tick off the world. */

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_client_connect(NYA_ConstCString address, u16 port, NYA_ConstCString name, NYA_NetClientConfig config) {
    return nya_net_client_connect_on(NYA_NET_TRANSPORT_UDP, address, port, name, config);
}

NYA_Error nya_net_client_connect_on(NYA_NetTransportKind kind, NYA_ConstCString address, u16 port, NYA_ConstCString name, NYA_NetClientConfig config) {
    if (_NYA_NET_CLIENT.active) return nya_error(NYA_ERROR_NOT_OK, "a client is already connected");
    if (address == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no address to connect to");

    NYA_Arena* allocator = nya_arena_create(.name = "net_client");

    NYA_NetTransport* transport = nullptr;
    NYA_Error         created   = NYA_OK;

    switch (kind) {
        case NYA_NET_TRANSPORT_UDP: {
            NYA_NetUdpOptions options = { .identity = config.identity, .conditions = config.conditions };
            nya_memcpy(options.server_key, config.server_key, NYA_NET_KEY_SIZE);

            created = nya_net_transport_udp_create(allocator, options, &transport);
        } break;

        case NYA_NET_TRANSPORT_STEAM: created = nya_net_transport_steam_create(allocator, &transport); break;

        case NYA_NET_TRANSPORT_LOOPBACK:
        case NYA_NET_TRANSPORT_KIND_COUNT:
        default:                         created = nya_error(NYA_ERROR_NOT_SUPPORTED, "that transport has no address to connect to"); break;
    }

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

    // the transport was allocated from this setup allocator, and attach made its own.
    _NYA_NET_CLIENT.owns_transport = true;

    return NYA_OK;
}

NYA_Error nya_net_client_attach(NYA_NetTransport* transport, NYA_ConstCString name, NYA_NetClientConfig config) {
    if (_NYA_NET_CLIENT.active) return nya_error(NYA_ERROR_NOT_OK, "a client is already connected");
    if (transport == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no transport");
    // zero is an unset nya_callback handle. Whether a handle resolves is only known per call, since hot
    // reload can remove the name.
    if (config.on_sample_command == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a client needs on_sample_command");
    if (config.on_apply_command == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a client needs on_apply_command");

    if (config.correction_threshold <= 0.0F) config.correction_threshold = _NYA_NET_CLIENT_DEFAULT_THRESHOLD;

    _NYA_NET_CLIENT = (_NYA_NetClientState){
        .active      = true,
        .delay_ticks = _NYA_NET_CLIENT_DELAY_MIN_TICKS * 2.0F,
        .arrival_gap_ticks = 1.0F,
        .state     = NYA_NET_CLIENT_CONNECTING,
        .config    = config,
        .transport = transport,
        .entity_remote = NYA_ENTITY_HANDLE_NONE,
        .entity_local  = NYA_ENTITY_HANDLE_NONE,
        .peer          = NYA_NET_PEER_NONE,

        .allocator  = nya_arena_create(.name = "net_client"),
        .tick_arena = nya_arena_create(.name = "net_client_tick"),
    };

    (void)snprintf(_NYA_NET_CLIENT.name, sizeof(_NYA_NET_CLIENT.name), "%s", name != nullptr ? name : "player");

    if (!nya_net_transport_is_local(transport)) {
        _NYA_NET_CLIENT.replicas  = nya_arena_alloc(_NYA_NET_CLIENT.allocator, sizeof(NYA_NetReplicaMap));
        nya_net_replica_map_clear(_NYA_NET_CLIENT.replicas);
    }

    /*
     * After the entities like the server's tick, and after the server's when there is one: a listen
     * server starts before its local client attaches, and systems no constraint separates keep the
     * order they were registered in. See nya_net_server_start for why this is here at all.
     *
     * Taken out first because a connection the server drops is reset from inside this very tick, where
     * a system may not remove itself from the registry: the entry can outlive the connection it was
     * made for, and reattaching is where that is noticed.
     */
    if (nya_app_get()->initialized) {
        nya_system_unregister("net_client");
        nya_system_register((NYA_SystemEntry){ .name = "net_client", .after = "entity", .tick = nya_callback(nya_net_client_system_tick) });
    }

    return NYA_OK;
}

void nya_net_client_disconnect(void) {
    // before the check: a connection the server already dropped left the entry behind, and this is
    // the first place outside the tick that can take it out. Idempotent, and free without an app.
    nya_system_unregister("net_client");

    if (!_NYA_NET_CLIENT.active) return;

    if (_NYA_NET_CLIENT.transport != nullptr && nya_net_peer_is_set(_NYA_NET_CLIENT.server_peer)) {
        nya_net_transport_disconnect(_NYA_NET_CLIENT.transport, _NYA_NET_CLIENT.server_peer, NYA_NET_DISCONNECT_REQUESTED);
    }

    // only if this client created it. A listen server's loopback end belongs to the server.
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
     */
    return _NYA_NET_CLIENT.entity_local;
}

NYA_EntityHandle nya_net_client_entity_remote(void) {
    return _NYA_NET_CLIENT.entity_remote;
}

NYA_EntityHandle nya_net_client_local_entity(NYA_EntityHandle remote) {
    // On a listen server there is one table and one world, so a server handle is already local.
    if (nya_net_transport_is_local(_NYA_NET_CLIENT.transport)) return remote;
    if (_NYA_NET_CLIENT.replicas == nullptr) return NYA_ENTITY_HANDLE_NONE;

    return nya_net_replica_local(_NYA_NET_CLIENT.replicas, remote);
}

NYA_NetPeerId nya_net_client_peer(void) {
    return _NYA_NET_CLIENT.peer;
}

NYA_NetPeerStats nya_net_client_stats(void) {
    if (!_NYA_NET_CLIENT.active || _NYA_NET_CLIENT.transport == nullptr) return (NYA_NetPeerStats){ 0 };

    NYA_NetPeerStats stats = nya_net_transport_stats(_NYA_NET_CLIENT.transport, _NYA_NET_CLIENT.server_peer);

    stats.snapshot_bytes         = _NYA_NET_CLIENT.snapshot_bytes;
    stats.interpolation_delay_ms = _NYA_NET_CLIENT.delay_ticks * (f32)nya_time_ns_to_s(nya_app_get()->options.time_step_ns) * 1000.0F;

    return stats;
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
    if (!_NYA_NET_CLIENT.active || _NYA_NET_CLIENT.state != NYA_NET_CLIENT_PLAYING) return;

    // nothing to smooth on a listen server: the entities are the server's own.
    if (nya_net_transport_is_local(_NYA_NET_CLIENT.transport) || _NYA_NET_CLIENT.server_tick == 0) return;
    if (!(delta_time_s > 0.0F)) return;

    f64 tick_ns = (f64)nya_app_get()->options.time_step_ns;
    if (tick_ns <= 0.0) return;

    f32 target_delay = nya_clamp(_NYA_NET_CLIENT.arrival_gap_ticks + (2.0F * _NYA_NET_CLIENT.arrival_jitter_ticks), _NYA_NET_CLIENT_DELAY_MIN_TICKS,
                                 _NYA_NET_CLIENT_DELAY_MAX_TICKS);

    // eased over about half a second, so a burst of jitter does not visibly lurch everything back.
    _NYA_NET_CLIENT.delay_ticks += (target_delay - _NYA_NET_CLIENT.delay_ticks) * nya_min(delta_time_s * 2.0F, 1.0F);

    // where the server is now, as far as the stream of snapshots says.
    f64 server_now = (f64)_NYA_NET_CLIENT.server_tick + ((f64)_nya_net_elapsed_ns(nya_clock_get_monotonic_ns(), _NYA_NET_CLIENT.arrival_ns) / tick_ns);
    f64 target     = server_now - (f64)_NYA_NET_CLIENT.delay_ticks;
    f64 advance    = (f64)delta_time_s * 1e9 / tick_ns;

    if (!_NYA_NET_CLIENT.render_started || fabs(target - _NYA_NET_CLIENT.render_tick) > _NYA_NET_CLIENT_RESYNC_TICKS) {
        _NYA_NET_CLIENT.render_tick    = target;
        _NYA_NET_CLIENT.render_started = true;
    } else {
        // a tenth faster or slower at most, so catching up never reads as fast forward.
        f64 drift = nya_clamp((target - (_NYA_NET_CLIENT.render_tick + advance)) * 0.1, -0.1, 0.1);

        _NYA_NET_CLIENT.render_tick += advance * (1.0 + drift);
    }

    f32 limit_s = (f32)(_NYA_NET_CLIENT.config.extrapolation_limit_ms == 0 ? NYA_NET_EXTRAPOLATION_LIMIT_MS_DEFAULT : _NYA_NET_CLIENT.config.extrapolation_limit_ms) / 1000.0F;

    nya_net_replica_interpolate(_NYA_NET_CLIENT.replicas, _NYA_NET_CLIENT.render_tick, (f32)(tick_ns / 1e9), limit_s, _NYA_NET_CLIENT.entity_remote);
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

                nya_log_info("Disconnected (%d).", (int)event.reason);

                // the transport belongs to whoever created it, and a caller may reconnect after reading the reason.
                NYA_NetDisconnect reason = event.reason;
                _nya_net_client_reset();
                _NYA_NET_CLIENT.disconnect_reason = reason;

                return;
            }

            case NYA_NET_TRANSPORT_EVENT_MESSAGE: {
                _nya_net_client_handle_message(event.data, event.size, delta_time_s);

                /*
                 * A message can end the connection, and the loop must stop when it does.
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
    nya_object_set(hello, "tick", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = _NYA_NET_CLIENT.local_tick });

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

            nya_log_info("Player '%s' %s.", name->as_string, joined ? "joined" : "left");

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
    NYA_Value* tick_ns           = nya_object_get(welcome, "tick_ns");

    if (peer_index == nullptr || peer_index->type != NYA_TYPE_U64) return;
    if (peer_generation == nullptr || peer_generation->type != NYA_TYPE_U64) return;

    _NYA_NET_CLIENT.peer = (NYA_NetPeerId){ .index = (u32)peer_index->as_u64, .generation = (u32)peer_generation->as_u64 };

    if (entity_index != nullptr && entity_index->type == NYA_TYPE_U64 && entity_generation != nullptr && entity_generation->type == NYA_TYPE_U64) {
        _NYA_NET_CLIENT.entity_remote = (NYA_EntityHandle){ .index = (u32)entity_index->as_u64, .generation = (u32)entity_generation->as_u64 };

        /*
         * On a listen server the two handle spaces are one, so the local name is known immediately.
         */
        if (nya_net_transport_is_local(_NYA_NET_CLIENT.transport)) _NYA_NET_CLIENT.entity_local = _NYA_NET_CLIENT.entity_remote;
    }

    /*
     * The server's replicated flag wins over the config's.
     */
    if (replicated_flag != nullptr && replicated_flag->type == NYA_TYPE_U64) {
        _NYA_NET_CLIENT.config.replicated_flag = replicated_flag->as_u64;
    }

    // the client ticks at the server's rate, so each command is one server tick of movement on both sides.
    NYA_App* app = nya_app_get();

    if (tick_ns != nullptr && tick_ns->type == NYA_TYPE_U64 && tick_ns->as_u64 >= NYA_NET_TICK_NS_MIN && tick_ns->as_u64 <= NYA_NET_TICK_NS_MAX
        && tick_ns->as_u64 != app->options.time_step_ns) {
        nya_log_info("Following the server's tick of %.2f ms.", (f64)tick_ns->as_u64 / 1e6);
        app->options.time_step_ns = tick_ns->as_u64;
    }

    _NYA_NET_CLIENT.state = NYA_NET_CLIENT_PLAYING;

    nya_log_info("Joined as peer %u, controlling server entity %u.", _NYA_NET_CLIENT.peer.index, _NYA_NET_CLIENT.entity_remote.index);
}

void _nya_net_client_handle_snapshot(const u8* body, u64 size, f32 delta_time_s) {
    // no predicted entity before the handshake, so a snapshot then is a stray.
    if (_NYA_NET_CLIENT.state != NYA_NET_CLIENT_PLAYING) return;

    u64 tick          = 0;
    u64 baseline_tick = 0;

    // older than what was applied, or a duplicate: applying it would move the world backwards.
    if (!nya_net_snapshot_peek(body, size, &tick, &baseline_tick)) return;
    if (tick <= _NYA_NET_CLIENT.server_tick) return;

    const NYA_NetSnapshot* baseline = nullptr;

    if (baseline_tick != 0) {
        const _NYA_NetClientBaseline* slot = &_NYA_NET_CLIENT.baselines[baseline_tick % NYA_NET_SNAPSHOT_HISTORY];

        // gone from the ring: this delta cannot be read, and the server falls back to a whole snapshot once its own ring moves on.
        if (slot->snapshot.tick != baseline_tick) return;

        baseline = &slot->snapshot;
    }

    NYA_NetSnapshot snapshot = { 0 };

    NYA_Error decoded = nya_net_snapshot_decode(_NYA_NET_CLIENT.tick_arena, body, size, baseline, &snapshot);

    if (!decoded.ok) {
        // dropped. a malformed snapshot is peer data, and the next one is a tick away.
        nya_log_debug("Discarding a malformed snapshot: %s", (NYA_ConstCString)decoded.message);
        return;
    }

    // a listen server's client shares the server's world, so it applies nothing.
    if (!nya_net_transport_is_local(_NYA_NET_CLIENT.transport)) {
        nya_net_snapshot_apply(&snapshot, _NYA_NET_CLIENT.config.replicated_flag, _NYA_NET_CLIENT.replicas, _NYA_NET_CLIENT.entity_remote);

        // the local name for the player, now that a snapshot may have spawned it.
        _NYA_NET_CLIENT.entity_local = nya_net_replica_local(_NYA_NET_CLIENT.replicas, _NYA_NET_CLIENT.entity_remote);
    }

    _nya_net_client_reconcile(&snapshot, delta_time_s);

    // kept after the reconcile, which reads from the tick arena copy, so the stored copy is free to reuse its slot's buffer.
    _NYA_NetClientBaseline* stored = &_NYA_NET_CLIENT.baselines[tick % NYA_NET_SNAPSHOT_HISTORY];

    if (stored->capacity < snapshot.entity_count) {
        if (stored->snapshot.entities != nullptr) nya_arena_free(_NYA_NET_CLIENT.allocator, stored->snapshot.entities, (u64)stored->capacity * sizeof(NYA_NetEntityState));

        stored->snapshot.entities = nya_arena_alloc(_NYA_NET_CLIENT.allocator, (u64)snapshot.entity_count * sizeof(NYA_NetEntityState));
        stored->capacity          = snapshot.entity_count;
    }

    NYA_NetEntityState* entities = stored->snapshot.entities;

    if (snapshot.entity_count > 0) nya_memcpy(entities, snapshot.entities, (u64)snapshot.entity_count * sizeof(NYA_NetEntityState));

    stored->snapshot          = snapshot;
    stored->snapshot.entities = entities;

    // how regularly snapshots come is what the interpolation delay is sized from.
    u64 now_ns  = nya_clock_get_monotonic_ns();
    f64 tick_ns = (f64)nya_app_get()->options.time_step_ns;

    if (_NYA_NET_CLIENT.server_tick != 0 && tick_ns > 0.0) {
        f32 gap     = (f32)(tick - _NYA_NET_CLIENT.server_tick);
        f32 arrived = (f32)((f64)_nya_net_elapsed_ns(now_ns, _NYA_NET_CLIENT.arrival_ns) / tick_ns);
        f32 off     = nya_min(fabsf(arrived - gap), _NYA_NET_CLIENT_DELAY_MAX_TICKS);

        _NYA_NET_CLIENT.arrival_gap_ticks    = (_NYA_NET_CLIENT.arrival_gap_ticks * 0.9F) + (nya_min(gap, _NYA_NET_CLIENT_DELAY_MAX_TICKS) * 0.1F);
        _NYA_NET_CLIENT.arrival_jitter_ticks = (_NYA_NET_CLIENT.arrival_jitter_ticks * 0.9F) + (off * 0.1F);
    }

    _NYA_NET_CLIENT.server_tick    = tick;
    _NYA_NET_CLIENT.snapshot_bytes = (u32)size;
    _NYA_NET_CLIENT.arrival_ns     = now_ns;
}

void _nya_net_client_reconcile(const NYA_NetSnapshot* snapshot, f32 delta_time_s) {
    // on a listen server there is nothing to reconcile.
    if (nya_net_transport_is_local(_NYA_NET_CLIENT.transport)) return;

    // looked up by the server's handle, the name a snapshot uses. The local handle could match an unrelated server entity.
    const NYA_NetEntityState* authoritative = nya_net_snapshot_find(snapshot, _NYA_NET_CLIENT.entity_remote);
    if (authoritative == nullptr) return;

    NYA_Entity* entity = nya_entity_get(_NYA_NET_CLIENT.entity_local);
    if (entity == nullptr) return;

    /*
     * The server's answer describes the player after command `command_tick`, so it is compared with where prediction
     * had the player after that same command, not with where the player is now, which is further along by every
     * command still in flight.
     */
    u64 acknowledged = snapshot->command_tick;

    const NYA_NetCommand* confirmed = &_NYA_NET_CLIENT.history[acknowledged % NYA_NET_COMMAND_HISTORY];

    b8 known = acknowledged != 0 && confirmed->tick == acknowledged;

    if (known) {
        f32x3 error = _NYA_NET_CLIENT.predicted[acknowledged % NYA_NET_COMMAND_HISTORY] - authoritative->position;

        // the server's answer is on the wire's fixed point grid, so nothing finer than one step of it is an error.
        u32 bits = snapshot->position_bits == 0 ? NYA_NET_POSITION_BITS_DEFAULT : snapshot->position_bits;

        f32 distance_squared = (error.x * error.x) + (error.y * error.y) + (error.z * error.z);
        f32 threshold        = nya_max(_NYA_NET_CLIENT.config.correction_threshold, 1.0F / (f32)(1U << bits));

        if (distance_squared <= threshold * threshold) return;

        _NYA_NET_CLIENT.correction_count++;
    }

    // snapped to the server's answer, then every command since replayed on top of it. Also when the server has not
    // confirmed a command this client still remembers, since then there is nothing to compare against.
    nya_net_entity_state_apply(entity, authoritative);

    NYA_NetApplyCommandFn apply_command = nya_callback_get(_NYA_NET_CLIENT.config.on_apply_command);
    if (apply_command == nullptr) return;

    u64 local_tick = _NYA_NET_CLIENT.local_tick;
    u64 from       = acknowledged + 1;

    // bounded by the ring: older slots have been overwritten.
    if (local_tick >= NYA_NET_COMMAND_HISTORY && from < local_tick - NYA_NET_COMMAND_HISTORY + 1) from = local_tick - NYA_NET_COMMAND_HISTORY + 1;

    for (u64 replay = from; replay < local_tick; replay++) {
        const NYA_NetCommand* command = &_NYA_NET_CLIENT.history[replay % NYA_NET_COMMAND_HISTORY];

        // a slot with a different tick was never filled or was overwritten. Replaying it is worse than skipping.
        if (command->tick != replay) continue;

        apply_command(entity, command, delta_time_s);

        _NYA_NET_CLIENT.predicted[replay % NYA_NET_COMMAND_HISTORY] = entity->position;
    }
}

void _nya_net_client_send_command(u64 tick, f32 delta_time_s) {
    NYA_NetCommand command = { .tick = tick };

    NYA_NetSampleCommandFn sample_command = nya_callback_get(_NYA_NET_CLIENT.config.on_sample_command);

    /*
     * A handle that no longer resolves is a reload that renamed or removed the game's sampler.
     */
    if (sample_command == nullptr) {
        nya_log_error("The client's on_sample_command no longer resolves; sending no commands.");
        return;
    }

    sample_command(&command);

    // stamped after sampling, so the tick has one source.
    command.tick = tick;

    _NYA_NET_CLIENT.history[tick % NYA_NET_COMMAND_HISTORY] = command;

    /* Applied locally before sending. */
    if (!nya_net_transport_is_local(_NYA_NET_CLIENT.transport)) {
        NYA_Entity* entity = nya_entity_get(_NYA_NET_CLIENT.entity_local);

        // null until the first snapshot spawns the player locally. Commands still go out, so the server
        // moves the player and the next snapshot catches the client up.
        NYA_NetApplyCommandFn apply_command = nya_callback_get(_NYA_NET_CLIENT.config.on_apply_command);

        if (entity != nullptr && apply_command != nullptr) {
            apply_command(entity, &command, delta_time_s);
            _NYA_NET_CLIENT.predicted[tick % NYA_NET_COMMAND_HISTORY] = entity->position;
        }
    }

    /*
     * The last several ticks go out together.
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

    // the newest applied snapshot rides along, so the server can delta against it without a packet of its own.
    nya_net_message_begin(payload, NYA_NET_MSG_COMMAND);
    _nya_net_write_varint(payload, _NYA_NET_CLIENT.server_tick);

    if (!nya_net_command_encode(payload, run, count).ok) return;

    (void)nya_net_transport_send(_NYA_NET_CLIENT.transport, _NYA_NET_CLIENT.server_peer, NYA_NET_CHANNEL_UNRELIABLE, payload->items, payload->length);
}

void _nya_net_client_reset(void) {
    /*
     * The replicated world goes with the connection.
     */
    if (_NYA_NET_CLIENT.replicas != nullptr) nya_net_replica_map_despawn_all(_NYA_NET_CLIENT.replicas);

    if (_NYA_NET_CLIENT.tick_arena != nullptr) nya_arena_destroy(_NYA_NET_CLIENT.tick_arena);
    if (_NYA_NET_CLIENT.allocator != nullptr) nya_arena_destroy(_NYA_NET_CLIENT.allocator);

    _NYA_NET_CLIENT = (_NYA_NetClientState){
        .entity_remote = NYA_ENTITY_HANDLE_NONE,
        .entity_local  = NYA_ENTITY_HANDLE_NONE,
        .peer          = NYA_NET_PEER_NONE,
    };
}

void nya_net_client_system_tick(f32 delta_time_s) {
    nya_net_client_tick(nya_world()->sim_system.tick, delta_time_s);
}

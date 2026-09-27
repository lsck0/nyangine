#include "nyangine-core/nyangine.h"

#ifdef NYA_TESTING

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

enum {
    /** Entities besides the player the server replicates, so a delta has something to leave out. */
    _NYA_TRAFFIC_NPC_MAX = 8,

    /** The entity type a non-player carries, so the world action finds them without the player. */
    _NYA_TRAFFIC_NPC_TYPE = 1,

    /** Messages the delay fault holds back at once. */
    _NYA_TRAFFIC_HELD_MAX = 16,

    /** Longest a message is held, in ticks: past NYA_NET_SNAPSHOT_HISTORY, so a late delta can outlive its baseline. */
    _NYA_TRAFFIC_DELAY_MAX_TICKS = NYA_NET_SNAPSHOT_HISTORY + 8,

    /** Clean ticks before convergence is checked: twice the 32 commands the server queues and applies one a tick. */
    _NYA_TRAFFIC_SETTLE_TICKS = 64,

    /** Passes an HTTP exchange may take, each waiting up to _NYA_TRAFFIC_HTTP_WAIT_MS, before the server is said to hang. */
    _NYA_TRAFFIC_HTTP_PASSES  = 400,
    _NYA_TRAFFIC_HTTP_WAIT_MS = 25,

    /** Room for the longest input a fault builds: a record twice over, for a duplicated or spliced request. */
    _NYA_TRAFFIC_BYTES_MAX = NYA_CAPTURE_RECORD_MAX_BYTES * 2,
};

/** The action bits the movement reads: right, left, up, down. */
enum { _NYA_TRAFFIC_RIGHT, _NYA_TRAFFIC_LEFT, _NYA_TRAFFIC_UP, _NYA_TRAFFIC_DOWN, _NYA_TRAFFIC_ACTION_COUNT };

/** The flag the session replicates under: high, so nothing the engine's own actions spawn carries it. */
static const u64 _NYA_TRAFFIC_REPLICATED = 1ULL << 40;

/** World units a second a held direction moves the player. */
static const f32 _NYA_TRAFFIC_SPEED = 120.0F;

/** How far a replica may sit from the server's entity: two steps of the wire's default fixed point grid. */
static const f32 _NYA_TRAFFIC_TOLERANCE = 2.0F / (f32)(1U << NYA_NET_POSITION_BITS_DEFAULT);

#define _NYA_TRAFFIC_HTTP_PATH "/simulation/echo"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A message the delay fault took off the wire, and when it goes back on. */
typedef struct {
    u8* data;
    u64 size;
    b8  to_client;
    u64 release_tick;
} _NYA_TrafficHeld;

/** One replicate session: a server, a client, the wire between them and a world each. Live while `arena` is set. */
typedef struct {
    /** The wire's messages and the held ones. Destroyed with the session. */
    NYA_Arena* arena;

    NYA_World*        server_world;
    NYA_World*        client_world;
    NYA_NetTransport* server_end;
    NYA_NetTransport* client_end;

    /** The capture's number for this session's wire. */
    u32 number;

    u64 tick;

    /** Which half of the tick comes next. Halves, so traffic both ways is in flight between two actions. */
    b8 client_turn;

    u64 held_actions;

    /** The client was handed bytes no honest server sends. It must survive them, not agree with them. */
    b8 hostile;

    _NYA_TrafficHeld held[_NYA_TRAFFIC_HELD_MAX];
    u32              held_count;

    /** Armed by a snapshot cut short: the rejected count the client must reach by its next drain. Zero is unarmed. */
    u64 rejected_expected;
} _NYA_TrafficSession;

typedef struct {
    b8 initialized;

    _NYA_TrafficSession session;

    /** Whether the HTTP server came up. Without a free port the HTTP actions do nothing. */
    b8  http_available;
    u16 http_port;

    /** Request bodies are serialised here, reset per request. */
    NYA_Arena* scratch;

    /** What a fault builds before it is sent. */
    u8 bytes[_NYA_TRAFFIC_BYTES_MAX];
} _NYA_SimulationTraffic;

NYA_INTERNAL _NYA_SimulationTraffic _NYA_TRAFFIC = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * THE GAME
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_traffic_apply_command(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
    f32x3 direction = { 0.0F, 0.0F, 0.0F };

    if (nya_net_command_holds(command, _NYA_TRAFFIC_RIGHT)) direction.x += 1.0F;
    if (nya_net_command_holds(command, _NYA_TRAFFIC_LEFT)) direction.x -= 1.0F;
    if (nya_net_command_holds(command, _NYA_TRAFFIC_UP)) direction.y += 1.0F;
    if (nya_net_command_holds(command, _NYA_TRAFFIC_DOWN)) direction.y -= 1.0F;

    entity->position += direction * _NYA_TRAFFIC_SPEED * delta_time_s;
}

NYA_INTERNAL void _nya_traffic_sample_command(OUT NYA_NetCommand* command) {
    command->actions = _NYA_TRAFFIC.session.held_actions;
}

NYA_INTERNAL NYA_EntityHandle _nya_traffic_spawn_player(NYA_NetPeerId peer, NYA_ConstCString name) {
    nya_unused(peer, name);

    return nya_entity_spawn(.name = "traffic_player", .flags = _NYA_TRAFFIC_REPLICATED);
}

NYA_INTERNAL void _nya_traffic_despawn_player(NYA_NetPeerId peer, NYA_EntityHandle entity) {
    nya_unused(peer);

    nya_entity_despawn(entity);
}

/*
 * ─────────────────────────────────────────────────────────
 * THE WIRE
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_ArrayᐸNYA_NetLoopbackMessageᐳ* _nya_traffic_inbox(NYA_SimulationRun* run, OUT b8* out_to_client) {
    _NYA_TrafficSession* session = &_NYA_TRAFFIC.session;

    *out_to_client = nya_simulation_chance(run, 50);

    return nya_net_loopback_inbox(*out_to_client ? session->client_end : session->server_end);
}

NYA_INTERNAL b8 _nya_traffic_unreliable_pick(NYA_SimulationRun* run, NYA_ArrayᐸNYA_NetLoopbackMessageᐳ* inbox, OUT u64* out_index) {
    // only the unreliable channel: a reliable message dropped or reordered is a transport broken, not a network.
    u64 count = 0;
    nya_array_foreach (inbox, message) count += message->channel == NYA_NET_CHANNEL_UNRELIABLE ? 1 : 0;

    if (count == 0) return false;

    u64 wanted = nya_simulation_below(run, count);

    for (u64 i = 0; i < inbox->length; i++) {
        if (inbox->items[i].channel != NYA_NET_CHANNEL_UNRELIABLE) continue;
        if (wanted == 0) {
            *out_index = i;
            return true;
        }

        wanted--;
    }

    nya_unreachable();
}

NYA_INTERNAL void _nya_traffic_inject(b8 to_client, const u8* data, u64 size) {
    _NYA_TrafficSession* session = &_NYA_TRAFFIC.session;
    nya_assert(size > 0, "a message is never empty; see nya_net_transport_send");

    NYA_NetTransport* transport = to_client ? session->client_end : session->server_end;

    // out of the allocator a poll frees it back to.
    u8* copy = nya_arena_alloc(transport->allocator, size);
    nya_memcpy(copy, data, size);

    nya_array_push_back(nya_net_loopback_inbox(transport), ((NYA_NetLoopbackMessage){ .data = copy, .size = size, .channel = NYA_NET_CHANNEL_UNRELIABLE }));
}

/** A record drawn from `capture` whose lane is `lane` (any when negative) and whose session is not `session` (any when zero). */
NYA_INTERNAL const NYA_CaptureRecord* _nya_traffic_record_pick(NYA_SimulationRun* run, const NYA_Capture* capture, s64 lane, u32 session) {
    u64 held  = nya_capture_count(capture);
    u64 count = 0;

    for (u64 i = 0; i < held; i++) {
        const NYA_CaptureRecord* record = nya_capture_at(capture, i);
        if ((lane < 0 || record->lane == (u32)lane) && (session == 0 || record->session != session) && record->size > 0) count++;
    }

    if (count == 0) return nullptr;

    u64 wanted = nya_simulation_below(run, count);

    for (u64 i = 0; i < held; i++) {
        const NYA_CaptureRecord* record = nya_capture_at(capture, i);
        if (!((lane < 0 || record->lane == (u32)lane) && (session == 0 || record->session != session) && record->size > 0)) continue;
        if (wanted == 0) return record;
        wanted--;
    }

    nya_unreachable();
}

/** Flips one to eight drawn bits of `data`. */
NYA_INTERNAL void _nya_traffic_flip(NYA_SimulationRun* run, u8* data, u64 size) {
    nya_assert(size > 0);

    u64 flips = 1 + nya_simulation_below(run, 8);

    for (u64 i = 0; i < flips; i++) data[nya_simulation_below(run, size)] ^= (u8)(1U << nya_simulation_below(run, 8));
}

/** The tick a snapshot message carries, or false when it is not one or does not peek. */
NYA_INTERNAL b8 _nya_traffic_snapshot_tick(const u8* data, u64 size, OUT u64* out_tick) {
    u64 body_offset = 0;
    if (nya_net_message_kind(data, size, &body_offset) != NYA_NET_MSG_SNAPSHOT) return false;

    u64 baseline_tick = 0;

    return nya_net_snapshot_peek(data + body_offset, size - body_offset, out_tick, &baseline_tick);
}

/*
 * ─────────────────────────────────────────────────────────
 * THE SESSION
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_traffic_session_open(NYA_SimulationRun* run) {
    _NYA_TrafficSession* session = &_NYA_TRAFFIC.session;
    nya_assert(session->arena == nullptr, "a session is already open");

    *session = (_NYA_TrafficSession){
        .arena        = nya_arena_create(.name = "simulation_traffic"),
        .server_world = nya_world_create(),
        .client_world = nya_world_create(),
        .tick         = 1,
    };

    NYA_EXPECT(nya_net_transport_loopback_wire_create(session->arena, &session->server_end, &session->client_end));
    session->number = nya_net_loopback_session(session->server_end);

    // max_speed past the diagonal, so the rule only ever catches movement no honest command makes.
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
        .replicated_flag   = _NYA_TRAFFIC_REPLICATED,
        .max_speed         = _NYA_TRAFFIC_SPEED * 1.5F,
        .on_spawn_player   = nya_callback(_nya_traffic_spawn_player),
        .on_despawn_player = nya_callback(_nya_traffic_despawn_player),
        .on_apply_command  = nya_callback(_nya_traffic_apply_command),
    }));
    NYA_EXPECT(nya_net_server_listen_transport(session->server_end));

    {
        NYA_World* previous = nya_world_set(session->client_world);
        defer (void)nya_world_set(previous);

        NYA_EXPECT(nya_net_client_attach(session->client_end, "simulated", (NYA_NetClientConfig){
            .replicated_flag   = _NYA_TRAFFIC_REPLICATED,
            .on_apply_command  = nya_callback(_nya_traffic_apply_command),
            .on_sample_command = nya_callback(_nya_traffic_sample_command),
        }));
    }

    NYA_World* previous = nya_world_set(session->server_world);
    defer (void)nya_world_set(previous);

    u64 count = 1 + nya_simulation_below(run, _NYA_TRAFFIC_NPC_MAX);

    for (u64 i = 0; i < count; i++) {
        f32x3 position = { nya_simulation_range_f32(run, -256.0F, 256.0F), nya_simulation_range_f32(run, -256.0F, 256.0F), 0.0F };
        (void)nya_entity_spawn(.name = "traffic_npc", .type = _NYA_TRAFFIC_NPC_TYPE, .flags = _NYA_TRAFFIC_REPLICATED, .position = position);
    }
}

NYA_INTERNAL void _nya_traffic_session_close(void) {
    _NYA_TrafficSession* session = &_NYA_TRAFFIC.session;
    if (session->arena == nullptr) return;

    {
        NYA_World* previous = nya_world_set(session->client_world);
        defer (void)nya_world_set(previous);

        nya_net_client_disconnect();
    }

    {
        NYA_World* previous = nya_world_set(session->server_world);
        defer (void)nya_world_set(previous);

        // destroys the server's end of the wire with it.
        nya_net_server_stop();
    }

    nya_net_transport_destroy(session->client_end);

    nya_world_destroy(session->server_world);
    nya_world_destroy(session->client_world);
    nya_arena_destroy(session->arena);

    *session = (_NYA_TrafficSession){ 0 };
}

/** Puts back what the delay fault held for the side about to tick, or everything when `all`. */
NYA_INTERNAL void _nya_traffic_release(b8 to_client, b8 all) {
    _NYA_TrafficSession* session = &_NYA_TRAFFIC.session;

    u32 kept = 0;

    for (u32 i = 0; i < session->held_count; i++) {
        _NYA_TrafficHeld* held = &session->held[i];

        b8 due = all || (held->to_client == to_client && held->release_tick <= session->tick);

        if (!due) {
            session->held[kept++] = *held;
            continue;
        }

        _nya_traffic_inject(held->to_client, held->data, held->size);

        NYA_NetTransport* transport = held->to_client ? session->client_end : session->server_end;
        nya_arena_free(transport->allocator, held->data, held->size);
    }

    session->held_count = kept;
    nya_assert(!all || kept == 0);
}

/** One half of a tick, the server's or the client's, in its own world. */
NYA_INTERNAL void _nya_traffic_step(NYA_SimulationRun* run) {
    _NYA_TrafficSession* session = &_NYA_TRAFFIC.session;
    nya_assert(session->arena != nullptr);

    f32 delta_time_s = nya_simulation_delta_s(run);

    _nya_traffic_release(session->client_turn, false);

    if (!session->client_turn) {
        NYA_World* previous = nya_world_set(session->server_world);
        defer (void)nya_world_set(previous);

        nya_net_server_tick(session->tick, delta_time_s);
        nya_system_sim_apply_commands();

        session->client_turn = true;
        return;
    }

    {
        NYA_World* previous = nya_world_set(session->client_world);
        defer (void)nya_world_set(previous);

        nya_net_client_tick(session->tick, delta_time_s);
        nya_system_sim_apply_commands();
    }

    if (session->rejected_expected != 0) {
        u64 rejected = nya_net_client_stats().packets_rejected;

        // a client the same drain disconnected has nothing left to count with.
        if (nya_net_client_state() == NYA_NET_CLIENT_PLAYING && rejected < session->rejected_expected) {
            nya_simulation_fail(run, "a snapshot cut short reached the client and its rejected count stayed at %llu", (unsigned long long)rejected);
        }

        session->rejected_expected = 0;
    }

    session->client_turn = false;
    session->tick++;
}

/** Checks what a session that ended agrees on, then starts another. */
NYA_INTERNAL void _nya_traffic_session_restart(NYA_SimulationRun* run) {
    _NYA_TrafficSession* session = &_NYA_TRAFFIC.session;

    // a client that left is a peer the server let go of too, unless a hostile server talked it into leaving.
    if (!session->hostile && nya_net_client_state() == NYA_NET_CLIENT_DISCONNECTED && nya_net_server_peer_count() != 0) {
        nya_simulation_fail(run, "the client left and the server still holds %u peers", nya_net_server_peer_count());
    }

    _nya_traffic_session_close();
    _nya_traffic_session_open(run);
}

/** Every entity the server replicates has a replica where the server has it, and no replica is extra. */
NYA_INTERNAL void _nya_traffic_check_converged(NYA_SimulationRun* run) {
    _NYA_TrafficSession* session = &_NYA_TRAFFIC.session;

    NYA_EntityHandle handles[_NYA_TRAFFIC_NPC_MAX + 1];
    f32x3            positions[_NYA_TRAFFIC_NPC_MAX + 1];
    u32              count = 0;

    {
        NYA_World* previous = nya_world_set(session->server_world);
        defer (void)nya_world_set(previous);

        nya_entity_foreach_flags (_NYA_TRAFFIC_REPLICATED, entity) {
            nya_assert(count < nya_carray_length(handles), "more replicated entities than the session spawns");

            handles[count]   = entity->handle;
            positions[count] = entity->position;
            count++;
        }
    }

    NYA_World* previous = nya_world_set(session->client_world);
    defer (void)nya_world_set(previous);

    u32 replicas = 0;
    nya_entity_foreach_flags (_NYA_TRAFFIC_REPLICATED, entity) replicas++;

    if (replicas != count) nya_simulation_fail(run, "the server replicates %u entities and the client holds %u after the faults stopped", count, replicas);

    for (u32 i = 0; i < count; i++) {
        NYA_Entity* replica = nya_entity_get(nya_net_client_local_entity(handles[i]));

        if (replica == nullptr) {
            nya_simulation_fail(run, "server entity %u has no replica after the faults stopped", handles[i].index);
            continue;
        }

        f32x3 error = replica->position - positions[i];

        if (fabsf(error.x) > _NYA_TRAFFIC_TOLERANCE || fabsf(error.y) > _NYA_TRAFFIC_TOLERANCE || fabsf(error.z) > _NYA_TRAFFIC_TOLERANCE) {
            nya_simulation_fail(run, "server entity %u is at (%f, %f) and its replica at (%f, %f) after the faults stopped", handles[i].index,
                                (f64)positions[i].x, (f64)positions[i].y, (f64)replica->position.x, (f64)replica->position.y);
        }
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * HTTP
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_HttpStatus _nya_traffic_http_query(NYA_HttpExchange* exchange) {
    char text[64] = { 0 };

    if (!nya_http_request_query_param(exchange->request, "text", text, sizeof(text))) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_BAD_REQUEST, "no text to answer");
    }

    NYA_Error written = nya_http_response_text(exchange->response, text, NYA_HTTP_MEDIA_TEXT);
    if (!written.ok) return nya_http_response_error(exchange, written);

    return NYA_HTTP_STATUS_OK;
}

NYA_INTERNAL NYA_HttpStatus _nya_traffic_http_echo(NYA_HttpExchange* exchange) {
    NYA_Object* document = nullptr;

    NYA_Error read = nya_http_request_document(exchange->request, exchange->arena, &document);
    if (!read.ok) return nya_http_response_error(exchange, read);

    NYA_Error written = nya_http_response_document(exchange->response, exchange->arena, document, nya_http_request_accepts(exchange->request));
    if (!written.ok) return nya_http_response_error(exchange, written);

    return NYA_HTTP_STATUS_OK;
}

NYA_INTERNAL const NYA_HttpRoute _NYA_TRAFFIC_HTTP_ROUTES[] = {
    {
     .method   = NYA_HTTP_METHOD_GET,
     .path     = _NYA_TRAFFIC_HTTP_PATH,
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = _nya_traffic_http_query,
     .summary  = "Answers the `text` query parameter",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
    {
     .method   = NYA_HTTP_METHOD_POST,
     .path     = _NYA_TRAFFIC_HTTP_PATH,
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = _nya_traffic_http_echo,
     .summary  = "Answers the body as a document, in whichever format was accepted",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_FORBIDDEN, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

NYA_INTERNAL const NYA_HttpRouter _NYA_TRAFFIC_HTTP_ROUTER = {
    .name        = "simulation",
    .routes      = _NYA_TRAFFIC_HTTP_ROUTES,
    .route_count = nya_carray_length(_NYA_TRAFFIC_HTTP_ROUTES),
};

/** A request an honest client sends, drawn. Its size. */
NYA_INTERNAL u64 _nya_traffic_http_request(NYA_SimulationRun* run, OUT u8* out, u64 capacity) {
    NYA_ConstCString connection = nya_simulation_chance(run, 50) ? "keep-alive" : "close";

    static const NYA_ConstCString ACCEPTS[] = { "application/json", "application/nya", "application/nya-binary" };
    NYA_ConstCString              accept    = ACCEPTS[nya_simulation_below(run, nya_carray_length(ACCEPTS))];

    s32 written = 0;

    switch (nya_simulation_below(run, 4)) {
        case 0:
        case 1: {
            NYA_ConstCString method = nya_simulation_chance(run, 50) ? "GET" : "HEAD";

            written = snprintf((char*)out, capacity, "%s " _NYA_TRAFFIC_HTTP_PATH "?text=step%llu HTTP/1.1\r\nHost: simulation\r\nConnection: %s\r\n\r\n", method,
                               (unsigned long long)run->step, connection);
        } break;

        case 2: {
            written = snprintf((char*)out, capacity, "GET " NYA_HTTP_HEALTHZ_PATH " HTTP/1.1\r\nHost: simulation\r\nConnection: %s\r\n\r\n", connection);
        } break;

        default: {
            nya_arena_free_all(_NYA_TRAFFIC.scratch);

            NYA_Object* object = nya_object_create(_NYA_TRAFFIC.scratch);
            nya_object_add(object, "step", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = run->step });
            nya_object_add(object, "ratio", (NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = nya_simulation_range_f32(run, -1.0F, 1.0F) });
            nya_object_add(object, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"simulated" });

            static const NYA_SerdeFormat  FORMATS[] = { NYA_SERDE_FORMAT_JSON, NYA_SERDE_FORMAT_NYA, NYA_SERDE_FORMAT_NYA_BINARY };
            u64                           format    = nya_simulation_below(run, nya_carray_length(FORMATS));
            NYA_String*                   body      = nya_serialize(_NYA_TRAFFIC.scratch, object, FORMATS[format], NYA_SERDE_NONE);

            nya_assert(body != nullptr && body->length > 0, "a three field object did not serialise");

            written = snprintf((char*)out, capacity,
                               "POST " _NYA_TRAFFIC_HTTP_PATH " HTTP/1.1\r\nHost: simulation\r\nConnection: %s\r\nContent-Type: %s\r\nAccept: %s\r\n"
                               "Content-Length: %llu\r\n\r\n",
                               connection, ACCEPTS[format], accept, (unsigned long long)body->length);

            nya_assert(written > 0 && (u64)written + body->length <= capacity, "a request past the %llu bytes a fault has room for", (unsigned long long)capacity);

            nya_memcpy(out + written, body->items, body->length);
            written += (s32)body->length;
        } break;
    }

    nya_assert(written > 0 && (u64)written <= capacity);

    return (u64)written;
}

/** Sends `data` over a fresh connection, `split` bytes of it a pass before the rest, and reads to the end. The status, or zero for none. */
NYA_INTERNAL u32 _nya_traffic_http_exchange(NYA_SimulationRun* run, const u8* data, u64 size, u64 split) {
    nya_assert(split <= size);

    NYA_OsAddress address = { 0 };
    nya_assert(nya_os_address_resolve("127.0.0.1", _NYA_TRAFFIC.http_port, NYA_OS_ADDRESS_V4, &address) == NYA_OS_SOCKET_OK);

    NYA_OsSocket       socket    = NYA_OS_SOCKET_NONE;
    NYA_OsSocketStatus connected = nya_os_socket_connect(address, &socket);
    defer nya_os_socket_close(socket);

    if (connected != NYA_OS_SOCKET_OK && connected != NYA_OS_SOCKET_WOULD_BLOCK) {
        nya_simulation_fail(run, "the HTTP server refused a connection");
        return 0;
    }

    NYA_OsSocketWait writable = { .socket = socket, .writable = true };
    u32              ready    = 0;
    (void)nya_os_socket_wait(&writable, 1, 1000, &ready);

    // the host takes a few kilobytes on loopback whole; a send that does not is the peer having closed already, which
    // is the server's answer to the first part and not a failure.
    u64 sent = 0;
    (void)nya_os_socket_send(socket, data, split, &sent);
    nya_system_http_tick();
    if (size > split) (void)nya_os_socket_send(socket, data + split, size - split, &sent);

    // the whole request is out, so the server reads the end of the stream after it and closes once it has answered.
    nya_os_socket_close_send(socket);

    char head[16] = { 0 };
    u64  head_size = 0;
    b8   closed    = false;

    for (u32 pass = 0; pass < _NYA_TRAFFIC_HTTP_PASSES && !closed; pass++) {
        nya_system_http_tick();

        u8                 chunk[4096];
        u64                read   = 0;
        NYA_OsSocketStatus status = nya_os_socket_receive(socket, chunk, sizeof(chunk), &read);

        if (status == NYA_OS_SOCKET_OK) {
            for (u64 i = 0; i < read && head_size + 1 < sizeof(head); i++) head[head_size++] = (char)chunk[i];
        } else if (status == NYA_OS_SOCKET_WOULD_BLOCK) {
            NYA_OsSocketWait readable = { .socket = socket, .readable = true };
            (void)nya_os_socket_wait(&readable, 1, _NYA_TRAFFIC_HTTP_WAIT_MS, &ready);
        } else {
            // the end of the stream, or a reset from a server that closed with bytes unread: either way it let go.
            closed = true;
        }
    }

    if (!closed) {
        nya_simulation_fail(run, "the HTTP server neither answered nor closed a %llu byte request", (unsigned long long)size);
        return 0;
    }

    if (nya_http_server_connection_count() != 0) nya_simulation_fail(run, "the HTTP server closed a connection and still holds its slot");

    if (head_size == 0) return 0;

    if (head_size < 12 || strncmp(head, "HTTP/1.1 ", 9) != 0) {
        nya_simulation_fail(run, "the HTTP server answered with something that is not a status line");
        return 0;
    }

    return (u32)strtoul(head + 9, nullptr, 10);
}

/** Replays `size` bytes of mutated input, which the server may refuse but must not blame on itself. */
NYA_INTERNAL void _nya_traffic_http_hostile(NYA_SimulationRun* run, u64 size, u64 split) {
    u32 status = _nya_traffic_http_exchange(run, _NYA_TRAFFIC.bytes, size, split);

    // the bytes are the client's fault whatever they are, so a 500 says the server blamed itself for them. 501 and 505
    // are the grammar's answers to a method and a version this server does not speak, and are right.
    if (status == NYA_HTTP_STATUS_INTERNAL_ERROR) nya_simulation_fail(run, "the HTTP server answered %llu bytes of hostile input with %u", (unsigned long long)size, status);
}

/*
 * ─────────────────────────────────────────────────────────
 * ACTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_traffic_do_tick(NYA_SimulationRun* run) {
    _nya_traffic_step(run);

    // a connection that ended is looked at once and replaced, which is also how the faults get sessions to splice between.
    if (!_NYA_TRAFFIC.session.client_turn && nya_net_client_state() == NYA_NET_CLIENT_DISCONNECTED) _nya_traffic_session_restart(run);
}

NYA_INTERNAL void _nya_traffic_do_input(NYA_SimulationRun* run) {
    _NYA_TRAFFIC.session.held_actions = nya_simulation_below(run, 1ULL << _NYA_TRAFFIC_ACTION_COUNT);
}

NYA_INTERNAL void _nya_traffic_do_world(NYA_SimulationRun* run) {
    NYA_World* previous = nya_world_set(_NYA_TRAFFIC.session.server_world);
    defer (void)nya_world_set(previous);

    NYA_EntityHandle npcs[_NYA_TRAFFIC_NPC_MAX];
    u32              count = 0;

    nya_entity_foreach_kind (_NYA_TRAFFIC_NPC_TYPE, entity) {
        if (count < _NYA_TRAFFIC_NPC_MAX) npcs[count++] = entity->handle;
    }

    f32x3 position = { nya_simulation_range_f32(run, -256.0F, 256.0F), nya_simulation_range_f32(run, -256.0F, 256.0F), 0.0F };

    switch (nya_simulation_below(run, 4)) {
        case 0: {
            if (count < _NYA_TRAFFIC_NPC_MAX) {
                (void)nya_entity_spawn(.name = "traffic_npc", .type = _NYA_TRAFFIC_NPC_TYPE, .flags = _NYA_TRAFFIC_REPLICATED, .position = position);
            }
        } break;

        case 1: {
            if (count > 0) nya_entity_despawn(npcs[nya_simulation_below(run, count)]);
        } break;

        default: {
            NYA_Entity* entity = count > 0 ? nya_entity_get(npcs[nya_simulation_below(run, count)]) : nullptr;
            if (entity != nullptr) entity->position = position;
        } break;
    }
}

NYA_INTERNAL void _nya_traffic_do_converge(NYA_SimulationRun* run) {
    _NYA_TrafficSession* session = &_NYA_TRAFFIC.session;

    // what the delay fault held arrives late, and then the network is clean and the player stands still.
    _nya_traffic_release(false, true);
    session->held_actions = 0;

    for (u32 i = 0; i < _NYA_TRAFFIC_SETTLE_TICKS * 2; i++) _nya_traffic_step(run);

    NYA_NetClientState state = nya_net_client_state();

    if (session->hostile || state == NYA_NET_CLIENT_DISCONNECTED) {
        _nya_traffic_session_restart(run);
        return;
    }

    // only reliable messages carry the handshake, and no network fault touches those.
    if (state != NYA_NET_CLIENT_PLAYING) {
        nya_simulation_fail(run, "the client is still in state %u after %d clean ticks", (u32)state, _NYA_TRAFFIC_SETTLE_TICKS);
        _nya_traffic_session_restart(run);
        return;
    }

    if (nya_net_server_peer_count() != 1) nya_simulation_fail(run, "the client is playing and the server holds %u peers", nya_net_server_peer_count());

    _nya_traffic_check_converged(run);
}

NYA_INTERNAL void _nya_traffic_do_http(NYA_SimulationRun* run) {
    if (!_NYA_TRAFFIC.http_available) return;

    u64 size   = _nya_traffic_http_request(run, _NYA_TRAFFIC.bytes, sizeof(_NYA_TRAFFIC.bytes));
    u32 status = _nya_traffic_http_exchange(run, _NYA_TRAFFIC.bytes, size, size);

    if (status != NYA_HTTP_STATUS_OK) nya_simulation_fail(run, "the HTTP server answered an honest request with %u", status);
}

/*
 * ─────────────────────────────────────────────────────────
 * NETWORK FAULTS
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_traffic_fault_net_drop(NYA_SimulationRun* run) {
    b8                                  to_client = false;
    NYA_ArrayᐸNYA_NetLoopbackMessageᐳ* inbox     = _nya_traffic_inbox(run, &to_client);

    u64 index = 0;
    if (!_nya_traffic_unreliable_pick(run, inbox, &index)) return;

    NYA_NetLoopbackMessage dropped = nya_array_remove(inbox, index);

    // it may be the cut snapshot the rejected count is waiting on, which now never arrives.
    if (to_client) _NYA_TRAFFIC.session.rejected_expected = 0;

    NYA_NetTransport* transport = to_client ? _NYA_TRAFFIC.session.client_end : _NYA_TRAFFIC.session.server_end;
    nya_arena_free(transport->allocator, dropped.data, dropped.size);
}

NYA_INTERNAL void _nya_traffic_fault_net_duplicate(NYA_SimulationRun* run) {
    b8                                  to_client = false;
    NYA_ArrayᐸNYA_NetLoopbackMessageᐳ* inbox     = _nya_traffic_inbox(run, &to_client);

    u64 index = 0;
    if (!_nya_traffic_unreliable_pick(run, inbox, &index)) return;

    _nya_traffic_inject(to_client, inbox->items[index].data, inbox->items[index].size);
}

NYA_INTERNAL void _nya_traffic_fault_net_reorder(NYA_SimulationRun* run) {
    b8                                  to_client = false;
    NYA_ArrayᐸNYA_NetLoopbackMessageᐳ* inbox     = _nya_traffic_inbox(run, &to_client);

    u64 first  = 0;
    u64 second = 0;
    if (!_nya_traffic_unreliable_pick(run, inbox, &first) || !_nya_traffic_unreliable_pick(run, inbox, &second)) return;

    NYA_NetLoopbackMessage swapped = inbox->items[first];
    inbox->items[first]            = inbox->items[second];
    inbox->items[second]           = swapped;
}

NYA_INTERNAL void _nya_traffic_fault_net_delay(NYA_SimulationRun* run) {
    _NYA_TrafficSession* session = &_NYA_TRAFFIC.session;
    if (session->held_count == _NYA_TRAFFIC_HELD_MAX) return;

    b8                                  to_client = false;
    NYA_ArrayᐸNYA_NetLoopbackMessageᐳ* inbox     = _nya_traffic_inbox(run, &to_client);

    u64 index = 0;
    if (!_nya_traffic_unreliable_pick(run, inbox, &index)) return;

    // the bytes stay where they are, in the allocator the poll would have freed them to, until the release does.
    NYA_NetLoopbackMessage held = nya_array_remove(inbox, index);

    // it may be the cut snapshot the rejected count is waiting on, which now arrives after its successors.
    if (to_client) session->rejected_expected = 0;

    session->held[session->held_count++] = (_NYA_TrafficHeld){
        .data         = held.data,
        .size         = held.size,
        .to_client    = to_client,
        .release_tick = session->tick + 1 + nya_simulation_below(run, _NYA_TRAFFIC_DELAY_MAX_TICKS),
    };
}

NYA_INTERNAL void _nya_traffic_fault_net_flip(NYA_SimulationRun* run) {
    b8 to_client = nya_simulation_chance(run, 50);

    // lane zero is what the server's end sent, so it is what the client's end receives.
    const NYA_CaptureRecord* record = _nya_traffic_record_pick(run, nya_net_loopback_capture(), to_client ? 0 : 1, 0);
    if (record == nullptr) return;

    nya_memcpy(_NYA_TRAFFIC.bytes, record->bytes, record->size);
    _nya_traffic_flip(run, _NYA_TRAFFIC.bytes, record->size);

    _nya_traffic_inject(to_client, _NYA_TRAFFIC.bytes, record->size);
    _NYA_TRAFFIC.session.hostile |= to_client;
}

NYA_INTERNAL void _nya_traffic_fault_net_truncate(NYA_SimulationRun* run) {
    _NYA_TrafficSession* session = &_NYA_TRAFFIC.session;

    b8                                  to_client = false;
    NYA_ArrayᐸNYA_NetLoopbackMessageᐳ* inbox     = _nya_traffic_inbox(run, &to_client);

    if (inbox->length == 0) return;

    u64                     index   = nya_simulation_below(run, inbox->length);
    NYA_NetLoopbackMessage* message = &inbox->items[index];

    if (message->size < 2) return;

    u64 keep = 1 + nya_simulation_below(run, message->size - 1);

    /*
     * The rejected count must move when the cut snapshot is the newest the client will see, whatever else is in
     * flight or held for it: an older one arriving after it would be stale and dropped without being read.
     */
    u64 tick = 0;

    if (to_client && nya_net_client_state() == NYA_NET_CLIENT_PLAYING && _nya_traffic_snapshot_tick(message->data, message->size, &tick)
        && tick > nya_net_client_server_tick()) {
        b8 newest = true;

        for (u64 i = 0; i < inbox->length; i++) {
            u64 other = 0;
            if (i != index && _nya_traffic_snapshot_tick(inbox->items[i].data, inbox->items[i].size, &other) && other >= tick) newest = false;
        }

        for (u32 i = 0; i < session->held_count; i++) {
            u64 other = 0;
            if (session->held[i].to_client && _nya_traffic_snapshot_tick(session->held[i].data, session->held[i].size, &other) && other >= tick) newest = false;
        }

        if (newest) session->rejected_expected = nya_net_client_stats().packets_rejected + 1;
    }

    NYA_NetTransport* transport = to_client ? session->client_end : session->server_end;

    // a fresh allocation of the kept size, so the poll frees exactly what it was handed.
    u8* cut = nya_arena_alloc(transport->allocator, keep);
    nya_memcpy(cut, message->data, keep);
    nya_arena_free(transport->allocator, message->data, message->size);

    message->data = cut;
    message->size = keep;

    session->hostile |= to_client;
}

NYA_INTERNAL void _nya_traffic_fault_net_splice(NYA_SimulationRun* run) {
    b8 to_client = nya_simulation_chance(run, 50);

    // genuine bytes, from a session that is over: a replay across connections, which only the session keys stop on a real wire.
    const NYA_CaptureRecord* record = _nya_traffic_record_pick(run, nya_net_loopback_capture(), to_client ? 0 : 1, _NYA_TRAFFIC.session.number);
    if (record == nullptr) return;

    _nya_traffic_inject(to_client, record->bytes, record->size);
    _NYA_TRAFFIC.session.hostile |= to_client;
}

NYA_INTERNAL void _nya_traffic_fault_net_restart(NYA_SimulationRun* run) {
    // crash-only, at a moment nobody chose: both ends go with whatever was in flight.
    _nya_traffic_session_close();
    _nya_traffic_session_open(run);
}

/*
 * ─────────────────────────────────────────────────────────
 * HTTP FAULTS
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_traffic_fault_http_flip(NYA_SimulationRun* run) {
    const NYA_CaptureRecord* record = _NYA_TRAFFIC.http_available ? _nya_traffic_record_pick(run, nya_http_server_capture(), -1, 0) : nullptr;
    if (record == nullptr) return;

    nya_memcpy(_NYA_TRAFFIC.bytes, record->bytes, record->size);
    _nya_traffic_flip(run, _NYA_TRAFFIC.bytes, record->size);

    _nya_traffic_http_hostile(run, record->size, record->size);
}

NYA_INTERNAL void _nya_traffic_fault_http_truncate(NYA_SimulationRun* run) {
    const NYA_CaptureRecord* record = _NYA_TRAFFIC.http_available ? _nya_traffic_record_pick(run, nya_http_server_capture(), -1, 0) : nullptr;
    if (record == nullptr || record->size < 2) return;

    u64 keep = 1 + nya_simulation_below(run, record->size - 1);
    nya_memcpy(_NYA_TRAFFIC.bytes, record->bytes, keep);

    _nya_traffic_http_hostile(run, keep, keep);
}

NYA_INTERNAL void _nya_traffic_fault_http_duplicate(NYA_SimulationRun* run) {
    const NYA_CaptureRecord* record = _NYA_TRAFFIC.http_available ? _nya_traffic_record_pick(run, nya_http_server_capture(), -1, 0) : nullptr;
    if (record == nullptr) return;

    // twice on one connection, pipelined: the second arrives before the first is answered.
    nya_memcpy(_NYA_TRAFFIC.bytes, record->bytes, record->size);
    nya_memcpy(_NYA_TRAFFIC.bytes + record->size, record->bytes, record->size);

    _nya_traffic_http_hostile(run, (u64)record->size * 2, (u64)record->size * 2);
}

NYA_INTERNAL void _nya_traffic_fault_http_delay(NYA_SimulationRun* run) {
    const NYA_CaptureRecord* record = _NYA_TRAFFIC.http_available ? _nya_traffic_record_pick(run, nya_http_server_capture(), -1, 0) : nullptr;
    if (record == nullptr) return;

    // the same bytes, split where the parser has to stop and wait for the rest.
    nya_memcpy(_NYA_TRAFFIC.bytes, record->bytes, record->size);

    _nya_traffic_http_hostile(run, record->size, nya_simulation_below(run, record->size));
}

NYA_INTERNAL void _nya_traffic_fault_http_splice(NYA_SimulationRun* run) {
    if (!_NYA_TRAFFIC.http_available) return;

    const NYA_CaptureRecord* head = _nya_traffic_record_pick(run, nya_http_server_capture(), -1, 0);
    if (head == nullptr) return;

    const NYA_CaptureRecord* tail = _nya_traffic_record_pick(run, nya_http_server_capture(), -1, head->session);
    if (tail == nullptr) return;

    // one connection's request up to a point, and another's from a point on: a head with somebody else's body.
    u64 head_size = 1 + nya_simulation_below(run, head->size);
    u64 tail_from = nya_simulation_below(run, tail->size);

    nya_memcpy(_NYA_TRAFFIC.bytes, head->bytes, head_size);
    nya_memcpy(_NYA_TRAFFIC.bytes + head_size, tail->bytes + tail_from, tail->size - tail_from);

    u64 size = head_size + tail->size - tail_from;

    _nya_traffic_http_hostile(run, size, size);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_simulation_traffic_add(NYA_SimulationRun* run) {
    nya_assert(run != nullptr);
    nya_assert(!_NYA_TRAFFIC.initialized, "the traffic set is already registered");

    _NYA_TRAFFIC = (_NYA_SimulationTraffic){ .initialized = true, .scratch = nya_arena_create(.name = "simulation_traffic_scratch") };

    // what an earlier run in this process captured is not this seed's to replay.
    nya_capture_clear(nya_net_loopback_capture());
    nya_capture_clear(nya_http_server_capture());

    _nya_traffic_session_open(run);

    // a busy host is an operating error, so the HTTP actions switch off rather than failing the run. The limits are
    // lifted because they read the wall clock, and a request refused by it would make the capture depend on timing.
    u16 port = 0;

    if (nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port).ok
        && nya_system_http_init((NYA_HttpConfig){ .port = port, .requests_per_second = U32_MAX, .request_burst = U32_MAX }).ok) {
        NYA_EXPECT(nya_http_server_merge(&_NYA_TRAFFIC_HTTP_ROUTER));
        NYA_EXPECT(nya_http_server_merge(nya_http_health_router()));

        _NYA_TRAFFIC.http_available = true;
        _NYA_TRAFFIC.http_port      = port;
    } else {
        nya_log_warn("The simulation found no free TCP port; its HTTP actions are off.");
    }

    // ticking outweighs everything else here, so a session runs long enough to be worth faulting.
    nya_simulation_action_add(run, "traffic_tick", 80, _nya_traffic_do_tick);
    nya_simulation_action_add(run, "traffic_input", 10, _nya_traffic_do_input);
    nya_simulation_action_add(run, "traffic_world", 10, _nya_traffic_do_world);
    nya_simulation_action_add(run, "traffic_converge", 3, _nya_traffic_do_converge);
    nya_simulation_action_add(run, "traffic_http", 12, _nya_traffic_do_http);

    nya_simulation_fault_add(run, "fault_net_drop", 4, _nya_traffic_fault_net_drop);
    nya_simulation_fault_add(run, "fault_net_duplicate", 3, _nya_traffic_fault_net_duplicate);
    nya_simulation_fault_add(run, "fault_net_reorder", 3, _nya_traffic_fault_net_reorder);
    nya_simulation_fault_add(run, "fault_net_delay", 3, _nya_traffic_fault_net_delay);
    nya_simulation_fault_add(run, "fault_net_flip", 3, _nya_traffic_fault_net_flip);
    nya_simulation_fault_add(run, "fault_net_truncate", 3, _nya_traffic_fault_net_truncate);
    nya_simulation_fault_add(run, "fault_net_splice", 3, _nya_traffic_fault_net_splice);
    nya_simulation_fault_add(run, "fault_net_restart", 1, _nya_traffic_fault_net_restart);

    nya_simulation_fault_add(run, "fault_http_flip", 3, _nya_traffic_fault_http_flip);
    nya_simulation_fault_add(run, "fault_http_truncate", 3, _nya_traffic_fault_http_truncate);
    nya_simulation_fault_add(run, "fault_http_duplicate", 2, _nya_traffic_fault_http_duplicate);
    nya_simulation_fault_add(run, "fault_http_delay", 2, _nya_traffic_fault_http_delay);
    nya_simulation_fault_add(run, "fault_http_splice", 3, _nya_traffic_fault_http_splice);
}

void nya_simulation_traffic_remove(void) {
    if (!_NYA_TRAFFIC.initialized) return;

    _nya_traffic_session_close();

    if (_NYA_TRAFFIC.http_available) nya_system_http_deinit();

    nya_arena_destroy(_NYA_TRAFFIC.scratch);

    _NYA_TRAFFIC = (_NYA_SimulationTraffic){ 0 };
}

#endif // NYA_TESTING

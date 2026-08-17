/**
 * The client's message handling, driven from the other end of a loopback pair.
 *
 * Every other net test reaches the client through a real server, which only ever sends it well-formed
 * messages in the expected order. That leaves most of the client untested: the rejection path, the
 * disconnect reasons, the two hooks a game registers, and what happens when snapshots arrive out of order
 * or before the handshake.
 *
 * So this holds the *server* end of a loopback pair directly and writes whatever it likes down it. From the
 * client's point of view that is indistinguishable from a server — which is the point, because a client
 * cannot assume the thing it connected to is well behaved either.
 *
 * ## The saturating clock helper
 *
 * Also covered here, because the bug it fixes was a handshake-breaking race that no in-process test caught.
 * Timers in net/ subtract two monotonic reads, and the earlier one can be recorded by a function that
 * samples the clock *after* its caller did. Unsigned, one millisecond of skew wraps to near U64_MAX, which
 * every timer reads as "ages have passed" — so a freshly added peer was declared timed out and removed
 * immediately, and a client dropped its server in the same call that added it.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FLAG_REPLICATED (1ULL << 9)

#define TICK_SECONDS (1.0F / 60.0F)

static u32              GAME_EVENTS   = 0;
static u32              PEERS_JOINED  = 0;
static u32              PEERS_LEFT    = 0;
static char             LAST_PEER_NAME[NYA_NET_MAX_NAME] = { 0 };

static void on_game_event(const NYA_Object* event) {
  nya_assert(event != nullptr, "a game event hook was called with no object");

  GAME_EVENTS++;
}

static void on_peer_change(NYA_NetPeerId peer, NYA_ConstCString name, b8 joined) {
  nya_unused(peer);

  nya_assert(name != nullptr, "a roster hook was called with no name");

  // Copied, because the header says it dies when this returns — and a test that kept the pointer would
  // be relying on something the contract does not promise.
  (void)snprintf(LAST_PEER_NAME, sizeof(LAST_PEER_NAME), "%s", name);

  if (joined) PEERS_JOINED++;
  else PEERS_LEFT++;
}

static void apply_movement(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
  nya_unused(command);

  if (entity != nullptr) entity->position.x += delta_time_s;
}

static void sample_command(OUT NYA_NetCommand* command) {
  command->actions = 1;
}

/** The peer id the loopback pair always uses: one peer, index zero, generation one. */
#define LOOPBACK_PEER ((NYA_NetPeerId){ .index = 0, .generation = 1 })

/** Attaches a client to one end of a fresh pair and returns the other end for the test to drive. */
static NYA_NetTransport* attach_client(NYA_Arena* arena, NYA_NetClientConfig config) {
  NYA_NetTransport* server_end = nullptr;
  NYA_NetTransport* client_end = nullptr;

  NYA_EXPECT(nya_net_transport_loopback_create(arena, &server_end, &client_end));

  config.replicated_flag   = FLAG_REPLICATED;
  config.on_apply_command  = nya_callback(apply_movement);
  config.on_sample_command = nya_callback(sample_command);

  NYA_EXPECT(nya_net_client_attach(client_end, "subject", config));

  return server_end;
}

/** Sends one handcrafted message as the server. */
static void send_as_server(NYA_NetTransport* server_end, const NYA_String* payload) {
  NYA_EXPECT(nya_net_transport_send(server_end, LOOPBACK_PEER, NYA_NET_CHANNEL_RELIABLE, payload->items, payload->length));
}

/** A WELCOME naming the given peer and entity. */
static NYA_String* build_welcome(NYA_Arena* arena, u32 peer_index, u32 entity_index) {
  NYA_String* payload = nya_string_create(arena);

  nya_net_message_begin(payload, NYA_NET_MSG_WELCOME);

  NYA_Object* body = nya_object_create(arena);
  nya_object_set(body, "peer_index", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = peer_index });
  nya_object_set(body, "peer_generation", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = 1 });
  nya_object_set(body, "entity_index", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = entity_index });
  nya_object_set(body, "entity_generation", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = 1 });
  nya_object_set(body, "replicated_flag", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = FLAG_REPLICATED });

  NYA_EXPECT(nya_net_message_write_object(arena, payload, body));

  return payload;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true, .options = { .time_step_ns = nya_time_ms_to_ns(16) } };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);
  defer nya_system_callback_deinit();

  NYA_Arena* arena = nya_arena_create(.name = "test_client");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the saturating clock helper
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: elapsed time saturates instead of wrapping\n");
  {
    nya_assert(_nya_net_elapsed_ms(1000, 400) == 600, "the ordinary direction still subtracts");
    nya_assert(_nya_net_elapsed_ms(1000, 1000) == 0, "equal reads are no time at all");

    /*
     * The case that broke the handshake.
     *
     * One millisecond of skew, which is what a timestamp recorded after `now` was sampled produces. Wrapped
     * this is 18446744073709551615, and every timer in net/ compares that against a few thousand — so a
     * peer added a moment ago was immediately declared timed out.
     */
    nya_assert(_nya_net_elapsed_ms(1000, 1001) == 0, "a later `then` saturates to zero rather than wrapping");
    nya_assert(_nya_net_elapsed_ms(0, U64_MAX) == 0, "and so does the extreme of it");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the handshake, and what it leaves the client knowing
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: WELCOME puts the client in a game\n");
  {
    NYA_NetTransport* server_end = attach_client(arena, (NYA_NetClientConfig){ 0 });

    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_CONNECTING, "a fresh client is connecting");
    nya_assert(!nya_entity_is_valid(nya_net_client_entity()), "and controls nothing yet");

    // The first tick sees the loopback's connection event and sends HELLO.
    nya_net_client_tick(1, TICK_SECONDS);

    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_HANDSHAKING, "the client is waiting for a WELCOME");

    send_as_server(server_end, build_welcome(arena, 3, 7));

    nya_net_client_tick(2, TICK_SECONDS);

    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_PLAYING);
    nya_assert(nya_net_client_peer().index == 3, "the client learned its peer id");

    /*
     * The server's handle for its entity, which is not usable locally.
     *
     * On a loopback the two handle spaces are one, so the local handle is set immediately — a remote client
     * waits for the first snapshot to spawn it. Both names are exposed because only one of them can be
     * handed to nya_entity_get.
     */
    nya_assert(nya_net_client_entity_remote().index == 7, "the client learned the server's handle for its entity");

    nya_net_client_disconnect();
    nya_net_transport_destroy(server_end);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: REJECT carries a reason the player can be shown
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: REJECT reports why\n");
  {
    NYA_NetTransport* server_end = attach_client(arena, (NYA_NetClientConfig){ 0 });

    nya_net_client_tick(1, TICK_SECONDS);

    NYA_String* payload = nya_string_create(arena);
    nya_net_message_begin(payload, NYA_NET_MSG_REJECT);

    NYA_Object* reject = nya_object_create(arena);
    nya_object_set(reject, "reason", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_DISCONNECT_VERSION });

    NYA_EXPECT(nya_net_message_write_object(arena, payload, reject));

    send_as_server(server_end, payload);

    nya_net_client_tick(2, TICK_SECONDS);

    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_DISCONNECTED, "a rejected client is not in a game");

    /*
     * The reason survives the reset.
     *
     * A player shown "connection lost" when the truth is "this server runs a different version" cannot act
     * on it, so the reason is deliberately preserved across the teardown that clears everything else.
     */
    nya_assert(nya_net_client_disconnect_reason() == NYA_NET_DISCONNECT_VERSION, "the rejection reason was lost");

    nya_net_transport_destroy(server_end);
  }

  printf("TEST: a REJECT with no readable reason still disconnects\n");
  {
    NYA_NetTransport* server_end = attach_client(arena, (NYA_NetClientConfig){ 0 });

    nya_net_client_tick(1, TICK_SECONDS);

    // Garbage where the document should be. A server that sends this is broken, and the client must still
    // end up disconnected rather than waiting forever in HANDSHAKING.
    NYA_String* payload = nya_string_create(arena);
    nya_net_message_begin(payload, NYA_NET_MSG_REJECT);
    for (u32 i = 0; i < 16; i++) nya_string_push_back(payload, 0xAB);

    send_as_server(server_end, payload);

    nya_net_client_tick(2, TICK_SECONDS);

    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_DISCONNECTED, "an unreadable rejection left the client hanging");

    nya_net_transport_destroy(server_end);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the roster hook
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: PEER_JOINED and PEER_LEFT reach the game\n");
  {
    PEERS_JOINED = 0;
    PEERS_LEFT   = 0;

    NYA_NetTransport* server_end = attach_client(arena, (NYA_NetClientConfig){ .on_peer_change = nya_callback(on_peer_change) });

    nya_net_client_tick(1, TICK_SECONDS);
    send_as_server(server_end, build_welcome(arena, 0, 1));
    nya_net_client_tick(2, TICK_SECONDS);

    for (u32 round = 0; round < 2; round++) {
      NYA_String* payload = nya_string_create(arena);
      nya_net_message_begin(payload, round == 0 ? NYA_NET_MSG_PEER_JOINED : NYA_NET_MSG_PEER_LEFT);

      NYA_Object* body = nya_object_create(arena);
      nya_object_set(body, "peer_index", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = 5 });
      nya_object_set(body, "peer_generation", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = 1 });
      // The same name both times, deliberately: the assertion below is that whichever message arrived last
      // reached the hook, and two different names would not distinguish that from only one arriving.
      nya_object_set(body, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "Grace" });

      NYA_EXPECT(nya_net_message_write_object(arena, payload, body));
      send_as_server(server_end, payload);
    }

    nya_net_client_tick(3, TICK_SECONDS);

    nya_assert(PEERS_JOINED == 1, "the join hook fired %u times", PEERS_JOINED);
    nya_assert(PEERS_LEFT == 1, "the leave hook fired %u times", PEERS_LEFT);
    nya_assert(nya_string_equals(LAST_PEER_NAME, "Grace"), "the name reached the hook");

    // A roster message missing a field is ignored rather than reaching the hook with nothing in it.
    {
      u32 before = PEERS_JOINED;

      NYA_String* payload = nya_string_create(arena);
      nya_net_message_begin(payload, NYA_NET_MSG_PEER_JOINED);

      NYA_Object* body = nya_object_create(arena);
      nya_object_set(body, "peer_index", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = 6 });
      // no generation, no name

      NYA_EXPECT(nya_net_message_write_object(arena, payload, body));
      send_as_server(server_end, payload);

      nya_net_client_tick(4, TICK_SECONDS);

      nya_assert(PEERS_JOINED == before, "an incomplete roster message reached the hook");
    }

    nya_net_client_disconnect();
    nya_net_transport_destroy(server_end);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the game event hook
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: GAME_EVENT reaches the game\n");
  {
    GAME_EVENTS = 0;

    NYA_NetTransport* server_end = attach_client(arena, (NYA_NetClientConfig){ .on_game_event = nya_callback(on_game_event) });

    nya_net_client_tick(1, TICK_SECONDS);
    send_as_server(server_end, build_welcome(arena, 0, 1));
    nya_net_client_tick(2, TICK_SECONDS);

    NYA_String* payload = nya_string_create(arena);
    nya_net_message_begin(payload, NYA_NET_MSG_GAME_EVENT);

    NYA_Object* event = nya_object_create(arena);
    nya_object_set(event, "chat", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "hello" });

    NYA_EXPECT(nya_net_message_write_object(arena, payload, event));
    send_as_server(server_end, payload);

    nya_net_client_tick(3, TICK_SECONDS);

    nya_assert(GAME_EVENTS == 1, "the game event hook fired %u times", GAME_EVENTS);

    // An unreadable event is dropped rather than handed over. A newer server may send a document this
    // build's serde cannot parse, and calling the hook with nothing would be worse than not calling it.
    {
      NYA_String* garbage = nya_string_create(arena);
      nya_net_message_begin(garbage, NYA_NET_MSG_GAME_EVENT);
      for (u32 i = 0; i < 32; i++) nya_string_push_back(garbage, 0x5A);

      send_as_server(server_end, garbage);
      nya_net_client_tick(4, TICK_SECONDS);

      nya_assert(GAME_EVENTS == 1, "an unreadable game event reached the hook");
    }

    // And sending one to the server works from a client in a game.
    nya_assert(nya_net_client_send_event(event).ok, "a client in a game can send an event");

    nya_net_client_disconnect();

    // But not once it has left.
    nya_assert(!nya_net_client_send_event(event).ok, "a disconnected client sent an event");

    nya_net_transport_destroy(server_end);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: snapshots before the handshake, and out of order
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: snapshots are ignored before PLAYING and when stale\n");
  {
    NYA_NetTransport* server_end = attach_client(arena, (NYA_NetClientConfig){ 0 });

    /** A snapshot naming one entity at the given tick and x position. */
    #define SNAPSHOT(at_tick, x)                                                                                                                     \
      ({                                                                                                                                             \
        NYA_NetEntityState _entity = {                                                                                                               \
          .handle = { .index = 40, .generation = 1 }, .position = { (x), 0.0F, 0.0F }, .scale = { 1.0F, 1.0F, 1.0F },                                 \
        };                                                                                                                                           \
        NYA_NetSnapshot _snapshot = { .tick = (at_tick), .entities = &_entity, .entity_count = 1 };                                                   \
        NYA_String*     _payload  = nya_string_create(arena);                                                                                        \
        nya_net_message_begin(_payload, NYA_NET_MSG_SNAPSHOT);                                                                                        \
        NYA_EXPECT(nya_net_snapshot_encode(arena, &_snapshot, nullptr, _payload));                                                                    \
        _payload;                                                                                                                                    \
      })

    // Before the handshake. There is no predicted entity to reconcile against and no replica map yet, so
    // this must be discarded rather than applied to a world the client has not been admitted to.
    send_as_server(server_end, SNAPSHOT(50, 10.0F));
    nya_net_client_tick(1, TICK_SECONDS);

    nya_assert(nya_net_client_server_tick() == 0, "a snapshot was applied before the handshake completed");

    send_as_server(server_end, build_welcome(arena, 0, 1));
    nya_net_client_tick(2, TICK_SECONDS);
    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_PLAYING);

    /*
     * On a loopback the client applies nothing — it shares the server's world.
     *
     * The tick is still tracked and acknowledged, because the server's baseline bookkeeping runs the same
     * way for every peer and a local one that never acknowledged would be sent full snapshots forever.
     */
    send_as_server(server_end, SNAPSHOT(60, 20.0F));
    nya_net_client_tick(3, TICK_SECONDS);

    nya_assert(nya_net_client_server_tick() == 60, "the applied tick was not tracked");

    // Older than what has been applied. Discarded: applying it would move the world backwards, and its
    // delta was computed against a baseline this client may already have replaced.
    send_as_server(server_end, SNAPSHOT(55, 99.0F));
    nya_net_client_tick(4, TICK_SECONDS);

    nya_assert(nya_net_client_server_tick() == 60, "a stale snapshot was applied over a newer one");

    // Newer still is taken.
    send_as_server(server_end, SNAPSHOT(61, 30.0F));
    nya_net_client_tick(5, TICK_SECONDS);

    nya_assert(nya_net_client_server_tick() == 61, "a newer snapshot was not applied");

    // A malformed snapshot is dropped without disturbing what has been applied — the channel is unreliable
    // and the next one arrives a sixteenth of a second later.
    {
      NYA_String* garbage = nya_string_create(arena);
      nya_net_message_begin(garbage, NYA_NET_MSG_SNAPSHOT);
      for (u32 i = 0; i < 40; i++) nya_string_push_back(garbage, 0xF0);

      send_as_server(server_end, garbage);
      nya_net_client_tick(6, TICK_SECONDS);

      nya_assert(nya_net_client_server_tick() == 61, "a malformed snapshot changed the applied tick");
    }

    #undef SNAPSHOT

    nya_net_client_disconnect();
    nya_net_transport_destroy(server_end);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: interpolation and stats are safe to call in every state
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: the query surface is safe when disconnected\n");
  {
    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_DISCONNECTED);

    // A game calls these from its render path without asking whether it is connected, so none of them may
    // require a connection to be safe.
    nya_net_client_interpolate(TICK_SECONDS);
    nya_net_client_tick(1, TICK_SECONDS);

    NYA_NetPeerStats stats = nya_net_client_stats();
    nya_assert(stats.rtt_ms == 0.0F && stats.packets_sent == 0, "a disconnected client reports no traffic");

    nya_assert(!nya_entity_is_valid(nya_net_client_entity()));
    nya_assert(!nya_net_peer_is_set(nya_net_client_peer()));
    nya_assert(nya_net_client_server_tick() == 0);
    nya_assert(nya_net_client_correction_count() == 0);

    // The translation helper answers for a handle nobody has mapped.
    nya_assert(!nya_entity_is_valid(nya_net_client_local_entity((NYA_EntityHandle){ .index = 1, .generation = 1 })));

    nya_net_client_disconnect(); // twice, because a shutdown path is not always the one it thinks
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: attach refuses an incomplete configuration
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: attach validates its configuration\n");
  {
    NYA_NetTransport* a = nullptr;
    NYA_NetTransport* b = nullptr;
    NYA_EXPECT(nya_net_transport_loopback_create(arena, &a, &b));

    // Both callbacks are required: without them a client predicts nothing and never learns what the player
    // wants, which is a silently broken game rather than an error.
    nya_assert(!nya_net_client_attach(b, "x", (NYA_NetClientConfig){ 0 }).ok, "attach accepted no callbacks");

    nya_assert(!nya_net_client_attach(b, "x", (NYA_NetClientConfig){ .on_apply_command = nya_callback(apply_movement) }).ok,
               "attach accepted a config with no sampler");

    nya_assert(!nya_net_client_attach(b, "x", (NYA_NetClientConfig){ .on_sample_command = nya_callback(sample_command) }).ok,
               "attach accepted a config with no movement function");

    nya_assert(!nya_net_client_attach(nullptr, "x",
                                      (NYA_NetClientConfig){ .on_apply_command = nya_callback(apply_movement),
                                                             .on_sample_command = nya_callback(sample_command) })
                    .ok,
               "attach accepted a null transport");

    // A complete one works, and a second is refused while the first is live.
    NYA_EXPECT(nya_net_client_attach(b, "x",
                                     (NYA_NetClientConfig){ .on_apply_command = nya_callback(apply_movement), .on_sample_command = nya_callback(sample_command) }));

    nya_assert(!nya_net_client_attach(b, "y",
                                      (NYA_NetClientConfig){ .on_apply_command = nya_callback(apply_movement),
                                                             .on_sample_command = nya_callback(sample_command) })
                    .ok,
               "a second client was attached over a live one");

    nya_net_client_disconnect();

    nya_net_transport_destroy(a);
    nya_net_transport_destroy(b);
  }

  printf("PASSED: test_client (0 failures)\n");

  return EXIT_SUCCESS;
}

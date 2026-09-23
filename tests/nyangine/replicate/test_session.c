/**
 * A whole session: server, clients, handshake, snapshots, commands, prediction, reconciliation.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#include <time.h>

#define FLAG_REPLICATED (1ULL << 2)

#define ACTION_RIGHT 0
#define ACTION_LEFT  1

/** World units per second a held direction moves a player. Whatever; it just has to be deterministic. */
#define SPEED 100.0F

#define PUMP_TIMEOUT_MS 5000

static void sleep_ms(u32 milliseconds) {
  struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
  (void)nanosleep(&request, nullptr);
}

/*
 * What both sides run. Deterministic in (entity, command, dt) alone: no clock, no RNG, no server only
 * state. That is the NYA_NetApplyCommandFn contract, and it is what keeps prediction correct.
 */
static void apply_movement(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
  f32 direction = 0.0F;

  if (nya_net_command_holds(command, ACTION_RIGHT)) direction += 1.0F;
  if (nya_net_command_holds(command, ACTION_LEFT)) direction -= 1.0F;

  entity->position.x += direction * SPEED * delta_time_s;
}

/** What the test wants the player to be doing this tick. Read by the sampler below. */
static u64 HELD_ACTIONS = 0;

static void sample_command(OUT NYA_NetCommand* command) {
  command->actions = HELD_ACTIONS;
}

static u32              SPAWN_CALLS  = 0;
static NYA_EntityHandle SPAWNED[NYA_NET_MAX_PEERS];

static NYA_EntityHandle spawn_player(NYA_NetPeerId peer, NYA_ConstCString name) {
  nya_unused(name);

  NYA_EntityHandle handle = nya_entity_spawn(.name = "player", .flags = FLAG_REPLICATED, .position = { 0.0F, 0.0F, 0.0F });

  if (peer.index < NYA_NET_MAX_PEERS) SPAWNED[peer.index] = handle;
  SPAWN_CALLS++;

  return handle;
}

static u32 DESPAWN_CALLS = 0;

static void despawn_player(NYA_NetPeerId peer, NYA_EntityHandle entity) {
  nya_unused(peer, entity);
  DESPAWN_CALLS++;
}

/** One fixed tick, matching the engine's default. */
#define TICK_SECONDS (1.0F / 60.0F)

/** Runs `count` ticks of both sides, the way the app loop would. */
static void run_ticks(u64* tick, u32 count, b8 with_client) {
  for (u32 i = 0; i < count; i++) {
    nya_net_server_tick(*tick, TICK_SECONDS);
    if (with_client) nya_net_client_tick(*tick, TICK_SECONDS);

    nya_system_sim_apply_commands();

    (*tick)++;
  }
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);
  defer nya_system_callback_deinit();

  u64 tick = 1;

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: single player costs nothing
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: single player allocates nothing per tick\n");
  {
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
      .replicated_flag  = FLAG_REPLICATED,
      .on_apply_command = nya_callback(apply_movement),
      .on_spawn_player  = nya_callback(spawn_player),
    }));

    nya_assert(nya_net_server_running());
    nya_assert(!nya_net_server_is_listening(), "starting a server does not open a socket");
    nya_assert(nya_net_server_is_dedicated(), "a server with no local player reports itself dedicated");
    nya_assert(nya_net_server_peer_count() == 0);

    // A world with something in it, so a snapshot would have work to do if one were taken.
    NYA_EntityHandle crate = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { 5.0F, 5.0F, 0.0F });

    /*
     * The claim being tested: with nobody listening, a tick does no networking at all.
     */
    run_ticks(&tick, 120, false);

    nya_assert(_NYA_NET_SERVER.tick_arena != nullptr);

    // Nothing was ever put in it, so nothing was ever used.
    NYA_ArenaStats stats = nya_arena_stats(_NYA_NET_SERVER.tick_arena);
    nya_assert(stats.used_bytes == 0, "single player allocated %llu bytes of networking scratch over 120 ticks",
               (unsigned long long)stats.used_bytes);

    nya_entity_despawn(crate);
    nya_net_server_stop();

    nya_assert(!nya_net_server_running());
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a listen server's host plays through the client code
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: listen server host joins over loopback\n");
  {
    SPAWN_CALLS   = 0;
    DESPAWN_CALLS = 0;

    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
      .replicated_flag   = FLAG_REPLICATED,
      .on_apply_command  = nya_callback(apply_movement),
      .on_spawn_player   = nya_callback(spawn_player),
      .on_despawn_player = nya_callback(despawn_player),
    }));

    NYA_NetTransport* client_end = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&client_end));

    nya_assert(!nya_net_server_is_dedicated(), "a server with a local player is not dedicated");
    nya_assert(nya_net_transport_is_local(client_end), "the host's transport reports itself local");

    NYA_EXPECT(nya_net_client_attach(client_end, "host", (NYA_NetClientConfig){
      .replicated_flag   = FLAG_REPLICATED,
      .on_apply_command  = nya_callback(apply_movement),
      .on_sample_command = nya_callback(sample_command),
    }));

    // The handshake is a couple of round trips through the loopback queues, so a few ticks.
    HELD_ACTIONS = 0;
    run_ticks(&tick, 8, true);

    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_PLAYING, "the host did not finish the handshake");
    nya_assert(SPAWN_CALLS == 1, "the host got exactly one entity");
    nya_assert(nya_net_server_peer_count() == 1);
    nya_assert(nya_net_peer_is_set(nya_net_server_local_peer()), "the host is a peer like any other");

    NYA_EntityHandle player = nya_net_client_entity();
    nya_assert(nya_entity_is_valid(player), "the host controls a real entity");

    NYA_Entity* entity = nya_entity_get(player);
    nya_assert(entity != nullptr);

    f32 start_x = entity->position.x;

    // ── the host moves ────────────────────────────────────────────────────────
    HELD_ACTIONS = 1ULL << ACTION_RIGHT;

    run_ticks(&tick, 30, true);

    entity = nya_entity_get(player);
    nya_assert(entity != nullptr);

    nya_assert(entity->position.x > start_x + 10.0F, "the host's player did not move (x went %f -> %f)", (f64)start_x, (f64)entity->position.x);

    /*
     * Moved exactly once per tick, not twice.
     */
    f32 expected = start_x + (SPEED * TICK_SECONDS * 30.0F);
    f32 drift    = entity->position.x - expected;

    nya_assert(drift < 5.0F && drift > -5.0F, "the host moved %f, expected about %f; double application?", (f64)entity->position.x, (f64)expected);

    nya_assert(nya_net_client_correction_count() == 0, "a listen server host is never corrected");

    HELD_ACTIONS = 0;

    nya_net_client_disconnect();
    nya_net_server_stop();

    // Whatever the two left behind, so the next case starts from an empty world.
    nya_entity_foreach (leftover) nya_entity_despawn_deferred(leftover->handle);
    nya_system_sim_apply_commands();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a real client joins over UDP and receives the world
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a UDP client joins and receives snapshots\n");
  {
    SPAWN_CALLS = 0;

    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
      .replicated_flag  = FLAG_REPLICATED,
      .on_apply_command = nya_callback(apply_movement),
      .on_spawn_player  = nya_callback(spawn_player),
    }));

    NYA_EXPECT(nya_net_server_listen(0), "the system had no free UDP port");

    const u16 port = nya_net_server_port();
    nya_assert(nya_net_server_is_listening());

    printf("  listening on %u\n", port);

    /*
     * The world the client is about to be told about.
     */
    (void)nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { 300.0F, 0.0F, 0.0F });
    (void)nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { 400.0F, 0.0F, 0.0F });

    NYA_EXPECT(nya_net_client_connect("127.0.0.1", port, "remote", (NYA_NetClientConfig){
      .replicated_flag   = FLAG_REPLICATED,
      .on_apply_command  = nya_callback(apply_movement),
      .on_sample_command = nya_callback(sample_command),
    }));

    /*
     * Server and client are in one process here, so they share one entity table.
     */
    HELD_ACTIONS = 0;

    u64 deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

    while (nya_net_client_state() != NYA_NET_CLIENT_PLAYING && nya_clock_get_monotonic_ms() < deadline) {
      run_ticks(&tick, 1, true);
      sleep_ms(2);
    }

    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_PLAYING, "the UDP handshake did not complete");
    nya_assert(SPAWN_CALLS == 1, "the server spawned exactly one player for the client");
    nya_assert(nya_net_server_peer_count() == 1);

    NYA_NetPeerId peer = nya_net_client_peer();
    nya_assert(nya_net_peer_is_set(peer), "the client learned its peer id from WELCOME");

    const NYA_NetServerPeer* server_view = nya_net_server_peer(peer);
    nya_assert(server_view != nullptr, "the server knows the peer the client thinks it is");
    nya_assert(nya_string_equals(server_view->name, "remote"), "the name crossed the handshake");
    nya_assert(!server_view->is_local, "a UDP peer is not local");

    // ── snapshots flow, and are acknowledged ──────────────────────────────────
    {
      u64 before = nya_net_client_server_tick();

      deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

      while (nya_net_client_server_tick() <= before && nya_clock_get_monotonic_ms() < deadline) {
        run_ticks(&tick, 1, true);
        sleep_ms(2);
      }

      nya_assert(nya_net_client_server_tick() > before, "no snapshot arrived over UDP");

      // And the server saw the acknowledgement, which is what lets it delta rather than send in full.
      deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

      while (_NYA_NET_SERVER.peers[peer.index]->acknowledged_tick == 0 && nya_clock_get_monotonic_ms() < deadline) {
        run_ticks(&tick, 1, true);
        sleep_ms(2);
      }

      nya_assert(_NYA_NET_SERVER.peers[peer.index]->acknowledged_tick > 0, "the server never received a snapshot acknowledgement");

      printf("  server tick %llu, client applied %llu\n", (unsigned long long)tick, (unsigned long long)nya_net_client_server_tick());
    }

    // ── commands reach the server and move the player ─────────────────────────
    {
      NYA_EntityHandle player = SPAWNED[peer.index];
      nya_assert(nya_entity_is_valid(player));

      NYA_Entity* entity = nya_entity_get(player);
      f32         before = entity->position.x;

      HELD_ACTIONS = 1ULL << ACTION_RIGHT;

      deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

      while (nya_net_server_last_command(peer).actions == 0 && nya_clock_get_monotonic_ms() < deadline) {
        run_ticks(&tick, 1, true);
        sleep_ms(2);
      }

      nya_assert(nya_net_server_last_command(peer).actions == (1ULL << ACTION_RIGHT), "the server never received the command");

      run_ticks(&tick, 40, true);

      entity = nya_entity_get(player);
      nya_assert(entity != nullptr);
      nya_assert(entity->position.x > before, "the command did not move the player on the server");

      HELD_ACTIONS = 0;
    }

    // ── the client leaves cleanly ─────────────────────────────────────────────
    {
      DESPAWN_CALLS = 0;

      nya_net_client_disconnect();

      nya_assert(nya_net_client_state() == NYA_NET_CLIENT_DISCONNECTED);

      // The server notices via the transport's disconnect, which takes a tick to drain.
      deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;

      while (nya_net_server_peer_count() > 0 && nya_clock_get_monotonic_ms() < deadline) {
        run_ticks(&tick, 1, false);
        sleep_ms(2);
      }

      nya_assert(nya_net_server_peer_count() == 0, "the server did not notice the client leaving");
    }

    nya_net_server_stop();

    nya_entity_foreach (leftover) nya_entity_despawn_deferred(leftover->handle);
    nya_system_sim_apply_commands();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: prediction moves the player before the server answers
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a remote client predicts its own movement\n");
  {
    /*
     * Prediction against a transport that is not local, with no server to correct it, isolates the
     * property: the client applies its command immediately instead of waiting a round trip.
     */
    NYA_Arena* arena = nya_arena_create(.name = "test_session_predict");
    defer      nya_arena_destroy(arena);

    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
      .replicated_flag  = FLAG_REPLICATED,
      .on_apply_command = nya_callback(apply_movement),
      .on_spawn_player  = nya_callback(spawn_player),
    }));

    NYA_EXPECT(nya_net_server_listen(0), "the system had no free UDP port");

    const u16 port = nya_net_server_port();

    SPAWN_CALLS = 0;

    NYA_EXPECT(nya_net_client_connect("127.0.0.1", port, "predictor", (NYA_NetClientConfig){
      .replicated_flag   = FLAG_REPLICATED,
      .on_apply_command  = nya_callback(apply_movement),
      .on_sample_command = nya_callback(sample_command),
      // Wide, so the shared entity table's exact agreement does not trip a correction. What is being
      // tested here is that prediction happens at all.
      .correction_threshold = 1000.0F,
    }));

    HELD_ACTIONS = 0;

    u64 deadline = nya_clock_get_monotonic_ms() + PUMP_TIMEOUT_MS;
    while (nya_net_client_state() != NYA_NET_CLIENT_PLAYING && nya_clock_get_monotonic_ms() < deadline) {
      run_ticks(&tick, 1, true);
      sleep_ms(2);
    }

    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_PLAYING);

    NYA_EntityHandle player = nya_net_client_entity();
    nya_assert(nya_entity_is_valid(player));

    NYA_Entity* entity = nya_entity_get(player);
    f32         before = entity->position.x;

    /*
     * One tick, holding right, and the player has already moved.
     */
    HELD_ACTIONS = 1ULL << ACTION_RIGHT;

    nya_net_client_tick(tick, TICK_SECONDS);

    entity = nya_entity_get(player);
    nya_assert(entity != nullptr);
    nya_assert(entity->position.x > before, "the client did not predict its own movement");

    printf("  predicted %f -> %f in one tick\n", (f64)before, (f64)entity->position.x);

    HELD_ACTIONS = 0;

    nya_net_client_disconnect();
    nya_net_server_stop();

    nya_entity_foreach (leftover) nya_entity_despawn_deferred(leftover->handle);
    nya_system_sim_apply_commands();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a version mismatch is refused before any state is exchanged
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a version mismatch is refused\n");
  {
    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
      .replicated_flag  = FLAG_REPLICATED,
      .on_apply_command = nya_callback(apply_movement),
      .on_spawn_player  = nya_callback(spawn_player),
    }));

    NYA_NetTransport* server_end = nullptr;
    NYA_NetTransport* client_end = nullptr;

    NYA_Arena* arena = nya_arena_create(.name = "test_session_version");
    defer      nya_arena_destroy(arena);

    NYA_EXPECT(nya_net_transport_loopback_create(arena, &server_end, &client_end));

    SPAWN_CALLS = 0;

    // a HELLO with a protocol this build does not speak, sent by hand since the client sends the right one.
    {
      NYA_String* payload = nya_string_create(arena);

      nya_net_message_begin(payload, NYA_NET_MSG_HELLO);

      NYA_Object* hello = nya_object_create(arena);
      nya_object_set(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION + 99 });
      nya_object_set(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
      nya_object_set(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "from the future" });

      NYA_EXPECT(nya_net_message_write_object(arena, payload, hello));

      // The loopback pair reports its connection on the first poll, so one drain first.
      NYA_NetTransportEvent event = { 0 };
      while (nya_net_transport_poll(client_end, &event)) { }

      NYA_EXPECT(nya_net_transport_send(client_end, (NYA_NetPeerId){ .index = 0, .generation = 1 }, NYA_NET_CHANNEL_RELIABLE, payload->items,
                                        payload->length));
    }

    // the server must be given this transport to drain it, but nya_net_server_attach_local makes its own
    // pair, so the message goes through the server's loopback.
    NYA_NetTransport* real_client = nullptr;
    NYA_EXPECT(nya_net_server_attach_local(&real_client));

    {
      NYA_String* payload = nya_string_create(arena);

      nya_net_message_begin(payload, NYA_NET_MSG_HELLO);

      NYA_Object* hello = nya_object_create(arena);
      nya_object_set(hello, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_PROTOCOL_VERSION + 99 });
      nya_object_set(hello, "snapshot", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_NET_SNAPSHOT_VERSION });
      nya_object_set(hello, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "from the future" });

      NYA_EXPECT(nya_net_message_write_object(arena, payload, hello));

      NYA_NetTransportEvent event = { 0 };
      while (nya_net_transport_poll(real_client, &event)) { }

      NYA_EXPECT(
        nya_net_transport_send(real_client, (NYA_NetPeerId){ .index = 0, .generation = 1 }, NYA_NET_CHANNEL_RELIABLE, payload->items, payload->length)
      );
    }

    run_ticks(&tick, 4, false);

    nya_assert(SPAWN_CALLS == 0, "a peer with the wrong version was given an entity");
    nya_assert(nya_net_server_peer_count() == 0, "a peer with the wrong version was admitted");

    // and it is dropped at once, told why, rather than left holding a connection until it times out.
    b8                    dropped = false;
    NYA_NetTransportEvent event   = { 0 };
    while (nya_net_transport_poll(real_client, &event)) {
      if (event.kind == NYA_NET_TRANSPORT_EVENT_DISCONNECTED) dropped = event.reason == NYA_NET_DISCONNECT_VERSION;
    }
    nya_assert(dropped, "a peer with the wrong version was not disconnected with that reason");

    nya_net_transport_destroy(server_end);
    nya_net_transport_destroy(client_end);
    nya_net_server_stop();
  }

  printf("PASSED: test_session (0 failures)\n");

  return EXIT_SUCCESS;
}

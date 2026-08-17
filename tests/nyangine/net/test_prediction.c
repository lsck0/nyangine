/**
 * Prediction and reconciliation, over a real socket against a real server.
 *
 * These paths are unreachable on a loopback, and deliberately so: a listen server's client shares the
 * server's world, there is no latency to hide, and reconciliation is a no-op. So every in-process test
 * covers the *decision* to skip it and none of them covers what it does when it runs.
 *
 * Which leaves the interesting half untested — the correction, the replay, and the baseline arena swap that
 * lets a delta be decoded against the snapshot it is about to replace.
 *
 * So this runs a UDP server and a UDP client in one process but over two sockets, with **two separate
 * worlds**: the server's and the client's replica. `nya_world_set` swaps which one the entity API talks to,
 * so every tick has to say which side it is running. That is fiddly and it is the only arrangement in which
 * reconciliation is real.
 *
 * ## Forcing a correction
 *
 * The two sides run *deliberately different* movement functions. That is exactly what the contract forbids —
 * NYA_NetApplyCommandFn must be deterministic and shared — and breaking it on purpose is the cleanest way to
 * make the server disagree with the prediction on every tick. What is being tested is the machinery's
 * response to disagreement, not the disagreement.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#include <time.h>

#define FLAG_REPLICATED (1ULL << 11)

#define FIRST_PORT 48300
#define TICK_SECONDS (1.0F / 60.0F)

/** How far the client's movement disagrees with the server's, per tick. Well past the correction threshold. */
#define DIVERGENCE 40.0F

static NYA_World* SERVER_WORLD = nullptr;
static NYA_World* CLIENT_WORLD = nullptr;

/** Which world the shared movement function should treat as its own. Set around every call into the engine. */
static b8 RUNNING_AS_CLIENT = false;

static NYA_EntityHandle SERVER_PLAYER = NYA_ENTITY_HANDLE_NONE;

static void sleep_ms(u32 milliseconds) {
  struct timespec request = { .tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000L };
  (void)nanosleep(&request, nullptr);
}

/**
 * Moves an entity, differently on each side.
 *
 * One function rather than two, because both sides are handed the same pointer and the *point* is that they
 * disagree — two functions would be two contracts and the test would prove less.
 * */
static void apply_movement(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
  if (entity == nullptr) return;
  if (!nya_net_command_holds(command, 0)) return;

  f32 speed = RUNNING_AS_CLIENT ? 100.0F + DIVERGENCE : 100.0F;

  entity->position.x += speed * delta_time_s;
}

static u64 HELD = 0;

static void sample_command(OUT NYA_NetCommand* command) {
  command->actions = HELD;
}

static NYA_EntityHandle spawn_player(NYA_NetPeerId peer, NYA_ConstCString name) {
  nya_unused(peer, name);

  SERVER_PLAYER = nya_entity_spawn(.name = "player", .flags = FLAG_REPLICATED, .position = { 0.0F, 0.0F, 0.0F });

  return SERVER_PLAYER;
}

/** One tick of the server, in the server's world. */
static void tick_server(u64 tick) {
  (void)nya_world_set(SERVER_WORLD);
  RUNNING_AS_CLIENT = false;

  nya_net_server_tick(tick, TICK_SECONDS);
  nya_system_sim_apply_commands();
}

/** One tick of the client, in the client's replica. */
static void tick_client(u64 tick) {
  (void)nya_world_set(CLIENT_WORLD);
  RUNNING_AS_CLIENT = true;

  nya_net_client_tick(tick, TICK_SECONDS);
  nya_system_sim_apply_commands();
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true, .options = { .time_step_ns = nya_time_ms_to_ns(16) } };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  defer nya_system_callback_deinit();

  SERVER_WORLD = nya_world_create();
  CLIENT_WORLD = nya_world_create();

  defer nya_world_destroy(CLIENT_WORLD);
  defer nya_world_destroy(SERVER_WORLD);

  /*
   * The client's table is pushed out of step with the server's before anything is replicated.
   *
   * Otherwise both are fresh and hand out the same indices, so the handle translation would be exercised
   * only coincidentally — the server's entity 0 and the client's entity 0 would be the same number and a
   * missing translation would look correct.
   */
  (void)nya_world_set(CLIENT_WORLD);
  {
    NYA_EntityHandle filler[6];
    for (u32 i = 0; i < 6; i++) filler[i] = nya_entity_spawn(.name = "clutter", .position = { (f32)i, 0.0F, 0.0F });

    // Interleaved, so the free list is not a clean run and the next index is genuinely unpredictable.
    for (u32 i = 0; i < 6; i += 2) nya_entity_despawn(filler[i]);
  }

  // ── a server on a socket, and a client on another ──────────────────────────
  (void)nya_world_set(SERVER_WORLD);

  NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
    .replicated_flag  = FLAG_REPLICATED,
    .on_spawn_player  = nya_callback(spawn_player),
    .on_apply_command = nya_callback(apply_movement),
  }));

  u16 port = 0;
  for (u16 candidate = FIRST_PORT; candidate < FIRST_PORT + 16; candidate++) {
    if (nya_net_server_listen(candidate).ok) {
      port = candidate;
      break;
    }
  }
  nya_assert(port != 0, "could not bind any port in the test range");

  (void)nya_world_set(CLIENT_WORLD);

  NYA_EXPECT(nya_net_client_connect("127.0.0.1", port, "predictor", (NYA_NetClientConfig){
    .replicated_flag   = FLAG_REPLICATED,
    .on_apply_command  = nya_callback(apply_movement),
    .on_sample_command = nya_callback(sample_command),
    // Small, so the deliberate divergence below is well past it and a correction is certain.
    .correction_threshold = 1.0F,
  }));

  u64 tick = 1;

  printf("TEST: a remote client completes the handshake over UDP\n");
  {
    u64 deadline = nya_clock_get_monotonic_ms() + 5000;

    while (nya_net_client_state() != NYA_NET_CLIENT_PLAYING && nya_clock_get_monotonic_ms() < deadline) {
      tick_server(tick);
      tick_client(tick);
      tick++;

      sleep_ms(2);
    }

    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_PLAYING, "the handshake did not complete");

    /*
     * The world is set before the entity is asked about, because every entity query is against whichever
     * world is *current* — and the loop above left that as the client's.
     *
     * This assertion passed by accident until the client's table was pushed out of step: with both worlds
     * fresh, the server's handle happened to name a live entity in the client's world too. Which is precisely
     * the confusion the two-handle-space design exists to prevent, reproduced in the test that tests it.
     */
    (void)nya_world_set(SERVER_WORLD);
    nya_assert(nya_entity_is_valid(SERVER_PLAYER), "the server spawned no player");

    // The server's handle for the client's entity, which is not usable in the client's world.
    nya_assert(nya_net_client_entity_remote().index == SERVER_PLAYER.index, "the client learned the wrong remote handle");
  }

  printf("TEST: the replica world is genuinely separate\n");
  {
    /*
     * The property that makes everything below meaningful.
     *
     * With two worlds the client has to spawn the server's entities into its own table, translate handles,
     * and reconcile against a state that is genuinely a round trip old. On a shared table none of that
     * happens and none of it is tested.
     */
    u64 deadline = nya_clock_get_monotonic_ms() + 5000;

    while (!nya_entity_is_valid(nya_net_client_entity()) && nya_clock_get_monotonic_ms() < deadline) {
      tick_server(tick);
      tick_client(tick);
      tick++;

      sleep_ms(2);
    }

    NYA_EntityHandle local = nya_net_client_entity();

    (void)nya_world_set(CLIENT_WORLD);
    nya_assert(nya_entity_is_valid(local), "the client never got a local handle for its own entity");

    printf("  server entity %u, client entity %u\n", SERVER_PLAYER.index, local.index);

    /*
     * The two really are different numbers, which is what says the translation happened.
     *
     * If they matched, a client with no handle translation at all would pass every assertion here — which is
     * exactly how that bug survived until tests/nyangine/net/test_replica.c forced them apart.
     */
    nya_assert(local.index != SERVER_PLAYER.index, "the two worlds handed out the same index; the translation is untested");

    // The client's copy exists in the client's world and not in the server's.
    (void)nya_world_set(CLIENT_WORLD);
    nya_assert(nya_entity_get(local) != nullptr, "the client's entity is not in the client's world");

    u32 client_replicated = 0;
    nya_entity_foreach (entity) {
      if ((entity->flags & FLAG_REPLICATED) != 0) client_replicated++;
    }

    nya_assert(client_replicated == 1, "the client replicated %u entities instead of one", client_replicated);
  }

  printf("TEST: prediction moves the client ahead of the server\n");
  {
    /*
     * The client applies its own command the instant it samples it. With the two sides moving at different
     * speeds on purpose, the client's copy runs away from the server's — which is what makes the correction
     * below observable rather than a fraction of a unit.
     */
    HELD = 1;

    for (u32 i = 0; i < 10; i++) {
      tick_server(tick);
      tick_client(tick);
      tick++;

      sleep_ms(2);
    }

    (void)nya_world_set(CLIENT_WORLD);
    NYA_Entity* predicted = nya_entity_get(nya_net_client_entity());
    nya_assert(predicted != nullptr);

    nya_assert(predicted->position.x > 0.0F, "the client did not predict its own movement at all");

    printf("  client predicted x = %f after ten ticks\n", (f64)predicted->position.x);
  }

  printf("TEST: a disagreeing prediction is corrected and replayed\n");
  {
    /*
     * The whole point of the file.
     *
     * The two movement functions disagree by DIVERGENCE per tick, so every snapshot the client applies
     * contradicts what it predicted. Reconciliation must notice, snap to the server's answer for the tick the
     * snapshot describes, and replay every command since — so the player ends up *recomputed* rather than
     * yanked back to where they were a round trip ago.
     */
    u64 corrections_before = nya_net_client_correction_count();

    u64 deadline = nya_clock_get_monotonic_ms() + 6000;

    while (nya_net_client_correction_count() == corrections_before && nya_clock_get_monotonic_ms() < deadline) {
      tick_server(tick);
      tick_client(tick);
      tick++;

      sleep_ms(2);
    }

    u64 corrections = nya_net_client_correction_count();

    printf("  %llu corrections after %llu ticks\n", (unsigned long long)corrections, (unsigned long long)tick);

    nya_assert(corrections > corrections_before, "a prediction that disagrees with the server was never corrected");

    /*
     * And the client is still ahead of where the server said it was, because the replay put it back.
     *
     * Without the replay the correction would leave the player exactly at the server's position — a round
     * trip in the past — and they would have to walk the distance again on every correction. Being *ahead* of
     * the authoritative position is the evidence that the commands since were re-applied.
     */
    (void)nya_world_set(SERVER_WORLD);
    NYA_Entity* authoritative = nya_entity_get(SERVER_PLAYER);
    nya_assert(authoritative != nullptr);
    f32 server_x = authoritative->position.x;

    (void)nya_world_set(CLIENT_WORLD);
    NYA_Entity* predicted = nya_entity_get(nya_net_client_entity());
    nya_assert(predicted != nullptr);

    printf("  server x = %f, client x = %f\n", (f64)server_x, (f64)predicted->position.x);

    nya_assert(predicted->position.x >= server_x, "the client was left behind the server's position, so nothing was replayed");
  }

  printf("TEST: snapshots keep flowing and being acknowledged\n");
  {
    /*
     * The baseline arena swap, which is only exercised by a client that actually applies deltas.
     *
     * Decoding a delta reads the current baseline while producing the next one, so the two live in arenas used
     * alternately — with one, the reset that frees the old would invalidate what the decode is reading. Many
     * snapshots in a row is what walks that swap repeatedly.
     */
    u64 before = nya_net_client_server_tick();

    for (u32 i = 0; i < 60; i++) {
      tick_server(tick);
      tick_client(tick);
      tick++;

      sleep_ms(2);
    }

    u64 after = nya_net_client_server_tick();

    nya_assert(after > before, "no snapshots arrived over sixty ticks (%llu -> %llu)", (unsigned long long)before,
               (unsigned long long)after);

    printf("  applied server tick %llu -> %llu\n", (unsigned long long)before, (unsigned long long)after);

    // The round trip is measured, which only happens once acknowledgements are flowing both ways.
    NYA_NetPeerStats stats = nya_net_client_stats();

    nya_assert(stats.packets_sent > 0 && stats.packets_received > 0, "no traffic was counted");
    nya_assert(stats.rtt_ms >= 0.0F && stats.rtt_ms < 1000.0F, "a loopback round trip of %f ms is not credible", (f64)stats.rtt_ms);
  }

  printf("TEST: interpolation runs on a remote client\n");
  {
    /*
     * On a loopback this returns immediately, so it is only ever a no-op in the other tests. Here there is a
     * replica map with entries in it and a real snapshot interval to measure.
     */
    (void)nya_world_set(CLIENT_WORLD);

    // Twice, so the second call has a non-zero alpha to advance from.
    nya_net_client_interpolate(TICK_SECONDS);
    nya_net_client_interpolate(TICK_SECONDS);

    // A frame far longer than a snapshot interval must clamp rather than overshoot.
    nya_net_client_interpolate(5.0F);

    NYA_Entity* predicted = nya_entity_get(nya_net_client_entity());
    nya_assert(predicted != nullptr, "interpolation despawned the predicted entity");

    // A zero or negative frame is treated as no smoothing rather than dividing by nothing.
    nya_net_client_interpolate(0.0F);
    nya_net_client_interpolate(-1.0F);
  }

  printf("TEST: the client notices the server going away\n");
  {
    (void)nya_world_set(SERVER_WORLD);
    nya_net_server_stop();

    u64 deadline = nya_clock_get_monotonic_ms() + 5000;

    while (nya_net_client_state() != NYA_NET_CLIENT_DISCONNECTED && nya_clock_get_monotonic_ms() < deadline) {
      tick_client(tick);
      tick++;

      sleep_ms(2);
    }

    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_DISCONNECTED, "the client did not notice the server closing");

    /*
     * And the replicated world went with the connection.
     *
     * These entities existed only because a server said so, and no server is saying so any more. Leaving them
     * would show a frozen snapshot of an ended session — and on reconnect the map would be empty, so every
     * one of them would be spawned a second time.
     */
    (void)nya_world_set(CLIENT_WORLD);
    nya_system_sim_apply_commands();

    u32 remaining = 0;
    nya_entity_foreach (entity) {
      if ((entity->flags & FLAG_REPLICATED) != 0) remaining++;
    }

    nya_assert(remaining == 0, "%u replicated entities survived the disconnection", remaining);

    printf("  reason %d, replicated world cleared\n", (int)nya_net_client_disconnect_reason());
  }

  nya_net_client_disconnect();

  (void)nya_world_set(SERVER_WORLD);

  printf("PASSED: test_prediction (0 failures)\n");

  return EXIT_SUCCESS;
}

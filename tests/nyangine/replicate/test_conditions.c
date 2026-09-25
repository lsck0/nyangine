/**
 * A server and a client over a real socket through a bad network: latency, jitter, loss, duplication and
 * reordering. Prediction, replication and interpolation have to converge anyway, and the bytes have to stay bounded.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#include <time.h>

#define FLAG_REPLICATED (1ULL << 13)

#define TICK_NS     16000000ULL
#define TICK_S      (1.0F / 62.5F)
#define WALK_SPEED  120.0F
#define CRATE_SPEED 90.0F

static NYA_World* SERVER_WORLD = nullptr;
static NYA_World* CLIENT_WORLD = nullptr;

static NYA_EntityHandle SERVER_PLAYER = NYA_ENTITY_HANDLE_NONE;

static u64 HELD = 0;

static void sleep_ns(u64 nanoseconds) {
  struct timespec request = { .tv_sec = (time_t)(nanoseconds / 1000000000ULL), .tv_nsec = (long)(nanoseconds % 1000000000ULL) };
  (void)nanosleep(&request, nullptr);
}

static void walk(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
  if (nya_net_command_holds(command, 0)) entity->position.x += WALK_SPEED * delta_time_s;
}

static void sample(OUT NYA_NetCommand* command) {
  command->actions = HELD;
}

static NYA_EntityHandle spawn_player(NYA_NetPeerId peer, NYA_ConstCString name) {
  nya_unused(peer, name);

  SERVER_PLAYER = nya_entity_spawn(.name = "player", .flags = FLAG_REPLICATED, .position = { 0.0F, 0.0F, 0.0F });
  return SERVER_PLAYER;
}

typedef struct {
  u64 tick;
  u64 next_ns;
} Clock;

/** One tick of both sides at the real tick rate, with the client drawing a frame, as the app loop would. */
static void step(Clock* clock, NYA_EntityHandle crate, b8 crate_moves) {
  (void)nya_world_set(SERVER_WORLD);

  NYA_Entity* moving = nya_entity_get(crate);
  moving->velocity   = (f32x3){ crate_moves ? CRATE_SPEED : 0.0F, 0.0F, 0.0F };
  moving->position  += moving->velocity * TICK_S;

  nya_net_server_tick(clock->tick, TICK_S);
  nya_system_sim_apply_commands();

  (void)nya_world_set(CLIENT_WORLD);
  nya_net_client_tick(clock->tick, TICK_S);
  nya_system_sim_apply_commands();
  nya_net_client_interpolate(TICK_S);

  clock->tick++;
  clock->next_ns += TICK_NS;

  u64 now = nya_clock_get_monotonic_ns();
  if (clock->next_ns > now) sleep_ns(clock->next_ns - now);
}

static f32 client_x_of(NYA_EntityHandle server_handle) {
  (void)nya_world_set(CLIENT_WORLD);

  NYA_Entity* entity = nya_entity_get(nya_net_client_local_entity(server_handle));
  return entity == nullptr ? NAN : entity->position.x;
}

static f32 server_x_of(NYA_EntityHandle handle) {
  (void)nya_world_set(SERVER_WORLD);

  return nya_entity_get(handle)->position.x;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true, .options = { .time_step_ns = TICK_NS } };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  defer nya_system_callback_deinit();

  SERVER_WORLD = nya_world_create();
  CLIENT_WORLD = nya_world_create();

  defer nya_world_destroy(CLIENT_WORLD);
  defer nya_world_destroy(SERVER_WORLD);

  // 120 ms round trip with 20 ms of jitter, 5% loss each way, and a little duplication and reordering.
  NYA_NetConditions bad = { .latency_ms = 60, .jitter_ms = 10, .loss_percent = 5.0F, .duplicate_percent = 1.0F, .reorder_percent = 1.0F };

  (void)nya_world_set(SERVER_WORLD);

  NYA_EntityHandle crate = nya_entity_spawn(.name = "crate", .flags = FLAG_REPLICATED, .position = { 0.0F, 50.0F, 0.0F });

  NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
    .replicated_flag  = FLAG_REPLICATED,
    .on_spawn_player  = nya_callback(spawn_player),
    .on_apply_command = nya_callback(walk),
    .max_speed        = WALK_SPEED,
  }));

  NYA_EXPECT(nya_net_server_listen(0), "the system had no free UDP port");

  const u16 port = nya_net_server_port();

  (void)nya_world_set(CLIENT_WORLD);

  NYA_EXPECT(nya_net_client_connect("127.0.0.1", port, "traveller", (NYA_NetClientConfig){
    .replicated_flag      = FLAG_REPLICATED,
    .on_apply_command     = nya_callback(walk),
    .on_sample_command    = nya_callback(sample),
    .correction_threshold = 0.05F,
    .conditions           = bad,
  }));

  nya_net_transport_condition(_NYA_NET_SERVER.udp, bad);

  Clock clock = { .tick = 1, .next_ns = nya_clock_get_monotonic_ns() };

  printf("TEST: the handshake completes through a bad network\n");
  {
    while (nya_net_client_state() != NYA_NET_CLIENT_PLAYING && clock.tick < 600) step(&clock, crate, false);

    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_PLAYING, "the client never joined through the conditioner");

    // until the player's own entity has been replicated.
    while (!nya_entity_is_valid(nya_net_client_entity()) && clock.tick < 900) step(&clock, crate, false);
    nya_assert(nya_entity_is_valid(nya_net_client_entity()), "the player was never replicated");

    printf("  joined and replicated by tick %llu\n", (unsigned long long)clock.tick);
  }

  printf("TEST: prediction and replication converge after two seconds of movement\n");
  {
    u64 corrections_before = nya_net_client_correction_count();

    (void)nya_world_set(CLIENT_WORLD);
    NYA_NetPeerStats before = nya_net_client_stats();

    u32 ticks = 125;

    HELD = 1;
    for (u32 i = 0; i < ticks; i++) step(&clock, crate, true);
    HELD = 0;

    // a second and a half of standing still for the last commands and snapshots to cross.
    for (u32 i = 0; i < 95; i++) step(&clock, crate, false);

    f32 server_player = server_x_of(SERVER_PLAYER);
    f32 client_player = client_x_of(SERVER_PLAYER);
    f32 server_crate  = server_x_of(crate);
    f32 client_crate  = client_x_of(crate);

    (void)nya_world_set(CLIENT_WORLD);
    NYA_NetPeerStats after = nya_net_client_stats();

    u64 seconds      = 1 + ((u64)(ticks + 95) * TICK_NS / 1000000000ULL);
    u64 down_per_sec = (after.bytes_received - before.bytes_received) / seconds;
    u64 up_per_sec   = (after.bytes_sent - before.bytes_sent) / seconds;
    u64 corrections  = nya_net_client_correction_count() - corrections_before;

    printf("  player: server %.3f, client %.3f; crate: server %.3f, client %.3f\n", (f64)server_player, (f64)client_player, (f64)server_crate, (f64)client_crate);
    printf("  %.0f ms rtt, %.1f ms jitter, %.1f%% loss, %llu resends, %.0f ms interpolation delay, %llu corrections\n", (f64)after.rtt_ms, (f64)after.jitter_ms,
           (f64)(after.packet_loss * 100.0F), (unsigned long long)after.retransmits, (f64)after.interpolation_delay_ms, (unsigned long long)corrections);
    printf("  about %llu B/s down, %llu B/s up\n", (unsigned long long)down_per_sec, (unsigned long long)up_per_sec);

    nya_assert(server_player > WALK_SPEED * 1.5F, "the server player barely moved (%f): commands were lost for good", (f64)server_player);
    nya_assert(fabsf(server_player - client_player) < 0.05F, "prediction did not converge: server %f, client %f", (f64)server_player, (f64)client_player);
    nya_assert(fabsf(server_crate - client_crate) < 1.0F / 32.0F, "the replicated crate did not converge: server %f, client %f", (f64)server_crate, (f64)client_crate);

    nya_assert(after.rtt_ms > 80.0F && after.rtt_ms < 400.0F, "a 120 ms link measured %f ms", (f64)after.rtt_ms);
    nya_assert(after.interpolation_delay_ms > 16.0F, "the interpolation delay did not follow the jitter (%f ms)", (f64)after.interpolation_delay_ms);
    nya_assert(corrections < 20, "%llu corrections for movement both sides compute identically", (unsigned long long)corrections);

    nya_assert(down_per_sec < 16000 && up_per_sec < 8000, "a player and a crate cost %llu B/s down and %llu up", (unsigned long long)down_per_sec, (unsigned long long)up_per_sec);
  }

  printf("TEST: a clean link needs less delay than a jittery one\n");
  {
    (void)nya_world_set(CLIENT_WORLD);
    f32 jittery = nya_net_client_stats().interpolation_delay_ms;

    nya_net_transport_condition(_NYA_NET_SERVER.udp, (NYA_NetConditions){ 0 });

    for (u32 i = 0; i < 250; i++) step(&clock, crate, true);

    (void)nya_world_set(CLIENT_WORLD);
    f32 clean = nya_net_client_stats().interpolation_delay_ms;

    printf("  %.1f ms of delay with jitter, %.1f ms without\n", (f64)jittery, (f64)clean);

    nya_assert(clean < jittery, "the delay did not come down on a clean link (%f then %f)", (f64)jittery, (f64)clean);
    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_PLAYING, "the client dropped out");
  }

  printf("TEST: the overlay line, from either side\n");
  {
    char line[NYA_NET_STATS_LINE_MAX];

    nya_assert(nya_net_stats_line(line, sizeof(line)), "a playing client has nothing to show");
    printf("  client: %s\n", line);

    // the server's view of the same connection, as a dedicated server with no client of its own would show it.
    NYA_NetClientState playing = _NYA_NET_CLIENT.state;
    _NYA_NET_CLIENT.state      = NYA_NET_CLIENT_DISCONNECTED;

    nya_assert(nya_net_stats_line(line, sizeof(line)), "a server with a remote player has nothing to show");
    printf("  server: %s\n", line);

    _NYA_NET_CLIENT.state = playing;
  }

  nya_net_client_disconnect();

  (void)nya_world_set(SERVER_WORLD);
  nya_net_server_stop();

  printf("PASSED: test_conditions (0 failures)\n");

  return EXIT_SUCCESS;
}

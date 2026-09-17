/**
 * What replication costs on the wire: a server and a client over localhost UDP, replicating a scene shaped like
 * gnyame's 2D demo (crates dropped onto a floor until they settle, drones flying, a player walking).
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FLAG_REPLICATED (1ULL << 11)

#define FIRST_PORT 48700
#define TICK_S     (1.0F / 60.0F)

/** Seconds of simulation, and the tail over which the settled figure is taken. */
#define SCENE_SECONDS   20
#define SETTLED_SECONDS 5

/** One crate every CRATE_EVERY ticks, like somebody clicking, until CRATES have fallen. */
#define CRATES      48
#define CRATE_EVERY 12

/** gnyame's GNY_ROBOT_DRONES. */
#define DRONES 6

static NYA_World* SERVER_WORLD = nullptr;
static NYA_World* CLIENT_WORLD = nullptr;

static u64 HELD = 0;

static void apply_movement(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s) {
    if (nya_net_command_holds(command, 0)) entity->position.x += 220.0F * delta_time_s;
    if (nya_net_command_holds(command, 1)) entity->position.x -= 220.0F * delta_time_s;
}

static void sample_command(OUT NYA_NetCommand* command) {
    command->actions = HELD;
}

static NYA_EntityHandle spawn_player(NYA_NetPeerId peer, NYA_ConstCString name) {
    nya_unused(peer, name);

    return nya_entity_spawn(.name = "player", .flags = FLAG_REPLICATED, .position = { 0.0F, 300.0F, 0.0F });
}

typedef struct {
    u64 server_to_client;
    u64 client_to_server;
} Bytes;

static Bytes bytes_now(void) {
    NYA_NetPeerStats stats = nya_net_client_stats();

    return (Bytes){ .server_to_client = stats.bytes_received, .client_to_server = stats.bytes_sent };
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

    (void)nya_world_set(SERVER_WORLD);

    NYA_EntityHandle floor = nya_entity_spawn(.name = "floor", .position = { 0.0F, 420.0F, 0.0F });
    nya_assert(nya_physics2d_body_attach(floor, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 4000.0F, 40.0F }));

    NYA_EntityHandle drones[DRONES];
    for (u32 i = 0; i < DRONES; i++) drones[i] = nya_entity_spawn(.name = "drone", .flags = FLAG_REPLICATED, .position = { 0.0F, 0.0F, 0.0F });

    NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){
        .replicated_flag  = FLAG_REPLICATED,
        .on_spawn_player  = nya_callback(spawn_player),
        .on_apply_command = nya_callback(apply_movement),
    }));

    u16 port = 0;
    for (u16 candidate = FIRST_PORT; candidate < FIRST_PORT + 16 && port == 0; candidate++) {
        if (nya_net_server_listen(candidate).ok) port = candidate;
    }
    nya_assert(port != 0, "could not bind any port in the bench range");

    (void)nya_world_set(CLIENT_WORLD);

    NYA_EXPECT(nya_net_client_connect("127.0.0.1", port, "bench", (NYA_NetClientConfig){
        .replicated_flag   = FLAG_REPLICATED,
        .on_apply_command  = nya_callback(apply_movement),
        .on_sample_command = nya_callback(sample_command),
    }));

    u64 tick = 1;

    for (u64 deadline = nya_clock_get_monotonic_ms() + 5000; nya_net_client_state() != NYA_NET_CLIENT_PLAYING && nya_clock_get_monotonic_ms() < deadline; tick++) {
        (void)nya_world_set(SERVER_WORLD);
        nya_net_server_tick(tick, TICK_S);
        nya_system_sim_apply_commands();

        (void)nya_world_set(CLIENT_WORLD);
        nya_net_client_tick(tick, TICK_S);
        nya_system_sim_apply_commands();

        SDL_Delay(1);
    }

    nya_assert(nya_net_client_state() == NYA_NET_CLIENT_PLAYING, "the bench client never joined");

    Bytes start   = bytes_now();
    Bytes settled = start;

    u32 ticks   = SCENE_SECONDS * 60;
    u32 crates  = 0;

    for (u32 at = 0; at < ticks; at++, tick++) {
        (void)nya_world_set(SERVER_WORLD);

        if (at % CRATE_EVERY == 0 && crates < CRATES) {
            f32 x    = (f32)((s32)(crates * 37 % 600) - 300);
            f32 size = 18.0F + (f32)(crates * 13 % 27);

            NYA_EntityHandle crate = nya_entity_spawn(.name = "crate", .flags = FLAG_REPLICATED, .position = { x, -200.0F, 0.0F });
            nya_assert(nya_physics2d_body_attach(crate, .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { size, size }));

            crates++;
        }

        // drones fly figure eights, written like the robots system writes them: position and velocity every tick.
        for (u32 i = 0; i < DRONES; i++) {
            f32 t = ((f32)at * TICK_S * 0.7F) + ((f32)i * 1.1F);

            NYA_Entity* drone = nya_entity_get(drones[i]);
            drone->velocity   = (f32x3){ cosf(t) * 180.0F, cosf(2.0F * t) * 120.0F, 0.0F };
            drone->position   = (f32x3){ sinf(t) * 260.0F, sinf(2.0F * t) * 60.0F - 100.0F, 0.0F };
        }

        nya_system_physics2d_update(TICK_S);
        nya_net_server_tick(tick, TICK_S);
        nya_system_sim_apply_commands();

        // walks right for two seconds, stands for one, walks left for two.
        u32 phase = (at / 60) % 5;
        HELD      = phase < 2 ? 1 : (phase == 2 ? 0 : 2);

        (void)nya_world_set(CLIENT_WORLD);
        nya_net_client_tick(tick, TICK_S);
        nya_system_sim_apply_commands();
        nya_net_client_interpolate(TICK_S);

        if (at == ticks - (SETTLED_SECONDS * 60)) settled = bytes_now();

        SDL_Delay(1);
    }

    Bytes end = bytes_now();

    u32 replicated = 0;
    nya_entity_foreach (entity) {
        if ((entity->flags & FLAG_REPLICATED) != 0) replicated++;
    }

    NYA_NetPeerStats stats = nya_net_client_stats();

    printf("net scene: %u crates, %d drones, 1 player; %u replicas on the client\n", crates, DRONES, replicated);
    printf("  whole run   down %8.0f B/s   up %6.0f B/s\n", (f64)(end.server_to_client - start.server_to_client) / SCENE_SECONDS,
           (f64)(end.client_to_server - start.client_to_server) / SCENE_SECONDS);
    printf("  settled     down %8.0f B/s   up %6.0f B/s\n", (f64)(end.server_to_client - settled.server_to_client) / SETTLED_SECONDS,
           (f64)(end.client_to_server - settled.client_to_server) / SETTLED_SECONDS);
    printf("  packets     down %llu   up %llu   retransmits %llu\n", (unsigned long long)stats.packets_received, (unsigned long long)stats.packets_sent,
           (unsigned long long)stats.retransmits);

    nya_net_client_disconnect();

    (void)nya_world_set(SERVER_WORLD);
    nya_net_server_stop();

    return EXIT_SUCCESS;
}

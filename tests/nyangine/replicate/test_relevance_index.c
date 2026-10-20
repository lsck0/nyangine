/**
 * The relevance radius filed in a spatial hash against the same radius scanned over every entity.
 *
 * Past _NYA_NET_SERVER_RELEVANCE_INDEX_PEERS the server files each tick's snapshot in a hash and asks it for each
 * peer's candidates; below it, it scans. The law is that a peer cannot tell which: the same entities sent in the same
 * order, and the same set remembered for next tick's hysteresis. Entities wander so that some sit between the enter
 * and leave radii, which is where the two thresholds and the remembered set matter.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#define FLAG_REPLICATED (1ULL << 11)

#define ENTITIES   600
#define PEERS      8
#define TICKS      40
#define WORLD_M    1000.0F
#define RADIUS_M   120.0F
#define WANDER_M   12.0F

typedef struct {
    u64 state;
} Rng;

__attr_no_sanitize("unsigned-integer-overflow") static u64 rng_next(Rng* rng) {
    u64 z = (rng->state += 0x9E3779B97F4A7C15ULL);
    z     = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z     = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;

    return z ^ (z >> 31);
}

static f32 rng_signed(Rng* rng) {
    return ((f32)(rng_next(rng) >> 40) / (f32)(1ULL << 23)) - 1.0F;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);
    defer nya_world_destroy(world);

    NYA_Arena* arena = nya_arena_create(.name = "test_relevance_index");
    defer      nya_arena_destroy(arena);

    _NYA_NET_SERVER.config.relevance_radius = RADIUS_M;

    Rng              rng = { .state = 7 };
    NYA_EntityHandle handles[ENTITIES];

    for (u32 i = 0; i < ENTITIES; i++) {
        f32x3 position = { (rng_signed(&rng) + 1.0F) * WORLD_M * 0.5F, (rng_signed(&rng) + 1.0F) * WORLD_M * 0.5F, rng_signed(&rng) * 40.0F };
        handles[i]     = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = position);
    }

    // one state per peer per path, so each path keeps its own memory of what it sent.
    _NYA_NetServerPeerState* scanned[PEERS];
    _NYA_NetServerPeerState* filed[PEERS];

    for (u32 p = 0; p < PEERS; p++) {
        scanned[p] = nya_arena_alloc(arena, sizeof(_NYA_NetServerPeerState));
        filed[p]   = nya_arena_alloc(arena, sizeof(_NYA_NetServerPeerState));

        nya_memset(scanned[p], 0, sizeof(_NYA_NetServerPeerState));
        nya_memset(filed[p], 0, sizeof(_NYA_NetServerPeerState));

        scanned[p]->public_state.entity = handles[p];
        filed[p]->public_state.entity   = handles[p];
    }

    NYA_Arena* tick_arena = nya_arena_create(.name = "test_relevance_index_tick");
    defer      nya_arena_destroy(tick_arena);

    printf("TEST: the filed relevance radius sends what the scanned one sends\n");

    u32 sent       = 0;
    u32 in_band    = 0;
    f32 leave      = _nya_net_server_relevance_leave();
    f32 enter      = RADIUS_M;

    for (u64 tick = 1; tick <= TICKS; tick++) {
        nya_arena_free_all(tick_arena);

        for (u32 i = 0; i < ENTITIES; i++) {
            NYA_Entity* entity = nya_entity_get(handles[i]);
            entity->position.x += rng_signed(&rng) * WANDER_M;
            entity->position.y += rng_signed(&rng) * WANDER_M;
        }

        NYA_NetSnapshot snapshot = { 0 };
        NYA_EXPECT(nya_net_snapshot_capture(tick_arena, FLAG_REPLICATED, tick, &snapshot));
        nya_check(snapshot.entity_count == ENTITIES, "captured %u of %u", snapshot.entity_count, ENTITIES);

        nya_check(_nya_net_server_relevance_index(tick_arena, &snapshot, _NYA_NET_SERVER_RELEVANCE_INDEX_PEERS - 1) == nullptr,
                  "below the threshold the radius scans");

        const NYA_SpatialHash3D* index = _nya_net_server_relevance_index(tick_arena, &snapshot, PEERS);
        nya_check(index != nullptr, "at %u peers the radius is filed", PEERS);

        for (u32 p = 0; p < PEERS; p++) {
            NYA_NetSnapshot by_scan  = _nya_net_server_relevant(tick_arena, &snapshot, nullptr, scanned[p]);
            NYA_NetSnapshot by_index = _nya_net_server_relevant(tick_arena, &snapshot, index, filed[p]);

            nya_check(by_scan.entity_count == by_index.entity_count, "tick %llu peer %u: the scan sends %u, the hash %u", (unsigned long long)tick, p,
                      by_scan.entity_count, by_index.entity_count);

            u32 shared = nya_min(by_scan.entity_count, by_index.entity_count);
            for (u32 i = 0; i < shared; i++) {
                nya_check(by_scan.entities[i].handle.index == by_index.entities[i].handle.index, "tick %llu peer %u: entry %u differs", (unsigned long long)tick, p, i);
            }

            nya_check(nya_memcmp(scanned[p]->relevant, filed[p]->relevant, sizeof(scanned[p]->relevant)) == 0,
                      "tick %llu peer %u: the remembered sets differ", (unsigned long long)tick, p);

            // count what only hysteresis keeps, so the test is known to reach the case it is about.
            f32x3 at = nya_entity_get(handles[p])->position;
            for (u32 i = 0; i < by_scan.entity_count; i++) {
                f32 distance = nya_vector_length(by_scan.entities[i].position - at);
                if (distance > enter && distance <= leave) in_band++;
            }

            sent += by_scan.entity_count;
        }
    }

    printf("  %u entities sent over %u ticks, %u of them kept only by the hysteresis band\n", sent, TICKS, in_band);
    nya_check(in_band > 0, "the wander puts some entities between the enter and leave radii");

    return nya_check_failures() == 0 ? 0 : 1;
}

/**
 * What the server's relevance radius costs per tick: every remote peer's filter over one captured snapshot, at the
 * snapshot's ceiling of NYA_NET_MAX_REPLICATED entities and below it, for a few peers and for a full server.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#define FLAG_REPLICATED (1ULL << 11)

/** A 4 km square with a 300 m radius: each peer sees about one five-hundredth of it, an open world's ratio. */
#define WORLD_SIDE_M       4000.0F
#define RELEVANCE_RADIUS_M 300.0F

typedef struct {
    u64 state;
} Rng;

__attr_no_sanitize("unsigned-integer-overflow") static u64 rng_next(Rng* rng) {
    u64 z = (rng->state += 0x9E3779B97F4A7C15ULL);
    z     = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z     = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;

    return z ^ (z >> 31);
}

static f32 rng_unit(Rng* rng) {
    return (f32)(rng_next(rng) >> 40) / (f32)(1ULL << 24);
}

/** The peer loop of nya_net_server_tick's snapshot send, without the encoding: what the radius adds to a tick. */
static u32 relevance_tick(NYA_Arena* arena, const NYA_NetSnapshot* snapshot, _NYA_NetServerPeerState** peers, u32 peer_count) {
    u32 kept = 0;

    const NYA_SpatialHash3D* index = _nya_net_server_relevance_index(arena, snapshot, peer_count);

    for (u32 p = 0; p < peer_count; p++) kept += _nya_net_server_relevant(arena, snapshot, index, peers[p]).entity_count;

    return kept;
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

    NYA_Arena* arena = nya_arena_create(.name = "bench_relevance");
    NYA_Arena* tick  = nya_arena_create(.name = "bench_relevance_tick");
    defer      nya_arena_destroy(tick);
    defer      nya_arena_destroy(arena);

    _NYA_NET_SERVER.config.relevance_radius = RELEVANCE_RADIUS_M;

    const u32 entity_counts[] = { 512, 1024, NYA_NET_MAX_REPLICATED };
    const u32 peer_counts[]   = { 1, 2, 4, 8, 16, NYA_NET_MAX_PEERS };

    Rng rng = { .state = 42 };

    for (u32 e = 0; e < nya_carray_length(entity_counts); e++) {
        const u32 entity_count = entity_counts[e];

        // the first NYA_NET_MAX_PEERS entities are the players, the rest whatever the world holds.
        NYA_EntityHandle players[NYA_NET_MAX_PEERS];
        for (u32 i = 0; i < entity_count; i++) {
            f32x3            position = { rng_unit(&rng) * WORLD_SIDE_M, rng_unit(&rng) * WORLD_SIDE_M, 0.0F };
            NYA_EntityHandle handle   = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = position);

            if (i < NYA_NET_MAX_PEERS) players[i] = handle;
        }

        NYA_NetSnapshot snapshot = { 0 };
        NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 1, &snapshot));
        nya_assert(snapshot.entity_count == entity_count, "captured %u of %u", snapshot.entity_count, entity_count);

        _NYA_NetServerPeerState* peers[NYA_NET_MAX_PEERS];
        for (u32 p = 0; p < NYA_NET_MAX_PEERS; p++) {
            peers[p] = nya_arena_alloc(arena, sizeof(_NYA_NetServerPeerState));
            nya_memset(peers[p], 0, sizeof(_NYA_NetServerPeerState));
            peers[p]->public_state.entity = players[p];
        }

        char group[96];
        (void)snprintf(group, sizeof(group), "relevance radius, %u entities", entity_count);
        nya_bench_begin(group);

        for (u32 c = 0; c < nya_carray_length(peer_counts); c++) {
            const u32 peer_count = peer_counts[c];

            char name[64];
            (void)snprintf(name, sizeof(name), "one tick, %u peers", peer_count);

            nya_bench(name, peer_count, {
                nya_arena_free_all(tick);
                nya_bench_keep(relevance_tick(tick, &snapshot, peers, peer_count));
            });
        }

        if (nya_bench_end() != 0) return 1;

        nya_entity_foreach (entity) nya_entity_despawn_deferred(entity->handle);
        nya_system_sim_apply_commands();
        nya_arena_free_all(arena);
    }

    return 0;
}

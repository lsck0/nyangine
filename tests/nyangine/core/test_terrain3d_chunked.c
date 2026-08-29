/**
 * Chunked terrain and GeoMipMapping: the chunk grid, the LOD bands, and what an update rebuilds.
 *
 * Headless, so no mesh is uploaded — which leaves exactly the parts worth pinning. The geometry a
 * chunk emits is a `nya_render3d_mesh_register` call away and that call is stubbed here; what decides
 * whether a landscape looks right is the *policy* around it, and all of that is arithmetic: how the
 * surface is cut up, which level a chunk lands in, and — the one with a real cost — that an update
 * rebuilds only what actually changed.
 *
 * ⚠ The distance measure is deliberately horizontal. A camera high above a landscape is far from
 * every chunk in a straight line, so a 3D distance would drop the whole surface to its coarsest level
 * the moment the viewer climbed, which is the opposite of what looking down at the ground wants.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

enum { KIND_TERRAIN = 9 };

s32 main(void) {
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();

    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);

    defer nya_world_destroy(world);
    defer nya_system_callback_deinit();

    NYA_Arena* arena = nya_arena_create(.name = "test_terrain3d_chunked");
    defer nya_arena_destroy(arena);

    NYA_Window window = { .screen_width = 320, .screen_height = 200 };

    // ── An unchunked terrain has no chunks, and update is a no-op on it.
    {
        NYA_Terrain3D* plain = nullptr;
        NYA_EXPECT(nya_terrain3d_create(arena, (NYA_Terrain3DOptions){ .entity_type = KIND_TERRAIN }, &plain));

        nya_check(plain->chunk_count == 0, "an unchunked terrain has no chunks, got " FMTu32, plain->chunk_count);
        nya_check(plain->chunks == nullptr, "and no chunk array");

        // A caller that may be handed either kind should not have to ask which it has.
        nya_terrain3d_update(plain, &window, f32x3_zero);
        nya_check(plain->chunks_rebuilt == 0, "updating an unchunked terrain does nothing");
    }

    // ── The chunk grid covers the resolution, rounding up.
    {
        NYA_Terrain3D* terrain = nullptr;
        NYA_EXPECT(nya_terrain3d_create(
            arena, (NYA_Terrain3DOptions){ .resolution = 64, .extent = 128.0F, .chunked = true, .entity_type = KIND_TERRAIN }, &terrain));

        u32 expected = 64 / NYA_TERRAIN3D_CHUNK_CELLS;

        nya_check(terrain->chunks_x == expected, "64 cells at %d per chunk is " FMTu32 " across, got " FMTu32, NYA_TERRAIN3D_CHUNK_CELLS,
                  expected, terrain->chunks_x);
        nya_check(terrain->chunk_count == expected * expected, "and " FMTu32 " in total, got " FMTu32, expected * expected,
                  terrain->chunk_count);

        // Every chunk starts unbuilt, which is what makes the first update rebuild all of them with no
        // special case — no level it could pick can equal the out-of-range marker.
        for (u32 i = 0; i < terrain->chunk_count; i++) {
            nya_check(terrain->chunks[i].lod == NYA_TERRAIN3D_LOD_LEVELS, "chunk " FMTu32 " should start unbuilt", i);
            nya_check(terrain->chunks[i].handle[0] != '\0', "and carry its own mesh handle");
        }

        // The handles have to be distinct, or every chunk would overwrite one registered mesh and the
        // surface would be a single square that follows the last chunk built.
        for (u32 i = 1; i < terrain->chunk_count; i++) {
            nya_check(!nya_string_equals(terrain->chunks[i].handle, terrain->chunks[0].handle),
                      "chunk " FMTu32 " must not share chunk 0's handle ('%s')", i, terrain->chunks[i].handle);
        }
    }

    // ── A resolution that is not a whole number of chunks is covered, not refused.
    {
        u32 awkward = (NYA_TERRAIN3D_CHUNK_CELLS * 2) + 3;

        NYA_Terrain3D* terrain = nullptr;
        NYA_EXPECT(nya_terrain3d_create(
            arena, (NYA_Terrain3DOptions){ .resolution = awkward, .extent = 64.0F, .chunked = true, .entity_type = KIND_TERRAIN },
            &terrain));

        nya_check(terrain->chunks_x == 3, "the last chunk is short rather than missing, got " FMTu32 " across", terrain->chunks_x);

        // Every cell has to belong to some chunk, or there is a strip of terrain nothing draws.
        u32 covered = 0;
        for (u32 cx = 0; cx < terrain->chunks_x; cx++) {
            u32 start = cx * NYA_TERRAIN3D_CHUNK_CELLS;
            covered += nya_min((u32)NYA_TERRAIN3D_CHUNK_CELLS, terrain->resolution - start);
        }

        nya_check(covered == awkward, "the chunks should cover every cell: " FMTu32 " against " FMTu32, covered, awkward);
    }

    /*
     * ── The LOD bands: nearer is finer, the levels double, and it saturates.
     *
     * Asserted as ordering and saturation rather than as specific distances. The band boundaries are a
     * policy that should be tunable without this file objecting; what must not change is that they go
     * the right way and stop.
     */
    {
        NYA_Terrain3D* terrain = nullptr;
        NYA_EXPECT(nya_terrain3d_create(arena,
                                        (NYA_Terrain3DOptions){ .resolution = 64,
                                                                .extent       = 128.0F,
                                                                .chunked      = true,
                                                                .lod_distance = 10.0F,
                                                                .entity_type  = KIND_TERRAIN },
                                        &terrain));

        nya_check(nya_terrain3d_lod_for_distance(terrain, 0.0F) == 0, "on top of a chunk is full detail");
        nya_check(nya_terrain3d_lod_for_distance(terrain, 9.9F) == 0, "and so is just inside the first band");
        nya_check(nya_terrain3d_lod_for_distance(terrain, 10.0F) == 1, "the first boundary drops a level");
        nya_check(nya_terrain3d_lod_for_distance(terrain, 20.0F) == 2, "and the next is at twice the distance");

        // Monotone: a chunk further away is never drawn in more detail than a nearer one.
        u32 previous = 0;
        for (f32 distance = 0.0F; distance < 5000.0F; distance += 7.0F) {
            u32 lod = nya_terrain3d_lod_for_distance(terrain, distance);

            nya_check(lod >= previous, "detail must not come back at %f, got " FMTu32 " after " FMTu32, (f64)distance, lod, previous);
            nya_check(lod < NYA_TERRAIN3D_LOD_LEVELS, "and must stay in range, got " FMTu32, lod);

            previous = lod;
        }

        nya_check(nya_terrain3d_lod_for_distance(terrain, 1e9F) == NYA_TERRAIN3D_LOD_LEVELS - 1, "very far saturates at the coarsest");
        nya_check(nya_terrain3d_lod_for_distance(nullptr, 100.0F) == 0, "and nothing at all is level zero rather than a crash");
    }

    /*
     * ── An update rebuilds only what changed, which is the whole reason the levels are banded.
     *
     * A rebuild uploads geometry, so an update that touched every chunk whenever the camera moved
     * would make walking around a landscape stutter continuously rather than never.
     */
    {
        NYA_Terrain3D* terrain = nullptr;
        NYA_EXPECT(nya_terrain3d_create(arena,
                                        (NYA_Terrain3DOptions){ .resolution = 64,
                                                                .extent       = 512.0F,
                                                                .chunked      = true,
                                                                .lod_distance = 60.0F,
                                                                .entity_type  = KIND_TERRAIN },
                                        &terrain));

        nya_terrain3d_generate(terrain, &window, 1234);

        // The first update has to build everything: nothing has geometry yet.
        nya_terrain3d_update(terrain, &window, f32x3_zero);
        nya_check(terrain->chunks_rebuilt == terrain->chunk_count, "the first update builds every chunk, got " FMTu32 " of " FMTu32,
                  terrain->chunks_rebuilt, terrain->chunk_count);

        // Standing still rebuilds nothing at all.
        nya_terrain3d_update(terrain, &window, f32x3_zero);
        nya_check(terrain->chunks_rebuilt == 0, "standing still rebuilds nothing, got " FMTu32, terrain->chunks_rebuilt);

        // A small move stays inside every band, so still nothing.
        nya_terrain3d_update(terrain, &window, (f32x3){ 0.5F, 0.0F, 0.5F });
        nya_check(terrain->chunks_rebuilt == 0, "a small move rebuilds nothing, got " FMTu32, terrain->chunks_rebuilt);

        // A large one crosses bands, and rebuilds *some* — not all, or the banding is doing nothing.
        nya_terrain3d_update(terrain, &window, (f32x3){ 220.0F, 0.0F, 220.0F });
        nya_check(terrain->chunks_rebuilt > 0, "crossing the map should re-level some chunks");
        nya_check(terrain->chunks_rebuilt <= terrain->chunk_count, "and never more than there are");

        // Every chunk that has been built has a real level and real bounds.
        for (u32 i = 0; i < terrain->chunk_count; i++) {
            const NYA_Terrain3DChunk* chunk = &terrain->chunks[i];

            nya_check(chunk->lod < NYA_TERRAIN3D_LOD_LEVELS, "chunk " FMTu32 " should be built, got level " FMTu32, i, chunk->lod);
            nya_check(chunk->radius > 0.0F, "chunk " FMTu32 " should have a bounding radius, got %f", i, (f64)chunk->radius);
        }

        /*
         * The chunk nearest the viewer must be finer than the one furthest away, which is the
         * end-to-end statement of what all of this is for.
         */
        u32 nearest = 0, furthest = 0;
        f32 nearest_distance = FLT_MAX, furthest_distance = 0.0F;

        f32x3 viewer = { 220.0F, 0.0F, 220.0F };

        for (u32 i = 0; i < terrain->chunk_count; i++) {
            f32 dx = terrain->chunks[i].center.x - viewer.x;
            f32 dz = terrain->chunks[i].center.z - viewer.z;

            f32 distance = sqrtf((dx * dx) + (dz * dz));

            if (distance < nearest_distance) { nearest_distance = distance; nearest = i; }
            if (distance > furthest_distance) { furthest_distance = distance; furthest = i; }
        }

        nya_check(terrain->chunks[nearest].lod <= terrain->chunks[furthest].lod,
                  "the nearest chunk must be at least as detailed as the furthest: " FMTu32 " against " FMTu32,
                  terrain->chunks[nearest].lod, terrain->chunks[furthest].lod);

        // Releasing marks them unbuilt again, so a regenerated terrain rebuilds rather than drawing
        // handles that name nothing.
        nya_terrain3d_release(terrain, &window);

        for (u32 i = 0; i < terrain->chunk_count; i++) {
            nya_check(terrain->chunks[i].lod == NYA_TERRAIN3D_LOD_LEVELS, "chunk " FMTu32 " should be unbuilt after release", i);
        }
    }

    /*
     * ── Hysteresis: drifting across a band boundary must not rebuild on every crossing.
     *
     * A chunk sitting on a boundary otherwise changes level every few frames as the viewer moves back
     * and forth over it, and every change uploads geometry. Seen in the demo as one chunk flipping
     * between 72 and 192 vertices while the camera orbited past it, which is what this pins.
     */
    {
        NYA_Terrain3D* terrain = nullptr;
        NYA_EXPECT(nya_terrain3d_create(arena,
                                        (NYA_Terrain3DOptions){ .resolution = 32,
                                                                .extent       = 256.0F,
                                                                .chunked      = true,
                                                                .lod_distance = 100.0F,
                                                                .entity_type  = KIND_TERRAIN },
                                        &terrain));

        nya_terrain3d_generate(terrain, &window, 7);

        nya_check(nya_terrain3d_lod_boundary(terrain, 0) == 0.0F, "level zero begins at the camera");
        nya_check(nya_terrain3d_lod_boundary(terrain, 1) == 100.0F, "level one at the given distance, got %f",
                  (f64)nya_terrain3d_lod_boundary(terrain, 1));
        nya_check(nya_terrain3d_lod_boundary(terrain, 2) == 200.0F, "and each after it at twice the last");

        // Settled first, so the run below is measuring drift rather than the initial build.
        nya_terrain3d_update(terrain, &window, f32x3_zero);

        f32 boundary = nya_terrain3d_lod_boundary(terrain, 1);

        // Back and forth across the first boundary, staying inside the dead band on both sides.
        u32 rebuilds = 0;

        for (u32 i = 0; i < 40; i++) {
            f32 offset = (i % 2 == 0) ? boundary * 0.97F : boundary * 1.03F;

            nya_terrain3d_update(terrain, &window, (f32x3){ offset, 0.0F, 0.0F });
            rebuilds += terrain->chunks_rebuilt;
        }

        // A few, from chunks whose distance happens to fall outside the band. Not forty, which is what
        // one rebuild per crossing would give.
        nya_check(rebuilds < 10, "drifting across a boundary should not rebuild every time, got " FMTu32 " rebuilds over 40 moves",
                  rebuilds);

        // And a move that clears the band entirely still re-levels, or the hysteresis has simply
        // frozen the terrain at whatever it first chose.
        nya_terrain3d_update(terrain, &window, (f32x3){ boundary * 8.0F, 0.0F, 0.0F });
        nya_check(terrain->chunks_rebuilt > 0, "a move well past the band must still re-level");

        nya_terrain3d_release(terrain, &window);
    }

    // ── Height sampling is unaffected by chunking: the collider and the samples are not chunked.
    {
        NYA_Terrain3D* terrain = nullptr;
        NYA_EXPECT(nya_terrain3d_create(
            arena, (NYA_Terrain3DOptions){ .resolution = 32, .extent = 64.0F, .chunked = true, .entity_type = KIND_TERRAIN }, &terrain));

        nya_terrain3d_generate(terrain, &window, 99);

        f32 height = nya_terrain3d_height_at(terrain, 0.0F, 0.0F);

        nya_check(height >= terrain->min_height && height <= terrain->max_height,
                  "a sampled height should be inside the surface's range, got %f in [%f, %f]", (f64)height, (f64)terrain->min_height,
                  (f64)terrain->max_height);

        nya_terrain3d_release(terrain, &window);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}

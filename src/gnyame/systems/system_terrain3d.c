/**
 * @file system_terrain3d.c
 *
 * The 3D scene's ground, created and drawn through nya_terrain3d_*. Only the shape and seed are the game's.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * The terrain itself is nya_terrain3d_*, in the engine. What is left here is the game's opinion about
 * it: the shape constants, which entity type the static body is, and where the handle lives.
 */

NYA_Terrain3D* gny_terrain3d(void) {
    return gny_world()->cube3d.terrain;
}

void gny_terrain3d_generate(NYA_Window* window, NYA_Arena* arena, u64 seed) {
    GNY_World* world = gny_world();

    // Created on first use rather than at world setup, so a save loaded straight into another scene
    // never pays for a height grid it does not draw.
    if (world->cube3d.terrain == nullptr) {
        NYA_EXPECT(
            nya_terrain3d_create(
                arena,
                (NYA_Terrain3DOptions){
                    .resolution   = GNY_TERRAIN3D_RES,
                    .extent       = GNY_TERRAIN3D_EXTENT,
                    .amplitude    = GNY_TERRAIN3D_AMPLITUDE,
                    .rim_start    = GNY_TERRAIN3D_RIM_START,
                    .rim_height   = GNY_TERRAIN3D_RIM_HEIGHT,
                    .frequency    = GNY_TERRAIN3D_FREQUENCY,
                    .octaves      = GNY_TERRAIN3D_OCTAVES,
                    .lacunarity   = GNY_TERRAIN3D_LACUNARITY,
                    .gain         = GNY_TERRAIN3D_GAIN,
                    .friction     = GNY_TERRAIN3D_FRICTION,
                    .color_low    = GNY_TERRAIN3D_COLOR_LOW,
                    .color_mid    = GNY_TERRAIN3D_COLOR_MID,
                    .color_high   = GNY_TERRAIN3D_COLOR_HIGH,
                    .color_peak   = GNY_TERRAIN3D_COLOR_PEAK,
                    .band_mid     = GNY_TERRAIN3D_BAND_MID,
                    .band_high    = GNY_TERRAIN3D_BAND_HIGH,
                    .band_peak    = GNY_TERRAIN3D_BAND_PEAK,
                    .shade_jitter = GNY_TERRAIN3D_SHADE_JITTER,
                    .entity_type  = GNY_ENTITY_TERRAIN,

                    /*
                     * Chunked, so the surface is culled and detailed per square rather than
                     * all-or-nothing.
                     *
                     * On this small scene the camera pass gains little, but the shadow pass draws
                     * the terrain once per cascade, and coarse far levels save those draws. It also
                     * keeps the chunking path exercised.
                     */
                    .chunked      = true,
                    .lod_distance = GNY_TERRAIN3D_LOD_DISTANCE,
                },
                &world->cube3d.terrain
            ),
            "while creating the 3D terrain"
        );
    }

    nya_terrain3d_generate(world->cube3d.terrain, window, seed);

    // named like the 2D ground, so a ray asking for props or crates passes straight through it.
    nya_physics3d_layers_set(nya_entity_get(world->cube3d.terrain->entity), nya_physics_layer(GNY_LAYER_TERRAIN), NYA_PHYSICS_LAYER_ALL);
}

void gny_terrain3d_destroy(NYA_Window* window) {
    NYA_Terrain3D* terrain = gny_terrain3d();
    if (terrain == nullptr) return;

    // The grid itself is the world arena's and goes with it; this is the body and the GPU mesh.
    nya_terrain3d_release(terrain, window);
}

f32 gny_terrain3d_height_at(f32 x, f32 z) {
    const NYA_Terrain3D* terrain = gny_terrain3d();
    if (terrain == nullptr) return 0.0F;

    return nya_terrain3d_height_at(terrain, x, z);
}

void gny_terrain3d_update(f32x3 viewer) {
    NYA_Terrain3D* terrain = gny_terrain3d();
    if (terrain == nullptr) return;

    // Null window: re-levelling a chunk registers a mesh, and nya_render3d_mesh_register does not need
    // a window for anything but the device it reads off the app.
    nya_terrain3d_update(terrain, nya_window_at_slot(0), viewer);
}

void gny_terrain3d_draw(NYA_Window* window) {
    const NYA_Terrain3D* terrain = gny_terrain3d();
    if (terrain == nullptr) return;

    nya_terrain3d_draw(terrain, window);
}

b8 gny_terrain3d_decal_probe(f32x3 origin, f32x3 direction, void* user_data, OUT f32x3* out_point, OUT f32x3* out_normal) {
    nya_unused(user_data);
    nya_assert(direction.x == 0.0F && direction.z == 0.0F && direction.y < 0.0F, "decals only probe straight down");

    const NYA_Terrain3D* terrain = gny_terrain3d();
    if (terrain == nullptr) return false;

    // straight down is all decals ask, which a height lookup answers without a raycast.
    f32 half = GNY_TERRAIN3D_EXTENT * 0.5F;
    if (fabsf(origin.x) > half || fabsf(origin.z) > half) return false;

    f32 height = nya_terrain3d_height_at(terrain, origin.x, origin.z);
    if (height > origin.y || height < origin.y + direction.y) return false;

    // the slope from the heights a cell either side.
    f32 step    = terrain->cell;
    f32 slope_x = nya_terrain3d_height_at(terrain, origin.x + step, origin.z) - nya_terrain3d_height_at(terrain, origin.x - step, origin.z);
    f32 slope_z = nya_terrain3d_height_at(terrain, origin.x, origin.z + step) - nya_terrain3d_height_at(terrain, origin.x, origin.z - step);

    *out_point  = (f32x3){ origin.x, height, origin.z };
    *out_normal = nya_vector_normalize((f32x3){ -slope_x, 2.0F * step, -slope_z });

    return true;
}

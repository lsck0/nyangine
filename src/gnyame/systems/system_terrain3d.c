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
                     * ⚠ This scene is small enough that chunking buys it little on its own — at
                     * GNY_TERRAIN3D_RES the whole surface is a handful of chunks and most of them are
                     * on screen. It is on because the *shadow* pass draws the terrain once per cascade
                     * on top of the camera pass, so a coarse level for the far cascades is four draws
                     * a frame of geometry nobody can resolve. And because a feature nothing in the game
                     * exercises is a feature nobody finds the bugs in.
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

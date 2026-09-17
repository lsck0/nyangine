/**
 * @file layer_background.c
 *
 * Sky, parallax ridges and motes drawn procedurally behind every screen.
 * */
#include "gnyame/gnyame.h"
#include "generated/assets.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */


/** Vertical bands from the top colour to the bottom one. The one plane that ignores the camera. */
NYA_INTERNAL void _gny_background_sky_draw(NYA_Window* window);

/** One ridge of hills: a sine profile drawn as a filled band across the window. */
NYA_INTERNAL void _gny_background_ridge_draw(NYA_Window* window, f32x2 camera, f32 depth, f32 base_y, f32 amplitude, f32 wavelength, NYA_Color color);

/** Slow moving specks, to make motion visible in the empty part of the sky. */
NYA_INTERNAL void _gny_background_motes_draw(NYA_Window* window, f32x2 camera);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON CREATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_background_on_create(NYA_Window* window) {
    nya_unused(window);

    // nothing to build: every plane below is analytic (a sine and a hash), so no texture to load or keep
    // across a reload.
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON DESTROY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_background_on_destroy(NYA_Window* window) {
    nya_unused(window);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON EVENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_background_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window, event);

    // Deliberately inert. This layer is in front of the others in the event order, so consuming
    // anything here would take it away from the game.
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON UPDATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_background_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(delta_time_s);

    // every tick, so an edit to the config file shows at the next frame. This layer is under every screen.
    nya_render_options_set(window, (NYA_RenderOptions){ .msaa_samples = NYA_CONFIG.engine.renderer.msaa_samples });

    gny_config_audio_apply();

    // Nothing to advance for the drawing: the motes are a function of uptime, read at render.
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON RENDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_background_on_render(NYA_Window* window) {
    NYA_Camera2DTopDown view = gny_entity_camera_get();

    // scaled by the zoom, so the background moves less than the world and far planes stay far.
    f32x2 camera = view.position * view.zoom;

    // in the sky's horizon colour, like the 3D fog. the config's fields win where it sets them.
    NYA_Render2DHaze haze = NYA_CONFIG.engine.renderer.haze;

    if (haze.color.a <= 0.0F) haze.color = gny_sky_state().bottom;

    nya_render2d_haze_set(window, haze);

    /*
     * Back to front. Each plane covers the one behind it, which is the only ordering there is. A plane moving at a
     * fraction of the camera's speed is that many times further away, which is how much haze lies between them.
     */
    f32 far_depth  = 0.15F;
    f32 near_depth = 0.35F;

    _gny_background_sky_draw(window);
    _gny_background_motes_draw(window, camera);
    _gny_background_ridge_draw(window, camera, far_depth, 0.42F, 46.0F, 640.0F, GNY_RIDGE_FAR);
    nya_render2d_haze_draw(window, (1.0F / far_depth) - (1.0F / near_depth));
    _gny_background_ridge_draw(window, camera, near_depth, 0.55F, 70.0F, 420.0F, GNY_RIDGE_NEAR);
    nya_render2d_haze_draw(window, (1.0F / near_depth) - 1.0F);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _gny_background_sky_draw(NYA_Window* window) {
    /*
     * Handed to the sky system, which owns the time of day.
     */
    gny_sky_draw(window);
}

void _gny_background_ridge_draw(NYA_Window* window, f32x2 camera, f32 depth, f32 base_y, f32 amplitude, f32 wavelength, NYA_Color color) {
    f32 width  = (f32)window->screen_width;
    f32 height = (f32)window->screen_height;

    // The parallax itself: the plane is offset by the camera's position scaled down by its depth, so
    // a smaller depth means a plane that barely responds to the camera moving.
    f32 offset_x = -camera.x * depth;
    f32 offset_y = -camera.y * depth;

    f32 horizon = (height * base_y) + offset_y;

    f32 column_width = width / (f32)GNY_RIDGE_COLUMNS;

    /*
     * Two sines of different periods, which is enough to stop the profile reading as a repeating
     * wave without needing the noise generator. Sampled per column and filled to the bottom of the
     * window, so the ridge is opaque and hides whatever is behind it.
     */
    for (u32 i = 0; i < GNY_RIDGE_COLUMNS; i++) {
        f32 x     = (f32)i * column_width;
        f32 world = x - offset_x;

        f32 profile = sinf(world / wavelength) + (0.45F * sinf(world / (wavelength * 0.37F)));

        f32 top = horizon - (profile * amplitude);

        // Clamped rather than skipped: a column whose top is below the window still has to be
        // filled, or the ridge develops holes as the camera moves.
        if (top > height) continue;
        if (top < 0.0F) top = 0.0F;

        nya_render2d_rect(window, x, top, column_width + 0.5F, height - top, color);
    }
}

void _gny_background_motes_draw(NYA_Window* window, f32x2 camera) {
    f32 width  = (f32)window->screen_width;
    f32 height = (f32)window->screen_height;

    f32 time_s = nya_app_get()->frame_stats.uptime_s;

    for (u32 i = 0; i < GNY_MOTE_COUNT; i++) {
        /*
         * The engine's integer hash, so each mote gets a stable position without an array to store
         * one in and without an RNG that would have to be seeded somewhere that survives a reload.
         */
        f32 unit_x = (nya_ihash2((s32)i, 0, GNY_MOTE_SEED) * 0.5F) + 0.5F;
        f32 unit_y = (nya_ihash2((s32)i, 1, GNY_MOTE_SEED) * 0.5F) + 0.5F;

        // The nearest plane in the scene drifts on its own as well as with the camera, which is what
        // reads as depth when the camera is standing still.
        f32 depth = 0.04F + (unit_y * 0.06F);
        f32 drift = time_s * (6.0F + (unit_x * 10.0F));

        // Wrapped into the window so a mote that leaves one edge comes back on the other, rather
        // than the field emptying as the camera pans.
        f32 x = fmodf((unit_x * width) - (camera.x * depth) + drift + width, width);
        f32 y = fmodf((unit_y * height) - (camera.y * depth) + height, height);

        f32 radius = 0.9F + (unit_y * 1.4F);

        nya_render2d_circle(window, (f32x2){ x, y }, radius, (NYA_Color){ 0.85F, 0.87F, 1.0F, 0.10F + (unit_x * 0.14F) });
    }
}

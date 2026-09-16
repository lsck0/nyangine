/**
 * @file layer_background.c
 * */
#include "gnyame/gnyame.h"
#include "generated/assets.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Starts the background track on the first tick that finds it loaded.
 * */
NYA_INTERNAL void _gny_music_start_when_ready(void);

/** Writes one frame's span breakdown to the log, once. See GNY_TRACE_LOG_AFTER_S. */
NYA_INTERNAL void _gny_trace_log_once(void);

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

    // Nothing to build for the drawing. Every plane below is analytic — a sine and a hash — rather
    // than an asset, so there is no texture to load and nothing that has to survive a reload.

    // Streamed rather than predecoded, unlike the impact clip. Predecoding a minute of stereo audio
    // means holding it uncompressed for the whole run to save a decode that only happens once, and
    // a track that starts a second into the process has nothing to be late for.
    NYA_Error music = nya_asset_load((NYA_AssetLoadParameters){
        .type     = NYA_ASSET_TYPE_SOUND,
        .handle   = NYA_ASSET_MUSIC_BGM_OPUS,
        .as_sound = { .predecode = false },
    });

    // Not fatal. A machine with no audio device still runs the demo.
    if (!music.ok) nya_log_warn("%s", (NYA_ConstCString)music.message);
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
    nya_unused(window, delta_time_s);

    // Nothing to advance for the drawing: the motes are a function of uptime, read at render.

    _gny_music_start_when_ready();
    _gny_trace_log_once();
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON RENDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_background_on_render(NYA_Window* window) {
    NYA_Camera2DTopDown view = gny_entity_camera_get();

    // Scaled by the zoom, so zooming in moves the background less than the world rather than the
    // same amount — the far planes stay far.
    f32x2 camera = view.position * view.zoom;

    /*
     * Back to front. Each plane covers the one behind it, which is the only ordering there is.
     */
    _gny_background_sky_draw(window);
    _gny_background_motes_draw(window, camera);
    _gny_background_ridge_draw(window, camera, 0.15F, 0.42F, 46.0F, 640.0F, GNY_RIDGE_FAR);
    _gny_background_ridge_draw(window, camera, 0.35F, 0.55F, 70.0F, 420.0F, GNY_RIDGE_NEAR);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _gny_music_start_when_ready(void) {
    GNY_World* world = gny_world();
    if (world->music_started) return;

    NYA_AssetStatus status = nya_asset_status(NYA_ASSET_MUSIC_BGM_OPUS);

    // Still queued. Checked every tick until it resolves, which costs a dictionary lookup on the
    // handful of frames before the load runs.
    if (status == NYA_ASSET_STATUS_LOADING || status == NYA_ASSET_STATUS_UNLOADED) return;

    // Latched either way. A track that failed to decode will not decode on the next tick either,
    // and retrying forever would be a lookup per tick for the life of the process.
    world->music_started = true;

    if (status != NYA_ASSET_STATUS_LOADED) {
        nya_log_warn("The background track '%s' could not be loaded; running without music.", NYA_ASSET_MUSIC_BGM_OPUS);
        return;
    }

    nya_audio_play_music_with(
        NYA_ASSET_MUSIC_BGM_OPUS,
        (NYA_MusicParams){
            // The track's own level in the mix, scaled by what the player asked for. Effective
            // rather than raw, so the master slider moves this too.
            .gain = GNY_MUSIC_GAIN * nya_settings_volume_effective(NYA_VOLUME_CHANNEL_MUSIC),
            .loop = true,

            // Zero, so the whole piece repeats. A track with an intro would name the millisecond the
            // loop returns to instead.
            .loop_start_ms = 0,

            .fade_in_ms = GNY_MUSIC_FADE_IN_MS,
        }
    );

    // Started and then stopped, rather than never started: nya_audio_resume_music has nothing to
    // resume otherwise, and `m` would appear to do nothing until the track had been played once.
    if (GNY_MUSIC_START_MUTED) nya_audio_pause_music();
}

void _gny_trace_log_once(void) {
    GNY_World* world = gny_world();
    if (world == nullptr || world->trace_logged) return;

    NYA_FrameStats stats = nya_app_get()->frame_stats;

    if (stats.uptime_s < GNY_TRACE_LOG_AFTER_S) return;

    world->trace_logged = true;

    u64 frame = nya_perf_frame_current();
    if (frame == 0) return;

    /*
     * Work against period, spelled out, because the two are constantly mistaken for each other.
     */
    nya_log_info("Perf: work %.3f ms, slept %.3f ms, period %.3f ms (%.0f fps, limit %u)", nya_time_ns_to_s(stats.work_ns) * 1000.0,
             nya_time_ns_to_s(stats.sleep_ns) * 1000.0, nya_time_ns_to_s(stats.elapsed_ns) * 1000.0, (f64)stats.fps,
             nya_app_get()->options.frame_rate_limit);

    nya_perf_frame_report(frame - 1);
}

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

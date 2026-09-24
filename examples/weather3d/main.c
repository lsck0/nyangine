/**
 * @file examples/weather3d/main.c
 *
 * Rain and snow, built on the particle system and carried by the shared wind field. One NYA_Weather
 * (render_weather.h) spawns drops or flakes inside a box that follows the camera, so the sky is full
 * wherever it looks without a world-sized particle count. The very same NYA_WindField that leans the
 * (foliage-style) reeds is handed to the weather, so rain slants and snow drifts on exactly the air that
 * bends the grass — composition, not a second wind.
 *
 * ```
 * ./build run example weather3d
 * NYA_WEATHER3D_FRAMES=6 ./build run example weather3d   # draw six frames and quit, for a headless run
 * ```
 *
 * `r` rains, `s` snows, `c` clears, up/down change the intensity, left/right turn the wind, `escape` quits.
 * */
#include "genyarated/assets.h"
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define WINDOW_TITLE  "nyangine — weather"
#define WINDOW_WIDTH  1024
#define WINDOW_HEIGHT 720

#define LAYER_ID "weather"

/** The pool ceiling: the most drops or flakes alive at once, whatever the intensity. */
#define WEATHER_POOL 8192

/** How wide the sky slab is, how tall, how deep, and how far overhead it floats above the look target. */
#define WEATHER_BOX     ((f32x3){ 34.0F, 18.0F, 34.0F })
#define WEATHER_CEILING 12.0F

#define GROUND_HALF 18.0F

/** How far a key press turns the wind and steps the intensity. */
#define WIND_TURN_STEP  0.2618F /* fifteen degrees */
#define INTENSITY_STEP  0.15F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct Weather Weather;

struct Weather {
    NYA_WindowHandle window;

    /** The one wind field the drops and flakes drift on. */
    NYA_WindField wind;
    f32           wind_azimuth;

    /** The falling weather, on the world arena so it shares the world's lifetime; its pool is the ceiling. */
    NYA_Weather* weather;
    f32          intensity;

    /** Total seconds, for the slow camera orbit. */
    f32 elapsed_s;

    /** When non-zero (from NYA_WEATHER3D_FRAMES), the scene quits after this many frames, for a headless run. */
    u32 max_frames;
    u32 frame_count;
};

NYA_INTERNAL Weather* weather_state(void) {
    return nya_world_user_data();
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void weather_layer_on_create(NYA_Window* window) {
    nya_unused(window);

    Weather* state = weather_state();

    // the weather rides the one scene wind, so slant and drift are the wind's doing, not the burst's.
    state->weather = nya_weather_create(nya_world()->allocator, WEATHER_POOL);
    nya_weather_wind_set(state->weather, &state->wind);
    nya_weather_box_set(state->weather, WEATHER_BOX, WEATHER_CEILING);

    // opt in: start with a moderate rain so the example shows something the moment it opens.
    nya_weather_set(state->weather, NYA_WEATHER_RAIN, state->intensity);
}

void weather_layer_on_destroy(NYA_Window* window) {
    nya_unused(window);
}

void weather_layer_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window, event);
}

void weather_layer_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window);

    Weather* state = weather_state();

    if (nya_input_key_pressed(NYA_KEY_ESCAPE)) nya_app_get()->should_quit = true;

    // pick the mode. intensity is kept across a mode switch, so `c` then `r` resumes the same downpour.
    if (nya_input_key_pressed(NYA_KEY_R)) nya_weather_set(state->weather, NYA_WEATHER_RAIN, state->intensity);
    if (nya_input_key_pressed(NYA_KEY_S)) nya_weather_set(state->weather, NYA_WEATHER_SNOW, state->intensity);
    if (nya_input_key_pressed(NYA_KEY_C)) nya_weather_set(state->weather, NYA_WEATHER_CLEAR, state->intensity);

    if (nya_input_key_pressed(NYA_KEY_UP) || nya_input_key_pressed(NYA_KEY_DOWN)) {
        f32 step        = nya_input_key_pressed(NYA_KEY_UP) ? INTENSITY_STEP : -INTENSITY_STEP;
        state->intensity = nya_clamp(state->intensity + step, 0.0F, 1.0F);
        nya_weather_set(state->weather, nya_weather_mode(state->weather), state->intensity);
    }

    // turn the wind; the weather leans and drifts on it because it shares the field.
    if (nya_input_key_pressed(NYA_KEY_LEFT) || nya_input_key_pressed(NYA_KEY_RIGHT)) {
        state->wind_azimuth += nya_input_key_pressed(NYA_KEY_LEFT) ? -WIND_TURN_STEP : WIND_TURN_STEP;
        nya_wind_set(&state->wind, (f32x3){ cosf(state->wind_azimuth), 0.0F, sinf(state->wind_azimuth) }, 4.0F, 0.6F);
    }

    // the one per-frame wind advance, and the weather following the look target so the box tracks the view.
    nya_wind_advance(&state->wind, delta_time_s);
    state->elapsed_s += delta_time_s;

    nya_weather_follow(state->weather, f32x3_zero);
    nya_weather_update(state->weather, delta_time_s);

    // a timed run quits itself once it has drawn its frames, so a headless CI run terminates.
    state->frame_count++;
    if (state->max_frames > 0 && state->frame_count >= state->max_frames) nya_app_get()->should_quit = true;
}

void weather_layer_on_render(NYA_Window* window) {
    nya_assert(window != nullptr);

    Weather* state = weather_state();

    // a slow orbit, so the slant and drift read in three dimensions rather than as a flat scroll.
    f32 orbit = state->elapsed_s * 0.15F;

    nya_render3d_begin(window, (NYA_Camera3DPerspective){
                                   .position  = { sinf(orbit) * 16.0F, 6.0F, cosf(orbit) * 16.0F },
                                   .target    = { 0.0F, 1.0F, 0.0F },
                                   .far_plane = 200.0F,
                               });

    nya_render3d_sky_draw(window, (NYA_Render3DSky){ .sun_direction = nya_vector_normalize((f32x3){ 0.3F, 0.7F, 0.4F }), .sun_angle = 0.03F });

    nya_render3d_light_set(window, (NYA_Render3DLight){
                                       .direction = nya_vector_normalize((f32x3){ -0.3F, -1.0F, -0.4F }),
                                       .color     = NYA_COLOR_WHITE,
                                       .ambient   = 0.45F,
                                       .intensity = 0.9F,
                                   });

    nya_render3d_plane(window, f32x3_zero, (f32x2){ GROUND_HALF * 2.0F, GROUND_HALF * 2.0F }, (NYA_Color){ 0.24F, 0.30F, 0.26F, 1.0F });

    // the weather itself, through the particle draw, inside the 3D pass.
    nya_weather_draw(window, state->weather);

    nya_render3d_end(window);

    NYA_ConstCString mode = nya_weather_mode(state->weather) == NYA_WEATHER_RAIN   ? "rain"
                            : nya_weather_mode(state->weather) == NYA_WEATHER_SNOW ? "snow"
                                                                                   : "clear";

    nya_render2d_textf_with_font(window, NYA_ASSET_FONTS_ALDRICH_TTF, 20.0F, 16.0F, 16.0F, NYA_COLOR_WHITE,
                                 "%s  ·  intensity %.0f%%  ·  %u drops  ·  r rain  s snow  c clear  ·  up/down intensity  ·  arrows turn wind",
                                 mode, (double)(state->intensity * 100.0F), nya_weather_count(state->weather));
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MAIN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();

    NYA_EXPECT(nya_app_init(.app_id = "weather3d"), "while starting the engine");

    Weather* state = nya_arena_alloc(nya_world()->allocator, sizeof(Weather));

    *state = (Weather){
        .window       = NYA_WINDOW_HANDLE_NONE,
        .wind_azimuth = 0.0F,
        .intensity    = 0.6F,
    };

    state->wind = nya_wind_field((NYA_WindOptions){ .direction = { 1, 0, 0 }, .strength = 4.0F, .gustiness = 0.6F });

    nya_world_user_data_set(state);

    // an optional frame budget, so a headless or CI run draws a fixed number of frames and then quits.
    NYA_ConstCString frames = getenv("NYA_WEATHER3D_FRAMES");
    if (frames != nullptr) state->max_frames = (u32)strtoul(frames, nullptr, 10);

    state->window = nya_window_create(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_RESIZABLE);
    nya_assert(nya_window_is_valid(state->window), "the window could not be created");

    nya_layer_push(state->window, nya_layer_of(weather_layer, LAYER_ID));

    nya_app_run();

    nya_app_deinit();

    return 0;
}

/**
 * The UI build pass: one immediate-mode frame declared and laid out, with no GPU and no font.
 *
 * The recorder presenter is the headless seam the tests drive — it measures every widget in cells
 * and records the stream, so what is timed here is the cost of building and laying out a frame, not
 * of drawing it. The tree is a settings screen with a scrolling list: panels, a section, labels,
 * buttons, toggles, sliders, radios, and a table of rows, which is the shape a real screen has.
 *
 * ns/iter is the cost of one frame; frames per second is 1e9 / that. ns/item divides by the widget
 * count, so it reads as the per-widget cost.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define ROWS 48U

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 1280, .screen_height = 800 };

/* The list's backing data, filled once. A real screen draws rows it already has in hand. */
static char row_name[ROWS][16];
static char row_score[ROWS][16];

/* Widget-owned state the tree reads and writes, exactly as a live screen would. */
static b8  state_fullscreen = true;
static b8  state_vsync      = false;
static f32 state_volume     = 0.6F;
static f32 state_gamma      = 1.0F;
static u32 state_quality    = 1;
static b8  state_section    = true;

/**
 * One whole frame. Not one line knows it is running on the recorder rather than the GPU presenter,
 * which is what makes the number the build cost and nothing else.
 * */
static void build_frame(void) {
    NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_DRAW);

    nya_ui_scrim(ui);

    if (nya_ui_panel_begin(ui, "settings", (NYA_UIPanel){ .width = nya_ui_fixed(420), .title = "settings" })) {
        nya_ui_label(ui, "display");

        (void)nya_ui_toggle(ui, "fullscreen", &state_fullscreen);
        (void)nya_ui_toggle(ui, "vsync", &state_vsync);
        (void)nya_ui_slider(ui, "gamma", &state_gamma, 0.5F, 2.0F, 0.05F);

        nya_ui_label(ui, "audio");
        (void)nya_ui_slider(ui, "volume", &state_volume, 0.0F, 1.0F, 0.05F);
        (void)nya_ui_radio(ui, "low", &state_quality, 0);
        (void)nya_ui_radio(ui, "high", &state_quality, 1);

        (void)nya_ui_button(ui, "apply");
        (void)nya_ui_button(ui, "cancel");

        if (nya_ui_section_begin(ui, "leaderboard", &state_section)) {
            const f32              widths[]  = { 0.0F, 80.0F };
            const NYA_ConstCString headers[] = { "player", "score" };

            if (nya_ui_table_begin(ui, "scores", (NYA_UITable){ .widths = widths, .columns = 2, .headers = headers, .striped = true })) {
                for (u32 i = 0; i < ROWS; i++) {
                    if (nya_ui_table_row_begin(ui)) {
                        nya_ui_label(ui, row_name[i]);
                        nya_ui_label(ui, row_score[i]);
                        nya_ui_table_row_end(ui);
                    }
                }

                nya_ui_table_end(ui);
            }

            nya_ui_section_end(ui);
        }

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_settings_init();
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_input_init();
    nya_system_asset_init();

    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);

    defer nya_system_settings_deinit();
    defer nya_system_callback_deinit();
    defer nya_system_events_deinit();
    defer nya_system_input_deinit();
    defer nya_system_asset_deinit();
    defer nya_world_destroy(world);

    for (u32 i = 0; i < ROWS; i++) {
        (void)snprintf(row_name[i], sizeof(row_name[i]), "player %u", i + 1);
        (void)snprintf(row_score[i], sizeof(row_score[i]), "%u", (i * 137) % 10000);
    }

    nya_ui_style_set(&window, (NYA_UIStyle){ .padding = 8.0F, .spacing = 6.0F });

    // The recorder presenter: it measures in cells, so a frame builds and lays out with no face
    // registered and no GPU, which is exactly the headless build pass we want to time.
    static NYA_UIRecorder recorder;
    nya_ui_recorder_init(&recorder, (f32x2){ 8.0F, 16.0F });
    defer nya_ui_recorder_deinit(&recorder);

    nya_ui_presenter_set(&window, nya_ui_recorder_presenter(&recorder));

    // Two passes to settle the persistent layout for every container, then one clean pass whose
    // count is the widgets a frame declares. That count is handed to the harness as the item count.
    build_frame();
    nya_ui_recorder_reset(&recorder);
    build_frame();
    const u64 widgets = nya_ui_recorder_count(&recorder);
    nya_assert(widgets > ROWS, "the frame declared the list and its chrome, got %u", (u32)widgets);

    nya_bench_begin("ui frame build (headless, recorder presenter)");

    // The recorder is reset inside the timed body: a live frame starts from an empty command stream
    // too, so the reset is part of what a frame costs rather than something to hide from it.
    nya_bench("build settings screen + list", widgets, {
        nya_ui_recorder_reset(&recorder);
        build_frame();
        nya_bench_keep(nya_ui_recorder_count(&recorder));
    });

    return nya_bench_end();
}

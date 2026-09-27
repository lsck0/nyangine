/**
 * The key hint lines in every locale the pause menu offers, laid out headless with the game's own style and faces:
 * no hint passes the edge of a 1280 wide window or is split from its key, and a line that broke takes one line again
 * once the window is wide enough for it.
 **/

#include "nyangine-core/nyangine.c"
#include "gnyame/gnyame.c"

#include "SDL3/SDL_init.h"

/** The narrowest window the game is played in, which the German 2D line is wider than. */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/** Wide enough for the longest line whole. */
#define WIDE_WIDTH 3840

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = WINDOW_WIDTH, .screen_height = WINDOW_HEIGHT };

/** The shape presenter, with every label it is handed written down first. */
static NYA_UIPresenter spy;
static NYA_Rectf       labels[GNY_UI_KEYS_MAX];
static u32             label_count = 0;

static void spy_draw(void* state, NYA_Window* target, const NYA_UIWidgetDraw* widget) {
    if (widget->kind == NYA_UI_WIDGET_LABEL) {
        nya_assert(label_count < GNY_UI_KEYS_MAX, "more labels than one line of hints can hold");
        labels[label_count++] = widget->rect;
    }

    nya_ui_presenter_shape()->draw(state, target, widget);
}

/** `keys` in the 2D HUD's panel, drawn until the faces have loaded and the panel has been measured. */
static void keys_draw(NYA_ConstCString keys) {
    for (u32 pass = 0; pass < 32; pass++) {
        nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
        label_count = 0;

        nya_ui_style_set(&window, nya_config_engine()->ui);
        NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_DRAW);

        if (nya_ui_panel_begin(ui, "keys", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_BOTTOM_LEFT, .overflow = NYA_UI_OVERFLOW_WRAP, .text = NYA_UI_TEXT_SMALL })) {
            gny_ui_keys(ui, keys, NYA_COLOR_WHITE);
            nya_ui_panel_end(ui);
        }

        nya_ui_end(ui);
    }
}

static u32 hints_count(NYA_ConstCString keys) {
    u32 count = 1;
    for (NYA_ConstCString at = strstr(keys, GNY_UI_KEYS_SEPARATOR); at != nullptr; at = strstr(at + strlen(GNY_UI_KEYS_SEPARATOR), GNY_UI_KEYS_SEPARATOR)) count++;
    return count;
}

/** The lines the last pass laid the labels on, told apart by their tops. */
static u32 lines_count(void) {
    u32 lines = 0;

    for (u32 i = 0; i < label_count; i++) {
        b8 seen = false;
        for (u32 j = 0; j < i; j++) seen = seen || labels[j].y == labels[i].y;
        if (!seen) lines++;
    }

    return lines;
}

s32 main(void) {
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_settings_init();
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_asset_init();
    nya_system_i18n_init();
    nya_system_config_init();

    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);

    defer nya_system_settings_deinit();
    defer nya_system_callback_deinit();
    defer nya_system_events_deinit();
    defer nya_system_asset_deinit();
    defer nya_system_i18n_deinit();
    defer nya_system_config_deinit();
    defer nya_world_destroy(world);

    gny_fonts_register();

    spy      = *nya_ui_presenter_shape();
    spy.name = "spy";
    spy.draw = spy_draw;
    nya_ui_presenter_set(&window, &spy);

    // Every line in every locale stays inside the window, one label per hint.
    for (u32 i = 0; i < nya_carray_length(_GNY_LOCALES); i++) {
        NYA_EXPECT(nya_i18n_load(_GNY_LOCALES[i].locale, NYA_STRING_KEYS, NYA_STRING_COUNT));

        NYA_ConstCString lines[] = { nya_string_hud_keys(), nya_string_cube3d_keys(), nya_string_cube3d_render_keys() };

        for (u32 line = 0; line < nya_carray_length(lines); line++) {
            keys_draw(lines[line]);

            nya_check(label_count == hints_count(lines[line]), "%s line %u: a label per hint, got %u of %u", _GNY_LOCALES[i].locale, line, label_count, hints_count(lines[line]));

            for (u32 label = 0; label < label_count; label++) {
                NYA_Rectf rect = labels[label];

                nya_check(rect.width > 0.0F, "%s line %u hint %u was measured", _GNY_LOCALES[i].locale, line, label);
                nya_check(rect.x >= 0.0F && rect.x + rect.width <= (f32)WINDOW_WIDTH, "%s line %u hint %u ends at %f, past %u", _GNY_LOCALES[i].locale,
                          line, label, (f64)(rect.x + rect.width), WINDOW_WIDTH);
                nya_check(rect.y >= 0.0F && rect.y + rect.height <= (f32)WINDOW_HEIGHT, "%s line %u hint %u is inside the window's height", _GNY_LOCALES[i].locale, line, label);
            }
        }
    }

    // The German 2D line is the one that did not fit: it breaks, and takes one line again in a window wide enough.
    {
        NYA_EXPECT(nya_i18n_load("de", NYA_STRING_KEYS, NYA_STRING_COUNT));

        keys_draw(nya_string_hud_keys());
        nya_check(lines_count() > 1, "the German line breaks at %u, got %u lines", WINDOW_WIDTH, lines_count());

        window.screen_width = WIDE_WIDTH;
        keys_draw(nya_string_hud_keys());
        nya_check(lines_count() == 1, "and is whole again at %u, got %u lines", WIDE_WIDTH, lines_count());

        window.screen_width = WINDOW_WIDTH;
    }

    return nya_check_failures() == 0 ? 0 : 1;
}

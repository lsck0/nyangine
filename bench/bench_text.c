/**
 * Shaping: what a frame of text costs laid out from scratch, and read back from the run cache.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

/** The point size the HUD is drawn at. Shaping cost is per glyph, not per pixel, but wrapping is not. */
#define HUD_POINT_SIZE 16.0F

/** A title, which is the other shape of text a frame draws: few glyphs, large, measured for centring. */
#define TITLE_POINT_SIZE 48.0F

/**
 * One frame of a debug overlay, copied in shape from debug_overlay.c and layer_ui.c.
 * */
static NYA_ConstCString hud_lines[] = {
    "  12.40 ms work    16.67 wall    60 fps",
    "avg   12.91     worst   31.02",
    "   14 draws     18432 verts",
    "    9x texture       0 dropped",
    "mem    41.2 MiB total",
    "  render                12.0 MiB",
    "  assets                 9.4 MiB",
    "  frame                  2.1 MiB",
    "  physics                1.8 MiB",
    "  audio                  0.9 MiB",
    "ceilings    19 tracked",
    "  glyphs_per_atlas       412/512    80%",
    "  text_run_glyphs        128/1024   13%",
    "  entities                64/8192    1%",
    "  tweens                   3/512     1%",
    "bodies 64   spawned 128   lost 12",
    "solver 0.42 ms   hits 7",
    "draw calls 14   verts 18432",
    "AVATAR",
    "position 128.0, 64.0   velocity 0.0, -9.81",
};

/** A paragraph, for the wrapped case. The shaper does the line breaking, which is not free. */
#define PARAGRAPH                                                                                                                                  \
    "Human negligence made the machines take over. You go back down for what is left: ore, power "                                                  \
    "cells, and whatever the last expedition did not carry out with them."

/** Runs frames until the cached cases' faces have loaded. */
static b8 faces_load(void) {
    for (u32 i = 0; i < 16; i++) {
        if (nya_text_font_for(FACE, HUD_POINT_SIZE) != nullptr && nya_text_font_for(FACE, TITLE_POINT_SIZE) != nullptr) return true;

        nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
    }

    return false;
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    // the cached cases resolve faces through the asset system.
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_asset_init();

    defer nya_system_asset_deinit();
    defer nya_system_events_deinit();
    defer nya_system_callback_deinit();

    nya_assert(TTF_Init(), "TTF_Init failed: %s", SDL_GetError());
    defer TTF_Quit();

    TTF_Font* hud = TTF_OpenFont(FACE, HUD_POINT_SIZE);
    nya_assert(hud != nullptr, "could not open " FACE ": %s", SDL_GetError());
    defer TTF_CloseFont(hud);

    TTF_Font* title = TTF_OpenFont(FACE, TITLE_POINT_SIZE);
    nya_assert(title != nullptr, "could not open " FACE " at title size: %s", SDL_GetError());
    defer TTF_CloseFont(title);

    // Static rather than on the stack: NYA_TextRun is about thirty kilobytes, which render_text.h says
    // is why a caller keeps one and reuses it. A benchmark that allocated one per iteration would be
    // measuring the memset.
    static NYA_TextRun run;

    u32 hud_line_count = (u32)nya_carray_length(hud_lines);

    u64 hud_glyphs = 0;
    for (u32 i = 0; i < hud_line_count; i++) hud_glyphs += nya_utf8_count(hud_lines[i]);

    nya_bench_begin("shaping");

    /*
     * The headline: one frame of a HUD, shaped from nothing.
     */
    nya_bench("hud frame, 20 lines", hud_glyphs, {
        for (u32 i = 0; i < hud_line_count; i++) {
            nya_bench_keep(nya_text_shape(hud, hud_lines[i], 0, 0, &run));
        }
    });

    // one short line, what a call site asks for. If twenty of these match the figure above, batching
    // gains nothing and only memoising is left.
    nya_bench("one hud line", nya_utf8_count(hud_lines[0]), {
        nya_bench_keep(nya_text_shape(hud, hud_lines[0], 0, 0, &run));
    });

    // Measuring without keeping the glyphs, which is the *other* half of what a frame does: every
    // centred label and every panel sized to its text measures before it draws, so a HUD pays this on
    // top of the shaping above rather than instead of it.
    nya_bench("measure one hud line", nya_utf8_count(hud_lines[0]), {
        nya_bench_keep(nya_text_measure_font(hud, hud_lines[0], 0).x);
    });

    // A title: few glyphs, and the case a memo would have served best, since it does not change.
    nya_bench("title, 6 glyphs", 6, { nya_bench_keep(nya_text_shape(title, "AVATAR", 0, 0, &run)); });

    // Wrapped, where the shaper breaks the lines. Line breaking is the part that is more
    // work than the old codepoint walk did, rather than the same work done properly.
    nya_bench("paragraph, wrapped to 320px", nya_utf8_count(PARAGRAPH), {
        nya_bench_keep(nya_text_shape(hud, PARAGRAPH, 0, 320, &run));
    });

    // The same paragraph unwrapped, so the pair prices the wrapping alone.
    nya_bench("paragraph, unwrapped", nya_utf8_count(PARAGRAPH), {
        nya_bench_keep(nya_text_shape(hud, PARAGRAPH, 0, 0, &run));
    });

    if (nya_bench_end() != 0) return 1;

    nya_assert(faces_load(), "the faces did not load through the asset system");

    nya_bench_begin("shaping, cached");

    /*
     * The same cases through nya_text_shape_with_font once the first call has filled the cache: the handle, the
     * lookup, and copying the laid out text into the run.
     */
    nya_bench("hud frame, 20 lines", hud_glyphs, {
        for (u32 i = 0; i < hud_line_count; i++) {
            nya_bench_keep(nya_text_shape_with_font(FACE, HUD_POINT_SIZE, hud_lines[i], 0, &run));
        }
    });

    nya_bench("one hud line", nya_utf8_count(hud_lines[0]), {
        nya_bench_keep(nya_text_shape_with_font(FACE, HUD_POINT_SIZE, hud_lines[0], 0, &run));
    });

    nya_bench("measure one hud line", nya_utf8_count(hud_lines[0]), {
        nya_bench_keep(nya_text_measure_with_font(FACE, HUD_POINT_SIZE, hud_lines[0], 0).x);
    });

    nya_bench("title, 6 glyphs", 6, { nya_bench_keep(nya_text_shape_with_font(FACE, TITLE_POINT_SIZE, "AVATAR", 0, &run)); });

    nya_bench("paragraph, wrapped to 320px", nya_utf8_count(PARAGRAPH), {
        nya_bench_keep(nya_text_shape_with_font(FACE, HUD_POINT_SIZE, PARAGRAPH, 320, &run));
    });

    return nya_bench_end();
}

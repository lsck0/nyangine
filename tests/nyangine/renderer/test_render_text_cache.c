/**
 * The shaped run cache: a string laid out once is read back, and a reloaded or changed face lays it out again.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE       "./assets/fonts/Aldrich.ttf"
#define POINT_SIZE 24.0F
#define HANDLE     FACE "@24"

static void end_frame(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
}

static TTF_Font* pump_until_loaded(void) {
    for (u32 i = 0; i < 16; i++) {
        TTF_Font* face = nya_text_font_for(FACE, POINT_SIZE);
        if (face != nullptr) return face;

        end_frame();
    }

    return nullptr;
}

/** The cache's entry count, read through its ceiling row. */
static u32 text_runs_live(void) {
    for (u32 i = 0; i < nya_ceiling_count(); i++) {
        if (nya_string_equals(nya_ceiling_name_at(i), "text_runs")) return nya_ceiling_live_at(i);
    }

    return 0;
}

static b8 runs_equal(const NYA_TextRun* a, const NYA_TextRun* b) {
    if (a->glyph_count != b->glyph_count || a->line_count != b->line_count) return false;
    if (a->width != b->width || a->height != b->height) return false;

    for (u32 i = 0; i < a->glyph_count; i++) {
        const NYA_TextGlyph* x = &a->glyphs[i];
        const NYA_TextGlyph* y = &b->glyphs[i];

        if (x->glyph_index != y->glyph_index || x->x != y->x || x->y != y->y || x->width != y->width || x->line != y->line) return false;
    }

    for (u32 i = 0; i < a->line_count; i++) {
        if (a->lines[i].first_glyph != b->lines[i].first_glyph || a->lines[i].glyph_count != b->lines[i].glyph_count) return false;
    }

    return true;
}

static NYA_TextRun cached;
static NYA_TextRun direct;

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_asset_init();

    defer nya_system_callback_deinit();
    defer nya_system_events_deinit();
    defer nya_system_asset_deinit();

    // ── Nothing to shape while the face loads.
    {
        nya_check(!nya_text_shape_with_font(FACE, POINT_SIZE, "Hello", 0, &cached), "a face still queued shapes nothing");
        nya_check(cached.glyph_count == 0, "and leaves the run empty");
        nya_check(nya_text_measure_with_font(FACE, POINT_SIZE, "Hello", 0).x == 0.0F, "and measures zero");
    }

    TTF_Font* face = pump_until_loaded();
    nya_check(face != nullptr, "the face should load within a few frames");
    if (face == nullptr) return 1;

    // ── A cached run is the run nya_text_shape produces, the first time and every time after.
    {
        u32 before = text_runs_live();

        nya_check(nya_text_shape(face, "Wave, AVA.", 0, 0, &direct), "direct shaping should succeed");
        nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, "Wave, AVA.", 0, &cached), "cached shaping should succeed");
        nya_check(runs_equal(&direct, &cached), "the first cached run should match the direct one");
        nya_check(text_runs_live() == before + 1, "and occupy one entry, got " FMTu32 " after " FMTu32, text_runs_live(), before);

        nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, "Wave, AVA.", 0, &cached), "a second call should succeed");
        nya_check(runs_equal(&direct, &cached), "and read back the same run");
        nya_check(text_runs_live() == before + 1, "without a new entry");

        f32x2 measured = nya_text_measure_with_font(FACE, POINT_SIZE, "Wave, AVA.", 0);
        nya_check(measured.x == (f32)direct.width && measured.y == (f32)direct.height, "measuring shares the entry and the box");
        nya_check(text_runs_live() == before + 1, "and adds nothing");
    }

    // ── The wrap width is part of the key.
    {
        static const char paragraph[] = "The quick brown fox jumps over the lazy dog, twice.";

        u32 before = text_runs_live();

        nya_check(nya_text_shape(face, paragraph, 0, 160, &direct), "direct wrapped shaping should succeed");
        nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, paragraph, 160, &cached), "cached wrapped shaping should succeed");
        nya_check(runs_equal(&direct, &cached), "a wrapped run should match too");
        nya_check(cached.line_count > 1, "and wrap, got " FMTu32 " lines", cached.line_count);

        nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, paragraph, 0, &cached), "unwrapped should succeed");
        nya_check(cached.line_count == 1, "and not reuse the wrapped layout, got " FMTu32 " lines", cached.line_count);
        nya_check(text_runs_live() == before + 2, "one entry per wrap width");
    }

    // ── An empty string and a string too long for a key both still shape.
    {
        nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, "", 0, &cached), "an empty string is not a failure");
        nya_check(cached.line_count == 1 && cached.glyph_count == 0, "and is one empty line");

        char long_text[NYA_TEXT_RUN_CACHE_KEY_MAX * 2];
        for (u32 i = 0; i + 1 < sizeof(long_text); i++) long_text[i] = (char)('a' + (i % 26));
        long_text[sizeof(long_text) - 1] = '\0';

        u32 before = text_runs_live();

        nya_check(nya_text_shape(face, long_text, 0, 0, &direct), "direct shaping of a long string should succeed");
        nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, long_text, 0, &cached), "a key too long to cache still shapes");
        nya_check(runs_equal(&direct, &cached), "to the same run");
        nya_check(text_runs_live() == before, "without an entry");
    }

    // ── Changing the face relays out a cached text: a distance field grows every glyph.
    {
        nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, "Hello", 0, &direct), "coverage shaping should succeed");

        nya_check(TTF_SetFontSDF(face, true), "the face should take a distance field");
        nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, "Hello", 0, &cached), "shaping after the change should succeed");
        nya_check(cached.glyphs[0].width > direct.glyphs[0].width, "the cached text should be laid out again, %d against %d", cached.glyphs[0].width,
                  direct.glyphs[0].width);

        nya_check(TTF_SetFontSDF(face, false), "and give it back");
        nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, "Hello", 0, &cached), "shaping after turning it off should succeed");
        nya_check(runs_equal(&direct, &cached), "and match the coverage run again");
    }

    // ── A reloaded face has a new generation, so the entry is replaced rather than read with a closed face.
    {
        nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, "Hello", 0, &direct), "shaping before the reload should succeed");

        u64 generation = nya_asset_get(HANDLE)->generation;
        u32 before     = text_runs_live();

        nya_check(nya_asset_unload(HANDLE), "the face should unload");
        end_frame();

        NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
            .type    = NYA_ASSET_TYPE_FONT,
            .handle  = HANDLE,
            .source  = FACE,
            .as_font = { .point_size = POINT_SIZE },
        }));

        face = pump_until_loaded();
        nya_check(face != nullptr, "the face should load again");
        nya_check(nya_asset_get(HANDLE)->generation != generation, "with a new generation");

        nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, "Hello", 0, &cached), "shaping after the reload should succeed");
        nya_check(runs_equal(&direct, &cached), "and lay the text out with the new face, got " FMTu32 " glyphs", cached.glyph_count);
        nya_check(text_runs_live() == before, "replacing the stale entry instead of adding one");
    }

    // ── Full, the least recently used entry goes and the count holds at the capacity.
    {
        char text[32];

        for (u32 i = 0; i < NYA_TEXT_RUN_CACHE_CAPACITY + 16; i++) {
            (void)snprintf(text, sizeof(text), "row " FMTu32, i);
            nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, text, 0, &cached), "shaping row " FMTu32 " should succeed", i);
        }

        nya_check(text_runs_live() == NYA_TEXT_RUN_CACHE_CAPACITY, "the cache should hold at capacity, got " FMTu32, text_runs_live());

        nya_check(nya_text_shape(face, "row 0", 0, 0, &direct), "direct shaping of an evicted row should succeed");
        nya_check(nya_text_shape_with_font(FACE, POINT_SIZE, "row 0", 0, &cached), "an evicted row shapes again");
        nya_check(runs_equal(&direct, &cached), "to the same run");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}

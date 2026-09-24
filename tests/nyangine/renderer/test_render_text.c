/**
 * Shaping: a string and a face become positioned glyph indices, headless.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

s32 main(void) {
    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_assert(TTF_Init(), "TTF_Init failed: %s", SDL_GetError());
    defer TTF_Quit();

    // Opened directly rather than through the asset system: this is a test of shaping, and the asset system would drag a whole app instance in to reach the same TTF_Font.
    TTF_Font* font = TTF_OpenFont(FACE, 24.0F);
    nya_assert(font != nullptr, "could not open " FACE ": %s", SDL_GetError());
    defer TTF_CloseFont(font);

    static NYA_TextRun run;

    // ── A shaped run has one glyph per character, positioned left to right.
    {
        nya_check(nya_text_shape(font, "Hello", 0, 0, &run), "shaping should succeed");
        nya_check(run.glyph_count == 5, "five letters should shape to five glyphs, got " FMTu32, run.glyph_count);
        nya_check(run.line_count == 1, "on one line, got " FMTu32, run.line_count);
        nya_check(run.width > 0 && run.height > 0, "and occupy a box, got %dx%d", run.width, run.height);
        nya_check(!run.overflowed, "well inside the run's capacity");

        for (u32 i = 1; i < run.glyph_count; i++) {
            nya_check(run.glyphs[i].x >= run.glyphs[i - 1].x, "glyph " FMTu32 " should not be left of the one before it", i);
        }

        // Glyph *indices*, not codepoints. 'H' is 72 and no face puts it at index 72; the check that matters is that the two letters of "ll" share an index and 'H' does not.
        nya_check(run.glyphs[2].glyph_index == run.glyphs[3].glyph_index, "the two l's should be the same glyph");
        nya_check(run.glyphs[0].glyph_index != run.glyphs[1].glyph_index, "H and e should not be");
    }

    /* Kerning happens, which is why text is shaped. "AV" is the canonical kerned pair: the diagonals nest, so a kerning face draws them closer than the sum of their advances. Compared against "AH", since an absolute number would only pin Aldrich. */
    {
        f32x2 kerned   = nya_text_measure_font(font, "AV", 0);
        f32x2 unkerned = nya_text_measure_font(font, "AH", 0);

        nya_check(kerned.x > 0.0F && unkerned.x > 0.0F, "both pairs should measure");
        nya_check(kerned.x < unkerned.x, "AV should be tighter than AH: %f against %f", (f64)kerned.x, (f64)unkerned.x);
    }

    // ── Measuring and shaping report the same box, because they are the same layout.
    {
        NYA_ConstCString sample = "The quick brown fox";

        nya_check(nya_text_shape(font, sample, 0, 0, &run), "shaping should succeed");

        f32x2 measured = nya_text_measure_font(font, sample, 0);

        nya_check((s32)measured.x == run.width, "measured width %f against laid out %d", (f64)measured.x, run.width);
        nya_check((s32)measured.y == run.height, "measured height %f against laid out %d", (f64)measured.y, run.height);
    }

    // ── A newline starts a line, and the run's lines cover every glyph exactly once.
    {
        nya_check(nya_text_shape(font, "one\ntwo\nthree", 0, 0, &run), "shaping should succeed");
        nya_check(run.line_count == 3, "three lines, got " FMTu32, run.line_count);

        u32 counted = 0;
        for (u32 line = 0; line < run.line_count; line++) counted += run.lines[line].glyph_count;

        nya_check(counted == run.glyph_count, "every glyph should belong to exactly one line: " FMTu32 " against " FMTu32,
                  counted, run.glyph_count);

        nya_check(run.lines[1].y > run.lines[0].y, "the second line should sit below the first");
    }

    // ── Wrapping is the shaper's, and it makes a long string taller and no wider than asked.
    {
        NYA_ConstCString paragraph = "the quick brown fox jumps over the lazy dog again and again";

        f32x2 unwrapped = nya_text_measure_font(font, paragraph, 0);
        f32x2 wrapped   = nya_text_measure_font(font, paragraph, 200);

        nya_check(wrapped.x <= 200.0F, "a wrapped line should fit the wrap width, got %f", (f64)wrapped.x);
        nya_check(wrapped.x < unwrapped.x, "and be narrower than the unwrapped form");
        nya_check(wrapped.y > unwrapped.y, "at the cost of being taller");

        nya_check(nya_text_shape(font, paragraph, 0, 200, &run), "shaping with a wrap should succeed");
        nya_check(run.line_count > 1, "and produce more than one line, got " FMTu32, run.line_count);
    }

    // ── The empty and degenerate cases, which callers reach constantly.
    {
        nya_check(nya_text_shape(font, "", 0, 0, &run), "an empty string is not a failure");
        nya_check(run.glyph_count == 0, "with no glyphs");
        nya_check(run.line_count == 1, "but still one line, since an empty row still occupies one");
        nya_check(run.height > 0, "of the font's line height, got %d", run.height);

        nya_check(!nya_text_shape(nullptr, "x", 0, 0, &run), "a null font fails rather than crashing");
        nya_check(!nya_text_shape(font, nullptr, 0, 0, &run), "and so does null text");

        nya_check(nya_text_measure_font(nullptr, "x", 0).x == 0.0F, "measuring with no font is zero");
        nya_check(nya_text_line_height(nullptr) == 0.0F, "and so are the metrics");
    }

    // ── Vertical metrics, including the sign flip on the descent.
    {
        nya_check(nya_text_line_height(font) > 0.0F, "a line height");
        nya_check(nya_text_ascent(font) > 0.0F, "a positive ascent");
        nya_check(nya_text_descent(font) > 0.0F, "and a descent reported positive, unlike SDL's own");
    }

    // ── A font handle spells its size the way "%.0f" does, rounding half to even.
    {
        const f32 sizes[] = { 17.0F, 44.0F, 22.4F, 22.5F, 23.5F, 0.3F, 1.0F, 100.0F, -3.0F, 1e10F };

        char handle[NYA_TEXT_FONT_HANDLE_MAX];
        char expected[NYA_TEXT_FONT_HANDLE_MAX];

        for (u32 i = 0; i < nya_carray_length(sizes); i++) {
            nya_text_font_handle(FACE, sizes[i], handle, sizeof(handle));
            (void)snprintf(expected, sizeof(expected), "%s@%.0f", FACE, (f64)sizes[i]);

            nya_check(nya_string_equals(handle, expected), "size %f should spell '%s', got '%s'", (f64)sizes[i], expected, handle);
        }

        char short_handle[16];
        char short_expected[16];

        nya_text_font_handle(FACE, 24.0F, short_handle, sizeof(short_handle));
        (void)snprintf(short_expected, sizeof(short_expected), "%s@%.0f", FACE, 24.0);

        nya_check(nya_string_equals(short_handle, short_expected), "a handle too long truncates like snprintf, got '%s'", short_handle);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}

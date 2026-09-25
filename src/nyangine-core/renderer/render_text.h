/**
 * @file render_text.h
 *
 * ```c
 * NYA_TextRun run;
 * if (nya_text_shape(font, "Wave, AVA.", 0, 0, &run)) {
 *     for (u32 i = 0; i < run.glyph_count; i++) {
 *         const NYA_TextGlyph* glyph = &run.glyphs[i];
 *         // glyph->glyph_index keys the atlas; x and y are already kerned.
 *     }
 * }
 * ```
 *
 * A run holds glyph indices, not codepoints. One codepoint can shape into several glyphs (a mark
 * cluster) and several into one (a ligature), so an atlas must be keyed by glyph index too.
 *
 * nya_text_shape does one allocating `TTF_CreateText` per call. Shape once per string and walk the run,
 * never per character.
 *
 * Text drawn every frame goes through nya_text_shape_with_font instead, which keeps the laid out text in a
 * cache keyed on (face, size, text, wrap width) and tagged with the font asset's generation, so a label
 * that does not change is shaped once and a reloaded face shapes again.
 * */
#pragma once

// SDL_textengine.h has TTF_TextData and the draw operations; included here since NYA_TextGlyph translates TTF_CopyOperation and the two are read side by side.
#include "SDL3_ttf/SDL_textengine.h"
#include "SDL3_ttf/SDL_ttf.h"

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Glyphs one run holds.
 * */
#ifndef NYA_TEXT_RUN_GLYPHS_MAX
#define NYA_TEXT_RUN_GLYPHS_MAX 1024
#endif

/** Lines one run holds. Wrapping and embedded newlines both produce these. */
#ifndef NYA_TEXT_RUN_LINES_MAX
#define NYA_TEXT_RUN_LINES_MAX 64
#endif

/** Directional runs one line is split into for bidi reordering. Bounded; a longer line keeps its first runs. */
#ifndef NYA_BIDI_RUNS_MAX
#define NYA_BIDI_RUNS_MAX 32
#endif

/** Longest derived font asset handle: a path, an '@', and a point size. */
#define NYA_TEXT_FONT_HANDLE_MAX 256

/** How far SDL_ttf extends a distance field past the ink on every side, in pixels. Its DEFAULT_SDF_SPREAD, unexported. */
#define NYA_TEXT_SDF_SPREAD 8

/**
 * Laid out strings kept by nya_text_shape_with_font, across every face. The main menu holds 5 and a scene HUD
 * about 15; strings that change every frame (a counter) cycle out least recently used.
 * */
#ifndef NYA_TEXT_RUN_CACHE_CAPACITY
#define NYA_TEXT_RUN_CACHE_CAPACITY 128
#endif

/**
 * Longest cache key: the wrap width, the face handle and the text. Every entry reserves this much. Longer
 * strings are shaped on every call.
 * */
#ifndef NYA_TEXT_RUN_CACHE_KEY_MAX
#define NYA_TEXT_RUN_CACHE_KEY_MAX 256
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_TextGlyph NYA_TextGlyph;
typedef struct NYA_TextLine  NYA_TextLine;
typedef struct NYA_TextRun   NYA_TextRun;
typedef struct NYA_BidiRun   NYA_BidiRun;

/**
 * One directional run of a single line: a byte range of the source and its bidi embedding level. An even level
 * reads left-to-right, an odd one right-to-left. The runs a line splits into come back in visual (left-to-right)
 * order, so laying them out is a plain left-to-right walk that offsets each by the width of those before it.
 * */
struct NYA_BidiRun {
    u32 offset;
    u32 length;
    u8  level;
};

/** One positioned glyph. Everything is in pixels, relative to the run's top-left. */
struct NYA_TextGlyph {
    /**
     * The face's own index for this glyph, which is what an atlas caches it under.
     * */
    u32 glyph_index;

    /** Where this glyph's image goes. Already carries the bearing and the kerning. */
    s32 x, y, width, height;

    /**
     * The sub-rectangle of the glyph's own image to take.
     * */
    s32 source_x, source_y;

    /** Which line of the run it is on. */
    u32 line;
};

/** One line of a run: the glyphs on it, its box, and the bytes of the source string it covers. */
struct NYA_TextLine {
    u32 first_glyph;
    u32 glyph_count;

    /** Relative to the run's top-left, like the glyphs. `width` is the line's advance, not its ink. */
    s32 x, y, width, height;

    /** Byte range within the string that was shaped. */
    u32 offset;
    u32 length;
};

/**
 * A shaped string.
 * */
struct NYA_TextRun {
    NYA_TextGlyph glyphs[NYA_TEXT_RUN_GLYPHS_MAX];
    u32           glyph_count;

    NYA_TextLine lines[NYA_TEXT_RUN_LINES_MAX];
    u32          line_count;

    /** The whole run's box, in pixels. */
    s32 width;
    s32 height;

    /** Set when the string needed more glyphs or lines than the run holds. What is there is valid. */
    b8 overflowed;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Shapes `text` with `font` into `out_run`. A line holding a right-to-left script is split into directional runs,
 * each shaped in its own direction and placed in visual order (see nya_text_bidi_runs); a purely left-to-right
 * line takes the shaper's own single-pass layout unchanged.
 * */
NYA_API b8 nya_text_shape(TTF_Font* font, NYA_ConstCString text, u64 length, s32 wrap_width, OUT NYA_TextRun* out_run);

/**
 * Splits one line of `text` into directional runs and orders them left-to-right in visual order, a reduced UAX #9.
 * `length` is bytes, or 0 for a NUL-terminated string. Returns the run count, at most `capacity`.
 *
 * Covered: the base direction from the first strong character (P2/P3), Hebrew and Arabic as right-to-left, digits
 * kept left-to-right, neutral runs resolved toward their neighbours (a reduced N1/N2), and the visual reordering
 * (L1/L2). Not covered: explicit embedding and override controls, isolates, mirrored brackets, and the full
 * weak-type resolution; folding Arabic letters into plain right-to-left is enough for those cases and no more.
 * */
NYA_API u32 nya_text_bidi_runs(NYA_ConstCString text, u64 length, OUT NYA_BidiRun* out_runs, u32 capacity);

/**
 * The size `text` would occupy, without keeping the glyphs.
 * */
NYA_API f32x2 nya_text_measure_font(TTF_Font* font, NYA_ConstCString text, s32 wrap_width) __attr_no_discard;

// Vertical metrics, wrapped so callers need not remember which SDL reports negative.

/** Baseline to baseline: what to advance y by for the next line. */
NYA_API f32 nya_text_line_height(TTF_Font* font) __attr_no_discard;

/** Top of the line box to the baseline. */
NYA_API f32 nya_text_ascent(TTF_Font* font) __attr_no_discard;

/** Baseline to the deepest descender, positive. SDL reports it negative. */
NYA_API f32 nya_text_descent(TTF_Font* font) __attr_no_discard;

// Faces through the asset system, here rather than render2d.c because the headless renderer needs faces and has no atlas.

/**
 * The asset handle for a face at a size: `"./assets/fonts/x.ttf@19"`.
 * */
NYA_API void nya_text_font_handle(NYA_ConstCString path, f32 point_size, OUT char* out_handle, u64 capacity);

/**
 * The `TTF_Font` for a face at a size, queueing the load on the first ask.
 * */
NYA_API TTF_Font* nya_text_font_for(NYA_ConstCString path, f32 point_size);

/**
 * nya_text_shape for a face at a size, laid out once and then read back from the cache. False while the face
 * is still loading. A change to the face itself (a distance field, a style) lays the text out again, since
 * SDL_ttf marks every text of a face it changes.
 * */
NYA_API b8 nya_text_shape_with_font(NYA_ConstCString path, f32 point_size, NYA_ConstCString text, s32 wrap_width, OUT NYA_TextRun* out_run);

/** nya_text_measure_font through the same cache. Zero while the face is still loading. */
NYA_API f32x2 nya_text_measure_with_font(NYA_ConstCString path, f32 point_size, NYA_ConstCString text, s32 wrap_width) __attr_no_discard;

/** Destroys every cached text. Before TTF_Quit, and before the asset system's arena goes. */
NYA_API void nya_text_run_cache_destroy(void);

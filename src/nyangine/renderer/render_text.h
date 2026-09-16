/**
 * @file render_text.h
 *
 * ```c
 * NYA_TextRun run;
 * if (nya_text_shape(font, "Wave, AVA.", 0, 0, &run)) {
 *     for (u32 i = 0; i < run.glyph_count; i++) {
 *         const NYA_TextGlyph* glyph = &run.glyphs[i];
 *         // glyph->glyph_index is what the atlas is keyed by; x and y are already kerned.
 *     }
 * }
 * ```
 *
 * ⚠ **A run is keyed by glyph index, not codepoint, and the two are not interchangeable.** Shaping
 * outputs indices into the face; one codepoint can become several glyphs (a mark cluster) and several
 * codepoints can become one (a ligature). An atlas consuming this has to be keyed the same way.
 *
 * ⚠ **One `TTF_CreateText` per call, and it allocates.** Fine for the handful of strings a frame
 * draws; not fine per character. Callers shape once per string and walk the run.
 * */
#pragma once

// SDL_textengine.h is where TTF_TextData and the draw operations live. Included here rather
// than in the .c because NYA_TextGlyph is a translation of TTF_CopyOperation and the two
// have to be read side by side.
#include "SDL3_ttf/SDL_textengine.h"
#include "SDL3_ttf/SDL_ttf.h"

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_vector.h"

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

/** Longest derived font asset handle: a path, an '@', and a point size. */
#define NYA_TEXT_FONT_HANDLE_MAX 256

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_TextGlyph NYA_TextGlyph;
typedef struct NYA_TextLine  NYA_TextLine;
typedef struct NYA_TextRun   NYA_TextRun;

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
 * Shapes `text` with `font` into `out_run`.
 * */
NYA_API b8 nya_text_shape(TTF_Font* font, NYA_ConstCString text, u64 length, s32 wrap_width, OUT NYA_TextRun* out_run);

/**
 * The size `text` would occupy, without keeping the glyphs.
 * */
NYA_API f32x2 nya_text_measure_font(TTF_Font* font, NYA_ConstCString text, s32 wrap_width) __attr_no_discard;

/*
 * ── Vertical metrics ──
 *
 * Thin wrappers, here rather than at call sites so that a caller holding a TTF_Font never has to
 * remember which of these SDL reports as a negative.
 */

/** Baseline to baseline: what to advance y by for the next line. */
NYA_API f32 nya_text_line_height(TTF_Font* font) __attr_no_discard;

/** Top of the line box to the baseline. */
NYA_API f32 nya_text_ascent(TTF_Font* font) __attr_no_discard;

/** Baseline to the deepest descender, **positive** — SDL reports it negative, and this flips it. */
NYA_API f32 nya_text_descent(TTF_Font* font) __attr_no_discard;

/*
 * ── Faces through the asset system ──
 *
 * Here rather than in render2d.c because both renderers need a TTF_Font from a path and a size, and
 * the headless one has no atlas to hang the lookup off.
 */

/**
 * The asset handle for a face at a size: `"./assets/fonts/x.ttf@19"`.
 * */
NYA_API void nya_text_font_handle(NYA_ConstCString path, f32 point_size, OUT char* out_handle, u64 capacity);

/**
 * The `TTF_Font` for a face at a size, queueing the load on the first ask.
 * */
NYA_API TTF_Font* nya_text_font_for(NYA_ConstCString path, f32 point_size);

/**
 * @file render_text.h
 *
 * Shaping and layout: a string plus a face becomes positioned glyph *indices*.
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
 * **Why this exists at all.** The text path used to walk codepoints, look each one up by codepoint,
 * and add a kerning correction per adjacent pair — which is not shaping, it is an approximation of
 * the one part of shaping that Latin needs. Getting even that part right took two bug fixes, because
 * the correction had to be reconstructed by measuring pairs of glyphs against their own metrics.
 * HarfBuzz was compiled and linked the whole time, through SDL3_ttf, and the text path simply never
 * reached it.
 *
 * It does now. `TTF_CreateText` runs the string through `hb_shape` and hands back a laid-out list of
 * glyph indices with pixel positions — kerning, ligatures, marks and reordering included, for every
 * script rather than for Latin. The kerning memo, the pair measurement that reconstructed GPOS, and
 * the hash whose wrapping multiply once aborted the process are all gone with it: none of them has
 * anything left to do.
 *
 * **The engine is null on purpose.** `TTF_CreateText` accepts a null `TTF_TextEngine` and still runs
 * the full layout — an engine is only what *draws* the result. So shaping needs no GPU device, no
 * renderer and no window, which is what lets the headless build measure text exactly the way the real
 * one does instead of answering zero. See render2d_headless.c.
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
 *
 * A run is one string handed to one draw call, so this is a per-call ceiling rather than a global
 * one. A thousand glyphs is several paragraphs; text past it is dropped with `overflowed` set rather
 * than growing, for the same reason the draw batch has a ceiling.
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
     *
     * Not a codepoint, and not derivable from one — see the warning in this file's header. Pass it to
     * `TTF_GetGlyphImageForIndex` to rasterise it.
     * */
    u32 glyph_index;

    /** Where this glyph's image goes. Already carries the bearing and the kerning. */
    s32 x, y, width, height;

    /**
     * The sub-rectangle of the glyph's own image to take.
     *
     * Normally the whole image, but the shaper is allowed to draw part of one — so it is carried
     * rather than assumed, and an atlas offsets its cell's uv by this.
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
 *
 * Big enough that it belongs in a static or an arena rather than on the stack of a draw call — about
 * thirty kilobytes. Callers keep one and reuse it.
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
 *
 * `length` of zero means the whole null-terminated string. `wrap_width` of zero means no wrapping —
 * lines then come only from newlines in the text. A positive `wrap_width` breaks the text at word
 * boundaries to fit that many pixels, which is the shaper's own line breaking rather than ours.
 *
 * False for a null font or a shaping failure, with `out_run` zeroed. An empty string succeeds with no
 * glyphs and one empty line, since an empty string still occupies a line.
 * */
NYA_API b8 nya_text_shape(TTF_Font* font, NYA_ConstCString text, u64 length, s32 wrap_width, OUT NYA_TextRun* out_run);

/**
 * The size `text` would occupy, without keeping the glyphs.
 *
 * The same layout `nya_text_shape` performs, so a measured width is the width that gets drawn. An
 * empty string measures as zero wide and one line tall.
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
 *
 * A face carries no size, so one file at two point sizes is two assets. Derived here rather than
 * invented by callers, which is what removes the need to register a handle like "neat_font" just to
 * have the same face at a second size.
 * */
NYA_API void nya_text_font_handle(NYA_ConstCString path, f32 point_size, OUT char* out_handle, u64 capacity);

/**
 * The `TTF_Font` for a face at a size, queueing the load on the first ask.
 *
 * Null until the asset system has finished loading it, which is normal for the first frames — every
 * caller copes by drawing or measuring nothing. Resolved through the asset system on **every** call
 * rather than cached as a bare pointer, because a hot reload replaces the `TTF_Font` and a stored
 * pointer would outlive it; `nya_asset_get` rate-limits its stat, so this is a dictionary hit.
 * */
NYA_API TTF_Font* nya_text_font_for(NYA_ConstCString path, f32 point_size);

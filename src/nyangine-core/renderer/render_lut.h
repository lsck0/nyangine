/**
 * @file render_lut.h
 *
 * ```c
 * NYA_Lut lut = { 0 };
 * NYA_TRY(nya_lut_parse(arena, file->items, file->length, &lut));
 * // lut.texels is lut.size cubed RGBA8 texels, ready for a 3D texture.
 *
 * // the same table at 40% for a deuteranope: both baked in, so the pass samples it at full strength.
 * lut = nya_lut_compose(arena, lut, 0.6F, NYA_COLOR_VISION_DEUTERANOPIA);
 * ```
 *
 * Colour vision is corrected in the table rather than in a pass of its own: the grade pass already looks every pixel
 * up, and a 3x3 correction composed after the grade costs that lookup nothing more.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/core/core_settings.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Most entries per side a lookup table may have. 64 is a megabyte of texels; a grade is smooth far below that. */
#define NYA_LUT_SIZE_MAX 64

/**
 * Fewest entries per side nya_lut_compose writes. The correction bends through the sRGB curve, which trilinear
 * filtering follows only as finely as the grid: measured over 20000 random colours, the mean error against the exact
 * correction is 17 steps of 255 at two entries, 0.43 at 17 and 0.24 at 33. 33 also holds every entry of a 17 table.
 * */
#define NYA_LUT_COMPOSE_SIZE_MIN 33

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Lut NYA_Lut;

/** A 3D colour lookup table: what a colour becomes, sampled at `size` steps per channel. */
struct NYA_Lut {
    u32 size;

    /** `size` cubed RGBA8 texels, red varying fastest and blue slowest, which is a 3D texture's order. */
    u8* texels;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Parses an Adobe `.cube` file, the format colour grading tools export. Only 3D tables over the unit domain are
 * accepted; anything else, a count that disagrees with LUT_3D_SIZE, or a stray token is NYA_ERROR_PARSE. Values
 * are clamped into [0, 1] and quantised to eight bits. `texels` is allocated from `arena`.
 * */
NYA_API NYA_Error nya_lut_parse(NYA_Arena* arena, const u8* text, u64 length, OUT NYA_Lut* out_lut) __attr_no_discard;

/**
 * Bakes a fade and a colour vision correction into a copy of `lut`. The table is pulled `fade` of the way back toward
 * changing nothing, then daltonized for `vision`: what that viewer cannot tell apart is shifted into channels they
 * can. At least NYA_LUT_COMPOSE_SIZE_MIN entries per side, allocated from `arena`. A `fade` of zero with no vision
 * returns `lut` itself.
 * */
NYA_API NYA_Lut nya_lut_compose(NYA_Arena* arena, NYA_Lut lut, f32 fade, NYA_ColorVision vision) __attr_no_discard;

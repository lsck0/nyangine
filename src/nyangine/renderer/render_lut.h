/**
 * @file render_lut.h
 *
 * ```c
 * NYA_Lut lut = { 0 };
 * NYA_TRY(nya_lut_parse(arena, file->items, file->length, &lut));
 * // lut.texels is lut.size cubed RGBA8 texels, ready for a 3D texture.
 * ```
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Most entries per side a lookup table may have. 64 is a megabyte of texels; a grade is smooth far below that. */
#define NYA_LUT_SIZE_MAX 64

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

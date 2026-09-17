/**
 * @file render_sort.h
 * */
#pragma once

#include "nyangine/base/base_types.h"

// declared, not included: renderer.h includes this file after defining the key and the ranges.
typedef struct NYA_Render3DSortKey   NYA_Render3DSortKey;
typedef struct NYA_Render2DDrawRange NYA_Render2DDrawRange;
typedef struct NYA_Render2DDraw      NYA_Render2DDraw;

/**
 * Sorts `count` keys ascending by depth, using `scratch` of the same size.
 * */
NYA_API void nya_render3d_sort_keys(NYA_Render3DSortKey* keys, NYA_Render3DSortKey* scratch, u32 count);

/**
 * Sorts `ranges` into paint order, layer then declaration, and groups them into draw calls in `out_draws`, which
 * holds `count`. A range joins the latest of the last NYA_RENDER2D_MERGE_LOOKBACK draws with the same state when no
 * draw after that one overlaps it. Returns the number of draws.
 * */
NYA_API u32 nya_render2d_ranges_merge(NYA_Render2DDrawRange* ranges, u32 count, NYA_Render2DDraw* out_draws) __attr_no_discard;

/**
 * Writes each draw's indices from `indices` into `out_indices` back to back, in draw and then paint order, and sets
 * the draws' first_index to where theirs landed.
 * */
NYA_API void nya_render2d_draws_indices_write(const NYA_Render2DDrawRange* ranges, NYA_Render2DDraw* draws, u32 draw_count, const u32* indices, u32* out_indices);

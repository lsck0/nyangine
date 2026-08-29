/**
 * @file render_sort.h
 *
 * The depth sort behind transparent drawing, kept apart from the renderer that calls it.
 *
 * Its own translation unit for two reasons: it touches no GPU state, so it compiles into a headless
 * build and can be tested there; and replacing a sort is exactly the change that silently reorders one
 * triangle in one scene, which is worth having a test able to reach.
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/renderer/renderer.h"

/**
 * Sorts `count` keys ascending by depth, using `scratch` of the same size.
 *
 * Ascending, because the caller walks the result backwards to draw furthest first — reversing a loop is
 * free and sorting descending is not.
 * */
NYA_API void nya_render3d_sort_keys(NYA_Render3DSortKey* keys, NYA_Render3DSortKey* scratch, u32 count);

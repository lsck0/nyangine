/**
 * @file render_sort.h
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/renderer/renderer.h"

/**
 * Sorts `count` keys ascending by depth, using `scratch` of the same size.
 * */
NYA_API void nya_render3d_sort_keys(NYA_Render3DSortKey* keys, NYA_Render3DSortKey* scratch, u32 count);

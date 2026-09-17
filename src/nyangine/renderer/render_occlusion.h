/**
 * @file render_occlusion.h
 *
 * ```c
 * // tens of kilobytes; keep it somewhere that outlives the frame, not on the stack.
 * static NYA_OcclusionBuffer occlusion;
 *
 * nya_render3d_begin(window, camera);
 *
 * nya_occlusion_begin(&occlusion, nya_render3d_view_projection(window));
 * nya_occlusion_box(&occlusion, wall_center, wall_half_extents, camera.position);
 * nya_render3d_occlusion(window, &occlusion);
 *
 * // call sites do not change; nya_render3d_mesh tests before queueing.
 * nya_render3d_mesh(window, MESH_CRATE, position, scale, rotation, tint);
 * ```
 *
 * Runs after frustum culling, not instead of it. `_nya_render3d_visible` (render_cull.c) does the cheap
 * frustum test first and only tests occlusion for what survived.
 *
 * Occluders must be solid and opaque. A fence or window submitted as an occluder hides what is behind
 * it.
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/math/math_matrix.h"
#include "nyangine/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The depth buffer's resolution.
 * */
#ifndef NYA_OCCLUSION_WIDTH
#define NYA_OCCLUSION_WIDTH 160
#endif

#ifndef NYA_OCCLUSION_HEIGHT
#define NYA_OCCLUSION_HEIGHT 90
#endif

/**
 * Pixels a single query may scan before it gives up and answers "visible".
 * */
#ifndef NYA_OCCLUSION_MAX_QUERY_PIXELS
#define NYA_OCCLUSION_MAX_QUERY_PIXELS ((NYA_OCCLUSION_WIDTH * NYA_OCCLUSION_HEIGHT) / 10)
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_OcclusionBuffer NYA_OcclusionBuffer;
typedef struct NYA_OcclusionStats  NYA_OcclusionStats;

/** What the buffer did since the last nya_occlusion_begin. Worth watching before trusting the feature. */
struct NYA_OcclusionStats {
    /** Occluders submitted, and how many of those actually wrote a pixel. */
    u32 occluders;
    u32 occluders_rasterized;

    u32 tests;

    /** Tests that came back hidden. The number the whole thing exists to make large. */
    u32 occluded;

    /** Tests that gave up on NYA_OCCLUSION_MAX_QUERY_PIXELS rather than answering. */
    u32 abandoned;
};

/**
 * A depth buffer holding, per pixel, the depth beyond which everything is hidden.
 * */
struct NYA_OcclusionBuffer {
    /**
     * Normalised device depth in [0, 1], nearer is smaller, 1 where nothing has been written.
     * */
    f32 depth[NYA_OCCLUSION_WIDTH * NYA_OCCLUSION_HEIGHT];

    f32_4x4 view_projection;

    /** False until nya_occlusion_begin, so a buffer that was never set up culls nothing. */
    b8 ready;

    NYA_OcclusionStats stats;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Clears the buffer and points it at a camera. Call once per camera per frame, before any occluder.
 * */
NYA_API void nya_occlusion_begin(NYA_OcclusionBuffer* buffer, f32_4x4 view_projection);

/**
 * Submits one solid convex quad as an occluder, wound in either direction.
 * */
NYA_API b8 nya_occlusion_quad(NYA_OcclusionBuffer* buffer, f32x3 a, f32x3 b, f32x3 c, f32x3 d);

/**
 * Submits an axis-aligned box as an occluder, from wherever `eye` is looking at it.
 * */
NYA_API u32 nya_occlusion_box(NYA_OcclusionBuffer* buffer, f32x3 center, f32x3 half_extents, f32x3 eye);

/**
 * Whether a bounding sphere is entirely hidden. True means it can be skipped.
 * */
NYA_API b8 nya_occlusion_test(const NYA_OcclusionBuffer* buffer, f32x3 center, f32 radius) __attr_no_discard;

/** What the buffer has done since nya_occlusion_begin. */
NYA_API NYA_OcclusionStats nya_occlusion_stats(const NYA_OcclusionBuffer* buffer) __attr_no_discard;

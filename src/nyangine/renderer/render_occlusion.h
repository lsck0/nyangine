/**
 * @file render_occlusion.h
 *
 * Occlusion culling: not drawing what is behind something solid, decided on the CPU before anything
 * is queued.
 *
 * ```c
 * // Once, somewhere that outlives the frame. It is tens of kilobytes; do not put it on the stack.
 * static NYA_OcclusionBuffer occlusion;
 *
 * nya_render3d_begin(window, camera);
 *
 * nya_occlusion_begin(&occlusion, nya_render3d_view_projection(window));
 * nya_occlusion_box(&occlusion, wall_center, wall_half_extents, camera.position);
 * nya_render3d_occlusion(window, &occlusion);
 *
 * // Nothing at the call site changes. nya_render3d_mesh tests before it queues anything.
 * nya_render3d_mesh(window, MESH_CRATE, position, scale, rotation, tint);
 * ```
 *
 * **Software, not GPU queries.** A hardware occlusion query needs three things this renderer does not
 * have: an identity for each object that survives between frames, a depth pre-pass to query against,
 * and a readback that arrives one or two frames late and has to be reconciled with a camera that has
 * moved since. Draws here are immediate-mode calls with no identity at all — `nya_render3d_mesh` is
 * a handle and a matrix — so a query would have to invent all three. A small depth buffer rasterised
 * on the CPU answers the same question this frame, for this camera, with no readback and no latency.
 *
 * **It hides only what you tell it about.** The engine does not guess which geometry occludes; the
 * game submits a handful of large solid shapes — a wall, a cliff, a building — and that is the whole
 * of it. Guessing is what makes occlusion culling expensive: rasterising the scene to find out what
 * hides the scene costs more than drawing it. Ten occluders in a 160×90 buffer is microseconds.
 *
 * **Conservative in one direction, on purpose.** Every approximation here errs toward *visible*: a
 * pixel only partly covered by an occluder is not written, an occluder crossing the near plane is
 * dropped entirely, and a query whose screen rectangle leaves the buffer is answered "visible". The
 * cost of erring that way is a draw call that did nothing. The cost of erring the other way is
 * geometry vanishing, which is the kind of bug that is reported as "the wall flickers" six months
 * later.
 *
 * ⚠ **This does not replace frustum culling and runs after it.** Frustum culling asks "is it on
 * screen", this asks "is something in front of it". `_nya_render3d_visible` does the cheap one first
 * and only reaches this for primitives that survived.
 *
 * ⚠ **An occluder must be solid and opaque.** Submitting a fence, a window, or anything the player
 * can see through will hide what is behind it. The buffer has no way to know.
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
 *
 * Small because it is scanned per query, not per pixel of the screen, and because a conservative
 * rasteriser only writes pixels an occluder *fully* covers — going finer buys sharper silhouettes
 * that then cost more to test against. 160×90 is 57 KB and keeps the whole thing in L2.
 * */
#ifndef NYA_OCCLUSION_WIDTH
#define NYA_OCCLUSION_WIDTH 160
#endif

#ifndef NYA_OCCLUSION_HEIGHT
#define NYA_OCCLUSION_HEIGHT 90
#endif

/**
 * Pixels a single query may scan before it gives up and answers "visible".
 *
 * A bound on the worst case rather than a tuning knob. Something filling the screen has a rectangle
 * of fourteen thousand pixels and is not going to be hidden by anything, so scanning it is time
 * spent proving what was already obvious. A tenth of the buffer is generous for anything that might
 * plausibly be behind a wall.
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
 *
 * Larger than a cache line by some margin: keep one per camera somewhere that outlives the frame,
 * not on the stack.
 * */
struct NYA_OcclusionBuffer {
    /**
     * Normalised device depth in [0, 1], nearer is smaller, 1 where nothing has been written.
     *
     * The value is the *nearest* depth any occluder claimed for that pixel, because the nearest
     * claim is the strongest: an occluder at 0.3 hides everything past 0.3, and a second at 0.6 says
     * nothing new. That is why writes take the minimum.
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
 *
 * The matrix has to be the same one the draws will be culled against, or the buffer describes a view
 * nobody is looking through and hides the wrong things.
 * */
NYA_API void nya_occlusion_begin(NYA_OcclusionBuffer* buffer, f32_4x4 view_projection);

/**
 * Submits one solid convex quad as an occluder, wound in either direction.
 *
 * Returns false when it wrote nothing — off screen, degenerate, crossing the near plane, or too small
 * to fully cover a single pixel. Not an error: most occluders in a scene are behind the camera.
 * */
NYA_API b8 nya_occlusion_quad(NYA_OcclusionBuffer* buffer, f32x3 a, f32x3 b, f32x3 c, f32x3 d);

/**
 * Submits an axis-aligned box as an occluder, from wherever `eye` is looking at it.
 *
 * The three faces pointing toward the eye are what get rasterised. Their depth is correct to use for
 * a *solid* box: anything behind the front surface within its silhouette is either inside the box or
 * behind it, and both are hidden. Using the far faces instead would be equally correct and hide
 * strictly less.
 *
 * Returns how many faces actually wrote anything.
 * */
NYA_API u32 nya_occlusion_box(NYA_OcclusionBuffer* buffer, f32x3 center, f32x3 half_extents, f32x3 eye);

/**
 * Whether a bounding sphere is entirely hidden. True means it can be skipped.
 *
 * The sphere is tested through the axis-aligned box that contains it, which is conservative twice
 * over — the box is bigger than the sphere, and the box's nearest corner is nearer than the sphere.
 * Both errors point at "visible".
 * */
NYA_API b8 nya_occlusion_test(const NYA_OcclusionBuffer* buffer, f32x3 center, f32 radius) __attr_no_discard;

/** What the buffer has done since nya_occlusion_begin. */
NYA_API NYA_OcclusionStats nya_occlusion_stats(const NYA_OcclusionBuffer* buffer) __attr_no_discard;

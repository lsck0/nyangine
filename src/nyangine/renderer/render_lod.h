/**
 * @file render_lod.h
 *
 * Level of detail: swapping a registered mesh for a cheaper one with distance, and dropping it entirely
 * past a range.
 *
 * ```c
 * nya_render3d_lod_register(MESH_TREE, (NYA_Render3DLodLevel[]){
 *     { .handle = MESH_TREE,        .max_distance = 30.0F },   // full detail up to 30 units
 *     { .handle = MESH_TREE_MID,    .max_distance = 90.0F },
 *     { .handle = MESH_TREE_FAR,    .max_distance = 250.0F },  // past this it is not drawn at all
 * }, 3);
 *
 * // Nothing at the call site changes. nya_render3d_mesh resolves the handle itself.
 * nya_render3d_mesh(window, MESH_TREE, position, scale, rotation, tint);
 * ```
 *
 * **Handle resolution, not a new draw path.** A chain maps one base handle to a list of registered
 * meshes, and `nya_render3d_mesh` swaps the handle before it looks up the instance group. A mesh with
 * no chain registered behaves exactly as it did — this is inert until something opts in.
 *
 * **Distances are compared squared**, so selecting a level costs a subtract, a dot and a few compares
 * and never a square root. The API takes and reports ordinary distances; the squaring is internal.
 *
 * ⚠ **This is not a substitute for frustum culling and does not replace it.** Frustum culling asks "is
 * it on screen"; this asks "is it worth drawing at that size". A scene wants both, and the engine's
 * bounding-sphere frustum test still runs first because it is cheaper and rejects more.
 *
 * ⚠ **It does not build the lower-detail meshes.** Those are the asset pipeline's job or the caller's;
 * this only chooses between meshes that already exist.
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Levels one chain may have. Four is more than a low-poly style has distinct silhouettes for. */
#ifndef NYA_RENDER3D_LOD_LEVELS
#define NYA_RENDER3D_LOD_LEVELS 4
#endif

/** How many distinct meshes may have a chain at once. */
#ifndef NYA_RENDER3D_LOD_CHAINS
#define NYA_RENDER3D_LOD_CHAINS 64
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Render3DLodLevel NYA_Render3DLodLevel;

/** One rung: which mesh to draw, and how far out it remains the right one. */
struct NYA_Render3DLodLevel {
    /** A handle already registered with nya_render3d_mesh_register, or a loaded mesh asset. */
    NYA_ConstCString handle;

    /**
     * The distance past which this level is no longer used, in world units.
     *
     * Levels are given nearest first and each one's distance must exceed the last. Past the final
     * level's distance the mesh is not drawn at all, which is what makes the last entry double as the
     * draw distance.
     * */
    f32 max_distance;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Registers a chain for `base_handle`. Replaces any existing chain for it.
 *
 * Returns false when the levels are not in increasing distance order, when there are none or too many,
 * or when the table is full — rather than silently accepting a chain that would select the wrong rung.
 * */
NYA_API b8 nya_render3d_lod_register(NYA_ConstCString base_handle, const NYA_Render3DLodLevel* levels, u32 level_count);

/** Forgets `base_handle`'s chain, so it draws at full detail again. */
NYA_API void nya_render3d_lod_unregister(NYA_ConstCString base_handle);

/** Forgets every chain. */
NYA_API void nya_render3d_lod_clear(void);

/** How many chains are registered. */
NYA_API u32 nya_render3d_lod_count(void) __attr_no_discard;

/** Whether `base_handle` has a chain. */
NYA_API b8 nya_render3d_lod_registered(NYA_ConstCString base_handle) __attr_no_discard;

/**
 * The handle to draw for something `distance` away, or null when it is past the last level.
 *
 * Returns `base_handle` unchanged when it has no chain, so a caller can route every draw through this
 * without asking first.
 * */
NYA_API NYA_ConstCString nya_render3d_lod_select(NYA_ConstCString base_handle, f32 distance) __attr_no_discard;

/** The same, given a squared distance — what a caller that already has one should use. */
NYA_API NYA_ConstCString nya_render3d_lod_select_squared(NYA_ConstCString base_handle, f32 distance_squared) __attr_no_discard;

/** Which rung `distance` selects, or NYA_RENDER3D_LOD_LEVELS when it is past the last. For debugging. */
NYA_API u32 nya_render3d_lod_level_at(NYA_ConstCString base_handle, f32 distance) __attr_no_discard;

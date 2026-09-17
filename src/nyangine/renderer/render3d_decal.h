/**
 * @file render3d_decal.h
 *
 * ```c
 * // once: switch decals on and say what they land on.
 * nya_render3d_decals_set(window, (NYA_Render3DDecals){ .enabled = true });
 * nya_render3d_decal_probe_set(window, nya_callback(ground_probe), nullptr);
 *
 * // in the scene pass, after the ground, like any other draw.
 * nya_render3d_decal(window, (NYA_Render3DDecal){
 *     .texture = SPLATS_PNG, .columns = 2, .rows = 2, .cell = 1,
 *     .center  = hit_point, .size = { 1.2F, 1.0F, 1.2F }, .color = NYA_COLOR_RED,
 * });
 * ```
 *
 * A decal is a box projected straight down onto whatever the probe reports, draped as a small grid so it
 * follows the ground's shape, and shaded like the surface under it. Its alpha is cut at one half, so a splat
 * has an inked edge rather than a soft one. Every decal of a frame goes out in one draw call per texture.
 *
 * Draped grids are remembered by their box, so a mark that stays put is probed once, not every frame. A probe
 * that starts answering differently, such as regenerated ground, is set again to forget them.
 * */
#pragma once

#include "SDL3/SDL_gpu.h"

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_callback.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/renderer/render_color.h"

typedef struct NYA_Window   NYA_Window;
typedef struct NYA_Arena    NYA_Arena;
typedef struct NYA_Cache    NYA_Cache;
typedef struct NYA_Vertex3D NYA_Vertex3D;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The handle the decal pipeline is registered under, queued when decals are first switched on. */
#define NYA_RENDER3D_PIPELINE_DECAL "nya_mesh3d_decal_pipeline"

/** Decals one frame can draw. Past it a decal is dropped and counted. */
#ifndef NYA_RENDER3D_DECAL_MAX
#define NYA_RENDER3D_DECAL_MAX 128
#endif

/** Cells per side of the grid a decal is draped as. More follows bumpier ground, at a probe per extra vertex. */
#ifndef NYA_RENDER3D_DECAL_GRID
#define NYA_RENDER3D_DECAL_GRID 4
#endif

/** How far a decal floats off the surface when NYA_Render3DDecals.lift is zero, in world units. */
#ifndef NYA_RENDER3D_DECAL_LIFT
#define NYA_RENDER3D_DECAL_LIFT 0.03F
#endif

/** Surfaces steeper than this cosine from level take no decal, so a splat does not smear down a cliff. */
#ifndef NYA_RENDER3D_DECAL_STEEPEST
#define NYA_RENDER3D_DECAL_STEEPEST 0.35F
#endif

#define NYA_RENDER3D_DECAL_VERTICES ((u32)((NYA_RENDER3D_DECAL_GRID + 1) * (NYA_RENDER3D_DECAL_GRID + 1)))
#define NYA_RENDER3D_DECAL_INDICES  ((u32)(NYA_RENDER3D_DECAL_GRID * NYA_RENDER3D_DECAL_GRID * 6))

static_assert(NYA_RENDER3D_DECAL_MAX * NYA_RENDER3D_DECAL_VERTICES <= 65536, "decal indices are sixteen bits");

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Render3DDecal     NYA_Render3DDecal;
typedef struct NYA_Render3DDecals    NYA_Render3DDecals;
typedef struct NYA_Render3DDecalsGPU NYA_Render3DDecalsGPU;

/**
 * Where a decal lands: the first surface along `direction` from `origin`, within its length. False for none.
 * `out_normal` is unit length and faces back toward the origin.
 * */
typedef b8 (*NYA_Render3DDecalProbe)(f32x3 origin, f32x3 direction, void* user_data, OUT f32x3* out_point, OUT f32x3* out_normal);

/** One decal for one frame. */
struct NYA_Render3DDecal {
    /** A texture asset. The decal waits for it to load. */
    NYA_ConstCString texture;

    /** The middle of the box. */
    f32x3 center;

    /** The box's full size: x and z across the ground, y the depth it is projected through. */
    f32x3 size;

    /** A turn about the vertical, in radians. */
    f32 rotation;

    /** Multiplied into the texture. The alpha scales opacity after the edge is cut. */
    NYA_Color color;

    /** The texture as a grid of `columns` by `rows` pictures, and which one, row by row. Zero columns or rows is one. */
    u8 columns;
    u8 rows;
    u8 cell;
};

/** Zero for any field but `enabled` picks its default. */
// @reflect
struct NYA_Render3DDecals {
    /** Off, a decal costs one comparison and nothing is allocated. */
    b8 enabled;

    /** How far a decal floats off the surface, against z-fighting. Zero becomes NYA_RENDER3D_DECAL_LIFT. */
    f32 lift;
};

/**
 * What decals hold while enabled. Allocated when the first decal of a session is drawn and released when
 * decals are switched off.
 * */
struct NYA_Render3DDecalsGPU {
    SDL_GPUBuffer*         vertex_buffer;
    SDL_GPUTransferBuffer* transfer_buffer;

    /** The same grid for every decal, offset per decal. Uploaded once. */
    SDL_GPUBuffer* index_buffer;
    b8             indices_uploaded;

    /** Owns the staging vertices and the draped grids. */
    NYA_Arena* arena;

    /** Draped grids by box. See the file comment. */
    NYA_Cache* grids;

    NYA_Vertex3D* vertices;

    /** Decals staged since the last flush, and the texture asset they share. */
    u32              count;
    NYA_ConstCString texture;

    /** Decals drawn this frame, for the ceiling. */
    u32 frame_count;

    NYA_CallbackHandle probe;
    void*              probe_user_data;

    /** Bumped by every nya_render3d_decal_probe_set, so grids draped by the last probe are stale. */
    u64 probe_generation;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Replaces the window's decal options. Switching decals off releases everything they hold. */
NYA_API void nya_render3d_decals_set(NYA_Window* window, NYA_Render3DDecals decals);

/** The decal options as given, before defaults. */
NYA_API NYA_Render3DDecals nya_render3d_decals(NYA_Window* window) __attr_no_discard;

/** Sets what decals land on, a NYA_Render3DDecalProbe, and forgets every grid the previous one draped. */
NYA_API void nya_render3d_decal_probe_set(NYA_Window* window, NYA_CallbackHandle probe, void* user_data);

/**
 * Drapes `decal` and stages it for the scene pass's next flush. Ignored inside a shadow or depth pass, and
 * while decals are off or no probe is set. Draw it after what it lands on.
 * */
NYA_API void nya_render3d_decal(NYA_Window* window, NYA_Render3DDecal decal);

/**
 * @file core_terrain3d.h
 *
 * ```c
 * NYA_Terrain3D* terrain = nullptr;
 * NYA_EXPECT(nya_terrain3d_create(arena, (NYA_Terrain3DOptions){ .entity_type = MY_ENTITY_TERRAIN }, &terrain));
 *
 * nya_terrain3d_generate(terrain, window, seed);
 *
 * // Each frame, between nya_render3d_begin and _end.
 * nya_terrain3d_draw(terrain, window);
 * ```
 *
 * ```c
 * nya_terrain3d_create(arena, (NYA_Terrain3DOptions){ .resolution = 128, .extent = 256.0F, .chunked = true, ... }, &terrain);
 * nya_terrain3d_generate(terrain, window, seed);
 *
 * nya_terrain3d_update(terrain, window, camera.position);   // in on_update
 * nya_terrain3d_draw(terrain, window);                      // in on_render
 * ```
 *
 * ⚠ **The collider is not chunked and does not change with LOD.** A body falling through a surface
 * because the camera drove away is not a trade anyone wants; the physics mesh stays at full resolution
 * and is built once.
 *
 * ⚠ **Two chunks at different levels do not share their border vertices**, so the seam between them
 * would show as a crack of background. Skirts close it: each chunk's edge drops a vertical flange into
 * the ground, hidden by the neighbouring surface whichever level it is at. Cheaper and far more robust
 * than stitching the border triangles, which needs each chunk to know its neighbours' levels and
 * re-triangulate when any of them changes.
 * */
#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_types.h"
#include "nyangine/renderer/render_color.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The handle the unchunked surface's geometry is registered under. See nya_render3d_mesh_register. */
#define NYA_TERRAIN3D_MESH "nya_terrain3d_mesh"

/**
 * Cells across one chunk.
 * */
#ifndef NYA_TERRAIN3D_CHUNK_CELLS
#define NYA_TERRAIN3D_CHUNK_CELLS 16
#endif

static_assert((NYA_TERRAIN3D_CHUNK_CELLS & (NYA_TERRAIN3D_CHUNK_CELLS - 1)) == 0, "NYA_TERRAIN3D_CHUNK_CELLS must be a power of two");

/**
 * Detail levels a chunk may be drawn at. Level `n` samples every `1 << n`-th vertex.
 * */
#ifndef NYA_TERRAIN3D_LOD_LEVELS
#define NYA_TERRAIN3D_LOD_LEVELS 4
#endif

static_assert((1 << (NYA_TERRAIN3D_LOD_LEVELS - 1)) <= NYA_TERRAIN3D_CHUNK_CELLS,
              "the coarsest LOD stride has to divide a chunk; lower NYA_TERRAIN3D_LOD_LEVELS or raise NYA_TERRAIN3D_CHUNK_CELLS");

/**
 * How far past a level boundary a chunk has to be before it changes level, as a fraction.
 *
 * ⚠ **Without this a chunk sitting on a boundary rebuilds every few frames**, and a rebuild uploads
 * geometry — seen in the demo as one chunk flipping between 72 and 192 vertices while the camera
 * orbited past it. A tenth is enough that ordinary movement never lands in the dead band for long, and
 * small enough that the level is never more than slightly stale.
 * */
#ifndef NYA_TERRAIN3D_LOD_HYSTERESIS
#define NYA_TERRAIN3D_LOD_HYSTERESIS 0.10F
#endif

/** Longest chunk mesh handle: a prefix and an index. */
#define NYA_TERRAIN3D_CHUNK_HANDLE_MAX 48

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Terrain3DOptions NYA_Terrain3DOptions;
typedef struct NYA_Terrain3DChunk   NYA_Terrain3DChunk;
typedef struct NYA_Terrain3D        NYA_Terrain3D;

/** One square of a chunked surface: where it is, how detailed it currently is, and its own mesh. */
struct NYA_Terrain3DChunk {
    /** Cell coordinates of this chunk's low corner, in the terrain's grid. */
    u32 cell_x, cell_z;

    /** Centre and radius of its bounding sphere, world units. What the renderer culls against. */
    f32x3 center;
    f32   radius;

    /**
     * The level its geometry is currently built at, or NYA_TERRAIN3D_LOD_LEVELS for "not built yet".
     * */
    u32 lod;

    /** Owned here because a registered mesh keeps the pointer it was handed. */
    char handle[NYA_TERRAIN3D_CHUNK_HANDLE_MAX];
};

/**
 * The shape of the surface. Every field has a usable default at zero, so `(NYA_Terrain3DOptions){ 0 }`
 * is a terrain — except `entity_type`, which only the caller knows.
 * */
struct NYA_Terrain3DOptions {
    /** Cells per side. The sample grid is one larger in each direction. Default 32. */
    u32 resolution;

    /** World units across, centred on the origin. Default 16. */
    f32 extent;

    /** Peak height of the noise, before the rim is folded in. Default 2.5. */
    f32 amplitude;

    /** Where the raised rim begins, as a fraction of the half-extent. Default 0.55. */
    f32 rim_start;

    /** How high the rim stands, in units of `amplitude`. Above one so it clears the terrain. Default 1.15. */
    f32 rim_height;

    /** fBm parameters. Defaults 0.09, 4 octaves, lacunarity 2, gain 0.5. */
    f32 frequency;
    u32 octaves;
    f32 lacunarity;
    f32 gain;

    /** Friction of the static body. Default 0.85. */
    f32 friction;

    /** The four colour bands, low to peak, and where each begins as a fraction of the height range. */
    NYA_Color color_low, color_mid, color_high, color_peak;
    f32       band_mid, band_high, band_peak;

    /** Per-triangle brightness jitter, so a band does not read as a flat sheet. Default 0.06. */
    f32 shade_jitter;

    /**
     * Cut the surface into chunks and pick a detail level per chunk. See the note at the top.
     * */
    b8 chunked;

    /**
     * World distance at which a chunk drops to the next detail level, doubling per level.
     * */
    f32 lod_distance;

    /**
     * How far a chunk's skirt hangs below its edge, world units. Zero is read from the cell size.
     * */
    f32 skirt_depth;

    /**
     * What the static body is spawned as, in the caller's own entity-type enum.
     * */
    u32 entity_type;
};

struct NYA_Terrain3D {
    /** Everything here came from this. Freeing it frees the terrain. */
    NYA_Arena* allocator;

    NYA_Terrain3DOptions options;

    /** Derived once: cells per side, samples per side, and world units per cell. */
    u32 resolution;
    u32 verts;
    f32 cell;

    /**
     * `verts * verts` samples, row major in z then x.
     * */
    f32* heights;

    /** The static body carrying the triangle mesh. Releasing it frees the mesh Box3D built. */
    NYA_EntityHandle entity;

    /** What produced the current surface, so a landscape can be asked for again. */
    u64 seed;

    /** The extremes of `heights`, for the colour bands and for deciding how high to drop things from. */
    f32 min_height;
    f32 max_height;

    /* ── chunking, when options.chunked is set ── */

    NYA_Terrain3DChunk* chunks;
    u32                 chunk_count;
    u32                 chunks_x, chunks_z;

    /** Chunks whose geometry was rebuilt by the last nya_terrain3d_update. For an overlay. */
    u32 chunks_rebuilt;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Allocates the terrain and its sample grid from `arena`, filling in defaults for anything left zero. */
NYA_API NYA_Error nya_terrain3d_create(NYA_Arena* arena, NYA_Terrain3DOptions options, OUT NYA_Terrain3D** out_terrain)
    __attr_no_discard;

/**
 * Samples the heightmap, builds the collider and spawns the static body carrying it.
 * */
NYA_API void nya_terrain3d_generate(NYA_Terrain3D* terrain, NYA_Window* window, u64 seed);

/** Despawns the body and releases the GPU mesh. The sample grid stays; it belongs to the arena. */
NYA_API void nya_terrain3d_release(NYA_Terrain3D* terrain, NYA_Window* window);

/**
 * The ground height at a world xz, bilinear between the four samples around it.
 * */
NYA_API f32 nya_terrain3d_height_at(const NYA_Terrain3D* terrain, f32 x, f32 z) __attr_no_discard;

/**
 * Re-picks each chunk's detail level from its distance to `viewer`, rebuilding the ones that changed.
 *
 * ⚠ **A rebuild uploads geometry**, so this is not free on the frames where the camera crosses a
 * level boundary — which is why the choice is banded rather than continuous, and why crossing back and
 * forth over one boundary rebuilds one chunk rather than all of them. `chunks_rebuilt` reports how
 * many changed, which is what to watch if a camera path ever makes this stutter.
 * */
NYA_API void nya_terrain3d_update(NYA_Terrain3D* terrain, NYA_Window* window, f32x3 viewer);

/**
 * The detail level a chunk at `distance` from the viewer is drawn at.
 * */
NYA_API u32 nya_terrain3d_lod_for_distance(const NYA_Terrain3D* terrain, f32 distance) __attr_no_discard;

/**
 * The distance at which `level` begins. Zero for level zero, which begins at the camera.
 * */
NYA_API f32 nya_terrain3d_lod_boundary(const NYA_Terrain3D* terrain, u32 level) __attr_no_discard;

/** Draws the surface — every chunk of it, if chunked. Must be called between nya_render3d_begin and _end. */
NYA_API void nya_terrain3d_draw(const NYA_Terrain3D* terrain, NYA_Window* window);

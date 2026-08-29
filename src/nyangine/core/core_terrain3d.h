/**
 * @file core_terrain3d.h
 *
 * A heightmap ground: fBm noise sampled onto a grid, drawn as flat triangles and collided against as a
 * static triangle mesh.
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
 * **The collider and the drawn surface are separate geometry built from the same samples.** The collider
 * shares vertices between triangles, because Box3D builds a BVH over them and duplicating vertices
 * doubles it. The drawn mesh does not, because flat shading needs one normal per triangle and a shared
 * vertex can only carry one normal. Neither representation can serve both jobs.
 *
 * ## Chunking and GeoMipMapping
 *
 * By default the surface is one mesh with one bounding sphere: frustum culling asks about it once and
 * the answer is all-or-nothing, which is right for something a few dozen metres across and wrong for a
 * landscape.
 *
 * `.chunked` changes that. The surface is cut into squares of NYA_TERRAIN3D_CHUNK_CELLS, each its own
 * registered mesh with its own bounds, and each drawn at one of NYA_TERRAIN3D_LOD_LEVELS detail levels
 * chosen by its distance from the camera — GeoMipMapping, which is simply "sample this chunk's grid
 * every stride-th vertex" with stride doubling per level.
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
 *
 * Everything comes from the arena passed to nya_terrain3d_create, so there is no destroy — freeing the
 * arena frees the terrain. nya_terrain3d_release exists only for the GPU mesh and the physics body,
 * which are not the arena's to reclaim.
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
 *
 * A power of two, because every LOD stride has to divide it exactly — a chunk that does not tile
 * evenly at some level leaves a strip of cells with nowhere to go. Sixteen is the usual size: large
 * enough that the per-chunk overhead is amortised, small enough that culling one is worth something.
 * */
#ifndef NYA_TERRAIN3D_CHUNK_CELLS
#define NYA_TERRAIN3D_CHUNK_CELLS 16
#endif

static_assert((NYA_TERRAIN3D_CHUNK_CELLS & (NYA_TERRAIN3D_CHUNK_CELLS - 1)) == 0, "NYA_TERRAIN3D_CHUNK_CELLS must be a power of two");

/**
 * Detail levels a chunk may be drawn at. Level `n` samples every `1 << n`-th vertex.
 *
 * Four levels means the coarsest chunk has an eighth of the edge resolution and a sixty-fourth of the
 * triangles. Bounded by the chunk size: at NYA_TERRAIN3D_CHUNK_CELLS of sixteen, level four would be
 * a single quad and level five would have nothing left to halve.
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
     *
     * The out-of-range value rather than a separate flag: it makes the first update rebuild every
     * chunk without a special case, since no level it could choose can equal it.
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
     *
     * Off by default, because it is not free: a chunked surface is `chunk_count` registered meshes
     * rather than one, and it needs nya_terrain3d_update called with the camera each frame. For a
     * surface that fits on screen the unchunked path is strictly better.
     * */
    b8 chunked;

    /**
     * World distance at which a chunk drops to the next detail level, doubling per level.
     *
     * So level 1 begins at this distance, level 2 at twice it, level 3 at four times. Zero is read as
     * eight times a chunk's width, which keeps full detail out to a comfortable middle distance.
     * */
    f32 lod_distance;

    /**
     * How far a chunk's skirt hangs below its edge, world units. Zero is read from the cell size.
     *
     * It only has to be deep enough to cover the largest height difference two adjacent levels can
     * disagree by at a border, which is bounded by how much the surface can rise across one coarse
     * cell. Too deep costs nothing visually — the skirt is inside the ground — and a little too
     * shallow shows as a flickering hairline crack.
     * */
    f32 skirt_depth;

    /**
     * What the static body is spawned as, in the caller's own entity-type enum.
     *
     * A parameter rather than an engine constant, the same way nya_tilemap_collision_build takes one:
     * the engine has no opinion about what a game calls its terrain.
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
     *
     * Kept across a regeneration: the grid never changes size, and an arena does not hand memory back,
     * so allocating per generation would grow the arena on every reseed.
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
 *
 * Releases the previous body first, so calling this again with another seed replaces the surface rather
 * than stacking a second one on top of it.
 * */
NYA_API void nya_terrain3d_generate(NYA_Terrain3D* terrain, NYA_Window* window, u64 seed);

/** Despawns the body and releases the GPU mesh. The sample grid stays; it belongs to the arena. */
NYA_API void nya_terrain3d_release(NYA_Terrain3D* terrain, NYA_Window* window);

/**
 * The ground height at a world xz, bilinear between the four samples around it.
 *
 * Clamped to the terrain rather than extrapolated, and approximate by a few centimetres in the middle of
 * a steep cell — this places things above the ground, it does not resolve contacts. Zero before the
 * first generation.
 * */
NYA_API f32 nya_terrain3d_height_at(const NYA_Terrain3D* terrain, f32 x, f32 z) __attr_no_discard;

/**
 * Re-picks each chunk's detail level from its distance to `viewer`, rebuilding the ones that changed.
 *
 * Call once per frame for a chunked terrain, with the camera's position. A no-op for an unchunked one,
 * so a caller that may be handed either does not have to ask.
 *
 * ⚠ **A rebuild uploads geometry**, so this is not free on the frames where the camera crosses a
 * level boundary — which is why the choice is banded rather than continuous, and why crossing back and
 * forth over one boundary rebuilds one chunk rather than all of them. `chunks_rebuilt` reports how
 * many changed, which is what to watch if a camera path ever makes this stutter.
 * */
NYA_API void nya_terrain3d_update(NYA_Terrain3D* terrain, NYA_Window* window, f32x3 viewer);

/**
 * The detail level a chunk at `distance` from the viewer is drawn at.
 *
 * Exposed because it is the whole of the LOD policy and worth being able to reason about from
 * outside — and because a test can check the bands without a device.
 * */
NYA_API u32 nya_terrain3d_lod_for_distance(const NYA_Terrain3D* terrain, f32 distance) __attr_no_discard;

/**
 * The distance at which `level` begins. Zero for level zero, which begins at the camera.
 *
 * The boundaries the level bands are cut at, exposed because the hysteresis in nya_terrain3d_update
 * is defined against them — and because a game tuning `lod_distance` wants to be able to ask where
 * the bands actually landed rather than rederiving the doubling.
 * */
NYA_API f32 nya_terrain3d_lod_boundary(const NYA_Terrain3D* terrain, u32 level) __attr_no_discard;

/** Draws the surface — every chunk of it, if chunked. Must be called between nya_render3d_begin and _end. */
NYA_API void nya_terrain3d_draw(const NYA_Terrain3D* terrain, NYA_Window* window);

#include "nyangine/nyangine.h"

/** Fixed seed for the per-triangle shade jitter, so a surface looks the same for a given terrain seed. */
#define _NYA_TERRAIN3D_SHADE_SEED 0x7E44A1
/**
 * @file system_terrain3d.c
 *
 * The 3D scene's ground: a heightmap from fBm noise, drawn as flat triangles and collided against as a
 * static triangle mesh.
 *
 * ## Why this replaced a plane
 *
 * The scene used to stand on nya_render3d_plane over a static box, which was honest about what it was —
 * a backdrop — and useless as a test. A cube landing on a flat floor exercises one contact normal, never
 * rolls, and settles in the first second. Nothing about that tells you whether the solver, the batching
 * or the shading works on anything harder.
 *
 * A heightmap is the smallest thing that does. It gives the renderer thousands of triangles with normals
 * pointing everywhere, the shadow pass a receiver with real relief on it, and the solver a surface where
 * a cube lands on a slope, tips, rolls, and comes to rest somewhere different every seed.
 *
 * ## The two representations, and why they differ
 *
 * The heights are stored once, as a grid of samples. Everything else is derived from them, twice, in two
 * shapes that genuinely cannot be shared:
 *
 * - **The collider** wants a *shared* vertex grid: one vertex per sample, indexed by the triangles that
 *   meet there. Duplicating them would give Box3D coincident vertices on every edge and a BVH twice the
 *   size it needs. Built once, at generation, and handed to the solver, which copies it.
 *
 * - **The draw** wants *unshared* vertices: three per triangle, so each carries its own face normal and
 *   its own flat colour. That is the low-poly look, and it is not achievable with a shared grid — a
 *   shared vertex has one normal, which is the definition of smooth shading.
 *
 * Both are built once, here, and neither is rebuilt per frame. The draw side used to be: the surface went
 * through the immediate batch, so all 6144 of its vertices were transformed and uploaded again for the
 * camera pass and for each shadow cascade — around four hundred kilobytes, four times a frame, for
 * geometry that changes only when the seed does. It is registered with the renderer instead and drawn as
 * one instance; see nya_render3d_mesh_register.
 *
 * What that trades away is per-triangle frustum culling: the surface is now culled as a single bounding
 * sphere, so a cascade covering any part of it processes all of it. That is the right trade here and at
 * any realistic size — a few thousand vertices the GPU discards cost far less than the CPU writing and
 * uploading them, which is what the per-triangle version was paying to avoid.
 *
 * ## What is deliberately missing
 *
 * No LOD, no chunking, no frustum culling: the whole surface is sixteen metres across and always fully
 * in view. A landscape that scrolled would need all three, and none of them would live here.
 * */

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The world position of vertex grid sample (i, j). */
NYA_INTERNAL f32x3 _nya_terrain3d_corner(const NYA_Terrain3D* terrain, u32 i, u32 j);

/** The flat colour for one triangle, from the height of its centre. */
NYA_INTERNAL NYA_Color _nya_terrain3d_shade(const NYA_Terrain3D* terrain, f32 height, u32 cell, u32 half);

/** A chunk's bounding sphere, from the height grid over its footprint. See the definition. */
NYA_INTERNAL void _nya_terrain3d_chunk_bounds(const NYA_Terrain3D* terrain, NYA_Terrain3DChunk* chunk);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */


NYA_Error nya_terrain3d_create(NYA_Arena* arena, NYA_Terrain3DOptions options, OUT NYA_Terrain3D** out_terrain) {
    if (arena == nullptr || out_terrain == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no arena or no out pointer");

    // Zero means "unset" for every field, so a caller can name only what it cares about.
    if (options.resolution == 0) options.resolution = 32;
    if (options.extent <= 0.0F) options.extent = 16.0F;
    if (options.amplitude <= 0.0F) options.amplitude = 2.5F;
    if (options.rim_start <= 0.0F) options.rim_start = 0.55F;
    if (options.rim_height <= 0.0F) options.rim_height = 1.15F;
    if (options.frequency <= 0.0F) options.frequency = 0.09F;
    if (options.octaves == 0) options.octaves = 4;
    if (options.lacunarity <= 0.0F) options.lacunarity = 2.0F;
    if (options.gain <= 0.0F) options.gain = 0.5F;
    if (options.friction <= 0.0F) options.friction = 0.85F;
    if (options.shade_jitter <= 0.0F) options.shade_jitter = 0.06F;

    if (options.color_low.a == 0.0F) options.color_low = (NYA_Color){ 0.42F, 0.56F, 0.38F, 1.0F };
    if (options.color_mid.a == 0.0F) options.color_mid = (NYA_Color){ 0.55F, 0.67F, 0.42F, 1.0F };
    if (options.color_high.a == 0.0F) options.color_high = (NYA_Color){ 0.72F, 0.74F, 0.58F, 1.0F };
    if (options.color_peak.a == 0.0F) options.color_peak = (NYA_Color){ 0.86F, 0.83F, 0.71F, 1.0F };

    if (options.band_mid <= 0.0F) options.band_mid = 0.35F;
    if (options.band_high <= 0.0F) options.band_high = 0.62F;
    if (options.band_peak <= 0.0F) options.band_peak = 0.84F;

    NYA_Terrain3D* terrain = nya_arena_alloc(arena, sizeof(NYA_Terrain3D));

    *terrain = (NYA_Terrain3D){
        .allocator  = arena,
        .options    = options,
        .resolution = options.resolution,
        .verts      = options.resolution + 1,
        .cell       = options.extent / (f32)options.resolution,
        .entity     = NYA_ENTITY_HANDLE_NONE,
    };

    // Allocated once and kept across regenerations: the grid never changes size, and an arena does not
    // hand memory back, so allocating per generation would grow it on every reseed.
    terrain->heights = nya_arena_alloc(arena, (u64)terrain->verts * (u64)terrain->verts * sizeof(f32));

    if (options.chunked) {
        /*
         * Rounded up, so a resolution that is not a whole number of chunks still covers the surface —
         * the last chunk in each direction is simply clipped to what is left. Refusing instead would
         * make the chunk size a constraint on the resolution, which is a surprising thing for a
         * rendering detail to impose.
         */
        terrain->chunks_x    = (terrain->resolution + NYA_TERRAIN3D_CHUNK_CELLS - 1) / NYA_TERRAIN3D_CHUNK_CELLS;
        terrain->chunks_z    = (terrain->resolution + NYA_TERRAIN3D_CHUNK_CELLS - 1) / NYA_TERRAIN3D_CHUNK_CELLS;
        terrain->chunk_count = terrain->chunks_x * terrain->chunks_z;

        terrain->chunks = nya_arena_alloc(arena, (u64)terrain->chunk_count * sizeof(NYA_Terrain3DChunk));

        for (u32 cz = 0; cz < terrain->chunks_z; cz++) {
            for (u32 cx = 0; cx < terrain->chunks_x; cx++) {
                NYA_Terrain3DChunk* chunk = &terrain->chunks[(cz * terrain->chunks_x) + cx];

                *chunk = (NYA_Terrain3DChunk){
                    .cell_x = cx * NYA_TERRAIN3D_CHUNK_CELLS,
                    .cell_z = cz * NYA_TERRAIN3D_CHUNK_CELLS,

                    // Out of range, so the first update rebuilds every chunk without a separate flag.
                    .lod = NYA_TERRAIN3D_LOD_LEVELS,
                };

                (void)snprintf(chunk->handle, sizeof(chunk->handle), "nya_terrain3d_chunk_%u", (cz * terrain->chunks_x) + cx);
            }
        }

        if (terrain->chunk_count > NYA_RENDER3D_MAX_REGISTERED_MESHES) {
            nya_log_warn("A chunked terrain of %ux%u needs " FMTu32 " registered meshes and only %d are available; "
                         "raise NYA_RENDER3D_MAX_REGISTERED_MESHES or NYA_TERRAIN3D_CHUNK_CELLS.",
                         terrain->chunks_x, terrain->chunks_z, terrain->chunk_count, NYA_RENDER3D_MAX_REGISTERED_MESHES);
        }
    }

    *out_terrain = terrain;

    return NYA_OK;
}

void nya_terrain3d_generate(NYA_Terrain3D* terrain, NYA_Window* window, u64 seed) {
    nya_assert(terrain != nullptr && terrain->heights != nullptr, "the terrain must come from nya_terrain3d_create");

    // The grid was allocated once by nya_terrain3d_create and is rewritten in place here: an arena does
    // not hand memory back, so allocating per generation would grow it on every reseed.

    // Immediate rather than deferred: this runs from the layer's on_create and from a key press, never
    // from inside an entity iteration, and the new body has to exist before this returns.
    if (nya_entity_is_valid(terrain->entity)) nya_entity_despawn(terrain->entity);

    terrain->entity = NYA_ENTITY_HANDLE_NONE;
    terrain->seed   = seed;

    // The RNG takes its seed as an uppercase hex string of at most 64 digits, left padded — not an
    // arbitrary label. The same trap the 2D terrain generator documents.
    char seed_text[17];
    (void)snprintf(seed_text, sizeof(seed_text), "%016llX", (unsigned long long)seed);

    NYA_RNG   rng   = nya_rng_create(.seed = seed_text);
    NYA_Noise noise = nya_noise_create(&rng);

    NYA_NoiseParams params = {
        .octaves    = terrain->options.octaves,
        .lacunarity = terrain->options.lacunarity,
        .gain       = terrain->options.gain,
    };

    f32 half = terrain->options.extent * 0.5F;

    terrain->min_height = FLT_MAX;
    terrain->max_height = -FLT_MAX;

    for (u32 j = 0; j < terrain->verts; j++) {
        for (u32 i = 0; i < terrain->verts; i++) {
            f32 x = -half + ((f32)i * terrain->cell);
            f32 z = -half + ((f32)j * terrain->cell);

            /*
             * Two terms: free noise in the middle, a rim that rises at the edge.
             *
             * fBm alone gives hills of the same character everywhere including hard against the
             * boundary, and the boundary is where the surface stops existing. The first version damped
             * the noise toward zero out there, on the reasoning that a flat beach is a gentle edge —
             * which was wrong in a way only the running scene showed: damping toward zero around a
             * middle that happens to be high makes the whole terrain a *dome*, and every cube dropped on
             * it rolls outward and off. A third of the pile had gone over the edge within a minute.
             *
             * Lifting the rim instead makes it a basin. The noise still has the middle to itself, the
             * boundary is a ring of hills a cube rolls back down from rather than over, and nothing has
             * to fence the scene in with invisible walls.
             *
             * Radial rather than per axis: a per-axis rim leaves the corners low, which reads as four
             * gaps in the ring — exactly where things would then escape.
             */
            f32 height = nya_noise_fbm2(&noise, x * terrain->options.frequency, z * terrain->options.frequency, params);

            /*
             * The distance to the nearest *edge*, not to the centre.
             *
             * A Euclidean radius describes a circle inscribed in a square terrain, so everything outside
             * that circle — the four corners, which is a fifth of the area — is past the end of the ramp
             * and sits at a flat plateau of rim height. On screen that is a large dead tabletop around a
             * small bowl, and it was the first thing wrong with the picture.
             *
             * The max-norm makes the level sets squares, so the rim follows the boundary it actually has
             * and reaches full height exactly at it. No plateau, and the whole surface is terrain.
             */
            f32 radial = nya_max(fabsf(x), fabsf(z)) / half;

            /*
             * Smoothstep by hand, so the rim eases out of the flat middle instead of creasing where it
             * starts to bite. math_scalar.h has min, max, clamp and lerp and no smoothstep; two lines
             * here is smaller than an addition to the engine with one caller.
             */
            f32 ramp = nya_clamp((radial - terrain->options.rim_start) / (1.0F - terrain->options.rim_start), 0.0F, 1.0F);
            f32 rim  = ramp * ramp * (3.0F - (2.0F * ramp));

            // The noise fades out exactly as the rim comes up, so the two never fight over the same
            // ground and the ring is a clean lip rather than a lumpy one.
            f32 value = ((height * (1.0F - rim)) + (rim * terrain->options.rim_height)) * terrain->options.amplitude;

            terrain->heights[(j * terrain->verts) + i] = value;

            terrain->min_height = nya_min(terrain->min_height, value);
            terrain->max_height = nya_max(terrain->max_height, value);
        }
    }

    /*
     * The collider, from a scratch copy of the same samples.
     *
     * Temporary because nya_physics3d_body_attach copies what it is given into Box3D's own structure —
     * see NYA_PHYSICS3D_SHAPE_MESH — so nothing here has to outlive the call.
     */
    u32 vertex_count = terrain->verts * terrain->verts;
    u32 index_count  = terrain->resolution * terrain->resolution * 6;

    u64 vertex_bytes = (u64)vertex_count * sizeof(f32x3);
    u64 index_bytes  = (u64)index_count * sizeof(u32);

    f32x3* vertices = nya_arena_alloc(nya_arena_temp, vertex_bytes);
    u32*   indices  = nya_arena_alloc(nya_arena_temp, index_bytes);

    for (u32 j = 0; j < terrain->verts; j++) {
        for (u32 i = 0; i < terrain->verts; i++) {
            vertices[(j * terrain->verts) + i] = _nya_terrain3d_corner(terrain, i, j);
        }
    }

    u32 written = 0;

    for (u32 j = 0; j < terrain->resolution; j++) {
        for (u32 i = 0; i < terrain->resolution; i++) {
            u32 a = (j * terrain->verts) + i;
            u32 b = a + 1;
            u32 c = a + terrain->verts;
            u32 d = c + 1;

            /*
             * Wound so the face normal points up, which for a collider decides which side a body is
             * pushed out toward rather than whether the triangle is visible.
             *
             * Getting it backwards in the renderer makes a surface disappear, which is obvious. Getting
             * it backwards here makes bodies fall through the ground while it still draws perfectly,
             * which is not — so the draw below deliberately walks the same corners in the same order.
             */
            indices[written++] = a;
            indices[written++] = c;
            indices[written++] = b;

            indices[written++] = b;
            indices[written++] = c;
            indices[written++] = d;
        }
    }

    terrain->entity = nya_entity_spawn(
        .name  = "terrain3d",
        .type  = terrain->options.entity_type,
        .state = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE | NYA_ENTITY_STATE_STATIC
    );

    nya_assert(nya_entity_is_valid(terrain->entity), "Failed to spawn the 3D terrain entity.");

    // The vertices are already world positions and the entity sits at the origin, so they are its body
    // frame unchanged — the same arrangement the 2D chain uses.
    b8 attached = nya_physics3d_body_attach(
        terrain->entity,
        .type         = NYA_PHYSICS_BODY_STATIC,
        .shape        = NYA_PHYSICS3D_SHAPE_MESH,
        .vertices     = vertices,
        .indices      = indices,
        .vertex_count = vertex_count,
        .index_count  = index_count,
        .friction     = terrain->options.friction
    );

    // Reverse order, so the temp arena can actually reclaim both rather than only the last one.
    nya_arena_free(nya_arena_temp, indices, index_bytes);
    nya_arena_free(nya_arena_temp, vertices, vertex_bytes);

    if (!attached) {
        // Not fatal, and not silent. The scene still draws; things dropped onto it fall through, which
        // is confusing enough to be worth a line naming the cause.
        nya_log_error("The 3D terrain has no collider; anything dropped on it will fall through.");
    }

    /*
     * The draw geometry, built once here and handed to the renderer to keep.
     *
     * Unshared vertices — three per triangle — so each face carries its own normal and its own flat
     * colour. That is the whole low-poly look and it is why this cannot reuse the collider's shared grid.
     *
     * Temporary, like the collider's arrays: nya_render3d_mesh_register copies to the GPU and keeps
     * nothing on the CPU.
     *
     * ⚠ **Skipped entirely for a chunked terrain**, which draws its chunks instead. Building both was
     * a third of a megabyte of vertices uploaded and then never drawn — visible only as a line in the
     * mesh registry saying how large it was.
     */
    if (!terrain->options.chunked) {

        u32 draw_vertex_count = terrain->resolution * terrain->resolution * 6;

        u64 draw_bytes = (u64)draw_vertex_count * sizeof(NYA_Vertex3D);

        NYA_Vertex3D* draw_vertices = nya_arena_alloc(nya_arena_temp, draw_bytes);

        u32 emitted = 0;

        for (u32 j = 0; j < terrain->resolution; j++) {
            for (u32 i = 0; i < terrain->resolution; i++) {
                f32x3 corner_a = _nya_terrain3d_corner(terrain, i, j);
                f32x3 corner_b = _nya_terrain3d_corner(terrain, i + 1, j);
                f32x3 corner_c = _nya_terrain3d_corner(terrain, i, j + 1);
                f32x3 corner_d = _nya_terrain3d_corner(terrain, i + 1, j + 1);

                u32 cell = (j * terrain->resolution) + i;

                /*
                 * Two triangles, wound and coloured exactly as the immediate version drew them.
                 *
                 * Same corners in the same order as the collider above, so the surface that is drawn is the
                 * surface that is hit — and the normal is the face normal, computed here once instead of by
                 * the renderer on every frame.
                 */
                f32x3 triangles[2][3] = {
                    { corner_a, corner_c, corner_b },
                    { corner_b, corner_c, corner_d },
                };

                for (u32 half = 0; half < 2; half++) {
                    f32x3 a = triangles[half][0];
                    f32x3 b = triangles[half][1];
                    f32x3 c = triangles[half][2];

                    f32x3 normal = nya_vector_normalize(nya_vector_cross(b - a, c - a));

                    NYA_Color color = _nya_terrain3d_shade(terrain, (a.y + b.y + c.y) / 3.0F, cell, half);

                    draw_vertices[emitted++] = (NYA_Vertex3D){ .position = a, .color = color, .normals = normal };
                    draw_vertices[emitted++] = (NYA_Vertex3D){ .position = b, .color = color, .normals = normal };
                    draw_vertices[emitted++] = (NYA_Vertex3D){ .position = c, .color = color, .normals = normal };
                }
            }
        }

        b8 registered = nya_render3d_mesh_register(window, NYA_TERRAIN3D_MESH, draw_vertices, emitted);

        nya_arena_free(nya_arena_temp, draw_vertices, draw_bytes);

        // Not fatal either: nya_terrain3d_draw asks the renderer to draw the handle, and a handle that names
        // nothing draws nothing. A collider with no surface is odd to look at and still simulates.
        if (!registered) nya_log_error("The 3D terrain has no drawable geometry; the scene will show a hole.");
    }


    /*
     * The chunk bounds, now that there are heights to measure.
     *
     * Before the first nya_terrain3d_update rather than during it, because the update measures
     * distance to these — see the note on _nya_terrain3d_chunk_bounds.
     */
    for (u32 index = 0; index < terrain->chunk_count; index++) {
        _nya_terrain3d_chunk_bounds(terrain, &terrain->chunks[index]);

        // Unbuilt, so a regeneration rebuilds every chunk against the new surface rather than keeping
        // geometry cut from the old one.
        terrain->chunks[index].lod = NYA_TERRAIN3D_LOD_LEVELS;
    }

    nya_log_info("3D terrain generated from seed %llu (%u triangles, height %.2f to %.2f).", (unsigned long long)seed, index_count / 3,
             (f64)terrain->min_height, (f64)terrain->max_height);
}

void nya_terrain3d_release(NYA_Terrain3D* terrain, NYA_Window* window) {

    // The geometry goes with the scene. Nothing else would release it: a registered mesh has no asset
    // behind it whose unload would take it, only the window's teardown as a backstop.
    nya_render3d_mesh_release(window, NYA_TERRAIN3D_MESH);

    // And every chunk's, which is chunk_count more of them. Marked unbuilt as they go, so a terrain
    // released and regenerated rebuilds rather than drawing handles that name nothing.
    for (u32 index = 0; index < terrain->chunk_count; index++) {
        nya_render3d_mesh_release(window, terrain->chunks[index].handle);
        terrain->chunks[index].lod = NYA_TERRAIN3D_LOD_LEVELS;
    }

    // Deferred, because this runs from a layer's on_destroy, which can itself be inside the layer
    // stack's iteration. Despawning takes the physics body — and its triangle mesh — with it.
    if (nya_entity_is_valid(terrain->entity)) nya_entity_despawn_deferred(terrain->entity);

    /*
     * The height grid is *not* released and the pointer is kept.
     *
     * It came from the world's arena, which frees as a whole when the world does, and holding the
     * pointer is what lets a scene that is left and re-entered reuse the same block instead of taking
     * another one. Everything else here is reset, because it names an entity that is going away.
     */
    terrain->entity     = NYA_ENTITY_HANDLE_NONE;
    terrain->seed       = 0;
    terrain->min_height = 0.0F;
    terrain->max_height = 0.0F;
}

f32 nya_terrain3d_height_at(const NYA_Terrain3D* terrain, f32 x, f32 z) {

    if (terrain->heights == nullptr) return 0.0F;

    f32 half = terrain->options.extent * 0.5F;

    // In grid cells from the low corner. Clamped rather than wrapped: outside the terrain the nearest
    // edge height is the useful answer, and a caller asking is placing something, not sampling a field.
    f32 grid_x = nya_clamp((x + half) / terrain->cell, 0.0F, (f32)terrain->resolution);
    f32 grid_z = nya_clamp((z + half) / terrain->cell, 0.0F, (f32)terrain->resolution);

    u32 i = (u32)grid_x;
    u32 j = (u32)grid_z;

    // The far edge lands exactly on the last sample, where the cell to its right does not exist.
    if (i >= terrain->resolution) i = terrain->resolution - 1;
    if (j >= terrain->resolution) j = terrain->resolution - 1;

    f32 fraction_x = grid_x - (f32)i;
    f32 fraction_z = grid_z - (f32)j;

    /*
     * Bilinear, which is not quite the surface.
     *
     * The mesh is two triangles per cell, so the true height is *piecewise* linear over one of them and
     * a bilinear patch is a smooth approximation that differs by a few centimetres in the middle of a
     * steep cell. That is well inside what this is used for — deciding where above the ground to start a
     * falling cube — and a triangle test here would be exact for no visible gain.
     */
    f32 h00 = terrain->heights[(j * terrain->verts) + i];
    f32 h10 = terrain->heights[(j * terrain->verts) + i + 1];
    f32 h01 = terrain->heights[((j + 1) * terrain->verts) + i];
    f32 h11 = terrain->heights[((j + 1) * terrain->verts) + i + 1];

    return nya_lerp(nya_lerp(h00, h10, fraction_x), nya_lerp(h01, h11, fraction_x), fraction_z);
}

/*
 * ─────────────────────────────────────────────────────────
 * CHUNKS AND GEOMIPMAPPING
 * ─────────────────────────────────────────────────────────
 */

/**
 * How far a chunk's skirt hangs below its edge.
 *
 * ⚠ **The bound that matters is the terrain's height range, not a multiple of the cell.** The skirt
 * only has to cover the largest height two adjacent levels can disagree by at a border, and that is
 * bounded by how far the surface rises at all — it cannot disagree by more than the whole relief.
 *
 * This used to be `cell * 2^LOD_LEVELS`, sixteen cells deep, which on a small terrain is deeper than
 * the terrain is wide. That went straight into every chunk's bounding radius, which is what the LOD
 * measures distance against, and the result was chunks reading as far away while the camera sat on
 * top of them.
 * */
NYA_INTERNAL f32 _nya_terrain3d_skirt_depth(const NYA_Terrain3D* terrain) {
    if (terrain->options.skirt_depth > 0.0F) return terrain->options.skirt_depth;

    f32 relief = terrain->max_height - terrain->min_height;

    // One cell as a floor, for a perfectly flat surface — where two levels cannot disagree at all, but
    // a skirt of literally zero would still leave a hairline seam to floating point.
    return nya_max(relief, terrain->cell);
}

/**
 * A chunk's bounding sphere, from the height grid over its footprint.
 *
 * ⚠ **Computed here rather than while building the geometry, and that is a fix rather than a
 * preference.** The bounds are what the LOD choice measures distance to, and building them alongside
 * the mesh meant the *first* choice was made against a zeroed centre — every chunk read as sitting at
 * the origin, all picked the same level, and the frame after that they all picked a different one and
 * rebuilt a second time. Caught by the test asserting that standing still rebuilds nothing.
 *
 * At full resolution, not at the chunk's current level: the sphere has to contain the surface however
 * coarsely it happens to be drawn, and a coarse sampling can only ever miss a peak, never invent one.
 * */
NYA_INTERNAL void _nya_terrain3d_chunk_bounds(const NYA_Terrain3D* terrain, NYA_Terrain3DChunk* chunk) {
    u32 cells_x = nya_min((u32)NYA_TERRAIN3D_CHUNK_CELLS, terrain->resolution - chunk->cell_x);
    u32 cells_z = nya_min((u32)NYA_TERRAIN3D_CHUNK_CELLS, terrain->resolution - chunk->cell_z);

    f32 lowest  = FLT_MAX;
    f32 highest = -FLT_MAX;

    for (u32 j = chunk->cell_z; j <= chunk->cell_z + cells_z; j++) {
        for (u32 i = chunk->cell_x; i <= chunk->cell_x + cells_x; i++) {
            f32 height = terrain->heights[(j * terrain->verts) + i];

            lowest  = nya_min(lowest, height);
            highest = nya_max(highest, height);
        }
    }

    f32 skirt = _nya_terrain3d_skirt_depth(terrain);

    f32 half        = terrain->options.extent * 0.5F;
    f32 world_x     = -half + ((f32)chunk->cell_x * terrain->cell);
    f32 world_z     = -half + ((f32)chunk->cell_z * terrain->cell);
    f32 world_width = (f32)cells_x * terrain->cell;
    f32 world_depth = (f32)cells_z * terrain->cell;

    chunk->center = (f32x3){ world_x + (world_width * 0.5F), (lowest + highest) * 0.5F, world_z + (world_depth * 0.5F) };

    f32 extent_x = world_width * 0.5F;

    // Plus the skirt, which hangs below everything the surface reaches.
    f32 extent_y = ((highest - lowest) * 0.5F) + skirt;
    f32 extent_z = world_depth * 0.5F;

    chunk->radius = sqrtf((extent_x * extent_x) + (extent_y * extent_y) + (extent_z * extent_z));
}

/** One triangle of the drawn surface, appended with its face normal and band colour. */
NYA_INTERNAL void _nya_terrain3d_emit_triangle(const NYA_Terrain3D* terrain, NYA_Vertex3D* out, OUT u32* count, f32x3 a, f32x3 b, f32x3 c,
                                               u32 cell, u32 half) {
    f32x3 edge_ab = b - a;
    f32x3 edge_ac = c - a;

    f32x3 normal = nya_vector_normalize(nya_vector_cross(edge_ab, edge_ac));

    NYA_Color color = _nya_terrain3d_shade(terrain, (a.y + b.y + c.y) / 3.0F, cell, half);

    out[(*count)++] = (NYA_Vertex3D){ .position = a, .color = color, .normals = normal };
    out[(*count)++] = (NYA_Vertex3D){ .position = b, .color = color, .normals = normal };
    out[(*count)++] = (NYA_Vertex3D){ .position = c, .color = color, .normals = normal };
}

/**
 * Builds one chunk's geometry at `lod` and registers it, replacing whatever was there.
 *
 * The stride is the whole of GeoMipMapping: at level `n` the chunk's grid is sampled every `1 << n`
 * vertices, so the surface keeps its shape and loses its detail. Everything else here — the winding,
 * the flat normal, the colour bands — is exactly what the unchunked path does, because a chunked
 * surface has to look like the same terrain.
 * */
NYA_INTERNAL void _nya_terrain3d_chunk_build(NYA_Terrain3D* terrain, NYA_Window* window, NYA_Terrain3DChunk* chunk, u32 lod) {
    u32 stride = 1u << lod;

    // Clipped at the terrain's edge: the last chunk in a row is short when the resolution is not a
    // whole number of chunks.
    u32 cells_x = nya_min((u32)NYA_TERRAIN3D_CHUNK_CELLS, terrain->resolution - chunk->cell_x);
    u32 cells_z = nya_min((u32)NYA_TERRAIN3D_CHUNK_CELLS, terrain->resolution - chunk->cell_z);

    // A stride coarser than what is left would step past the chunk entirely and emit nothing, which is
    // a hole rather than a coarse patch.
    while (stride > 1 && (cells_x % stride != 0 || cells_z % stride != 0)) stride >>= 1;

    u32 steps_x = cells_x / stride;
    u32 steps_z = cells_z / stride;

    if (steps_x == 0 || steps_z == 0) return;

    // Surface plus a skirt down each of the four sides. Six vertices per quad throughout, since the
    // mesh is unshared — see the note on flat shading in this file's header.
    u32 quad_count   = (steps_x * steps_z) + (2 * steps_x) + (2 * steps_z);
    u32 vertex_count = quad_count * 6;

    u64           bytes    = (u64)vertex_count * sizeof(NYA_Vertex3D);
    NYA_Vertex3D* vertices = nya_arena_alloc(nya_arena_temp, bytes);

    u32 emitted = 0;

    f32 skirt = _nya_terrain3d_skirt_depth(terrain);

    for (u32 sz = 0; sz < steps_z; sz++) {
        for (u32 sx = 0; sx < steps_x; sx++) {
            u32 i = chunk->cell_x + (sx * stride);
            u32 j = chunk->cell_z + (sz * stride);

            f32x3 corner_a = _nya_terrain3d_corner(terrain, i, j);
            f32x3 corner_b = _nya_terrain3d_corner(terrain, i + stride, j);
            f32x3 corner_c = _nya_terrain3d_corner(terrain, i, j + stride);
            f32x3 corner_d = _nya_terrain3d_corner(terrain, i + stride, j + stride);

            // The same cell index the unchunked path uses, so the per-cell shade jitter is identical
            // and switching a chunk's level does not reshuffle its colours.
            u32 cell = (j * terrain->resolution) + i;

            _nya_terrain3d_emit_triangle(terrain, vertices, &emitted, corner_a, corner_c, corner_b, cell, 0);
            _nya_terrain3d_emit_triangle(terrain, vertices, &emitted, corner_b, corner_c, corner_d, cell, 1);
        }
    }

    /*
     * The skirt: a vertical flange hanging from every edge vertex.
     *
     * Two chunks at different levels sample their shared border at different points, so their edges do
     * not meet — the gap between them is a crack straight through to the background, and it is the one
     * artifact that makes GeoMipMapping look broken rather than merely coarse. The flange fills that
     * gap with terrain-coloured geometry, which the neighbour's own surface then covers whichever level
     * it is at. Never seen, unless it is too short.
     *
     * Wound so each side faces outward — a skirt culled away is a skirt that is not there.
     */
    for (u32 side = 0; side < 4; side++) {
        u32 steps = (side < 2) ? steps_x : steps_z;

        for (u32 step = 0; step < steps; step++) {
            u32 i, j, next_i, next_j;

            switch (side) {
                case 0:   // north edge, j fixed at the low border
                    i = chunk->cell_x + (step * stride); j = chunk->cell_z;
                    next_i = i + stride; next_j = j;
                    break;
                case 1:   // south edge
                    i = chunk->cell_x + ((step + 1) * stride); j = chunk->cell_z + (steps_z * stride);
                    next_i = i - stride; next_j = j;
                    break;
                case 2:   // west edge
                    i = chunk->cell_x; j = chunk->cell_z + ((step + 1) * stride);
                    next_i = i; next_j = j - stride;
                    break;
                default:  // east edge
                    i = chunk->cell_x + (steps_x * stride); j = chunk->cell_z + (step * stride);
                    next_i = i; next_j = j + stride;
                    break;
            }

            f32x3 top_a = _nya_terrain3d_corner(terrain, i, j);
            f32x3 top_b = _nya_terrain3d_corner(terrain, next_i, next_j);

            f32x3 bottom_a = { top_a.x, top_a.y - skirt, top_a.z };
            f32x3 bottom_b = { top_b.x, top_b.y - skirt, top_b.z };

            u32 cell = (j * terrain->resolution) + (i < terrain->resolution ? i : terrain->resolution - 1);

            _nya_terrain3d_emit_triangle(terrain, vertices, &emitted, top_a, bottom_a, top_b, cell, 0);
            _nya_terrain3d_emit_triangle(terrain, vertices, &emitted, top_b, bottom_a, bottom_b, cell, 1);
        }
    }

    if (!nya_render3d_mesh_register(window, chunk->handle, vertices, emitted)) {
        nya_log_error("Terrain chunk '%s' has no drawable geometry; the scene will show a hole.", chunk->handle);
    }

    nya_arena_free(nya_arena_temp, vertices, bytes);

    chunk->lod = lod;
}

u32 nya_terrain3d_lod_for_distance(const NYA_Terrain3D* terrain, f32 distance) {
    if (terrain == nullptr) return 0;

    f32 first = terrain->options.lod_distance > 0.0F ? terrain->options.lod_distance
                                                     : terrain->cell * (f32)NYA_TERRAIN3D_CHUNK_CELLS * 8.0F;

    /*
     * Banded and doubling, rather than a continuous function of distance.
     *
     * The level has to be a small integer anyway, and a band is what makes crossing a boundary rebuild
     * one chunk rather than every chunk edging over a threshold at once. Doubling matches the stride
     * doubling: each level covers twice the ground at half the density, so the triangles a chunk
     * contributes stay roughly constant with distance, which is the whole idea.
     */
    u32 lod = 0;

    while (lod + 1 < NYA_TERRAIN3D_LOD_LEVELS && distance >= first) {
        first *= 2.0F;
        lod++;
    }

    return lod;
}

f32 nya_terrain3d_lod_boundary(const NYA_Terrain3D* terrain, u32 level) {
    if (terrain == nullptr || level == 0) return 0.0F;

    f32 boundary = terrain->options.lod_distance > 0.0F ? terrain->options.lod_distance
                                                        : terrain->cell * (f32)NYA_TERRAIN3D_CHUNK_CELLS * 8.0F;

    // Doubling, matching nya_terrain3d_lod_for_distance: level 1 begins at the first distance, level 2
    // at twice it, level 3 at four times.
    for (u32 i = 1; i < level; i++) boundary *= 2.0F;

    return boundary;
}

void nya_terrain3d_update(NYA_Terrain3D* terrain, NYA_Window* window, f32x3 viewer) {
    if (terrain == nullptr || !terrain->options.chunked || terrain->chunks == nullptr) return;

    nya_perf_time_this_function();

    terrain->chunks_rebuilt = 0;

    for (u32 index = 0; index < terrain->chunk_count; index++) {
        NYA_Terrain3DChunk* chunk = &terrain->chunks[index];

        /*
         * Horizontal distance to the chunk's **sphere**, not to its centre.
         *
         * Horizontal because a camera high above a landscape is far from every chunk by the
         * straight-line measure, so everything would drop to the coarsest level the moment it climbed —
         * the opposite of what a viewer looking down at the ground wants.
         *
         * ⚠ **To the sphere because a chunk is not a point.** Measuring to the centre makes a chunk
         * half its own width "away" even when the camera is standing on its near edge, so a chunk the
         * viewer is *inside* reads as a chunk's-width distant and coarsens. On a terrain whose chunks
         * are large relative to the view — which is any small scene — that is enough on its own to
         * strip the detail off everything, which is exactly how it was first noticed.
         */
        f32 dx = chunk->center.x - viewer.x;
        f32 dz = chunk->center.z - viewer.z;

        f32 distance = sqrtf((dx * dx) + (dz * dz)) - chunk->radius;

        if (distance < 0.0F) distance = 0.0F;

        u32 wanted = nya_terrain3d_lod_for_distance(terrain, distance);

        /*
         * Hysteresis, but only for a chunk that already has geometry — an unbuilt one takes whatever
         * the distance says, since there is nothing to keep.
         *
         * A chunk sitting on a band boundary otherwise changes level every few frames as the viewer
         * drifts back and forth across it, and every change is a geometry upload. Widening each
         * boundary into a dead band costs a slightly stale level and turns a continuous stutter into
         * nothing at all. See NYA_TERRAIN3D_LOD_HYSTERESIS.
         */
        if (chunk->lod < NYA_TERRAIN3D_LOD_LEVELS && wanted != chunk->lod) {
            if (wanted > chunk->lod) {
                // Coarsening: has to be past the boundary it is crossing, plus the margin.
                f32 boundary = nya_terrain3d_lod_boundary(terrain, chunk->lod + 1);

                if (distance < boundary * (1.0F + NYA_TERRAIN3D_LOD_HYSTERESIS)) wanted = chunk->lod;
            } else {
                // Refining: has to be back under the boundary it came up through, minus the margin.
                f32 boundary = nya_terrain3d_lod_boundary(terrain, chunk->lod);

                if (distance > boundary * (1.0F - NYA_TERRAIN3D_LOD_HYSTERESIS)) wanted = chunk->lod;
            }
        }

        if (wanted == chunk->lod) continue;

        _nya_terrain3d_chunk_build(terrain, window, chunk, wanted);
        terrain->chunks_rebuilt++;
    }
}

void nya_terrain3d_draw(const NYA_Terrain3D* terrain, NYA_Window* window) {
    nya_perf_time_this_scope("nya_terrain3d_draw");

    if (terrain == nullptr) return;

    if (terrain->options.chunked && terrain->chunks != nullptr) {
        /*
         * One draw per chunk, each with its own bounds — which is the point of chunking, because the
         * renderer culls per drawn mesh and a single surface is all-or-nothing.
         *
         * A chunk whose geometry has never been built is skipped rather than drawn: it has no mesh
         * registered under its handle, and asking for one draws nothing anyway. Skipping it here keeps
         * the draw call count honest.
         */
        for (u32 index = 0; index < terrain->chunk_count; index++) {
            const NYA_Terrain3DChunk* chunk = &terrain->chunks[index];
            if (chunk->lod >= NYA_TERRAIN3D_LOD_LEVELS) continue;

            nya_render3d_mesh(window, chunk->handle, f32x3_zero, (f32x3){ 1.0F, 1.0F, 1.0F }, nya_quaternion_identity, NYA_COLOR_WHITE);
        }

        return;
    }

    /*
     * One instanced draw of geometry that was uploaded when the surface was generated.
     *
     * This used to walk 1024 cells and emit 2048 triangles through nya_render3d_triangle, every pass —
     * so 6144 vertices were transformed and uploaded for the camera and again for each shadow cascade.
     * The surface does not change between generations, which makes all of that work to produce the same
     * bytes four times a frame.
     *
     * White, because the colours are already in the vertices: the tint multiplies them, and anything but
     * white would wash the height bands toward it.
     */
    nya_render3d_mesh(window, NYA_TERRAIN3D_MESH, f32x3_zero, (f32x3){ 1.0F, 1.0F, 1.0F }, nya_quaternion_identity, NYA_COLOR_WHITE);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32x3 _nya_terrain3d_corner(const NYA_Terrain3D* terrain, u32 i, u32 j) {
    f32 half = terrain->options.extent * 0.5F;

    return (f32x3){
        -half + ((f32)i * terrain->cell),
        terrain->heights[(j * terrain->verts) + i],
        -half + ((f32)j * terrain->cell),
    };
}

NYA_Color _nya_terrain3d_shade(const NYA_Terrain3D* terrain, f32 height, u32 cell, u32 half) {
    f32 range = terrain->max_height - terrain->min_height;

    // A perfectly flat seed is possible in principle and divides by nothing. Everything is then in the
    // lowest band, which is the right answer for a surface with no relief.
    f32 unit = range > NYA_EPSILON ? (height - terrain->min_height) / range : 0.0F;

    NYA_Color color = terrain->options.color_low;

    if (unit >= terrain->options.band_peak) {
        color = terrain->options.color_peak;
    } else if (unit >= terrain->options.band_high) {
        color = terrain->options.color_high;
    } else if (unit >= terrain->options.band_mid) {
        color = terrain->options.color_mid;
    }

    /*
     * A per-triangle nudge on top of the band.
     *
     * Without it a band is a flat sheet of one colour and the facets vanish into it — which defeats the
     * whole reason for unshared vertices. The engine's integer hash rather than a multiplicative one
     * written here; a hand-rolled version does not survive a sanitized
     * build.
     *
     * Keyed on the cell *and* which half of it, so the two triangles of a cell differ from each other.
     * Keyed on the index rather than the position, so it is stable while the camera moves and identical
     * between the shadow pass and the camera pass — a jitter that differed between them would put noise
     * in the shadows.
     */
    f32 jitter = nya_ihash2((s32)cell, (s32)half, _NYA_TERRAIN3D_SHADE_SEED) * terrain->options.shade_jitter;

    return (NYA_Color){
        nya_clamp(color.r + jitter, 0.0F, 1.0F),
        nya_clamp(color.g + jitter, 0.0F, 1.0F),
        nya_clamp(color.b + jitter, 0.0F, 1.0F),
        color.a,
    };
}

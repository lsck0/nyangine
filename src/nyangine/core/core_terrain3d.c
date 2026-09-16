#include "nyangine/nyangine.h"

/** Fixed seed for the per-triangle shade jitter, so a surface looks the same for a given terrain seed. */
#define _NYA_TERRAIN3D_SHADE_SEED 0x7E44A1

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The world position of vertex grid sample (i, j). */
NYA_INTERNAL f32x3 _nya_terrain3d_corner(const NYA_Terrain3D* terrain, u32 i, u32 j);

/** The flat colour for one triangle, from the height of its centre. */
NYA_INTERNAL NYA_Color _nya_terrain3d_shade(const NYA_Terrain3D* terrain, f32 height, u32 cell, u32 half);

/** A chunk's bounding sphere, from the height grid over its footprint. */
NYA_INTERNAL void _nya_terrain3d_chunk_bounds(const NYA_Terrain3D* terrain, NYA_Terrain3DChunk* chunk);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */


NYA_Error nya_terrain3d_create(NYA_Arena* arena, NYA_Terrain3DOptions options, OUT NYA_Terrain3D** out_terrain) {
    if (arena == nullptr || out_terrain == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no arena or no out pointer");

    // zero means unset for every field.
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

    // allocated once: the grid never changes size, and an arena would grow on every reseed.
    terrain->heights = nya_arena_alloc(arena, (u64)terrain->verts * (u64)terrain->verts * sizeof(f32));

    if (options.chunked) {
        /* Rounded up, so a resolution that is not a whole number of chunks is covered by clipping the last chunk. */
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

                    // out of range, so the first update builds every chunk.
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

    // the grid is rewritten in place; an arena does not hand memory back.

    // immediate: this runs from on_create and a key press, never inside entity iteration, and the body must exist
    // on return.
    if (nya_entity_is_valid(terrain->entity)) nya_entity_despawn(terrain->entity);

    terrain->entity = NYA_ENTITY_HANDLE_NONE;
    terrain->seed   = seed;

    // the RNG seed is an uppercase hex string of at most 64 digits, left padded, not a label.
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

            /* Two terms: free noise in the middle, a rim rising at the edge. */
            f32 height = nya_noise_fbm2(&noise, x * terrain->options.frequency, z * terrain->options.frequency, params);

            /* The distance to the nearest edge, not to the centre. */
            f32 radial = nya_max(fabsf(x), fabsf(z)) / half;

            /*
             * Smoothstep, so the rim eases out of the flat middle without a crease. Written here; the engine has no
             * smoothstep and this is its only user.
             */
            f32 ramp = nya_clamp((radial - terrain->options.rim_start) / (1.0F - terrain->options.rim_start), 0.0F, 1.0F);
            f32 rim  = ramp * ramp * (3.0F - (2.0F * ramp));

            // the noise fades out as the rim comes up, so the lip is clean.
            f32 value = ((height * (1.0F - rim)) + (rim * terrain->options.rim_height)) * terrain->options.amplitude;

            terrain->heights[(j * terrain->verts) + i] = value;

            terrain->min_height = nya_min(terrain->min_height, value);
            terrain->max_height = nya_max(terrain->max_height, value);
        }
    }

    /*
     * The collider is the height grid as a Box3D heightfield. It finds the cell under a point by arithmetic
     * instead of descending a triangle BVH; as a mesh, b3SolveContacts_Mesh was 4.3% of a release profile.
     */
    /*
     * At the grid's near corner: Box3D lays a heightfield out toward +x and +z from the body, while the drawn
     * surface is centred on the origin. Nothing renders from this entity's position.
     */
    terrain->entity = nya_entity_spawn(
        .name     = "terrain3d",
        .type     = terrain->options.entity_type,
        .position = { -half, 0.0F, -half },
        .state    = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE | NYA_ENTITY_STATE_STATIC
    );

    nya_assert(nya_entity_is_valid(terrain->entity), "Failed to spawn the 3D terrain entity.");

    // row major with x fastest, as terrain->heights and Box3D both index: (z * countX) + x.
    b8 attached = nya_physics3d_body_attach(
        terrain->entity,
        .type             = NYA_PHYSICS_BODY_STATIC,
        .shape            = NYA_PHYSICS3D_SHAPE_HEIGHTFIELD,
        .heights          = terrain->heights,
        .height_count_x   = terrain->verts,
        .height_count_z   = terrain->verts,
        .height_cell_size = { terrain->cell, terrain->cell },
        .friction         = terrain->options.friction
    );

    if (!attached) {
        // not fatal but logged: the scene draws, and things fall through it.
        nya_log_error("The 3D terrain has no collider; anything dropped on it will fall through.");
    }

    /*
     * The draw geometry, built once and kept by the renderer. Skipped for a chunked terrain, which draws its
     * chunks instead.
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

                /* Two triangles, wound and coloured like the immediate version. */
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

                    draw_vertices[emitted++] = nya_vertex3d(a, color, normal, f32x2_zero);
                    draw_vertices[emitted++] = nya_vertex3d(b, color, normal, f32x2_zero);
                    draw_vertices[emitted++] = nya_vertex3d(c, color, normal, f32x2_zero);
                }
            }
        }

        b8 registered = nya_render3d_mesh_register(window, NYA_TERRAIN3D_MESH, draw_vertices, emitted);

        nya_arena_free(nya_arena_temp, draw_vertices, draw_bytes);

        // not fatal: a handle naming nothing draws nothing, and the collider still simulates.
        if (!registered) nya_log_error("The 3D terrain has no drawable geometry; the scene will show a hole.");
    }


    /* The chunk bounds, now that there are heights. */
    for (u32 index = 0; index < terrain->chunk_count; index++) {
        _nya_terrain3d_chunk_bounds(terrain, &terrain->chunks[index]);

        // unbuilt, so a regeneration rebuilds every chunk against the new surface.
        terrain->chunks[index].lod = NYA_TERRAIN3D_LOD_LEVELS;
    }

    nya_log_info("3D terrain generated from seed %llu (%ux%u heightfield, height %.2f to %.2f).", (unsigned long long)seed,
                 terrain->verts, terrain->verts, (f64)terrain->min_height, (f64)terrain->max_height);
}

void nya_terrain3d_release(NYA_Terrain3D* terrain, NYA_Window* window) {

    // the geometry goes with the scene; a registered mesh has no asset to unload it.
    nya_render3d_mesh_release(window, NYA_TERRAIN3D_MESH);

    // and every chunk's, marked unbuilt so a regenerated terrain rebuilds them.
    for (u32 index = 0; index < terrain->chunk_count; index++) {
        nya_render3d_mesh_release(window, terrain->chunks[index].handle);
        terrain->chunks[index].lod = NYA_TERRAIN3D_LOD_LEVELS;
    }

    // deferred: on_destroy can run inside the layer stack's iteration. despawning takes the physics body.
    if (nya_entity_is_valid(terrain->entity)) nya_entity_despawn_deferred(terrain->entity);

    /* The height grid is not released, and the pointer is kept. */
    terrain->entity     = NYA_ENTITY_HANDLE_NONE;
    terrain->seed       = 0;
    terrain->min_height = 0.0F;
    terrain->max_height = 0.0F;
}

f32 nya_terrain3d_height_at(const NYA_Terrain3D* terrain, f32 x, f32 z) {

    if (terrain->heights == nullptr) return 0.0F;

    f32 half = terrain->options.extent * 0.5F;

    // cells from the low corner. clamped: outside the terrain the nearest edge height is what a caller placing
    // something wants.
    f32 grid_x = nya_clamp((x + half) / terrain->cell, 0.0F, (f32)terrain->resolution);
    f32 grid_z = nya_clamp((z + half) / terrain->cell, 0.0F, (f32)terrain->resolution);

    u32 i = (u32)grid_x;
    u32 j = (u32)grid_z;

    // the far edge is the last sample; there is no cell beyond it.
    if (i >= terrain->resolution) i = terrain->resolution - 1;
    if (j >= terrain->resolution) j = terrain->resolution - 1;

    f32 fraction_x = grid_x - (f32)i;
    f32 fraction_z = grid_z - (f32)j;

    /* Bilinear, which is close to the surface but not exact. */
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
 * How far a chunk's skirt hangs below its edge. Bounded by the terrain's height range, since two levels can
 * never disagree at a border by more than the whole relief.
 * */
NYA_INTERNAL f32 _nya_terrain3d_skirt_depth(const NYA_Terrain3D* terrain) {
    if (terrain->options.skirt_depth > 0.0F) return terrain->options.skirt_depth;

    f32 relief = terrain->max_height - terrain->min_height;

    // one cell as a floor: even a flat surface needs a skirt against floating point hairlines.
    return nya_max(relief, terrain->cell);
}

/**
 * A chunk's bounding sphere, from the height grid over its footprint. Computed before any geometry is built,
 * because the LOD choice measures distance to it; zeroed bounds made the first choice wrong and every chunk
 * rebuilt twice.
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

    // plus the skirt below the surface.
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

    out[(*count)++] = nya_vertex3d(a, color, normal, f32x2_zero);
    out[(*count)++] = nya_vertex3d(b, color, normal, f32x2_zero);
    out[(*count)++] = nya_vertex3d(c, color, normal, f32x2_zero);
}

/** Builds one chunk's geometry at `lod` and registers it, replacing what was there. */
NYA_INTERNAL void _nya_terrain3d_chunk_build(NYA_Terrain3D* terrain, NYA_Window* window, NYA_Terrain3DChunk* chunk, u32 lod) {
    u32 stride = 1U << lod;

    // clipped at the terrain's edge.
    u32 cells_x = nya_min((u32)NYA_TERRAIN3D_CHUNK_CELLS, terrain->resolution - chunk->cell_x);
    u32 cells_z = nya_min((u32)NYA_TERRAIN3D_CHUNK_CELLS, terrain->resolution - chunk->cell_z);

    // a stride coarser than what is left would emit nothing, a hole.
    while (stride > 1 && (cells_x % stride != 0 || cells_z % stride != 0)) stride >>= 1;

    u32 steps_x = cells_x / stride;
    u32 steps_z = cells_z / stride;

    if (steps_x == 0 || steps_z == 0) return;

    // surface plus a skirt down each side, six vertices per quad since the mesh is flat shaded.
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

            // the unchunked path's cell index, so shade jitter is identical and changing level keeps the colours.
            u32 cell = (j * terrain->resolution) + i;

            _nya_terrain3d_emit_triangle(terrain, vertices, &emitted, corner_a, corner_c, corner_b, cell, 0);
            _nya_terrain3d_emit_triangle(terrain, vertices, &emitted, corner_b, corner_c, corner_d, cell, 1);
        }
    }

    /* The skirt: a vertical flange from every edge vertex. */
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

    /* Banded and doubling, not continuous in distance. */
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

    // doubling, as nya_terrain3d_lod_for_distance: level 1 at the first distance, level 2 at twice it.
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
         * Horizontal distance to the chunk's sphere, not its centre. Measured to the centre, a chunk the camera
         * stands in reads as half a chunk away and coarsens, which strips detail off small scenes.
         */
        f32 dx = chunk->center.x - viewer.x;
        f32 dz = chunk->center.z - viewer.z;

        f32 distance = sqrtf((dx * dx) + (dz * dz)) - chunk->radius;

        if (distance < 0.0F) distance = 0.0F;

        u32 wanted = nya_terrain3d_lod_for_distance(terrain, distance);

        /* Hysteresis only for a chunk that has geometry; an unbuilt one takes the plain answer. */
        if (chunk->lod < NYA_TERRAIN3D_LOD_LEVELS && wanted != chunk->lod) {
            if (wanted > chunk->lod) {
                // coarsening: past the boundary plus the margin.
                f32 boundary = nya_terrain3d_lod_boundary(terrain, chunk->lod + 1);

                if (distance < boundary * (1.0F + NYA_TERRAIN3D_LOD_HYSTERESIS)) wanted = chunk->lod;
            } else {
                // refining: back under the boundary minus the margin.
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
        /* One draw per chunk with its own bounds, so the renderer culls per chunk. */
        for (u32 index = 0; index < terrain->chunk_count; index++) {
            const NYA_Terrain3DChunk* chunk = &terrain->chunks[index];
            if (chunk->lod >= NYA_TERRAIN3D_LOD_LEVELS) continue;

            nya_render3d_mesh(window, chunk->handle, f32x3_zero, (f32x3){ 1.0F, 1.0F, 1.0F }, nya_quaternion_identity, NYA_COLOR_WHITE);
        }

        return;
    }

    /* One instanced draw of geometry uploaded at generation. */
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

    // a perfectly flat seed would divide by zero; everything is then the lowest band.
    f32 unit = range > NYA_EPSILON ? (height - terrain->min_height) / range : 0.0F;

    NYA_Color color = terrain->options.color_low;

    if (unit >= terrain->options.band_peak) {
        color = terrain->options.color_peak;
    } else if (unit >= terrain->options.band_high) {
        color = terrain->options.color_high;
    } else if (unit >= terrain->options.band_mid) {
        color = terrain->options.color_mid;
    }

    /* A per-triangle nudge on top of the band. */
    f32 jitter = nya_ihash2((s32)cell, (s32)half, _NYA_TERRAIN3D_SHADE_SEED) * terrain->options.shade_jitter;

    return (NYA_Color){
        nya_clamp(color.r + jitter, 0.0F, 1.0F),
        nya_clamp(color.g + jitter, 0.0F, 1.0F),
        nya_clamp(color.b + jitter, 0.0F, 1.0F),
        color.a,
    };
}

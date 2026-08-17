#include "gnyame/gnyame.h"

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
NYA_INTERNAL f32x3 _gny_terrain3d_corner(const GNY_Terrain3D* terrain, u32 i, u32 j);

/** The flat colour for one triangle, from the height of its centre. */
NYA_INTERNAL NYA_Color _gny_terrain3d_shade(const GNY_Terrain3D* terrain, f32 height, u32 cell, u32 half);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

GNY_Terrain3D* gny_terrain3d(void) {
    // On the world rather than in a static, so it survives a hot reload — the same reason the scene it
    // belongs to keeps its state there. See layers.h.
    return &gny_world()->cube3d.terrain;
}

void gny_terrain3d_generate(NYA_Window* window, NYA_Arena* arena, u64 seed) {
    GNY_Terrain3D* terrain = gny_terrain3d();

    nya_assert(arena != nullptr, "gny_terrain3d_generate needs an allocator for the height grid.");

    /*
     * Allocated once and reused, because the grid never changes size.
     *
     * Regenerating with a new seed rewrites the samples in place. Allocating again per generation would
     * grow the world arena by a hundred kilobytes on every press of R, and an arena does not hand memory
     * back until it is freed as a whole.
     */
    if (terrain->heights == nullptr) {
        terrain->heights = nya_arena_alloc(arena, (u64)(GNY_TERRAIN3D_VERTS * GNY_TERRAIN3D_VERTS) * sizeof(f32));
    }

    // Immediate rather than deferred: this runs from the layer's on_create and from a key press, never
    // from inside an entity iteration, and the new body has to exist before this returns.
    if (nya_entity_is_valid(terrain->entity)) nya_entity_despawn(terrain->entity);

    terrain->entity = NYA_ENTITY_HANDLE_NONE;
    terrain->seed   = seed;

    // The RNG takes its seed as an uppercase hex string of at most 64 digits, left padded — not an
    // arbitrary label. The same trap gny_terrain_generate documents for the 2D world.
    char seed_text[17];
    (void)snprintf(seed_text, sizeof(seed_text), "%016llX", (unsigned long long)seed);

    NYA_RNG   rng   = nya_rng_create(.seed = seed_text);
    NYA_Noise noise = nya_noise_create(&rng);

    NYA_NoiseParams params = {
        .octaves    = GNY_TERRAIN3D_OCTAVES,
        .lacunarity = GNY_TERRAIN3D_LACUNARITY,
        .gain       = GNY_TERRAIN3D_GAIN,
    };

    f32 half = GNY_TERRAIN3D_EXTENT * 0.5F;

    terrain->min_height = FLT_MAX;
    terrain->max_height = -FLT_MAX;

    for (u32 j = 0; j < GNY_TERRAIN3D_VERTS; j++) {
        for (u32 i = 0; i < GNY_TERRAIN3D_VERTS; i++) {
            f32 x = -half + ((f32)i * GNY_TERRAIN3D_CELL);
            f32 z = -half + ((f32)j * GNY_TERRAIN3D_CELL);

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
            f32 height = nya_noise_fbm2(&noise, x * GNY_TERRAIN3D_FREQUENCY, z * GNY_TERRAIN3D_FREQUENCY, params);

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
            f32 ramp = nya_clamp((radial - GNY_TERRAIN3D_RIM_START) / (1.0F - GNY_TERRAIN3D_RIM_START), 0.0F, 1.0F);
            f32 rim  = ramp * ramp * (3.0F - (2.0F * ramp));

            // The noise fades out exactly as the rim comes up, so the two never fight over the same
            // ground and the ring is a clean lip rather than a lumpy one.
            f32 value = ((height * (1.0F - rim)) + (rim * GNY_TERRAIN3D_RIM_HEIGHT)) * GNY_TERRAIN3D_AMPLITUDE;

            terrain->heights[(j * GNY_TERRAIN3D_VERTS) + i] = value;

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
    u32 vertex_count = GNY_TERRAIN3D_VERTS * GNY_TERRAIN3D_VERTS;
    u32 index_count  = GNY_TERRAIN3D_RES * GNY_TERRAIN3D_RES * 6;

    u64 vertex_bytes = (u64)vertex_count * sizeof(f32x3);
    u64 index_bytes  = (u64)index_count * sizeof(u32);

    f32x3* vertices = nya_arena_alloc(nya_arena_temp, vertex_bytes);
    u32*   indices  = nya_arena_alloc(nya_arena_temp, index_bytes);

    for (u32 j = 0; j < GNY_TERRAIN3D_VERTS; j++) {
        for (u32 i = 0; i < GNY_TERRAIN3D_VERTS; i++) {
            vertices[(j * GNY_TERRAIN3D_VERTS) + i] = _gny_terrain3d_corner(terrain, i, j);
        }
    }

    u32 written = 0;

    for (u32 j = 0; j < GNY_TERRAIN3D_RES; j++) {
        for (u32 i = 0; i < GNY_TERRAIN3D_RES; i++) {
            u32 a = (j * GNY_TERRAIN3D_VERTS) + i;
            u32 b = a + 1;
            u32 c = a + GNY_TERRAIN3D_VERTS;
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
        .type  = GNY_ENTITY_TERRAIN,
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
        .friction     = GNY_TERRAIN3D_FRICTION
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
     */
    u32 draw_vertex_count = GNY_TERRAIN3D_RES * GNY_TERRAIN3D_RES * 6;

    u64 draw_bytes = (u64)draw_vertex_count * sizeof(NYA_Vertex3D);

    NYA_Vertex3D* draw_vertices = nya_arena_alloc(nya_arena_temp, draw_bytes);

    u32 emitted = 0;

    for (u32 j = 0; j < GNY_TERRAIN3D_RES; j++) {
        for (u32 i = 0; i < GNY_TERRAIN3D_RES; i++) {
            f32x3 corner_a = _gny_terrain3d_corner(terrain, i, j);
            f32x3 corner_b = _gny_terrain3d_corner(terrain, i + 1, j);
            f32x3 corner_c = _gny_terrain3d_corner(terrain, i, j + 1);
            f32x3 corner_d = _gny_terrain3d_corner(terrain, i + 1, j + 1);

            u32 cell = (j * GNY_TERRAIN3D_RES) + i;

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

                NYA_Color color = _gny_terrain3d_shade(terrain, (a.y + b.y + c.y) / 3.0F, cell, half);

                draw_vertices[emitted++] = (NYA_Vertex3D){ .position = a, .color = color, .normals = normal };
                draw_vertices[emitted++] = (NYA_Vertex3D){ .position = b, .color = color, .normals = normal };
                draw_vertices[emitted++] = (NYA_Vertex3D){ .position = c, .color = color, .normals = normal };
            }
        }
    }

    b8 registered = nya_render3d_mesh_register(window, GNY_TERRAIN3D_MESH, draw_vertices, emitted);

    nya_arena_free(nya_arena_temp, draw_vertices, draw_bytes);

    // Not fatal either: gny_terrain3d_draw asks the renderer to draw the handle, and a handle that names
    // nothing draws nothing. A collider with no surface is odd to look at and still simulates.
    if (!registered) nya_log_error("The 3D terrain has no drawable geometry; the scene will show a hole.");

    nya_info("3D terrain generated from seed %llu (%u triangles, height %.2f to %.2f).", (unsigned long long)seed, index_count / 3,
             (f64)terrain->min_height, (f64)terrain->max_height);
}

void gny_terrain3d_destroy(NYA_Window* window) {
    GNY_Terrain3D* terrain = gny_terrain3d();

    // The geometry goes with the scene. Nothing else would release it: a registered mesh has no asset
    // behind it whose unload would take it, only the window's teardown as a backstop.
    nya_render3d_mesh_release(window, GNY_TERRAIN3D_MESH);

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

f32 gny_terrain3d_height_at(f32 x, f32 z) {
    const GNY_Terrain3D* terrain = gny_terrain3d();

    if (terrain->heights == nullptr) return 0.0F;

    f32 half = GNY_TERRAIN3D_EXTENT * 0.5F;

    // In grid cells from the low corner. Clamped rather than wrapped: outside the terrain the nearest
    // edge height is the useful answer, and a caller asking is placing something, not sampling a field.
    f32 grid_x = nya_clamp((x + half) / GNY_TERRAIN3D_CELL, 0.0F, (f32)GNY_TERRAIN3D_RES);
    f32 grid_z = nya_clamp((z + half) / GNY_TERRAIN3D_CELL, 0.0F, (f32)GNY_TERRAIN3D_RES);

    u32 i = (u32)grid_x;
    u32 j = (u32)grid_z;

    // The far edge lands exactly on the last sample, where the cell to its right does not exist.
    if (i >= GNY_TERRAIN3D_RES) i = GNY_TERRAIN3D_RES - 1;
    if (j >= GNY_TERRAIN3D_RES) j = GNY_TERRAIN3D_RES - 1;

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
    f32 h00 = terrain->heights[(j * GNY_TERRAIN3D_VERTS) + i];
    f32 h10 = terrain->heights[(j * GNY_TERRAIN3D_VERTS) + i + 1];
    f32 h01 = terrain->heights[((j + 1) * GNY_TERRAIN3D_VERTS) + i];
    f32 h11 = terrain->heights[((j + 1) * GNY_TERRAIN3D_VERTS) + i + 1];

    return nya_lerp(nya_lerp(h00, h10, fraction_x), nya_lerp(h01, h11, fraction_x), fraction_z);
}

void gny_terrain3d_draw(NYA_Window* window) {
    nya_perf_time_this_scope("gny_terrain3d_draw");

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
    nya_render3d_mesh(window, GNY_TERRAIN3D_MESH, f32x3_zero, (f32x3){ 1.0F, 1.0F, 1.0F }, nya_quaternion_identity, NYA_COLOR_WHITE);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32x3 _gny_terrain3d_corner(const GNY_Terrain3D* terrain, u32 i, u32 j) {
    f32 half = GNY_TERRAIN3D_EXTENT * 0.5F;

    return (f32x3){
        -half + ((f32)i * GNY_TERRAIN3D_CELL),
        terrain->heights[(j * GNY_TERRAIN3D_VERTS) + i],
        -half + ((f32)j * GNY_TERRAIN3D_CELL),
    };
}

NYA_Color _gny_terrain3d_shade(const GNY_Terrain3D* terrain, f32 height, u32 cell, u32 half) {
    f32 range = terrain->max_height - terrain->min_height;

    // A perfectly flat seed is possible in principle and divides by nothing. Everything is then in the
    // lowest band, which is the right answer for a surface with no relief.
    f32 unit = range > NYA_EPSILON ? (height - terrain->min_height) / range : 0.0F;

    NYA_Color color = GNY_TERRAIN3D_COLOR_LOW;

    if (unit >= GNY_TERRAIN3D_BAND_PEAK) {
        color = GNY_TERRAIN3D_COLOR_PEAK;
    } else if (unit >= GNY_TERRAIN3D_BAND_HIGH) {
        color = GNY_TERRAIN3D_COLOR_HIGH;
    } else if (unit >= GNY_TERRAIN3D_BAND_MID) {
        color = GNY_TERRAIN3D_COLOR_MID;
    }

    /*
     * A per-triangle nudge on top of the band.
     *
     * Without it a band is a flat sheet of one colour and the facets vanish into it — which defeats the
     * whole reason for unshared vertices. The engine's integer hash rather than a multiplicative one
     * written here; see the note in _gny_sky_random for what a hand-rolled version does to a sanitized
     * build.
     *
     * Keyed on the cell *and* which half of it, so the two triangles of a cell differ from each other.
     * Keyed on the index rather than the position, so it is stable while the camera moves and identical
     * between the shadow pass and the camera pass — a jitter that differed between them would put noise
     * in the shadows.
     */
    f32 jitter = nya_ihash2((s32)cell, (s32)half, GNY_TERRAIN3D_SEED) * GNY_TERRAIN3D_SHADE_JITTER;

    return (NYA_Color){
        nya_clamp(color.r + jitter, 0.0F, 1.0F),
        nya_clamp(color.g + jitter, 0.0F, 1.0F),
        nya_clamp(color.b + jitter, 0.0F, 1.0F),
        color.a,
    };
}

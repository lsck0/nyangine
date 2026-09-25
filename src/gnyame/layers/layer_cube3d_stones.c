/**
 * @file layer_cube3d_stones.c
 *
 * The ring of standing stones around the basin, and the three renderer features they exist to drive.
 *
 * The slabs are built here rather than loaded, because what the scene needed was geometry it could describe at
 * three levels of detail and vouch for the inside of:
 *
 * - `nya_render3d_mesh_register` takes the three levels, and `nya_render3d_lod_register` chains them, so
 *   `nya_render3d_mesh` picks one by distance. Nothing in the game had a detail chain before.
 * - Every slab is convex and opaque, so its far face is a sound occluder: everything behind that face is behind
 *   solid stone. `nya_occlusion_quad` takes it and `nya_render3d_occlusion` hands the buffer to the camera pass,
 *   which is the only caller of the occlusion buffer anywhere.
 * - The bodies are static and on `GNY_LAYER_STONE`, so the pick ray can look past them, and the audio trace
 *   finds them: standing between the ear and the bonfire drops the fire the way the terrain's rim does.
 * */
#include "gnyame/gnyame.h"

// What nya_watch() below expands to, written by src/nyangine-build/pp/watch.c from the @watch annotation.
#include "genyarated/watches/gnyame_layers_layer_cube3d_stones_c.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The slab's cross-section in units of half its width and half its depth, walking the same way round as
 * nya_render3d_sphere's segments: by increasing atan2(z, x). That direction is what makes the wall triangles
 * below come out facing away from the middle, which is the winding SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE keeps.
 * */
NYA_INTERNAL const f32x2 _GNY_CUBE3D_STONE_SECTION_4[4] = {
    { 1.0F, 1.0F },
    { -1.0F, 1.0F },
    { -1.0F, -1.0F },
    { 1.0F, -1.0F },
};

/** The same rectangle with every corner cut off, so the eight point section keeps the four flat faces. */
NYA_INTERNAL const f32x2 _GNY_CUBE3D_STONE_SECTION_8[8] = {
    { 1.0F, 1.0F - GNY_CUBE3D_STONE_CHAMFER },
    { 1.0F - GNY_CUBE3D_STONE_CHAMFER, 1.0F },
    { -(1.0F - GNY_CUBE3D_STONE_CHAMFER), 1.0F },
    { -1.0F, 1.0F - GNY_CUBE3D_STONE_CHAMFER },
    { -1.0F, -(1.0F - GNY_CUBE3D_STONE_CHAMFER) },
    { -(1.0F - GNY_CUBE3D_STONE_CHAMFER), -1.0F },
    { 1.0F - GNY_CUBE3D_STONE_CHAMFER, -1.0F },
    { 1.0F, -(1.0F - GNY_CUBE3D_STONE_CHAMFER) },
};

/** One detail level: the mesh it is registered as, how it is built, and how far out it remains the right one. */
typedef struct {
    NYA_ConstCString handle;

    /** Points around the section, and bands up the height. */
    u32 sides;
    u32 segments;

    f32 max_distance;
} _GNY_Cube3DStoneLevel;

NYA_INTERNAL const _GNY_Cube3DStoneLevel _GNY_CUBE3D_STONE_LEVELS[] = {
    { GNY_CUBE3D_STONE_MESH_NEAR, 8, 3, GNY_CUBE3D_STONE_LOD_NEAR },
    { GNY_CUBE3D_STONE_MESH_MID, 8, 1, GNY_CUBE3D_STONE_LOD_MID },
    { GNY_CUBE3D_STONE_MESH_FAR, 4, 1, GNY_CUBE3D_STONE_LOD_FAR },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * _gny_cube3d_scene is layer_cube3d.c's, and gnyame.c includes that file before this one, so it is already
 * declared here. Not redeclared: clang-tidy counts a second declaration in one translation unit as noise.
 */

/** How bright the `index`-th face of the `band`-th band is, against one. Hashed, so every level agrees. */
NYA_INTERNAL NYA_Color _gny_cube3d_stone_shade(u32 index, u32 band);

/** Two triangles for the quad `a b c d`, with the flat normal the winding gives. Writes six vertices. */
NYA_INTERNAL u32 _gny_cube3d_stone_face(NYA_Vertex3D* out, f32x3 a, f32x3 b, f32x3 c, f32x3 d, NYA_Color color);

/**
 * Builds one level into `out`, which must hold GNY_CUBE3D_STONE_VERTICES_MAX. The slab spans [-1, 1] on x and
 * z and [0, 1] on y, so a draw scales it by half its width, its height and half its depth.
 * */
NYA_INTERNAL u32 _gny_cube3d_stone_build(NYA_Vertex3D* out, u32 sides, u32 segments);

/** Where the `index`-th slab stands, how big it is and how far it is turned. Hashed, so `r` replays the ring. */
NYA_INTERNAL GNY_Cube3DStone _gny_cube3d_stone_at(u32 index);

/** The middle of the slab's body, which is half its height above where it meets the ground. */
NYA_INTERNAL f32x3 _gny_cube3d_stone_center(const GNY_Cube3DStone* stone);

/** The slab's turn about y, as the draw and the body both take it. */
NYA_INTERNAL NYA_Quaternion _gny_cube3d_stone_rotation(const GNY_Cube3DStone* stone);

/**
 * The four world corners of the flat face facing away from `eye`, in `out`. That face is the occluder: it is
 * inside the solid, so nothing between it and the camera is wrongly hidden, and it is a whole face, so it
 * covers as much as a convex quad honestly can.
 * */
NYA_INTERNAL void _gny_cube3d_stone_far_face(const GNY_Cube3DStone* stone, f32x3 eye, OUT f32x3* out);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_cube3d_stones_create(NYA_Window* window) {
    nya_assert(window != nullptr);

    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    gny_layer_cube3d_stones_register(window);

    // the ceiling is registered once for the run: the counter lives on the world, which outlives every visit,
    // and the registry does not dedupe.
    static b8 ceiling_registered = false;

    if (!ceiling_registered) {
        nya_ceiling_register("cube3d_stones", GNY_CUBE3D_STONE_COUNT, &scene->stone_count);
        ceiling_registered = true;
    }

    scene->stone_count = 0;

    for (u32 i = 0; i < GNY_CUBE3D_STONE_COUNT; i++) {
        GNY_Cube3DStone stone = _gny_cube3d_stone_at(i);

        stone.entity = nya_entity_spawn(
            .name     = "standing stone",
            .type     = GNY_ENTITY_CUBE3D,
            .position = _gny_cube3d_stone_center(&stone),
            .rotation = _gny_cube3d_stone_rotation(&stone),
            .state    = NYA_ENTITY_STATE_ACTIVE
        );

        // a full entity table is an operating error: the rest of the ring still stands.
        if (!nya_entity_is_valid(stone.entity)) continue;

        /*
         * The body is the block inside the chamfer, at the section's mid height width, so a crate rests against
         * the face it looks like it is resting against rather than against the circumscribed box.
         */
        f32 shrink = (1.0F + GNY_CUBE3D_STONE_TAPER) * 0.5F;

        b8 attached = nya_physics3d_body_attach(
            stone.entity,
            .type          = NYA_PHYSICS_BODY_STATIC,
            .shape         = NYA_PHYSICS3D_SHAPE_BOX,
            .size          = { stone.size.x * (1.0F - GNY_CUBE3D_STONE_CHAMFER) * shrink, stone.size.y, stone.size.z * shrink },
            .friction      = GNY_CUBE3D_STONE_FRICTION,
            .restitution   = GNY_CUBE3D_STONE_RESTITUTION,
            // scenery: everything meets it, and the pick ray asks for GNY_LAYER_PROP instead.
            .layers        = nya_physics_layer(GNY_LAYER_STONE),
            .collides_with = NYA_PHYSICS_LAYER_ALL
        );

        if (!attached) {
            nya_entity_despawn(stone.entity);
            continue;
        }

        scene->stones[scene->stone_count++] = stone;
    }
}

void gny_layer_cube3d_stones_destroy(NYA_Window* window) {
    nya_assert(window != nullptr);

    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    // deferred, since this runs from the layer stack's iteration, like the pile's.
    for (u32 i = 0; i < scene->stone_count; i++) nya_entity_despawn_deferred(scene->stones[i].entity);

    scene->stone_count = 0;

    nya_render3d_lod_unregister(GNY_CUBE3D_STONE_MESH_NEAR);

    for (u64 i = 0; i < nya_carray_length(_GNY_CUBE3D_STONE_LEVELS); i++) {
        nya_render3d_mesh_release(window, _GNY_CUBE3D_STONE_LEVELS[i].handle);
    }
}

void gny_layer_cube3d_stones_register(NYA_Window* window) {
    nya_assert(window != nullptr);

    // the registry is the single source of truth for whether this ran: a code reload zeroes it while the
    // scene's own state survives on the world, so a flag there would lie.
    if (nya_render3d_lod_registered(GNY_CUBE3D_STONE_MESH_NEAR)) return;

    NYA_Render3DLodLevel chain[nya_carray_length(_GNY_CUBE3D_STONE_LEVELS)];

    for (u64 i = 0; i < nya_carray_length(_GNY_CUBE3D_STONE_LEVELS); i++) {
        const _GNY_Cube3DStoneLevel* level = &_GNY_CUBE3D_STONE_LEVELS[i];

        // on the stack: the biggest level is under 8 KiB and the renderer copies it to the GPU here.
        NYA_Vertex3D vertices[GNY_CUBE3D_STONE_VERTICES_MAX];

        u32 count = _gny_cube3d_stone_build(vertices, level->sides, level->segments);

        if (!nya_render3d_mesh_register(window, level->handle, vertices, count)) {
            nya_log_error("Could not register '%s'; the stone ring will not be drawn.", level->handle);
            return;
        }

        chain[i] = (NYA_Render3DLodLevel){ .handle = level->handle, .max_distance = level->max_distance };
    }

    if (!nya_render3d_lod_register(GNY_CUBE3D_STONE_MESH_NEAR, chain, (u32)nya_carray_length(chain))) {
        // not fatal: every draw then uses the finest level, which is what no chain means.
        nya_log_warn("Could not chain the standing stones' detail levels; they will all draw at full detail.");
    }
}

void gny_layer_cube3d_stones_place(void) {
    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    /*
     * `r` regenerates the ground under the ring, so every slab is put back on the new height. Teleported rather
     * than respawned, so the handles and the bodies stay as they were.
     */
    for (u32 i = 0; i < scene->stone_count; i++) {
        NYA_EntityHandle entity = scene->stones[i].entity;

        scene->stones[i]        = _gny_cube3d_stone_at(i);
        scene->stones[i].entity = entity;

        NYA_Entity* body = nya_entity_get(entity);
        if (body == nullptr) continue;

        nya_physics3d_teleport(body, _gny_cube3d_stone_center(&scene->stones[i]), _gny_cube3d_stone_rotation(&scene->stones[i]));
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_cube3d_stones_occlude(NYA_Window* window, f32x3 eye) {
    nya_assert(window != nullptr);

    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    // off is free: no buffer is cleared, no face is rasterized, and the batch keeps the null it began with.
    if (scene->occlusion == nullptr || !nya_render_feature_enabled(window, NYA_RENDER_FEATURE_OCCLUSION_CULLING)) return;

    nya_occlusion_begin(scene->occlusion, nya_render3d_view_projection(window));

    for (u32 i = 0; i < scene->stone_count; i++) {
        /*
         * A slab past the chain's last rung is not drawn, and an occluder for geometry that is not there would
         * hide what is behind a gap. The camera cannot get that far from the ring today; the guard is here so
         * that stops being a thing anyone has to know when a distance changes.
         */
        f32 distance = nya_vector_length(scene->stones[i].base - eye);

        if (nya_render3d_lod_select(GNY_CUBE3D_STONE_MESH_NEAR, distance) == nullptr) continue;

        f32x3 face[4];
        _gny_cube3d_stone_far_face(&scene->stones[i], eye, face);

        (void)nya_occlusion_quad(scene->occlusion, face[0], face[1], face[2], face[3]);
    }

    nya_render3d_occlusion(window, scene->occlusion);
}

void gny_layer_cube3d_stones_levels(f32x3 eye, OUT u32* out_nearest, OUT u32* out_farthest) {
    nya_assert(out_nearest != nullptr && out_farthest != nullptr);

    const GNY_Cube3DScene* scene = _gny_cube3d_scene();

    // no ring is no level, which the HUD shows as the last rung: nothing drawn.
    *out_nearest  = NYA_RENDER3D_LOD_LEVELS;
    *out_farthest = 0;

    for (u32 i = 0; i < scene->stone_count; i++) {
        // the same point the renderer measures from: the mesh's origin, which is where the slab stands.
        u32 level = nya_render3d_lod_level_at(GNY_CUBE3D_STONE_MESH_NEAR, nya_vector_length(scene->stones[i].base - eye));

        if (level < *out_nearest) *out_nearest = level;
        if (level > *out_farthest) *out_farthest = level;
    }

    nya_assert(*out_nearest <= *out_farthest || scene->stone_count == 0);
}

void gny_layer_cube3d_stones_draw(NYA_Window* window) {
    nya_assert(window != nullptr);

    const GNY_Cube3DScene* scene = _gny_cube3d_scene();

    // dry rock: no highlight to speak of, so the facets carry the shape.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.0F, .roughness = 0.95F });

    for (u32 i = 0; i < scene->stone_count; i++) {
        const GNY_Cube3DStone* stone = &scene->stones[i];

        // the unit slab spans [-1, 1] on x and z, so half the width and half the depth scale it.
        f32x3 scale = { stone->size.x * 0.5F, stone->size.y, stone->size.z * 0.5F };

        // the base, not the middle: the mesh stands on y = 0. nya_render3d_mesh picks the level by distance.
        nya_render3d_mesh(window, GNY_CUBE3D_STONE_MESH_NEAR, stone->base, scale, _gny_cube3d_stone_rotation(stone), GNY_CUBE3D_STONE_COLOR);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Color _gny_cube3d_stone_shade(u32 index, u32 band) {
    // hashed rather than random, so the three levels shade the same face the same way.
    f32 shade = 1.0F + (nya_ihash2((s32)index, (s32)band, GNY_CUBE3D_STONE_SEED) * GNY_CUBE3D_STONE_SHADE_JITTER);

    return (NYA_Color){ shade, shade, shade, 1.0F };
}

u32 _gny_cube3d_stone_face(NYA_Vertex3D* out, f32x3 a, f32x3 b, f32x3 c, f32x3 d, NYA_Color color) {
    nya_assert(out != nullptr);

    // flat, from the winding: the facets are the look, so no vertex is shared between two faces.
    f32x3 normal = nya_vector_normalize(nya_vector_cross(b - a, c - a));

    out[0] = nya_vertex3d(a, color, normal, f32x2_zero);
    out[1] = nya_vertex3d(b, color, normal, f32x2_zero);
    out[2] = nya_vertex3d(c, color, normal, f32x2_zero);
    out[3] = nya_vertex3d(a, color, normal, f32x2_zero);
    out[4] = nya_vertex3d(c, color, normal, f32x2_zero);
    out[5] = nya_vertex3d(d, color, normal, f32x2_zero);

    return 6;
}

// @watch
u32 _gny_cube3d_stone_build(NYA_Vertex3D* out, u32 sides, u32 segments) {
    nya_assert(out != nullptr);
    nya_assert(sides == 4 || sides == 8, "the section is a rectangle or a rectangle with its corners cut off");
    nya_assert(segments >= 1);

    const f32x2* section = sides == 4 ? _GNY_CUBE3D_STONE_SECTION_4 : _GNY_CUBE3D_STONE_SECTION_8;

    u32 at = 0;
    nya_watch(_gny_cube3d_stone_build);

    /*
     * The walls, band by band up the height. The section shrinks linearly toward the top, so however many bands
     * a level uses the silhouette is the same and only the facet count differs.
     */
    for (u32 band = 0; band < segments; band++) {
        f32 low  = (f32)band / (f32)segments;
        f32 high = (f32)(band + 1) / (f32)segments;

        f32 low_scale  = nya_lerp(1.0F, GNY_CUBE3D_STONE_TAPER, low);
        f32 high_scale = nya_lerp(1.0F, GNY_CUBE3D_STONE_TAPER, high);

        for (u32 i = 0; i < sides; i++) {
            f32x2 here = section[i];
            f32x2 next = section[(i + 1) % sides];

            f32x3 base_here = { here.x * low_scale, low, here.y * low_scale };
            f32x3 base_next = { next.x * low_scale, low, next.y * low_scale };
            f32x3 top_here  = { here.x * high_scale, high, here.y * high_scale };
            f32x3 top_next  = { next.x * high_scale, high, next.y * high_scale };

            at += _gny_cube3d_stone_face(out + at, base_here, top_here, top_next, base_next, _gny_cube3d_stone_shade(i, band));
        }
    }

    /*
     * Both caps, as fans. The bottom is buried by GNY_CUBE3D_STONE_SINK and never seen, but a closed solid is
     * what the shadow pass wants: it records far faces, and an open one casts nothing from below.
     */
    NYA_Color cap = _gny_cube3d_stone_shade(sides, segments);

    for (u32 i = 0; i < sides; i++) {
        f32x2 here = section[i];
        f32x2 next = section[(i + 1) % sides];

        f32x3 top_here = { here.x * GNY_CUBE3D_STONE_TAPER, 1.0F, here.y * GNY_CUBE3D_STONE_TAPER };
        f32x3 top_next = { next.x * GNY_CUBE3D_STONE_TAPER, 1.0F, next.y * GNY_CUBE3D_STONE_TAPER };

        // reversed against the walls' order, because the top is seen from the other side.
        out[at++] = nya_vertex3d((f32x3){ 0.0F, 1.0F, 0.0F }, cap, (f32x3){ 0.0F, 1.0F, 0.0F }, f32x2_zero);
        out[at++] = nya_vertex3d(top_next, cap, (f32x3){ 0.0F, 1.0F, 0.0F }, f32x2_zero);
        out[at++] = nya_vertex3d(top_here, cap, (f32x3){ 0.0F, 1.0F, 0.0F }, f32x2_zero);

        out[at++] = nya_vertex3d(f32x3_zero, cap, (f32x3){ 0.0F, -1.0F, 0.0F }, f32x2_zero);
        out[at++] = nya_vertex3d((f32x3){ here.x, 0.0F, here.y }, cap, (f32x3){ 0.0F, -1.0F, 0.0F }, f32x2_zero);
        out[at++] = nya_vertex3d((f32x3){ next.x, 0.0F, next.y }, cap, (f32x3){ 0.0F, -1.0F, 0.0F }, f32x2_zero);
    }

    // both sides printed: what a level emitted is only ever wrong against what it sized itself for.
    nya_assert_eq(at, (sides * segments * 6) + (sides * 6));
    nya_assert_le(at, (u32)GNY_CUBE3D_STONE_VERTICES_MAX);

    return at;
}

GNY_Cube3DStone _gny_cube3d_stone_at(u32 index) {
    nya_assert(index < GNY_CUBE3D_STONE_COUNT);

    // one hash channel per number, like the pile's placement: sharing a channel correlates two of them.
    f32 even  = (2.0F * (f32)M_PI * (f32)index) / (f32)GNY_CUBE3D_STONE_COUNT;
    f32 angle = even + (nya_ihash2((s32)index, 0, GNY_CUBE3D_STONE_SEED) * GNY_CUBE3D_STONE_ANGLE_JITTER);

    f32 radius = GNY_CUBE3D_STONE_RADIUS + (nya_ihash2((s32)index, 1, GNY_CUBE3D_STONE_SEED) * GNY_CUBE3D_STONE_RADIUS_JITTER);

    f32 x = cosf(angle) * radius;
    f32 z = sinf(angle) * radius;

    f32x2 widths  = GNY_CUBE3D_STONE_WIDTH;
    f32x2 heights = GNY_CUBE3D_STONE_HEIGHT;

    f32 width  = nya_lerp(widths.x, widths.y, (nya_ihash2((s32)index, 2, GNY_CUBE3D_STONE_SEED) * 0.5F) + 0.5F);
    f32 height = nya_lerp(heights.x, heights.y, (nya_ihash2((s32)index, 3, GNY_CUBE3D_STONE_SEED) * 0.5F) + 0.5F);

    /*
     * The slab is thin along its local z, so turning local z onto the radial direction puts its wide face across
     * the ring. Local z maps to (sin yaw, 0, cos yaw), and the radial direction is (cos angle, 0, sin angle), so
     * yaw is a quarter turn less the angle. The jitter keeps the ring from reading as a fence.
     */
    f32 yaw = ((f32)M_PI * 0.5F) - angle + (nya_ihash2((s32)index, 4, GNY_CUBE3D_STONE_SEED) * GNY_CUBE3D_STONE_YAW_JITTER);

    return (GNY_Cube3DStone){
        .base = { x, gny_terrain3d_height_at(x, z) - GNY_CUBE3D_STONE_SINK, z },
        .size = { width, height, width * GNY_CUBE3D_STONE_THICKNESS },
        .yaw  = yaw,
    };
}

f32x3 _gny_cube3d_stone_center(const GNY_Cube3DStone* stone) {
    nya_assert(stone != nullptr);

    return stone->base + (f32x3){ 0.0F, stone->size.y * 0.5F, 0.0F };
}

NYA_Quaternion _gny_cube3d_stone_rotation(const GNY_Cube3DStone* stone) {
    nya_assert(stone != nullptr);

    return nya_quaternion_from_axis_angle((f32x3){ 0.0F, 1.0F, 0.0F }, stone->yaw);
}

void _gny_cube3d_stone_far_face(const GNY_Cube3DStone* stone, f32x3 eye, OUT f32x3* out) {
    nya_assert(stone != nullptr && out != nullptr);

    NYA_Quaternion rotation = _gny_cube3d_stone_rotation(stone);

    f32x3 scale = { stone->size.x * 0.5F, stone->size.y, stone->size.z * 0.5F };

    /*
     * The two flat faces sit at local z = 1 and z = -1, spanning the section's width less the chamfer. Which of
     * them is the occluder is whichever is further from the camera; the near one would hide the stone's own
     * front half.
     */
    f32x3 toward = nya_quaternion_rotate(rotation, (f32x3){ 0.0F, 0.0F, 1.0F });

    f32 side = nya_vector_dot(toward, _gny_cube3d_stone_center(stone) - eye) > 0.0F ? 1.0F : -1.0F;

    const f32 flat = 1.0F - GNY_CUBE3D_STONE_CHAMFER;

    // bottom pair first, then the tapered top pair back the other way, so the quad is wound round its edge.
    f32x3 local[4] = {
        { -flat, 0.0F, side },
        { flat, 0.0F, side },
        { flat * GNY_CUBE3D_STONE_TAPER, 1.0F, side * GNY_CUBE3D_STONE_TAPER },
        { -flat * GNY_CUBE3D_STONE_TAPER, 1.0F, side * GNY_CUBE3D_STONE_TAPER },
    };

    for (u32 i = 0; i < 4; i++) out[i] = stone->base + nya_quaternion_rotate(rotation, local[i] * scale);
}

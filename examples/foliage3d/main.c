/**
 * @file examples/foliage3d/main.c
 *
 * Wind field + foliage sway. One analytic wind field (render_wind.h) drives grass, leaves and branches,
 * all through one shader (foliage.vert.hlsl) and one draw path (nya_render3d_foliage) — the three looks
 * differ only in the sway parameters NYA_Render3DFoliage carries.
 *
 * ```
 * ./build run example foliage3d
 * NYA_FOLIAGE_FRAMES=6 ./build run example foliage3d   # draw six frames and quit, for a headless run
 * ```
 *
 * Left/right arrows turn the wind, up/down change its strength, `g` toggles the gust, `tab` switches
 * scenes, `escape` quits.
 *
 * ## The shape of it
 *
 * Each plant is a mesh registered once with its base at y = 0 and a flexibility weight up its height in
 * the vertex colour's alpha (0 at the anchored base, 1 at the free tips). Every frame the wind field is
 * advanced once and sampled at each plant's position; the sampled vector plus the plant's style is handed
 * to nya_render3d_foliage, whose vertex stage bends the model-space geometry about its base before
 * view-projecting it. No compute pass and no per-frame vertex work on the CPU.
 *
 * ## Two scenes
 *
 * The default scene is a checker of the three looks, one draw call per plant. Press `tab` for the second:
 * a dense carpet of thousands of blades drawn in ONE instanced call through nya_render3d_grass, every blade
 * bent by the same wind and each swaying on its own phase (derived in the shader from its world position).
 * That is what makes a field dense — the cost is one draw, not one draw per blade.
 * */
#include "genyarated/assets.h"
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/* CONSTANTS */

#define WINDOW_TITLE  "nyangine — foliage"
#define WINDOW_WIDTH  1024
#define WINDOW_HEIGHT 720

#define LAYER_ID "foliage"

/** Registered mesh handles. Reserved names, not asset paths: these meshes are built here, not loaded. */
#define MESH_GRASS  "foliage/grass_tuft"
#define MESH_LEAVES "foliage/leaf_bush"
#define MESH_BRANCH "foliage/branch"
#define MESH_BLADE  "foliage/blade"

/** The field: a square of plants, this many on a side, this far apart. */
#define FIELD_SIDE    9
#define FIELD_SPACING 1.6F

/**
 * The instanced-grass scene: a dense carpet of single blades, this many on a side, drawn in ONE instanced
 * call through nya_render3d_grass rather than one draw per plant. 120 * 120 is 14,400 blades, a lawn no
 * per-plant loop would keep up with, all swaying on the same wind. Press TAB to switch to it.
 * */
#define GRASS_SIDE    120
#define GRASS_COUNT   (GRASS_SIDE * GRASS_SIDE)
#define GRASS_SPACING 0.16F

/** How far a key press turns the wind, and steps its strength. */
#define WIND_TURN_STEP     0.2618F /* fifteen degrees */
#define WIND_STRENGTH_STEP 0.5F
#define WIND_STRENGTH_MAX  6.0F

/** The most vertices any one built plant uses, so it can be staged in a fixed arena buffer. */
#define PLANT_VERTICES_MAX 4096

/** The creature that wades through the field: a dynamic physics sphere, re-launched when it runs off the edge. */
#define CREATURE_RADIUS   0.45F
#define CREATURE_SPEED    3.5F
#define CREATURE_DISTURB_RADIUS   1.6F
#define CREATURE_DISTURB_STRENGTH 0.9F

/** How far out the field reaches, so the creature knows when to loop back. */
#define FIELD_REACH ((f32)(FIELD_SIDE - 1) * 0.5F * FIELD_SPACING)

enum {
    ENTITY_NONE = 0,
    ENTITY_GROUND,
    ENTITY_CREATURE,
};

/* STATE */

typedef struct Foliage Foliage;

struct Foliage {
    NYA_WindowHandle window;

    /** The one wind field everything reads. */
    NYA_WindField wind;

    /** Where the wind points, as an angle about +y, and how hard it blows, so a key press can step them. */
    f32 wind_azimuth;
    f32 wind_strength;
    f32 wind_gustiness;

    /** Total seconds, for the slow camera orbit. */
    f32 elapsed_s;

    b8 meshes_ready;

    /** TAB switches between the per-plant checker field and the dense instanced-grass carpet. */
    b8 instanced_scene;

    /**
     * The instanced grass carpet, placed once and drawn every frame from one instanced call: a per-blade
     * model matrix and a tint. The wind and sway are shared and set per frame, not stored here.
     * */
    NYA_Render3DInstance* blades;
    u32                   blade_count;

    /** When non-zero (from NYA_FOLIAGE_FRAMES), the scene quits after this many frames, for a headless run. */
    u32 frame_count;
    u32 max_frames;
};

NYA_INTERNAL Foliage* foliage(void) {
    return nya_world_user_data();
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * GEOMETRY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Small builders that append flat-shaded triangles to a caller's buffer. The colour's alpha is the sway
 * flexibility, so a base vertex gets 0 and a tip gets 1; the foliage vertex shader consumes it and never
 * lets it reach the fragment stage as opacity.
 */

/** One triangle, its normal taken from its winding, each corner carrying its own flexibility in alpha. */
NYA_INTERNAL void plant_triangle(NYA_Vertex3D* vertices, u32* count, f32x3 a, f32x3 b, f32x3 c, f32x3 rgb, f32 flex_a, f32 flex_b,
                                 f32 flex_c) {
    if (*count + 3 > PLANT_VERTICES_MAX) return;

    f32x3 normal = nya_vector_normalize(nya_vector_cross(b - a, c - a));

    // rgb is the base colour; the alpha carries the sway flexibility the vertex shader reads.
    vertices[(*count)++] = nya_vertex3d(a, (NYA_Color){ rgb.x, rgb.y, rgb.z, flex_a }, normal, f32x2_zero);
    vertices[(*count)++] = nya_vertex3d(b, (NYA_Color){ rgb.x, rgb.y, rgb.z, flex_b }, normal, f32x2_zero);
    vertices[(*count)++] = nya_vertex3d(c, (NYA_Color){ rgb.x, rgb.y, rgb.z, flex_c }, normal, f32x2_zero);
}

/** A quad as two triangles: the bottom edge a-b anchored (flex_low), the top edge c-d free (flex_high). */
NYA_INTERNAL void plant_quad(NYA_Vertex3D* vertices, u32* count, f32x3 a, f32x3 b, f32x3 c, f32x3 d, f32x3 rgb, f32 flex_low,
                             f32 flex_high) {
    plant_triangle(vertices, count, a, b, c, rgb, flex_low, flex_low, flex_high);
    plant_triangle(vertices, count, a, c, d, rgb, flex_low, flex_high, flex_high);
}

/** A tuft of grass: several thin blades leaning out from the origin, each tapering to a point. */
NYA_INTERNAL u32 build_grass(NYA_Vertex3D* vertices, NYA_RNG* rng) {
    u32 count = 0;

    const u32 blades = 7;

    for (u32 i = 0; i < blades; i++) {
        f32 angle  = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { 0.0, 6.2831853 } });
        f32 radius = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { 0.0, 0.18 } });
        f32 height = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { 0.7, 1.1 } });
        f32 width  = 0.06F;

        f32x3 base   = { cosf(angle) * radius, 0.0F, sinf(angle) * radius };
        f32x3 across = { cosf(angle + 1.5708F) * width, 0.0F, sinf(angle + 1.5708F) * width };

        // a slight lean out from the centre, so the tuft is not a bundle of parallel blades.
        f32x3 tip = base + (f32x3){ cosf(angle) * 0.18F, height, sinf(angle) * 0.18F };

        // a darker green low, brighter at the tip.
        f32x3 rgb = { 0.22F, 0.52F, 0.20F };

        plant_quad(vertices, &count, base - across, base + across, tip + (across * 0.2F), tip - (across * 0.2F), rgb, 0.0F, 1.0F);
    }

    return count;
}

/** A leafy bush: a short stem and a cloud of small leaf cards, every card fully flexible so it flutters. */
NYA_INTERNAL u32 build_leaves(NYA_Vertex3D* vertices, NYA_RNG* rng) {
    u32 count = 0;

    // a thin stem the leaves sit on, mostly stiff (low flex) so the bush keeps its footing.
    f32x3 stem_rgb = { 0.36F, 0.25F, 0.12F };
    plant_quad(vertices, &count, (f32x3){ -0.03F, 0, 0 }, (f32x3){ 0.03F, 0, 0 }, (f32x3){ 0.03F, 0.5F, 0 }, (f32x3){ -0.03F, 0.5F, 0 },
               stem_rgb, 0.0F, 0.3F);

    const u32 leaves = 40;

    for (u32 i = 0; i < leaves; i++) {
        f32 angle  = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { 0.0, 6.2831853 } });
        f32 radius = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { 0.05, 0.45 } });
        f32 height = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { 0.45, 1.0 } });
        f32 size   = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { 0.10, 0.18 } });

        f32x3 center = { cosf(angle) * radius, height, sinf(angle) * radius };

        // a card in the plane spun by `angle`, so the leaves face every way.
        f32x3 right = { cosf(angle) * size, 0.0F, sinf(angle) * size };
        f32x3 up    = { 0.0F, size, 0.0F };

        f32x3 rgb = { 0.20F, 0.46F + (0.1F * cosf(angle)), 0.18F };

        // high in the canopy and fully flexible, so these are what the flutter moves; the flex is nearly
        // uniform across the small card, ramped a touch top-over-bottom.
        plant_quad(vertices, &count, center - right - up, center + right - up, center + right + up, center - right + up, rgb, 0.8F, 1.0F);
    }

    return count;
}

/** A branch: a tapered upright trunk and two arms, stiff and barely flexing near the base. */
NYA_INTERNAL u32 build_branch(NYA_Vertex3D* vertices, NYA_RNG* rng) {
    nya_unused(rng);

    u32 count = 0;

    f32x3 rgb = { 0.42F, 0.28F, 0.14F };

    f32 half_lo = 0.10F;
    f32 half_hi = 0.05F;
    f32 top     = 1.6F;

    // four faces of a tapered box, flex ramping 0 at the base to a stiff 0.5 at the top.
    f32x3 blf = { -half_lo, 0, -half_lo }, brf = { half_lo, 0, -half_lo }, brb = { half_lo, 0, half_lo }, blb = { -half_lo, 0, half_lo };
    f32x3 tlf = { -half_hi, top, -half_hi }, trf = { half_hi, top, -half_hi }, trb = { half_hi, top, half_hi }, tlb = { -half_hi, top, half_hi };

    plant_quad(vertices, &count, blf, brf, trf, tlf, rgb, 0.0F, 0.5F);
    plant_quad(vertices, &count, brf, brb, trb, trf, rgb, 0.0F, 0.5F);
    plant_quad(vertices, &count, brb, blb, tlb, trb, rgb, 0.0F, 0.5F);
    plant_quad(vertices, &count, blb, blf, tlf, tlb, rgb, 0.0F, 0.5F);

    // two arms rising from mid-trunk, their tips a little more willing to give (0.7).
    f32x3 arm_a0 = { 0.05F, 1.0F, 0.0F }, arm_a1 = { 0.7F, 1.5F, 0.1F };
    f32x3 arm_b0 = { -0.05F, 1.1F, 0.0F }, arm_b1 = { -0.6F, 1.6F, -0.15F };
    f32x3 thin   = { 0.0F, 0.06F, 0.0F };

    plant_quad(vertices, &count, arm_a0 - thin, arm_a0 + thin, arm_a1 + thin, arm_a1 - thin, rgb, 0.2F, 0.7F);
    plant_quad(vertices, &count, arm_b0 - thin, arm_b0 + thin, arm_b1 + thin, arm_b1 - thin, rgb, 0.2F, 0.7F);

    return count;
}

/**
 * One grass blade: a single tapered card from its base (y = 0) to a near-point at the top, flex ramping 0 to
 * 1 up its height. Registered once and instanced across the whole carpet, so the mesh is deliberately tiny —
 * the density comes from the instance count, not the geometry.
 * */
NYA_INTERNAL u32 build_blade(NYA_Vertex3D* vertices) {
    u32 count = 0;

    f32   width  = 0.05F;
    f32   height = 1.0F;
    f32x3 rgb    = { 0.24F, 0.55F, 0.22F };

    f32x3 base_left  = { -width, 0.0F, 0.0F };
    f32x3 base_right = { width, 0.0F, 0.0F };
    f32x3 tip_right  = { width * 0.15F, height, 0.0F };
    f32x3 tip_left   = { -width * 0.15F, height, 0.0F };

    // a single card. the foliage pipeline does not cull, so the one quad shades from both sides.
    plant_quad(vertices, &count, base_left, base_right, tip_right, tip_left, rgb, 0.0F, 1.0F);

    return count;
}

/**
 * Places the instanced carpet once: a jittered grid of blades, each turned to a random heading and scaled a
 * little, with a touch of colour variation, so a field of one mesh does not read as a stamped pattern. The
 * per-instance sway phase is not stored — the shader derives it from each blade's world position.
 * */
NYA_INTERNAL void build_grass_field(Foliage* state, NYA_RNG* rng) {
    f32 half = (f32)(GRASS_SIDE - 1) * 0.5F;

    u32 index = 0;

    for (u32 z = 0; z < GRASS_SIDE; z++) {
        for (u32 x = 0; x < GRASS_SIDE; x++) {
            f32 jitter_x = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { -0.45, 0.45 } });
            f32 jitter_z = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { -0.45, 0.45 } });
            f32 angle    = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { 0.0, 6.2831853 } });
            f32 scale    = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { 0.7, 1.3 } });
            f32 shade    = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { 0.8, 1.15 } });

            f32x3 position = {
                ((f32)x - half + jitter_x) * GRASS_SPACING,
                0.0F,
                ((f32)z - half + jitter_z) * GRASS_SPACING,
            };

            NYA_Quaternion heading = nya_quaternion_from_axis_angle(f32x3_unit_y, angle);

            state->blades[index++] = (NYA_Render3DInstance){
                .model = nya_matrix_transform(position, nya_quaternion_to_matrix3(heading), (f32x3){ scale, scale, scale }),
                // a green multiplied onto the blade's own colour, lighter or darker per blade.
                .tint  = (NYA_Color){ shade, shade, shade, 1.0F },
            };
        }
    }

    state->blade_count = index;
}

/* LAYER */

void foliage_layer_on_create(NYA_Window* window) {
    nya_assert(window != nullptr);

    Foliage* state = foliage();

    // one scratch arena for the three meshes; the GPU keeps its own copy, so this frees after registering.
    NYA_Arena* scratch = nya_arena_create(.name = "foliage_build");
    defer nya_arena_destroy(scratch);

    NYA_Vertex3D* vertices = nya_arena_alloc(scratch, PLANT_VERTICES_MAX * sizeof(NYA_Vertex3D));
    NYA_RNG*      rng       = nya_rng_create_in(scratch, "F0713A6E00000001");

    u32 grass_count = build_grass(vertices, rng);
    state->meshes_ready = nya_render3d_mesh_register(window, MESH_GRASS, vertices, grass_count);

    u32 leaf_count = build_leaves(vertices, rng);
    state->meshes_ready = nya_render3d_mesh_register(window, MESH_LEAVES, vertices, leaf_count) && state->meshes_ready;

    u32 branch_count = build_branch(vertices, rng);
    state->meshes_ready = nya_render3d_mesh_register(window, MESH_BRANCH, vertices, branch_count) && state->meshes_ready;

    u32 blade_count = build_blade(vertices);
    state->meshes_ready = nya_render3d_mesh_register(window, MESH_BLADE, vertices, blade_count) && state->meshes_ready;

    // the instanced carpet's placements, built once into world-lifetime storage and drawn every frame.
    state->blades = nya_arena_alloc(nya_world()->allocator, GRASS_COUNT * sizeof(NYA_Render3DInstance));
    build_grass_field(state, rng);

    // a static floor so the creature rolls rather than falls, and the creature itself: a dynamic sphere
    // given a sideways shove, which the physics system steps every tick.
    NYA_EntityHandle ground = nya_entity_spawn(.name = "ground", .type = ENTITY_GROUND, .position = { 0.0F, -0.5F, 0.0F });
    (void)nya_physics3d_body_attach(ground, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX,
                                    .size = { FIELD_REACH * 3.0F, 1.0F, FIELD_REACH * 3.0F }, .friction = 0.6F);

    NYA_EntityHandle creature = nya_entity_spawn(.name = "creature", .type = ENTITY_CREATURE, .position = { -FIELD_REACH - 2.0F, CREATURE_RADIUS, 0.0F });
    (void)nya_physics3d_body_attach(creature, .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS3D_SHAPE_SPHERE,
                                    .radius = CREATURE_RADIUS, .density = 400.0F, .friction = 0.4F, .restitution = 0.1F);

    NYA_Entity* body = nya_entity_get(creature);
    if (body != nullptr) nya_physics3d_velocity_set(body, (f32x3){ CREATURE_SPEED, 0.0F, 0.0F });
}

void foliage_layer_on_destroy(NYA_Window* window) {
    // pair every register with a release, though the batch's registry also frees these at shutdown.
    nya_render3d_mesh_release(window, MESH_GRASS);
    nya_render3d_mesh_release(window, MESH_LEAVES);
    nya_render3d_mesh_release(window, MESH_BRANCH);
    nya_render3d_mesh_release(window, MESH_BLADE);
}

void foliage_layer_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window, event);
}

void foliage_layer_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window);

    Foliage* state = foliage();

    if (nya_input_key_pressed(NYA_KEY_ESCAPE)) nya_app_get()->should_quit = true;

    // TAB switches between the per-plant checker field and the dense instanced-grass carpet.
    if (nya_input_key_pressed(NYA_KEY_TAB)) state->instanced_scene = !state->instanced_scene;

    // a timed run quits itself once it has drawn its frames, so a headless CI run terminates.
    state->frame_count++;
    if (state->max_frames > 0 && state->frame_count >= state->max_frames) nya_app_get()->should_quit = true;

    // turn and strengthen the wind on a key press, then re-point the field. nya_wind_set leaves the
    // field's clock alone, so the sway does not jump when the wind changes.
    b8 changed = false;

    if (nya_input_key_pressed(NYA_KEY_LEFT)) { state->wind_azimuth -= WIND_TURN_STEP; changed = true; }
    if (nya_input_key_pressed(NYA_KEY_RIGHT)) { state->wind_azimuth += WIND_TURN_STEP; changed = true; }
    if (nya_input_key_pressed(NYA_KEY_UP)) { state->wind_strength = nya_min(state->wind_strength + WIND_STRENGTH_STEP, WIND_STRENGTH_MAX); changed = true; }
    if (nya_input_key_pressed(NYA_KEY_DOWN)) { state->wind_strength = nya_max(state->wind_strength - WIND_STRENGTH_STEP, 0.0F); changed = true; }
    if (nya_input_key_pressed(NYA_KEY_G)) { state->wind_gustiness = state->wind_gustiness > 0.0F ? 0.0F : 0.7F; changed = true; }

    if (changed) {
        f32x3 direction = { cosf(state->wind_azimuth), 0.0F, sinf(state->wind_azimuth) };
        nya_wind_set(&state->wind, direction, state->wind_strength, state->wind_gustiness);
    }

    // the one per-frame advance the whole field animates from.
    nya_wind_advance(&state->wind, delta_time_s);
    state->elapsed_s += delta_time_s;

    // when the creature rolls off the far edge, set it back at the start with a fresh shove, so it keeps
    // sweeping through the field. the physics system integrated its motion this tick already.
    nya_entity_foreach_kind (ENTITY_CREATURE, creature) {
        f32x3 at = nya_entity_render_position(creature);

        if (at.x > FIELD_REACH + 2.0F) {
            nya_physics3d_teleport(creature, (f32x3){ -FIELD_REACH - 2.0F, CREATURE_RADIUS, 0.0F }, nya_quaternion_identity);
            nya_physics3d_velocity_set(creature, (f32x3){ CREATURE_SPEED, 0.0F, 0.0F });
        }
    }
}

/** Draws one plant of a given style at a grid cell, sampling the wind at its base. */
NYA_INTERNAL void draw_plant(NYA_Window* window, NYA_ConstCString handle, NYA_FoliageStyle style, f32x3 position, NYA_Color tint) {
    Foliage* state = foliage();

    NYA_Render3DFoliage plant = nya_render3d_foliage_style(style);

    plant.wind = nya_wind_sample(&state->wind, position);
    plant.time = state->wind.time;
    plant.tint = tint;

    nya_render3d_foliage(window, handle, position, (f32x3){ 1, 1, 1 }, nya_quaternion_identity, plant);
}

void foliage_layer_on_render(NYA_Window* window) {
    nya_assert(window != nullptr);

    Foliage* state = foliage();

    // a slow orbit, so the sway reads in three dimensions rather than as a flat scroll.
    f32 orbit = state->elapsed_s * 0.15F;

    nya_render3d_begin(window, (NYA_Camera3DPerspective){
                                   .position = { sinf(orbit) * 11.0F, 5.5F, cosf(orbit) * 11.0F },
                                   .target   = { 0.0F, 1.2F, 0.0F },
                               });

    nya_render3d_sky_draw(window, (NYA_Render3DSky){ .sun_direction = nya_vector_normalize((f32x3){ 0.4F, 0.8F, 0.3F }), .sun_angle = 0.03F });

    nya_render3d_light_set(window, (NYA_Render3DLight){
                                       .direction = nya_vector_normalize((f32x3){ -0.4F, -1.0F, -0.3F }),
                                       .color     = NYA_COLOR_WHITE,
                                       .ambient   = 0.4F,
                                       .intensity = 1.0F,
                                   });

    // the ground the field stands on.
    nya_render3d_plane(window, f32x3_zero, (f32x2){ FIELD_SIDE * FIELD_SPACING * 1.4F, FIELD_SIDE * FIELD_SPACING * 1.4F }, (NYA_Color){ 0.30F, 0.36F, 0.22F, 1.0F });

    // the creature, and the disturber it presses into the foliage. fed here, after begin and before the
    // plants, because disturbers are cleared at begin. this is where physics reaches the sway.
    nya_entity_foreach_kind (ENTITY_CREATURE, creature) {
        f32x3 at = nya_entity_render_position(creature);

        nya_render3d_foliage_disturb(window, at, CREATURE_DISTURB_RADIUS, CREATURE_DISTURB_STRENGTH);
        nya_render3d_sphere(window, at, CREATURE_RADIUS, (NYA_Color){ 0.85F, 0.35F, 0.30F, 1.0F });
    }

    if (state->meshes_ready && state->instanced_scene) {
        // the whole carpet in one instanced draw: one wind sample at its centre, shared by every blade, each
        // swaying on its own phase (the shader takes it from the blade's world position). This is the density
        // the per-plant path cannot reach — thousands of blades, one draw call.
        NYA_Render3DFoliage look = nya_render3d_foliage_style(NYA_FOLIAGE_GRASS);

        look.wind = nya_wind_sample(&state->wind, f32x3_zero);
        look.time = state->wind.time;
        look.tint = (NYA_Color){ 0.6F, 1.0F, 0.6F, 1.0F };

        nya_render3d_grass(window, MESH_BLADE, state->blades, state->blade_count, look);
    } else if (state->meshes_ready) {
        f32 half = (f32)(FIELD_SIDE - 1) * 0.5F;

        for (u32 z = 0; z < FIELD_SIDE; z++) {
            for (u32 x = 0; x < FIELD_SIDE; x++) {
                f32x3 at = { ((f32)x - half) * FIELD_SPACING, 0.0F, ((f32)z - half) * FIELD_SPACING };

                // a checker of the three looks, so all three sway from the same field side by side.
                u32 kind = (x + z) % 3;

                if (kind == 0) draw_plant(window, MESH_GRASS, NYA_FOLIAGE_GRASS, at, (NYA_Color){ 0.6F, 1.0F, 0.6F, 1.0F });
                else if (kind == 1) draw_plant(window, MESH_LEAVES, NYA_FOLIAGE_LEAVES, at, (NYA_Color){ 0.8F, 1.0F, 0.7F, 1.0F });
                else draw_plant(window, MESH_BRANCH, NYA_FOLIAGE_BRANCHES, at, NYA_COLOR_WHITE);
            }
        }
    }

    nya_render3d_end(window);

    nya_render2d_textf_with_font(window, NYA_ASSET_FONTS_ALDRICH_TTF, 20.0F, 16.0F, 16.0F, NYA_COLOR_WHITE,
                                 "wind %.1f  gust %s  ·  arrows steer/strengthen  ·  g gust  ·  tab %s  ·  the ball parts the grass",
                                 (double)state->wind_strength, state->wind_gustiness > 0.0F ? "on" : "off",
                                 state->instanced_scene ? "per-plant field" : "instanced carpet");
}

/* MAIN */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();

    NYA_EXPECT(nya_app_init(.app_id = "foliage3d"), "while starting the engine");

    Foliage* state = nya_arena_alloc(nya_world()->allocator, sizeof(Foliage));

    *state = (Foliage){
        .window         = NYA_WINDOW_HANDLE_NONE,
        .wind_azimuth   = 0.0F,
        .wind_strength  = 2.5F,
        .wind_gustiness = 0.7F,
    };

    state->wind = nya_wind_field((NYA_WindOptions){ .direction = { 1, 0, 0 }, .strength = state->wind_strength, .gustiness = state->wind_gustiness });

    // a frame budget for a headless run: NYA_FOLIAGE_FRAMES=N draws N frames and quits, so CI can exercise
    // both scenes under the sanitizers without a display. Zero (unset) runs until the window is closed.
    NYA_ConstCString frames = getenv("NYA_FOLIAGE_FRAMES");
    if (frames != nullptr) state->max_frames = (u32)strtoul(frames, nullptr, 10);

    // start on the instanced carpet when a frame budget is set, so a headless run exercises the new path.
    state->instanced_scene = state->max_frames > 0;

    nya_world_user_data_set(state);

    state->window = nya_window_create(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_RESIZABLE);
    nya_assert(nya_window_is_valid(state->window), "the window could not be created");

    nya_layer_push(state->window, nya_layer_of(foliage_layer, LAYER_ID));

    nya_app_run();

    nya_app_deinit();

    return 0;
}

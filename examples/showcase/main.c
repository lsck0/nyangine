/**
 * @file examples/showcase/main.c
 *
 * One living valley that composes every atmospheric system the renderer grew into a single scene: a river
 * flowing down a terrain channel, grass, leaves and branches swaying on the banks, pollen drifting on the
 * air, a graded sky with fog and aerial perspective, and volumetric light shafts streaming from the sun
 * through it all — kept inside the flat, stylized look (restrained palette, one soft shading step, never
 * photoreal).
 *
 * ```
 * ./build run example showcase
 * NYA_SHOWCASE_FRAMES=6 ./build run example showcase   # draw six frames and quit, for a headless run
 * ```
 *
 * ## What ties it together
 *
 * A single NYA_WindField (render_wind.h) is the one source of motion. It is advanced once a frame and then
 * read three ways: sampled per plant to bend the foliage, sampled at the surface to hurry the water's flow
 * and lift its chop, and sampled per particle (through the dust system's on_update hook) so the pollen
 * literally rides the same air. The sun that lights and shadows the scene is the sun the sky paints and the
 * sun the light-shaft post pass gathers toward, so the beams line up with the shading. A physics sphere
 * wades along the near bank and is fed to the foliage as a disturber, so something visibly parts the grass.
 *
 * The whole 3D scene is drawn through a post chain (a render texture): the water needs a resolved image to
 * refract, and the light shafts and bloom are scene features nya_post_end runs over that same target.
 * */
#include "genyarated/assets.h"
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define WINDOW_TITLE  "nyangine — showcase"
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

#define LAYER_ID "showcase"

/** Registered mesh handles. Reserved names, not asset paths: these meshes are built here, not loaded. */
#define MESH_TERRAIN "showcase/terrain"
#define MESH_RIVER   "showcase/river"
#define MESH_GRASS   "showcase/grass_tuft"
#define MESH_LEAVES  "showcase/leaf_bush"
#define MESH_BRANCH  "showcase/branch"

/** The valley: the river runs along x, the banks rise out along z, the terrain reaches this far each way. */
#define WORLD_HALF_X 42.0F
#define WORLD_HALF_Z 30.0F

/** The band the channel carves through the middle: how wide the whole cut is, and how far the flat water reaches. */
#define RIVER_WIDTH 15.0F
#define WATER_HALF  4.2F

/** How deep the channel bed sits below the water plane (y = 0), and how high the banks stand above it. */
#define BED_DEPTH   1.3F
#define BANK_HEIGHT 1.9F

/** Terrain and water grid resolution: cells along the valley by cells across it. Both stay under the 16-bit index cap. */
#define TERRAIN_COLS 96
#define TERRAIN_ROWS 64
#define RIVER_COLS   140
#define RIVER_ROWS   20

static_assert((u32)TERRAIN_COLS * (u32)TERRAIN_ROWS * 6u <= 65536, "the terrain mesh must fit sixteen-bit indices");
static_assert((u32)RIVER_COLS * (u32)RIVER_ROWS * 6u <= 65536, "the river mesh must fit sixteen-bit indices");

/** The widest built mesh, so terrain, river and plants all stage through one fixed scratch buffer. */
#define MESH_VERTICES_MAX 40000u

/** The most vertices any one built plant uses. */
#define PLANT_VERTICES_MAX 4096u

/** How many plants scatter over the banks. Each is one foliage draw; placement is hashed, not stored. */
#define PLANT_COUNT 240u

/** Deterministic scatter seed, so the same banks grow the same plants every run. */
#define SCATTER_SEED 0x5C0FF3u

/** The dust/pollen pool, and how many motes each timed burst releases. */
#define DUST_POOL   2400u
#define DUST_PER_BURST 40u

/** The creature that wades the near bank and parts the grass: a dynamic sphere re-launched off the far edge. */
#define CREATURE_RADIUS 0.5F
#define CREATURE_SPEED  3.0F
#define CREATURE_BANK_Z (WATER_HALF + 2.2F)
#define CREATURE_DISTURB_RADIUS   2.0F
#define CREATURE_DISTURB_STRENGTH 1.0F

#define HUD_FONT      NYA_ASSET_FONTS_ALDRICH_TTF
#define HUD_FONT_SIZE 16.0F

enum {
    ENTITY_NONE = 0,
    ENTITY_GROUND,
    ENTITY_CREATURE,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct Showcase Showcase;

struct Showcase {
    NYA_WindowHandle window;

    /** The one wind field the whole scene animates from: foliage sway, water chop and drifting pollen all read it. */
    NYA_WindField wind;

    /** The drifting pollen. Lives on the world arena, so it goes with the world; its motes ride `wind`. */
    NYA_ParticleSystem* dust;

    /** The post chain: the scene renders into its texture, which the water refraction, bloom and shafts read. */
    NYA_PostChain post;

    /** Total seconds, driving the slow camera fly-through. */
    f32 elapsed_s;

    /** Fed on a timer so the pollen release rate does not depend on frame rate. */
    f32 emit_timer_s;

    b8 meshes_ready;

    /** When non-zero (from NYA_SHOWCASE_FRAMES), the scene quits after this many frames, for a headless run. */
    u32 max_frames;
    u32 frame_count;

    /** The last frame's fps, captured in on_render and logged at teardown so a headless run reports a number. */
    f32 last_fps;
};

NYA_INTERNAL Showcase* showcase(void) {
    return nya_world_user_data();
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TERRAIN & RIVER GEOMETRY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A smooth 0..1 ramp between two edges, the Hermite the shaders use; the math module has no scalar one. */
NYA_INTERNAL f32 example_smoothstep(f32 edge0, f32 edge1, f32 x) {
    f32 t = nya_clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);

    return t * t * (3.0F - (2.0F * t));
}

/**
 * The ground height at a world xz: a channel dipping below the water plane between banks that rise above it,
 * with rolling hills that only lift the banks and uplands (so the channel bed stays calm for the water) and a
 * faint bed ripple for the refraction to bend over.
 * */
NYA_INTERNAL f32 terrain_height(f32 x, f32 z) {
    // 0 down the centre line, 1 out past the bank shoulder.
    f32 across  = nya_clamp(fabsf(z) / (RIVER_WIDTH * 0.5F), 0.0F, 1.0F);
    f32 profile = example_smoothstep(0.30F, 0.72F, across);

    // the bed climbs from the channel floor up to the bank top across the middle band.
    f32 channel = nya_lerp(-BED_DEPTH, BANK_HEIGHT, profile);

    // eroded hills, faded in by `profile` so they raise the banks and uplands but leave the wet channel smooth.
    f32 hills = (sinf(x * 0.06F) * 2.2F) + (cosf((z * 0.05F) + 1.3F) * 1.6F) + (sinf((x + z) * 0.09F) * 0.8F);

    // a shallow ripple, strongest in the channel, so the water's refraction has relief to distort.
    f32 ripple = 0.10F * sinf(x * 0.7F) * cosf(z * 0.6F);

    return channel + (hills * profile) + (ripple * (1.0F - profile));
}

/** The bed colour: sandy in the wet channel, grassy up the banks, muted olive on the uplands. */
NYA_INTERNAL f32x3 terrain_color(f32 z) {
    f32 across  = nya_clamp(fabsf(z) / (RIVER_WIDTH * 0.5F), 0.0F, 1.0F);
    f32 profile = example_smoothstep(0.32F, 0.70F, across);

    f32x3 sand = { 0.52F, 0.45F, 0.32F };
    f32x3 bank = { 0.26F, 0.42F, 0.21F };

    f32x3 base = nya_lerp(sand, bank, profile);

    // a touch drier and paler on the high uplands, so the far banks read as distinct ground.
    f32   upland = example_smoothstep(0.75F, 1.0F, across);
    f32x3 dry    = { 0.36F, 0.40F, 0.26F };

    return nya_lerp(base, dry, upland * 0.6F);
}

/** One grid triangle, its normal taken from its winding; the caller passes each corner's colour and alpha. */
NYA_INTERNAL void grid_triangle(NYA_Vertex3D* vertices, u32* count, f32x3 a, f32x3 b, f32x3 c, NYA_Color ca, NYA_Color cb,
                                NYA_Color cc) {
    if (*count + 3 > MESH_VERTICES_MAX) return;

    f32x3 normal = nya_vector_normalize(nya_vector_cross(b - a, c - a));

    vertices[(*count)++] = nya_vertex3d(a, ca, normal, f32x2_zero);
    vertices[(*count)++] = nya_vertex3d(b, cb, normal, f32x2_zero);
    vertices[(*count)++] = nya_vertex3d(c, cc, normal, f32x2_zero);
}

/** The valley heightfield: banks and a channel, coloured sand to grass. Normals follow the relief per triangle. */
NYA_INTERNAL u32 build_terrain(NYA_Vertex3D* vertices) {
    u32 count = 0;

    for (u32 row = 0; row < TERRAIN_ROWS; row++) {
        for (u32 col = 0; col < TERRAIN_COLS; col++) {
            f32 x0 = nya_lerp(-WORLD_HALF_X, WORLD_HALF_X, (f32)col / (f32)TERRAIN_COLS);
            f32 x1 = nya_lerp(-WORLD_HALF_X, WORLD_HALF_X, (f32)(col + 1) / (f32)TERRAIN_COLS);
            f32 z0 = nya_lerp(-WORLD_HALF_Z, WORLD_HALF_Z, (f32)row / (f32)TERRAIN_ROWS);
            f32 z1 = nya_lerp(-WORLD_HALF_Z, WORLD_HALF_Z, (f32)(row + 1) / (f32)TERRAIN_ROWS);

            f32x3 p00 = { x0, terrain_height(x0, z0), z0 };
            f32x3 p10 = { x1, terrain_height(x1, z0), z0 };
            f32x3 p11 = { x1, terrain_height(x1, z1), z1 };
            f32x3 p01 = { x0, terrain_height(x0, z1), z1 };

            f32x3 k00 = terrain_color(z0);
            f32x3 k10 = terrain_color(z0);
            f32x3 k11 = terrain_color(z1);
            f32x3 k01 = terrain_color(z1);

            NYA_Color c00 = { k00.x, k00.y, k00.z, 1.0F };
            NYA_Color c10 = { k10.x, k10.y, k10.z, 1.0F };
            NYA_Color c11 = { k11.x, k11.y, k11.z, 1.0F };
            NYA_Color c01 = { k01.x, k01.y, k01.z, 1.0F };

            grid_triangle(vertices, &count, p00, p10, p11, c00, c10, c11);
            grid_triangle(vertices, &count, p00, p11, p01, c00, c11, c01);
        }
    }

    return count;
}

/** The water strip at y = 0, spanning the channel, with the shore weight (0 mid-channel, 1 at the banks) in alpha. */
NYA_INTERNAL u32 build_river(NYA_Vertex3D* vertices) {
    u32 count = 0;

    for (u32 row = 0; row < RIVER_ROWS; row++) {
        for (u32 col = 0; col < RIVER_COLS; col++) {
            f32 x0 = nya_lerp(-WORLD_HALF_X, WORLD_HALF_X, (f32)col / (f32)RIVER_COLS);
            f32 x1 = nya_lerp(-WORLD_HALF_X, WORLD_HALF_X, (f32)(col + 1) / (f32)RIVER_COLS);
            f32 z0 = nya_lerp(-WATER_HALF, WATER_HALF, (f32)row / (f32)RIVER_ROWS);
            f32 z1 = nya_lerp(-WATER_HALF, WATER_HALF, (f32)(row + 1) / (f32)RIVER_ROWS);

            // the still surface is flat at y = 0; the water vertex shader lifts it into travelling waves.
            f32x3 p00 = { x0, 0.0F, z0 };
            f32x3 p10 = { x1, 0.0F, z0 };
            f32x3 p11 = { x1, 0.0F, z1 };
            f32x3 p01 = { x0, 0.0F, z1 };

            // shore weight in alpha; rgb is unused (the colours come from the water uniform), so leave it white.
            f32 s0 = nya_clamp(fabsf(z0) / WATER_HALF, 0.0F, 1.0F);
            f32 s1 = nya_clamp(fabsf(z1) / WATER_HALF, 0.0F, 1.0F);

            NYA_Color c00 = { 1.0F, 1.0F, 1.0F, s0 };
            NYA_Color c11 = { 1.0F, 1.0F, 1.0F, s1 };

            grid_triangle(vertices, &count, p00, p10, p11, c00, c00, c11);
            grid_triangle(vertices, &count, p00, p11, p01, c00, c11, c11);
        }
    }

    return count;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PLANT GEOMETRY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * The three plant meshes are built once with their base at y = 0 and a sway flexibility up their height in the
 * vertex colour's alpha (0 anchored at the base, 1 free at the tips). The foliage vertex shader consumes that
 * alpha to bend the geometry about its base, and never lets it reach the fragment stage as opacity.
 * */

/** One triangle, its normal from its winding, each corner carrying its own flexibility in alpha. */
NYA_INTERNAL void plant_triangle(NYA_Vertex3D* vertices, u32* count, f32x3 a, f32x3 b, f32x3 c, f32x3 rgb, f32 flex_a, f32 flex_b,
                                 f32 flex_c) {
    if (*count + 3 > PLANT_VERTICES_MAX) return;

    f32x3 normal = nya_vector_normalize(nya_vector_cross(b - a, c - a));

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
        f32 height = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { 0.6, 1.0 } });
        f32 width  = 0.06F;

        f32x3 base   = { cosf(angle) * radius, 0.0F, sinf(angle) * radius };
        f32x3 across  = { cosf(angle + 1.5708F) * width, 0.0F, sinf(angle + 1.5708F) * width };
        f32x3 tip    = base + (f32x3){ cosf(angle) * 0.18F, height, sinf(angle) * 0.18F };

        f32x3 rgb = { 0.22F, 0.52F, 0.20F };

        plant_quad(vertices, &count, base - across, base + across, tip + (across * 0.2F), tip - (across * 0.2F), rgb, 0.0F, 1.0F);
    }

    return count;
}

/** A leafy bush: a short stem and a cloud of small leaf cards, high in the canopy and fully flexible so they flutter. */
NYA_INTERNAL u32 build_leaves(NYA_Vertex3D* vertices, NYA_RNG* rng) {
    u32 count = 0;

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
        f32x3 right  = { cosf(angle) * size, 0.0F, sinf(angle) * size };
        f32x3 up     = { 0.0F, size, 0.0F };

        f32x3 rgb = { 0.20F, 0.46F + (0.1F * cosf(angle)), 0.18F };

        plant_quad(vertices, &count, center - right - up, center + right - up, center + right + up, center - right + up, rgb, 0.8F, 1.0F);
    }

    return count;
}

/** A branch: a tapered upright trunk and two arms, stiff and barely flexing near the base. */
NYA_INTERNAL u32 build_branch(NYA_Vertex3D* vertices) {
    u32 count = 0;

    f32x3 rgb = { 0.42F, 0.28F, 0.14F };

    f32 half_lo = 0.10F;
    f32 half_hi = 0.05F;
    f32 top     = 1.6F;

    f32x3 blf = { -half_lo, 0, -half_lo }, brf = { half_lo, 0, -half_lo }, brb = { half_lo, 0, half_lo }, blb = { -half_lo, 0, half_lo };
    f32x3 tlf = { -half_hi, top, -half_hi }, trf = { half_hi, top, -half_hi }, trb = { half_hi, top, half_hi }, tlb = { -half_hi, top, half_hi };

    plant_quad(vertices, &count, blf, brf, trf, tlf, rgb, 0.0F, 0.5F);
    plant_quad(vertices, &count, brf, brb, trb, trf, rgb, 0.0F, 0.5F);
    plant_quad(vertices, &count, brb, blb, tlb, trb, rgb, 0.0F, 0.5F);
    plant_quad(vertices, &count, blb, blf, tlf, tlb, rgb, 0.0F, 0.5F);

    f32x3 arm_a0 = { 0.05F, 1.0F, 0.0F }, arm_a1 = { 0.7F, 1.5F, 0.1F };
    f32x3 arm_b0 = { -0.05F, 1.1F, 0.0F }, arm_b1 = { -0.6F, 1.6F, -0.15F };
    f32x3 thin   = { 0.0F, 0.06F, 0.0F };

    plant_quad(vertices, &count, arm_a0 - thin, arm_a0 + thin, arm_a1 + thin, arm_a1 - thin, rgb, 0.2F, 0.7F);
    plant_quad(vertices, &count, arm_b0 - thin, arm_b0 + thin, arm_b1 + thin, arm_b1 - thin, rgb, 0.2F, 0.7F);

    return count;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SCATTER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A hashed value in [0, 1] for plant `index`, channel `k`. nya_ihash2 returns [-1, 1], so fold it up. */
NYA_INTERNAL f32 plant_unit(u32 index, s32 k) {
    return (nya_ihash2((s32)index, k, SCATTER_SEED) * 0.5F) + 0.5F;
}

/**
 * Where plant `index` stands and whether it belongs on dry bank. Placed by hashing, so the count is a pure knob
 * and no per-instance memory grows with it; plants that would fall in the wet channel are rejected by the caller.
 * */
NYA_INTERNAL f32x3 plant_position(u32 index, OUT b8* out_on_bank) {
    f32 x = nya_lerp(-WORLD_HALF_X + 3.0F, WORLD_HALF_X - 3.0F, plant_unit(index, 0));

    // push placement out to the banks: choose a side, then an offset from the water's edge into the uplands.
    f32 side   = (plant_unit(index, 1) < 0.5F) ? -1.0F : 1.0F;
    f32 offset = nya_lerp(WATER_HALF + 0.4F, WORLD_HALF_Z - 2.0F, plant_unit(index, 2));
    f32 z      = side * offset;

    f32 y = terrain_height(x, z);

    // only dry, gently-sloped ground grows plants: above the water and below the steep upland tops.
    *out_on_bank = (y > 0.15F) && (y < BANK_HEIGHT + 2.4F);

    return (f32x3){ x, y, z };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DUST / POLLEN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Steers each pollen mote after its integration: ease its velocity toward the shared wind field's push at its
 * own position, so the dust drifts on exactly the air that bends the foliage and hurries the water. `user_data`
 * is the scene's NYA_WindField.
 * */
NYA_INTERNAL void dust_ride_wind(NYA_Particle* particle, f32 t, f32 delta_time_s, void* user_data) {
    nya_unused(t);

    const NYA_WindField* wind = user_data;

    f32x3 push = nya_wind_sample(wind, particle->position);

    // ride the horizontal wind, with a hint of lift so motes hang in the light rather than settling at once.
    f32x3 target = { push.x * 0.6F, 0.2F, push.z * 0.6F };

    particle->velocity = nya_lerp(particle->velocity, target, nya_min(delta_time_s * 1.5F, 1.0F));
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYER: CREATE / DESTROY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void showcase_layer_on_create(NYA_Window* window) {
    nya_assert(window != nullptr);

    Showcase* state = showcase();

    // a dim dawn sky-blue behind everything, so the terrain's rim reads before the sky pass paints over it.
    nya_render_clear_color_set(window, (NYA_Color){ 0.06F, 0.08F, 0.12F, 1.0F });

    // one scratch arena builds every mesh; the GPU keeps its own copy, so this frees after registering.
    NYA_Arena* scratch = nya_arena_create(.name = "showcase_build");
    defer nya_arena_destroy(scratch);

    NYA_Vertex3D* vertices = nya_arena_alloc(scratch, MESH_VERTICES_MAX * sizeof(NYA_Vertex3D));
    NYA_RNG*      rng       = nya_rng_create_in(scratch, "5A0FF3E000000001");

    b8 ok = nya_render3d_mesh_register(window, MESH_TERRAIN, vertices, build_terrain(vertices));
    ok    = nya_render3d_mesh_register(window, MESH_RIVER, vertices, build_river(vertices)) && ok;
    ok    = nya_render3d_mesh_register(window, MESH_GRASS, vertices, build_grass(vertices, rng)) && ok;
    ok    = nya_render3d_mesh_register(window, MESH_LEAVES, vertices, build_leaves(vertices, rng)) && ok;
    ok    = nya_render3d_mesh_register(window, MESH_BRANCH, vertices, build_branch(vertices)) && ok;

    state->meshes_ready = ok;
    if (!ok) nya_log_error("showcase: a mesh failed to register; the scene will draw incomplete.");

    // the drifting pollen, from the world arena so it shares the world's lifetime. Its on_update reads the one
    // wind field, which is why the motes travel with the same air as the grass and the water.
    state->dust = nya_particles_create(nya_world()->allocator, DUST_POOL);
    nya_particles_space_set(state->dust, NYA_PARTICLE_SPACE_3D);
    nya_particles_texture_set(state->dust, NYA_ASSET_TEXTURES_PUFF_PNG);
    nya_particles_on_update_set(state->dust, dust_ride_wind, &state->wind);

    // the sun's cascaded shadows, so the trees and banks cast into the valley.
    nya_render3d_shadow_options_set(window, (NYA_Render3DShadowOptions){
                                                .cascades = 3,
                                                .map_size = 2048,
                                                .color    = { 0.10F, 0.12F, 0.20F, 0.55F },
                                            });

    // the flat, stylized surf glow: a gentle bloom, kept restrained so nothing blows out.
    nya_post_bloom_set(window, (NYA_PostBloom){ .enabled = true, .threshold = 0.92F, .intensity = 0.7F });

    // the light beams: volumetric shafts gathered from the sky's sun through the scene. This is a scene feature
    // nya_post_end runs over the post target, so it needs the scene drawn through the chain (it is, below).
    nya_post_light_shafts_set(window, (NYA_PostLightShafts){ .enabled = true, .intensity = 0.9F, .length = 0.75F });

    // a static floor collider under the near bank, and the creature that wades it: a dynamic sphere given a
    // sideways shove, which the physics system steps every tick. It parts the grass as a foliage disturber.
    f32 bank_y = terrain_height(0.0F, CREATURE_BANK_Z);

    NYA_EntityHandle ground = nya_entity_spawn(.name = "bank", .type = ENTITY_GROUND,
                                               .position = { 0.0F, bank_y - CREATURE_RADIUS - 0.5F, CREATURE_BANK_Z });
    (void)nya_physics3d_body_attach(ground, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX,
                                    .size = { WORLD_HALF_X * 2.0F, 1.0F, 6.0F }, .friction = 0.6F);

    NYA_EntityHandle creature = nya_entity_spawn(.name = "creature", .type = ENTITY_CREATURE,
                                                 .position = { -WORLD_HALF_X + 4.0F, bank_y + CREATURE_RADIUS, CREATURE_BANK_Z });
    (void)nya_physics3d_body_attach(creature, .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS3D_SHAPE_SPHERE,
                                    .radius = CREATURE_RADIUS, .density = 400.0F, .friction = 0.4F, .restitution = 0.1F);

    NYA_Entity* body = nya_entity_get(creature);
    if (body != nullptr) nya_physics3d_velocity_set(body, (f32x3){ CREATURE_SPEED, 0.0F, 0.0F });
}

void showcase_layer_on_destroy(NYA_Window* window) {
    Showcase* state = showcase();

    // turn every scene feature this layer switched on back off, so a re-enter (a hot reload) starts clean.
    nya_post_bloom_set(window, (NYA_PostBloom){ 0 });
    nya_post_light_shafts_set(window, (NYA_PostLightShafts){ 0 });

    nya_post_chain_destroy(&state->post);

    // the meshes' GPU buffers; the pollen pool lives on the world arena and goes with it.
    nya_render3d_mesh_release(window, MESH_TERRAIN);
    nya_render3d_mesh_release(window, MESH_RIVER);
    nya_render3d_mesh_release(window, MESH_GRASS);
    nya_render3d_mesh_release(window, MESH_LEAVES);
    nya_render3d_mesh_release(window, MESH_BRANCH);

    // leave the frame count in the log so a headless run reports it verified something.
    nya_log_info("showcase: shutting down after %u frames, last %.1f fps.", state->frame_count, (f64)state->last_fps);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYER: UPDATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void showcase_layer_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window);
    nya_assert(event != nullptr);

    if (event->type == NYA_EVENT_KEY_DOWN && event->as_key_event.key == NYA_KEY_ESCAPE) {
        nya_app_get()->should_quit = true;
        event->was_handled         = true;
    }
}

void showcase_layer_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window);

    Showcase* state = showcase();

    // the one per-frame advance the whole scene animates from: foliage, water and pollen all sample this field.
    nya_wind_advance(&state->wind, delta_time_s);
    state->elapsed_s += delta_time_s;

    nya_particles_update(state->dust, delta_time_s);

    // release pollen on a timer, so its rate does not ride the frame rate. It spawns high over the valley and
    // is then carried by dust_ride_wind, so where it drifts is the wind's doing, not the burst's.
    state->emit_timer_s += delta_time_s;

    while (state->emit_timer_s >= 0.05F) {
        state->emit_timer_s -= 0.05F;

        (void)nya_particles_emit(state->dust, (NYA_ParticleBurst){
                                                  .shape       = NYA_PARTICLE_SHAPE_BOX,
                                                  .position    = { 0.0F, 6.0F, 0.0F },
                                                  .volume      = { WORLD_HALF_X * 1.4F, 5.0F, WORLD_HALF_Z * 1.2F },
                                                  .count       = DUST_PER_BURST,
                                                  .speed       = { 0.2F, 0.8F },
                                                  .lifetime_s  = { 4.0F, 8.0F },
                                                  .size        = { 0.04F, 0.10F },
                                                  .size_end    = { 0.02F, 0.06F },
                                                  .color_start = { 0.92F, 0.90F, 0.72F, 0.5F },
                                                  .color_end   = { 0.85F, 0.82F, 0.60F, 0.0F },
                                                  .gravity     = { 0.0F, -0.15F, 0.0F },
                                                  .damping     = 0.4F,
                                              });
    }

    // loop the creature back to the start when it rolls off the far end, so it keeps parting the grass.
    nya_entity_foreach_kind (ENTITY_CREATURE, creature) {
        f32x3 at = nya_entity_render_position(creature);

        if (at.x > WORLD_HALF_X - 4.0F) {
            f32 bank_y = terrain_height(-WORLD_HALF_X + 4.0F, CREATURE_BANK_Z);
            nya_physics3d_teleport(creature, (f32x3){ -WORLD_HALF_X + 4.0F, bank_y + CREATURE_RADIUS, CREATURE_BANK_Z }, nya_quaternion_identity);
            nya_physics3d_velocity_set(creature, (f32x3){ CREATURE_SPEED, 0.0F, 0.0F });
        }
    }

    // a timed run quits itself once it has drawn its frames, so a headless CI run terminates.
    state->frame_count++;
    if (state->max_frames > 0 && state->frame_count >= state->max_frames) nya_app_get()->should_quit = true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYER: RENDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Which way the sun's light travels. Kept in one place so the sky disc, the shading and the shafts agree. */
NYA_INTERNAL f32x3 sun_travel(void) {
    // low and coming from over the far end of the valley, so the beams rake across the water and through the trees.
    return nya_vector_normalize((f32x3){ -0.55F, -0.42F, 0.16F });
}

/** Draws one plant of a given style at a bank position, sampling the one wind field at its base. */
NYA_INTERNAL void draw_plant(NYA_Window* window, NYA_ConstCString handle, NYA_FoliageStyle style, f32x3 position, NYA_Color tint) {
    Showcase* state = showcase();

    NYA_Render3DFoliage plant = nya_render3d_foliage_style(style);

    plant.wind = nya_wind_sample(&state->wind, position);
    plant.time = state->wind.time;
    plant.tint = tint;

    nya_render3d_foliage(window, handle, position, (f32x3){ 1, 1, 1 }, nya_quaternion_identity, plant);
}

/** The whole 3D scene, between begin and end, so the post chain can capture it for water, bloom and shafts. */
NYA_INTERNAL void draw_scene(NYA_Window* window) {
    Showcase* state = showcase();

    // a slow fly-through: a wide, low orbit of the valley that rises and falls and lets its target sweep, so the
    // river, both banks and the sun all pass through frame and the scene reads as a living world rather than a still.
    f32 t      = state->elapsed_s * 0.05F;
    f32 radius = 26.0F + (sinf(t * 0.7F) * 5.0F);

    f32x3 eye = {
        cosf(t) * radius,
        6.5F + (sinf(t * 1.3F) * 2.5F),
        (sinf(t) * radius * 0.6F) - 8.0F,
    };

    // look at a point drifting along the river, a little above the water, so the camera pans down the valley.
    f32x3 target = { sinf(t * 0.5F) * 12.0F, 1.2F, 0.0F };

    nya_render3d_begin(window, (NYA_Camera3DPerspective){ .position = eye, .target = target, .far_plane = 300.0F });

    f32x3 sun = sun_travel();

    // the sky behind everything, its sun aligned with the directional light and the shafts.
    nya_render3d_sky_draw(window, (NYA_Render3DSky){
                                      .zenith        = { 0.20F, 0.34F, 0.58F, 1.0F },
                                      .horizon       = { 0.62F, 0.66F, 0.66F, 1.0F },
                                      .ground        = { 0.14F, 0.14F, 0.15F, 1.0F },
                                      .sun_direction = -sun,
                                      .sun_color     = { 1.0F, 0.86F, 0.62F, 1.0F },
                                      .sun_intensity = 1.5F,
                                      .sun_angle     = 0.04F,
                                  });

    // the one warm sun, low so the shadows are long and the beams rake.
    nya_render3d_light_set(window, (NYA_Render3DLight){
                                       .direction = sun,
                                       .color     = { 1.0F, 0.94F, 0.82F, 1.0F },
                                       .ambient   = 0.38F,
                                       .intensity = 1.05F,
                                       .sky       = { 0.46F, 0.56F, 0.72F, 1.0F },
                                       .ground    = { 0.28F, 0.26F, 0.20F, 1.0F },
                                   });

    // distance and height fog with aerial perspective: the depth cue a flat-shaded valley otherwise lacks, and
    // what lets the far banks recede into the sky's hue.
    nya_render3d_fog_set(window, (NYA_Render3DFog){
                                     .color          = { 0.62F, 0.66F, 0.68F, 1.0F },
                                     .density        = 0.010F,
                                     .height_falloff = 0.04F,
                                     .sun_amount     = 0.35F,
                                     .aerial         = 0.75F,
                                 });

    if (!state->meshes_ready) {
        nya_render3d_end(window);
        return;
    }

    // the creature and the disturber it presses into the foliage. Fed here, after begin and before the plants,
    // because disturbers are cleared at begin — this is where physics reaches the sway.
    nya_entity_foreach_kind (ENTITY_CREATURE, creature) {
        f32x3 at = nya_entity_render_position(creature);

        nya_render3d_foliage_disturb(window, at, CREATURE_DISTURB_RADIUS, CREATURE_DISTURB_STRENGTH);
        nya_render3d_sphere(window, at, CREATURE_RADIUS, (NYA_Color){ 0.85F, 0.42F, 0.34F, 1.0F });
    }

    // the ground, opaque, drawn first so the water's refraction capture sees the bed behind the surface.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .roughness = 1.0F });
    nya_render3d_mesh(window, MESH_TERRAIN, f32x3_zero, (f32x3){ 1, 1, 1 }, nya_quaternion_identity, NYA_COLOR_WHITE);

    // a few boulders on the bed, so the refraction bends over something with shape and colour.
    for (u32 i = 0; i < 7; i++) {
        f32       fx = ((f32)i - 3.0F) * 9.0F;
        f32       fz = sinf((f32)i * 1.7F) * 1.8F;
        f32       fy = terrain_height(fx, fz);
        NYA_Color c  = { 0.44F + (0.08F * (f32)(i % 3)), 0.40F, 0.34F, 1.0F };

        nya_render3d_sphere(window, (f32x3){ fx, fy + 0.25F, fz }, 0.42F, c);
    }

    // the foliage on the banks: grass, leaves and branches, all bent by the same field sampled at each base.
    for (u32 i = 0; i < PLANT_COUNT; i++) {
        b8    on_bank;
        f32x3 at = plant_position(i, &on_bank);

        if (!on_bank) continue;

        switch (i % 3) {
            case 0: draw_plant(window, MESH_GRASS, NYA_FOLIAGE_GRASS, at, (NYA_Color){ 0.6F, 1.0F, 0.6F, 1.0F }); break;
            case 1: draw_plant(window, MESH_LEAVES, NYA_FOLIAGE_LEAVES, at, (NYA_Color){ 0.8F, 1.0F, 0.7F, 1.0F }); break;
            default: draw_plant(window, MESH_BRANCH, NYA_FOLIAGE_BRANCHES, at, NYA_COLOR_WHITE); break;
        }
    }

    // the drifting pollen, through the sorted transparent pass, before the water.
    nya_particles_draw(window, state->dust);

    // a glossy, reflective surface for the sun glint, set just before the water so it applies to it alone.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.85F, .roughness = 0.25F, .reflectance = 0.4F });

    NYA_Render3DWater river = {
        .flow_direction = { 1.0F, 0.0F, 0.0F },
        .flow_speed     = 1.2F,
        .wave_amplitude = 0.12F,
        .wave_frequency = 0.5F,
        .choppiness     = 0.4F,
        .deep_color     = { 0.03F, 0.12F, 0.18F, 0.90F },
        .shallow_color  = { 0.12F, 0.36F, 0.42F, 0.35F },
        .opacity        = 0.85F,
        .refraction     = 0.55F,
        .foam           = 0.20F,
    };

    // the same wind field hurries the flow and lifts the chop: water shares the air with foliage and pollen.
    river.wind           = nya_wind_sample(&state->wind, f32x3_zero);
    river.wind_influence = 0.55F;

    nya_render3d_water(window, MESH_RIVER, f32x3_zero, (f32x3){ 1, 1, 1 }, nya_quaternion_identity, river);

    nya_render3d_end(window);
}

void showcase_layer_on_render(NYA_Window* window) {
    nya_assert(window != nullptr);

    Showcase* state = showcase();

    // fit the sun's cascades around the valley centre, ahead of the camera path.
    nya_render3d_shadow_set(window, (NYA_Render3DShadowFit){ .near_distance = 0.1F, .range = 120.0F, .strength = 0.5F });

    // HDR so the sunlit surf and the shafts lift above one for the bloom and the beams to catch.
    nya_render_output_set(window, (NYA_RenderOutput){ .hdr = true });

    // the scene target keeps its depth, for the 3D pass and so the water refraction reads a resolved image.
    state->post.scene = (NYA_RenderTextureOptions){ .depth = NYA_RENDER_TEXTURE_DEPTH_ATTACHED };

    // through the chain (a render texture, so refraction, bloom and the light shafts have a target); straight to
    // the window only if the chain cannot be set up this frame, where the water falls back to its colour.
    if (nya_post_begin(window, &state->post)) {
        draw_scene(window);
        nya_post_end(window, &state->post, nullptr, 0);
    } else {
        draw_scene(window);
        nya_render_output_scene_end(window);
    }

    state->last_fps = nya_app_get()->frame_stats.fps;

    nya_render2d_textf_with_font(window, HUD_FONT, HUD_FONT_SIZE, 12.0F, 12.0F, NYA_COLOR_WHITE,
                                 "wind · water · pollen · sky · light beams   %.1f fps   Esc quit", (f64)state->last_fps);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MAIN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();

    NYA_EXPECT(nya_app_init(.app_id = "showcase"), "while starting the engine");

    // the scene's root lives on the world, so it shares the world's arena and lifetime.
    Showcase* state = nya_arena_alloc(nya_world()->allocator, sizeof(Showcase));

    *state = (Showcase){ .window = NYA_WINDOW_HANDLE_NONE };

    // one gently gusting field, blowing down the valley — the single source of every motion in the scene.
    state->wind = nya_wind_field((NYA_WindOptions){ .direction = { 1.0F, 0.0F, 0.25F }, .strength = 2.6F, .gustiness = 0.6F });

    // an optional frame budget, so a headless or CI run draws a fixed number of frames and then quits.
    NYA_ConstCString frames = getenv("NYA_SHOWCASE_FRAMES");
    if (frames != nullptr) state->max_frames = (u32)strtoul(frames, nullptr, 10);

    nya_world_user_data_set(state);

    state->window = nya_window_create(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_RESIZABLE);
    nya_assert(nya_window_is_valid(state->window), "the window could not be created");

    nya_layer_push(state->window, nya_layer_of(showcase_layer, LAYER_ID));

    nya_app_run();

    nya_app_deinit();

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}

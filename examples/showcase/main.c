/**
 * @file examples/showcase/main.c
 *
 * One large living valley that composes every atmospheric system the renderer grew into a single scene, in the
 * flat, stylized look of gnyame's 3D cube demo (a restrained palette, one soft shading step, never photoreal),
 * only bigger and lusher: a wide tiled heightfield of hills and uplands with a river carving the valley floor,
 * grass, leaves and branches swaying on the banks, pollen drifting on the air, a herd of physics balls rolling
 * across the terrain that part the grass and leave fading trails on the ground behind them, translucent crystals
 * that refract the scene, a warm low sun casting long cascaded shadows, and volumetric light beams streaming from
 * that sun through it all — the whole 3D pass drawn through the cube3d-style post chain (bloom, aerial fog, light
 * shafts, HDR tonemap).
 *
 * ```
 * ./build run example showcase
 * NYA_SHOWCASE_FRAMES=6 ./build run example showcase   # draw six frames and quit, for a headless run
 * ```
 *
 * ## What ties it together
 *
 * A single NYA_WindField (render_wind.h) is the one source of motion. It is advanced once a frame and then read
 * four ways: sampled per plant to bend the foliage, sampled at the surface to hurry the water's flow and lift its
 * chop, sampled per particle (through the dust system's on_update hook) so the pollen literally rides the same
 * air, and combined with the rolling balls' foliage disturbers so wind and creatures part the same grass. The sun
 * that lights and shadows the scene is the sun the sky paints and the sun the light-shaft post pass gathers
 * toward, so the beams line up with the shading.
 *
 * The terrain is tiled so it can be both large and detailed under the sixteen-bit index cap, and each tile carries
 * a two-rung LOD chain (render_lod.h): near tiles draw full, far tiles draw a coarse mesh, and nya_render3d_mesh
 * resolves which by the camera's distance to each tile. The balls roll on a single static heightfield collider
 * built from the same terrain_height() the visual mesh is, so physics and picture agree.
 *
 * The whole 3D scene is drawn through a post chain (a render texture): the water and the crystals need a resolved
 * image to refract, and the light shafts and bloom are scene features nya_post_end runs over that same target.
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
#define MESH_RIVER  "showcase/river"
#define MESH_GRASS  "showcase/grass_tuft"
#define MESH_LEAVES "showcase/leaf_bush"
#define MESH_BRANCH "showcase/branch"

/** The valley: the river runs along x, the banks rise out along z, the terrain reaches this far each way. */
#define WORLD_HALF_X 92.0F
#define WORLD_HALF_Z 70.0F

/** The band the channel carves through the middle: how wide the whole cut is, and how far the flat water reaches. */
#define RIVER_WIDTH 22.0F
#define WATER_HALF  6.0F

/** How deep the channel bed sits below the water plane (y = 0), and how high the banks stand above it. */
#define BED_DEPTH   1.8F
#define BANK_HEIGHT 2.6F

/**
 * The terrain is a grid of tiles, each its own registered mesh with an LOD chain, so the world can be far larger
 * and more detailed than one mesh's sixteen-bit indices allow. Tiles across the valley by tiles along it.
 * */
#define TILE_COLS 7u
#define TILE_ROWS 5u

/** Cells per tile at full detail and at the coarse (distant) rung. Full stays well under the index cap. */
#define TILE_FULL_CELLS   22u
#define TILE_COARSE_CELLS 7u

static_assert((TILE_FULL_CELLS + 1u) * (TILE_FULL_CELLS + 1u) * 6u <= 65536, "a full tile must fit sixteen-bit indices");

/** Where the LOD chain switches a tile from its full mesh to its coarse one, and how far it stays drawn at all. */
#define TILE_LOD_NEAR 62.0F
#define TILE_LOD_FAR  9000.0F

/** The river surface grid: cells along the valley by cells across it. Under the 16-bit index cap. */
#define RIVER_COLS 180u
#define RIVER_ROWS 24u

static_assert(RIVER_COLS * RIVER_ROWS * 6u <= 65536, "the river mesh must fit sixteen-bit indices");

/** The physics heightfield's grid resolution. Coarser than the visual mesh; the balls roll on a smoothed valley. */
#define GROUND_POINTS_X 121u
#define GROUND_POINTS_Z 91u

/** The widest built mesh, so every tile, the river and the ground grid stage through one fixed scratch buffer. */
#define MESH_VERTICES_MAX 30000u

/** The most vertices any one built plant uses. */
#define PLANT_VERTICES_MAX 4096u

/** How many plants scatter over the banks. Each is one foliage draw; placement is hashed, not stored. */
#define PLANT_COUNT 900u

/** Deterministic scatter seed, so the same banks grow the same plants every run. */
#define SCATTER_SEED 0x5C0FF3u

/** The dust/pollen pool, and how many motes each timed burst releases. */
#define DUST_POOL      6000u
#define DUST_PER_BURST 70u

/** The herd of balls that roll the banks: dynamic spheres, steered to wander and re-homed if they stray. */
#define BALL_COUNT             6u
#define BALL_RADIUS            0.7F
#define BALL_ACCEL             9.0F
#define BALL_MAX_SPEED         5.5F
#define BALL_DISTURB_RADIUS    2.6F
#define BALL_DISTURB_STRENGTH  1.0F
/** The near bank the herd keeps to, as an offset from the water's edge, and how far up the bank they may wander. */
#define BALL_BANK_MIN (WATER_HALF + 2.4F)
#define BALL_BANK_MAX (WORLD_HALF_Z - 8.0F)

/**
 * The ground trail each ball leaves: a fading decal dropped on a timer, kept in one ring so the whole herd's
 * marks share a fixed budget under the decal ceiling. A mark shrinks away over its life.
 * */
#define TRAIL_MARK_COUNT 96u
#define TRAIL_INTERVAL_S 0.28F
#define TRAIL_LIFETIME_S 4.0F

/** The 2x2 decal atlas the marks stamp from, and which cell (2 is the soft blob, as in the cube3d demo). */
#define TRAIL_TEXTURE NYA_ASSET_TEXTURES_DECALS_PNG
#define TRAIL_CELL    2u

/** The translucent crystals on the banks: how many, and their glass look, drawn after the opaque pass. */
#define CRYSTAL_COUNT 10u

#define HUD_FONT      NYA_ASSET_FONTS_ALDRICH_TTF
#define HUD_FONT_SIZE 16.0F

enum {
    ENTITY_NONE = 0,
    ENTITY_GROUND,
    ENTITY_BALL,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct TrailMark TrailMark;

/** One fading ground mark left by a rolling ball. Position and rotation drape the decal; born_s ages it out. */
struct TrailMark {
    f32x3     position;
    f32       rotation;
    f32       size;
    NYA_Color color;
    f32       born_s;
};

typedef struct Showcase Showcase;

struct Showcase {
    NYA_WindowHandle window;

    /** The one wind field the whole scene animates from: foliage sway, water chop and drifting pollen all read it. */
    NYA_WindField wind;

    /** The drifting pollen. Lives on the world arena, so it goes with the world; its motes ride `wind`. */
    NYA_ParticleSystem* dust;

    /** The post chain: the scene renders into its texture, which the water refraction, bloom and shafts read. */
    NYA_PostChain post;

    /**
     * The tile mesh handles, in persistent storage: the LOD registry keeps the pointer it is handed, not a copy,
     * so the handle strings must outlive the chain — a per-frame stack buffer would dangle. Indexed row-major.
     * */
    char tile_near[TILE_COLS * TILE_ROWS][NYA_RENDER3D_MESH_HANDLE_MAX];
    char tile_far[TILE_COLS * TILE_ROWS][NYA_RENDER3D_MESH_HANDLE_MAX];

    /** The rolling balls, their tints, and a per-ball phase so the herd wanders out of lockstep. */
    NYA_EntityHandle balls[BALL_COUNT];
    NYA_Color        ball_color[BALL_COUNT];
    f32              ball_phase[BALL_COUNT];

    /** The herd's shared trail marks, a ring so the newest overwrite the oldest under the decal budget. */
    TrailMark trail[TRAIL_MARK_COUNT];
    u32       trail_next;
    f32       trail_timer_s;

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

/** A tile's full-detail mesh handle, into a caller's buffer. The LOD chain is registered under this handle. */
NYA_INTERNAL NYA_ConstCString tile_handle(char* out, u64 out_size, u32 col, u32 row) {
    (void)snprintf(out, out_size, "showcase/tile/%u_%u", col, row);
    return out;
}

/** A tile's coarse (distant) mesh handle. */
NYA_INTERNAL NYA_ConstCString tile_handle_far(char* out, u64 out_size, u32 col, u32 row) {
    (void)snprintf(out, out_size, "showcase/tile/%u_%u/far", col, row);
    return out;
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
 * The ground height at a world xz: a channel dipping below the water plane between banks that rise above it, with
 * rolling hills that only lift the banks and uplands (so the channel bed stays calm for the water) and a faint bed
 * ripple for the refraction to bend over. One shared function, so the visual tiles and the physics heightfield are
 * the same surface.
 * */
NYA_INTERNAL f32 terrain_height(f32 x, f32 z) {
    // 0 down the centre line, 1 out past the bank shoulder.
    f32 across  = nya_clamp(fabsf(z) / (RIVER_WIDTH * 0.5F), 0.0F, 1.0F);
    f32 profile = example_smoothstep(0.30F, 0.72F, across);

    // the bed climbs from the channel floor up to the bank top across the middle band.
    f32 channel = nya_lerp(-BED_DEPTH, BANK_HEIGHT, profile);

    // eroded hills, faded in by `profile` so they raise the banks and uplands but leave the wet channel smooth. A
    // second, longer octave gives the wide world a few distinct ridges rather than one repeating swell.
    f32 hills = (sinf(x * 0.055F) * 2.6F) + (cosf((z * 0.045F) + 1.3F) * 2.0F) + (sinf((x + z) * 0.085F) * 1.1F)
                + (cosf((x * 0.020F) - (z * 0.017F)) * 3.2F);

    // a shallow ripple, strongest in the channel, so the water's refraction has relief to distort.
    f32 ripple = 0.10F * sinf(x * 0.7F) * cosf(z * 0.6F);

    return channel + (hills * profile) + (ripple * (1.0F - profile));
}

/** The bed colour: sandy in the wet channel, grassy up the banks, muted olive on the uplands. */
NYA_INTERNAL f32x3 terrain_color(f32 x, f32 z) {
    f32 across  = nya_clamp(fabsf(z) / (RIVER_WIDTH * 0.5F), 0.0F, 1.0F);
    f32 profile = example_smoothstep(0.32F, 0.70F, across);

    f32x3 sand = { 0.52F, 0.45F, 0.32F };
    f32x3 bank = { 0.26F, 0.42F, 0.21F };

    f32x3 base = nya_lerp(sand, bank, profile);

    // a touch drier and paler on the high uplands, so the far banks read as distinct ground.
    f32   upland = example_smoothstep(0.75F, 1.0F, across);
    f32x3 dry    = { 0.36F, 0.40F, 0.26F };

    f32x3 color = nya_lerp(base, dry, upland * 0.6F);

    // a faint large-scale mottle, so the flat-shaded uplands are not one solid green over the whole wide world.
    f32 mottle = 0.05F * sinf((x * 0.03F) + (z * 0.04F));

    return (f32x3){ nya_clamp(color.x + mottle, 0.0F, 1.0F), nya_clamp(color.y + mottle, 0.0F, 1.0F),
                    nya_clamp(color.z + (mottle * 0.5F), 0.0F, 1.0F) };
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

/**
 * One terrain tile as a heightfield, built in local space about its own centre (so the LOD chain can place it with
 * a distinct draw position and pick a rung by distance). `cells` is the resolution: the same tile is built once
 * full and once coarse. Normals follow the relief per triangle, for the faceted flat look.
 * */
NYA_INTERNAL u32 build_tile(NYA_Vertex3D* vertices, f32 world_x0, f32 world_x1, f32 world_z0, f32 world_z1, u32 cells) {
    u32 count = 0;

    f32 cx = (world_x0 + world_x1) * 0.5F;
    f32 cz = (world_z0 + world_z1) * 0.5F;

    for (u32 row = 0; row < cells; row++) {
        for (u32 col = 0; col < cells; col++) {
            f32 wx0 = nya_lerp(world_x0, world_x1, (f32)col / (f32)cells);
            f32 wx1 = nya_lerp(world_x0, world_x1, (f32)(col + 1) / (f32)cells);
            f32 wz0 = nya_lerp(world_z0, world_z1, (f32)row / (f32)cells);
            f32 wz1 = nya_lerp(world_z0, world_z1, (f32)(row + 1) / (f32)cells);

            // local xz about the tile centre; y stays absolute, so the draw at (cx, 0, cz) rebuilds world height.
            f32x3 p00 = { wx0 - cx, terrain_height(wx0, wz0), wz0 - cz };
            f32x3 p10 = { wx1 - cx, terrain_height(wx1, wz0), wz0 - cz };
            f32x3 p11 = { wx1 - cx, terrain_height(wx1, wz1), wz1 - cz };
            f32x3 p01 = { wx0 - cx, terrain_height(wx0, wz1), wz1 - cz };

            f32x3 k00 = terrain_color(wx0, wz0);
            f32x3 k10 = terrain_color(wx1, wz0);
            f32x3 k11 = terrain_color(wx1, wz1);
            f32x3 k01 = terrain_color(wx0, wz1);

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
        f32x3 across = { cosf(angle + 1.5708F) * width, 0.0F, sinf(angle + 1.5708F) * width };
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
    *out_on_bank = (y > 0.15F) && (y < BANK_HEIGHT + 5.0F);

    return (f32x3){ x, y, z };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DUST / POLLEN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Steers each pollen mote after its integration: ease its velocity toward the shared wind field's push at its own
 * position, so the dust drifts on exactly the air that bends the foliage and hurries the water. `user_data` is the
 * scene's NYA_WindField.
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
 * DECAL PROBE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Where a downward-projected decal lands: the valley surface at the decal's xz. Decals only probe straight down,
 * which a height lookup answers without a raycast — the same terrain_height() the tiles and the physics ground are.
 * The normal comes from the slope a step either side.
 * */
NYA_INTERNAL b8 ground_decal_probe(f32x3 origin, f32x3 direction, void* user_data, OUT f32x3* out_point, OUT f32x3* out_normal) {
    nya_unused(user_data);

    if (fabsf(origin.x) > WORLD_HALF_X || fabsf(origin.z) > WORLD_HALF_Z) return false;

    f32 height = terrain_height(origin.x, origin.z);

    // the ground must fall inside the box the decal is projected through, top (origin.y) to bottom.
    if (height > origin.y || height < origin.y + direction.y) return false;

    const f32 step    = 0.5F;
    f32       slope_x = terrain_height(origin.x + step, origin.z) - terrain_height(origin.x - step, origin.z);
    f32       slope_z = terrain_height(origin.x, origin.z + step) - terrain_height(origin.x, origin.z - step);

    *out_point  = (f32x3){ origin.x, height, origin.z };
    *out_normal = nya_vector_normalize((f32x3){ -slope_x, 2.0F * step, -slope_z });

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BALLS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A deterministic bank spawn for ball `index`: spread along the valley, on one bank or the other. */
NYA_INTERNAL f32x3 ball_spawn(u32 index) {
    f32 u    = (f32)index / (f32)BALL_COUNT;
    f32 x    = nya_lerp(-WORLD_HALF_X + 8.0F, WORLD_HALF_X - 8.0F, u);
    f32 side = (index % 2u == 0u) ? -1.0F : 1.0F;
    f32 z    = side * nya_lerp(BALL_BANK_MIN + 1.0F, BALL_BANK_MAX - 1.0F, plant_unit(index, 7));

    return (f32x3){ x, terrain_height(x, z) + BALL_RADIUS + 0.5F, z };
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

    // one scratch arena builds every mesh and the physics heights; the GPU and Box3D keep their own copies, so it
    // frees after registering. Big enough for the widest single mesh (a full tile or the river) staged at a time.
    NYA_Arena* scratch = nya_arena_create(.name = "showcase_build");
    defer nya_arena_destroy(scratch);

    NYA_Vertex3D* vertices = nya_arena_alloc(scratch, MESH_VERTICES_MAX * sizeof(NYA_Vertex3D));
    NYA_RNG*      rng      = nya_rng_create_in(scratch, "5A0FF3E000000001");

    b8 ok = true;

    // the terrain, tiled: each tile is built full and coarse, both registered, and an LOD chain under the full
    // handle lets nya_render3d_mesh pick the rung by the camera's distance to that tile. Reusing one scratch
    // buffer means a T-junction can crack where a full tile meets a coarse one, but the fog and the stylized flat
    // shading hide it at the distance the switch happens.
    for (u32 row = 0; row < TILE_ROWS; row++) {
        for (u32 col = 0; col < TILE_COLS; col++) {
            f32 wx0 = nya_lerp(-WORLD_HALF_X, WORLD_HALF_X, (f32)col / (f32)TILE_COLS);
            f32 wx1 = nya_lerp(-WORLD_HALF_X, WORLD_HALF_X, (f32)(col + 1) / (f32)TILE_COLS);
            f32 wz0 = nya_lerp(-WORLD_HALF_Z, WORLD_HALF_Z, (f32)row / (f32)TILE_ROWS);
            f32 wz1 = nya_lerp(-WORLD_HALF_Z, WORLD_HALF_Z, (f32)(row + 1) / (f32)TILE_ROWS);

            u32 idx = (row * TILE_COLS) + col;

            // formatted into the state's persistent storage, so the pointers the LOD chain keeps stay valid.
            NYA_ConstCString near_handle = tile_handle(state->tile_near[idx], sizeof(state->tile_near[idx]), col, row);
            NYA_ConstCString far_handle  = tile_handle_far(state->tile_far[idx], sizeof(state->tile_far[idx]), col, row);

            ok = nya_render3d_mesh_register(window, near_handle, vertices, build_tile(vertices, wx0, wx1, wz0, wz1, TILE_FULL_CELLS)) && ok;
            ok = nya_render3d_mesh_register(window, far_handle, vertices, build_tile(vertices, wx0, wx1, wz0, wz1, TILE_COARSE_CELLS)) && ok;

            ok = nya_render3d_lod_register(near_handle,
                                           (NYA_Render3DLodLevel[]){
                                               { .handle = near_handle, .max_distance = TILE_LOD_NEAR },
                                               { .handle = far_handle, .max_distance = TILE_LOD_FAR },
                                           },
                                           2)
                 && ok;
        }
    }

    ok = nya_render3d_mesh_register(window, MESH_RIVER, vertices, build_river(vertices)) && ok;
    ok = nya_render3d_mesh_register(window, MESH_GRASS, vertices, build_grass(vertices, rng)) && ok;
    ok = nya_render3d_mesh_register(window, MESH_LEAVES, vertices, build_leaves(vertices, rng)) && ok;
    ok = nya_render3d_mesh_register(window, MESH_BRANCH, vertices, build_branch(vertices)) && ok;

    state->meshes_ready = ok;
    if (!ok) nya_log_error("showcase: a mesh or LOD chain failed to register; the scene will draw incomplete.");

    // the drifting pollen, from the world arena so it shares the world's lifetime. Its on_update reads the one
    // wind field, which is why the motes travel with the same air as the grass and the water.
    state->dust = nya_particles_create(nya_world()->allocator, DUST_POOL);
    nya_particles_space_set(state->dust, NYA_PARTICLE_SPACE_3D);
    nya_particles_texture_set(state->dust, NYA_ASSET_TEXTURES_PUFF_PNG);
    nya_particles_on_update_set(state->dust, dust_ride_wind, &state->wind);

    // the sun's cascaded shadows, tuned like the cube3d demo: three cascades, a large map, and a cool tinted
    // shade so shadows read blue rather than merely dark, keeping the stylized look.
    nya_render3d_shadow_options_set(window, (NYA_Render3DShadowOptions){
                                                .cascades = NYA_RENDER3D_SHADOW_CASCADES,
                                                .map_size = 2048,
                                                .color    = { 0.10F, 0.12F, 0.20F, 0.55F },
                                            });

    // the cube3d-style bloom: threshold and spread that let the sunlit surf and the crystals' glints bleed.
    nya_post_bloom_set(window, (NYA_PostBloom){ .enabled = true, .threshold = 0.78F, .intensity = 1.10F, .spread = 3.0F });

    // the light beams: volumetric shafts gathered from the sky's sun through the scene. This is a scene feature
    // nya_post_end runs over the post target, so it needs the scene drawn through the chain (it is, below).
    nya_post_light_shafts_set(window, (NYA_PostLightShafts){ .enabled = true, .intensity = 1.0F, .length = 0.8F });

    // the trails the balls leave land as decals on the ground: switch decals on, say what they land on (the same
    // valley surface), and preload the atlas so the first mark is not blank while it loads.
    nya_render3d_decals_set(window, (NYA_Render3DDecals){ .enabled = true });
    nya_render3d_decal_probe_set(window, nya_callback(ground_decal_probe), nullptr);
    (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = TRAIL_TEXTURE });

    // 3D is y up, so gravity is negative y; the balls fall onto the heightfield and roll.
    nya_physics3d_gravity_set(NYA_PHYSICS3D_GRAVITY_DEFAULT);

    // one static heightfield collider for the whole valley, from the same terrain_height() the tiles are, so the
    // balls roll on exactly the ground that is drawn. The grid's point [0,0] is the body's origin and it extends
    // along +x, +z, x fastest; Box3D quantises the heights at creation, so the scratch array can go after attach.
    f32* heights   = nya_arena_alloc(scratch, (u64)GROUND_POINTS_X * (u64)GROUND_POINTS_Z * sizeof(f32));
    f32  cell_x    = (2.0F * WORLD_HALF_X) / (f32)(GROUND_POINTS_X - 1);
    f32  cell_z    = (2.0F * WORLD_HALF_Z) / (f32)(GROUND_POINTS_Z - 1);

    for (u32 j = 0; j < GROUND_POINTS_Z; j++) {
        for (u32 i = 0; i < GROUND_POINTS_X; i++) {
            f32 wx                            = -WORLD_HALF_X + ((f32)i * cell_x);
            f32 wz                            = -WORLD_HALF_Z + ((f32)j * cell_z);
            heights[(j * GROUND_POINTS_X) + i] = terrain_height(wx, wz);
        }
    }

    NYA_EntityHandle ground = nya_entity_spawn(.name = "valley", .type = ENTITY_GROUND, .position = { -WORLD_HALF_X, 0.0F, -WORLD_HALF_Z });
    (void)nya_physics3d_body_attach(ground, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_HEIGHTFIELD, .heights = heights,
                                    .height_count_x = GROUND_POINTS_X, .height_count_z = GROUND_POINTS_Z,
                                    .height_cell_size = { cell_x, cell_z }, .friction = 0.7F);

    // the herd: dynamic spheres given a shove along the valley, each with its own colour and wander phase. They
    // cast into the shadow cascades like any mesh, part the grass as foliage disturbers, and lay down trails.
    static const NYA_Color palette[] = {
        { 0.86F, 0.38F, 0.32F, 1.0F }, { 0.36F, 0.62F, 0.86F, 1.0F }, { 0.92F, 0.78F, 0.36F, 1.0F },
        { 0.52F, 0.80F, 0.46F, 1.0F }, { 0.78F, 0.50F, 0.82F, 1.0F }, { 0.40F, 0.78F, 0.78F, 1.0F },
    };

    for (u32 i = 0; i < BALL_COUNT; i++) {
        state->ball_color[i] = palette[i % nya_carray_length(palette)];
        state->ball_phase[i] = plant_unit(i, 9) * 6.2831853F;

        state->balls[i] = nya_entity_spawn(.name = "ball", .type = ENTITY_BALL, .position = ball_spawn(i));
        (void)nya_physics3d_body_attach(state->balls[i], .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS3D_SHAPE_SPHERE,
                                        .radius = BALL_RADIUS, .density = 320.0F, .friction = 0.6F, .restitution = 0.2F);

        NYA_Entity* body = nya_entity_get(state->balls[i]);
        if (body != nullptr) nya_physics3d_velocity_set(body, (f32x3){ BALL_MAX_SPEED * 0.5F, 0.0F, 0.0F });
    }
}

void showcase_layer_on_destroy(NYA_Window* window) {
    Showcase* state = showcase();

    // turn every scene feature this layer switched on back off, so a re-enter (a hot reload) starts clean.
    nya_post_bloom_set(window, (NYA_PostBloom){ 0 });
    nya_post_light_shafts_set(window, (NYA_PostLightShafts){ 0 });
    nya_render3d_decals_set(window, (NYA_Render3DDecals){ 0 });

    nya_post_chain_destroy(&state->post);

    // the balls and the valley collider; despawning takes their bodies.
    for (u32 i = 0; i < BALL_COUNT; i++) nya_entity_despawn_deferred(state->balls[i]);

    nya_entity_foreach_kind (ENTITY_GROUND, ground) nya_entity_despawn_deferred(ground->handle);

    // every tile's LOD chain, then every registered mesh's GPU buffers; the pollen pool lives on the world arena.
    nya_render3d_lod_clear();

    for (u32 i = 0; i < TILE_COLS * TILE_ROWS; i++) {
        nya_render3d_mesh_release(window, state->tile_near[i]);
        nya_render3d_mesh_release(window, state->tile_far[i]);
    }

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

/** Steers ball `index` for one tick: a wandering push along the bank, corralled away from the water and the rim. */
NYA_INTERNAL void ball_steer(Showcase* state, u32 index, f32 delta_time_s) {
    nya_unused(delta_time_s);

    NYA_Entity* body = nya_entity_get(state->balls[index]);
    if (body == nullptr) return;

    f32x3 at = nya_entity_render_position(body);

    // a ball that rolled off the rim or into the river is re-homed to its bank spawn, so the herd keeps rolling.
    b8 off_bank = fabsf(at.z) < BALL_BANK_MIN || fabsf(at.z) > BALL_BANK_MAX || fabsf(at.x) > WORLD_HALF_X - 4.0F
                  || at.y < -BED_DEPTH - 4.0F;

    if (off_bank) {
        nya_physics3d_teleport(body, ball_spawn(index), nya_quaternion_identity);
        nya_physics3d_velocity_set(body, (f32x3){ BALL_MAX_SPEED * 0.5F, 0.0F, 0.0F });
        return;
    }

    // a heading that drifts on its own clock, so the herd wanders rather than marching in a line.
    f32   heading = state->ball_phase[index] + (state->elapsed_s * 0.35F) + (sinf(state->elapsed_s * 0.6F + state->ball_phase[index]) * 1.4F);
    f32x3 push    = { cosf(heading), 0.0F, sinf(heading) };

    // corral: bias the push back toward the middle of the bank band it belongs to, so it never sticks at an edge.
    f32 side       = at.z >= 0.0F ? 1.0F : -1.0F;
    f32 bank_mid   = side * ((BALL_BANK_MIN + BALL_BANK_MAX) * 0.5F);
    push.z        += nya_clamp((bank_mid - at.z) * 0.15F, -1.0F, 1.0F);
    push.x        += nya_clamp((-at.x) * 0.01F, -0.6F, 0.6F);

    push = nya_vector_normalize(push);

    nya_physics3d_apply_force(body, push * (BALL_ACCEL * 320.0F));

    // cap the horizontal roll so the wander stays a stroll: bleed off speed past the limit with a counter-force.
    f32x3 velocity   = nya_physics3d_velocity(body);
    f32x3 horizontal = { velocity.x, 0.0F, velocity.z };
    f32   speed      = nya_vector_length(horizontal);

    if (speed > BALL_MAX_SPEED) nya_physics3d_apply_force(body, horizontal * (-(speed - BALL_MAX_SPEED) * 260.0F));
}

void showcase_layer_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window);

    Showcase* state = showcase();

    // the one per-frame advance the whole scene animates from: foliage, water and pollen all sample this field.
    nya_wind_advance(&state->wind, delta_time_s);
    state->elapsed_s += delta_time_s;

    nya_particles_update(state->dust, delta_time_s);

    // release pollen on a timer, so its rate does not ride the frame rate. It spawns high over the valley and is
    // then carried by dust_ride_wind, so where it drifts is the wind's doing, not the burst's.
    state->emit_timer_s += delta_time_s;

    while (state->emit_timer_s >= 0.05F) {
        state->emit_timer_s -= 0.05F;

        (void)nya_particles_emit(state->dust, (NYA_ParticleBurst){
                                                  .shape       = NYA_PARTICLE_SHAPE_BOX,
                                                  .position    = { 0.0F, 8.0F, 0.0F },
                                                  .volume      = { WORLD_HALF_X * 1.5F, 6.0F, WORLD_HALF_Z * 1.3F },
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

    // the herd: each ball wanders, and on a timer drops a fading mark under itself into the shared ring, so the
    // trail is a stream of decals thinning behind it rather than one smear.
    for (u32 i = 0; i < BALL_COUNT; i++) ball_steer(state, i, delta_time_s);

    state->trail_timer_s += delta_time_s;

    while (state->trail_timer_s >= TRAIL_INTERVAL_S) {
        state->trail_timer_s -= TRAIL_INTERVAL_S;

        for (u32 i = 0; i < BALL_COUNT; i++) {
            const NYA_Entity* body = nya_entity_get(state->balls[i]);
            if (body == nullptr) continue;

            f32x3 at = nya_entity_render_position(body);

            // only mark firm bank, not the water's edge, so trails never float on the river.
            if (fabsf(at.z) < BALL_BANK_MIN - 0.5F) continue;

            f32 rotation = nya_ihash2((s32)(at.x * 32.0F), (s32)(at.z * 32.0F), SCATTER_SEED) * (f32)M_PI;

            // a scuff in the ball's own colour, dimmed and half-transparent, so the herd's trails read apart.
            NYA_Color color = state->ball_color[i];
            color.r        *= 0.7F;
            color.g        *= 0.7F;
            color.b        *= 0.7F;
            color.a         = 0.5F;

            state->trail[state->trail_next] = (TrailMark){
                .position = { at.x, terrain_height(at.x, at.z), at.z },
                .rotation = rotation,
                .size     = BALL_RADIUS * 2.2F,
                .color    = color,
                .born_s   = state->elapsed_s,
            };

            state->trail_next = (state->trail_next + 1u) % TRAIL_MARK_COUNT;
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

/** The herd's fading trails, draped as decals after the terrain they lie on; a mark shrinks away over its life. */
NYA_INTERNAL void draw_trails(NYA_Window* window) {
    Showcase* state = showcase();

    for (u32 i = 0; i < TRAIL_MARK_COUNT; i++) {
        const TrailMark* mark = &state->trail[i];

        f32 age = state->elapsed_s - mark->born_s;

        if (mark->born_s <= 0.0F || age >= TRAIL_LIFETIME_S) continue;

        // shrinks away over its last third rather than fading, so the decal's inked edge stays hard.
        f32 left = nya_clamp((TRAIL_LIFETIME_S - age) / (TRAIL_LIFETIME_S / 3.0F), 0.0F, 1.0F);
        f32 size = mark->size * left;

        if (size <= 0.0F) continue;

        nya_render3d_decal(window, (NYA_Render3DDecal){
                                       .texture  = TRAIL_TEXTURE,
                                       .center   = { mark->position.x, mark->position.y, mark->position.z },
                                       .size     = { size, 2.0F, size },
                                       .rotation = mark->rotation,
                                       .color    = mark->color,
                                       .columns  = 2,
                                       .rows     = 2,
                                       .cell     = (u8)TRAIL_CELL,
                                   });
    }
}

/** The whole 3D scene, between begin and end, so the post chain can capture it for water, crystals, bloom and shafts. */
NYA_INTERNAL void draw_scene(NYA_Window* window) {
    Showcase* state = showcase();

    // a slow, wide fly-through of the enlarged valley: a low orbit that rises and falls and lets its target sweep
    // down the river, so both banks, the herd and the sun all pass through frame and it reads as a living world.
    f32 t      = state->elapsed_s * 0.045F;
    f32 radius = 52.0F + (sinf(t * 0.7F) * 10.0F);

    f32x3 eye = {
        cosf(t) * radius,
        12.0F + (sinf(t * 1.3F) * 4.0F),
        (sinf(t) * radius * 0.6F) - 12.0F,
    };

    // look at a point drifting along the river, a little above the water, so the camera pans down the valley.
    f32x3 target = { sinf(t * 0.5F) * 26.0F, 1.6F, 0.0F };

    nya_render3d_begin(window, (NYA_Camera3DPerspective){ .position = eye, .target = target, .far_plane = 500.0F });

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
    // what lets the far tiles recede into the sky's hue as the LOD coarsens them.
    nya_render3d_fog_set(window, (NYA_Render3DFog){
                                     .color          = { 0.62F, 0.66F, 0.68F, 1.0F },
                                     .density        = 0.008F,
                                     .height_falloff = 0.04F,
                                     .sun_amount     = 0.35F,
                                     .aerial         = 0.75F,
                                 });

    if (!state->meshes_ready) {
        nya_render3d_end(window);
        return;
    }

    // the herd, and the disturbers they press into the foliage. Fed here, after begin and before the plants,
    // because disturbers are cleared at begin — this is where physics reaches the sway. The spheres are opaque, so
    // they are drawn with the ground's material and cast into the shadow cascades.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.0F, .roughness = 0.85F });

    for (u32 i = 0; i < BALL_COUNT; i++) {
        const NYA_Entity* body = nya_entity_get(state->balls[i]);
        if (body == nullptr) continue;

        f32x3 at = nya_entity_render_position(body);

        nya_render3d_foliage_disturb(window, at, BALL_DISTURB_RADIUS, BALL_DISTURB_STRENGTH);
        nya_render3d_sphere(window, at, BALL_RADIUS, state->ball_color[i]);
    }

    // the ground, opaque, drawn first so the water's and crystals' refraction capture sees the bed behind them.
    // Each tile is drawn at its own centre, so its LOD chain resolves the right rung by distance.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .roughness = 1.0F });

    for (u32 row = 0; row < TILE_ROWS; row++) {
        for (u32 col = 0; col < TILE_COLS; col++) {
            f32 cx = nya_lerp(-WORLD_HALF_X, WORLD_HALF_X, ((f32)col + 0.5F) / (f32)TILE_COLS);
            f32 cz = nya_lerp(-WORLD_HALF_Z, WORLD_HALF_Z, ((f32)row + 0.5F) / (f32)TILE_ROWS);

            nya_render3d_mesh(window, state->tile_near[(row * TILE_COLS) + col], (f32x3){ cx, 0.0F, cz }, (f32x3){ 1, 1, 1 },
                              nya_quaternion_identity, NYA_COLOR_WHITE);
        }
    }

    // the herd's fading trails, draped after the ground they lie on.
    draw_trails(window);

    // a few boulders on the bed, so the refraction bends over something with shape and colour.
    for (u32 i = 0; i < 9; i++) {
        f32       fx = ((f32)i - 4.0F) * 18.0F;
        f32       fz = sinf((f32)i * 1.7F) * 2.4F;
        f32       fy = terrain_height(fx, fz);
        NYA_Color c  = { 0.44F + (0.08F * (f32)(i % 3)), 0.40F, 0.34F, 1.0F };

        nya_render3d_sphere(window, (f32x3){ fx, fy + 0.25F, fz }, 0.5F, c);
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

    // the drifting pollen, through the sorted transparent pass, before the refractive surfaces.
    nya_particles_draw(window, state->dust);

    // the translucent crystals: the scene's opacity pass, drawn after the opaque solids so they refract and blur
    // the resolved image behind them, like the cube3d demo's glass. Grouped under one glass material, so the whole
    // cluster is one flush.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){
                                          .metallic    = 0.1F,
                                          .roughness   = 0.15F,
                                          .reflectance = 0.5F,
                                          .refraction  = 0.4F,
                                          .blur        = 0.08F,
                                          .edge        = 0.0F,
                                      });

    for (u32 i = 0; i < CRYSTAL_COUNT; i++) {
        f32 side = (i % 2u == 0u) ? -1.0F : 1.0F;
        f32 cx   = nya_lerp(-WORLD_HALF_X + 10.0F, WORLD_HALF_X - 10.0F, plant_unit(i, 11));
        f32 cz   = side * nya_lerp(WATER_HALF + 3.0F, WORLD_HALF_Z - 12.0F, plant_unit(i, 12));
        f32 base = terrain_height(cx, cz);
        f32 tall = nya_lerp(1.6F, 3.2F, plant_unit(i, 13));

        // a slim upright shard, tinted a pale glacial cyan; the alpha is what the opacity pass blends.
        nya_render3d_cube(window, (f32x3){ cx, base + (tall * 0.5F), cz }, (f32x3){ 0.7F, tall, 0.7F },
                          nya_quaternion_from_axis_angle((f32x3){ 0, 1, 0 }, plant_unit(i, 14) * 6.2831853F),
                          (NYA_Color){ 0.70F, 0.90F, 0.95F, 0.55F });
    }

    // a glossy, reflective surface for the sun glint, set just before the water so it applies to it alone.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.85F, .roughness = 0.22F, .reflectance = 0.55F });

    NYA_Render3DWater river = {
        .flow_direction = { 1.0F, 0.0F, 0.0F },
        .flow_speed     = 1.2F,
        .wave_amplitude = 0.14F,
        .wave_frequency = 0.5F,
        .choppiness     = 0.4F,
        .deep_color     = { 0.03F, 0.12F, 0.18F, 0.90F },
        .shallow_color  = { 0.12F, 0.36F, 0.42F, 0.35F },
        .opacity        = 0.82F,
        .refraction     = 0.6F,
        .foam           = 0.22F,
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

    // fit the sun's cascades around the valley centre, ahead of the camera path. Tuned like cube3d's strength.
    nya_render3d_shadow_set(window, (NYA_Render3DShadowFit){ .near_distance = 0.1F, .range = 180.0F, .strength = 0.45F });

    // HDR so the sunlit surf and the shafts lift above one for the bloom and the beams to catch; the output pass
    // tonemaps it back down, the last stage of the cube3d post stack.
    nya_render_output_set(window, (NYA_RenderOutput){ .hdr = true });

    // the scene target keeps its depth, for the 3D pass and so the water and crystal refraction read a resolved image.
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
                                 "wind · water · balls · trails · crystals · light beams   %.1f fps   Esc quit", (f64)state->last_fps);
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

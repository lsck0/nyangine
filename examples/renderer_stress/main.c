/**
 * @file examples/renderer_stress/main.c
 *
 * A deliberately oversized 3D scene whose only purpose is to make the renderer sweat: a large generated
 * terrain, thousands of instanced props behind an LOD chain, the point-light cap plus a cascaded-shadow
 * sun, three particle systems, a simulated smoke/fire volume, decals, an occlusion pass, a translucent
 * glass pass and the full post stack — bloom, depth of field, motion blur, eye adaptation, light shafts,
 * aerial perspective, and CRT / LUT / grayscale colour grading on top.
 *
 * ```
 * ./build run example renderer_stress
 * NYA_STRESS_SCALE=insane ./build run example renderer_stress
 * ./renderer_stress.example.bin --scale large      # when run by hand
 * ```
 *
 * ## What it is for
 *
 * Every heavy count is a knob, grouped into three named levels — `small`, `large`, `insane` — chosen from
 * `--scale`, the `NYA_STRESS_SCALE` environment variable, or the `1` `2` `3` keys at runtime. Each frame the
 * HUD prints frame time and FPS beside the renderer's own per-frame counters (draw calls, instances drawn,
 * frustum-culled, occlusion-culled, dropped draws) and the physics and fluid step times, and `Tab` cycles
 * the engine's built-in debug overlay through its stats / trace / systems pages so the CPU/GPU trace table,
 * the VRAM-by-kind gauges and the fixed-capacity `nya_ceiling` fullness lines are all visible. The point is
 * to see *which* resource saturates first as the scale climbs.
 *
 * It only calls the public renderer API; it registers no engine systems and edits no engine files. Wind,
 * foliage and water are intentionally absent — they land separately; see the TODO hook in the draw path.
 * */
#include "genyarated/assets.h"
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define WINDOW_TITLE  "nyangine — renderer stress"
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/** The layer's id, compared by content so it survives a code reload. */
#define LAYER_ID "renderer_stress"

/** Terrain: a square grid of quads centred on the origin, one registered mesh drawn once per frame. */
#define TERRAIN_CELLS     100
#define TERRAIN_CELL_SIZE 2.0F

/** Half the world the terrain covers, so props scatter inside it. */
#define TERRAIN_HALF (((f32)TERRAIN_CELLS * TERRAIN_CELL_SIZE) * 0.5F)

/** Six vertices a terrain quad, so the whole grid's vertex count. Kept under the sixteen-bit index limit. */
#define TERRAIN_VERTICES ((u32)TERRAIN_CELLS * (u32)TERRAIN_CELLS * 6u)

static_assert(TERRAIN_VERTICES <= 65536, "the terrain mesh must fit sixteen-bit indices");

/** The three prop LOD meshes and the base handle instanced draws pick between. */
#define PROP_MESH_L0 "nya_stress_prop_l0"
#define PROP_MESH_L1 "nya_stress_prop_l1"
#define PROP_MESH_L2 "nya_stress_prop_l2"

/** Widest prop mesh vertex list: an eight-sided, three-segment prism with both caps. Stack sized. */
#define PROP_VERTICES_MAX 2048u

/** Distinct opaque prop materials. Each is one flush, so more materials cost more instanced draw calls. */
#define MATERIAL_COUNT 6u

/** Every this-th prop is glass, drawn refractive in the translucent pass after the opaque ones. */
#define GLASS_EVERY 11u

/** The megaliths: big boxes that are both drawn and submitted as occluders, so props behind them cull. */
#define MEGALITH_COUNT 16u

/** The directional sun and the four point lights (the engine's per-draw cap). */
#define POINT_LIGHT_COUNT NYA_RENDER3D_MAX_POINT_LIGHTS

/** The custom post pipelines this example builds from the engine's effect shaders. */
#define PIPELINE_GRAYSCALE "nya_stress_grayscale_pipeline"
#define PIPELINE_CRT       "nya_stress_crt_pipeline"
#define PIPELINE_GRADE     "nya_stress_grade_pipeline"

/** The colour grade table the grade pass samples. */
#define GRADE_LUT NYA_ASSET_GRADES_VIVID_CUBE

/** The decal sheet and the soft particle sprite, both shared by the whole scene. */
#define DECAL_TEXTURE    NYA_ASSET_TEXTURES_DECALS_PNG
#define PARTICLE_TEXTURE NYA_ASSET_TEXTURES_PUFF_PNG

/** Scattered decals stamped flat on the ground, exercising the decal pass. */
#define DECAL_COUNT 48u

#define HUD_FONT      NYA_ASSET_FONTS_ALDRICH_TTF
#define HUD_FONT_SIZE 16.0F

/** Deterministic scatter seed, so a given scale draws the same scene every run. */
#define SCATTER_SEED 0x5715C3u

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** This example's own input actions, continuing the engine's numbering. Unnamed to stay NYA_InputAction. */
enum {
    STRESS_ACTION_SCALE_SMALL = NYA_INPUT_ACTION_USER,
    STRESS_ACTION_SCALE_LARGE,
    STRESS_ACTION_SCALE_INSANE,
    STRESS_ACTION_OVERLAY_PAGE,
    STRESS_ACTION_TOGGLE_FREEFLY,
    STRESS_ACTION_TOGGLE_POST,
    STRESS_ACTION_TOGGLE_SHADOWS,
    STRESS_ACTION_TOGGLE_OCCLUSION,
};

/** One named scale level: every heavy count the scene multiplies out from. */
typedef struct {
    NYA_ConstCString name;

    /** Instanced prop copies attempted each frame. Past NYA_RENDER3D_MAX_INSTANCES the surplus is dropped. */
    u32 instances;

    /** Per-system particle pool sizes. */
    u32 dust_pool;
    u32 spark_pool;
    u32 smoke_pool;

    /** The simulated smoke/fire volume's interior cells per axis. Cost grows with the cube of these. */
    u32 fluid_width;
    u32 fluid_height;
    u32 fluid_depth;

    /** Shadow atlas: cascades split across the view, and texels per cascade side. */
    u32 shadow_cascades;
    u32 shadow_map_size;

    /** Whether the exotic post passes (motion blur, eye adaptation, light shafts, DoF, grade, CRT) start on. */
    b8 heavy_post;
} StressScale;

/** The scene's state, hung off the world so it survives a hot reload. */
typedef struct {
    NYA_WindowHandle window;

    /** Which StressScale is live, an index into the table below. */
    u32 scale;

    /** Rebuilt whenever the scale changes: the pools are fixed-capacity, so a new size is a new system. */
    NYA_ParticleSystem* dust;
    NYA_ParticleSystem* sparks;
    NYA_ParticleSystem* smoke;
    NYA_Fluid*          plume;
    u32                 built_scale; ///< the scale the systems above were built for, or U32_MAX for none.

    /** Kept across a rebuild, since it is only ever cleared and refilled, never resized. */
    NYA_OcclusionBuffer* occlusion;

    /** The post chain's offscreen targets, and the toggles the HUD flips. */
    NYA_PostChain post;
    b8            post_on;
    b8            shadows_on;
    b8            occlusion_on;

    /** Classic SSAO in place of the stylised ambient occlusion, from NYA_STRESS_SSAO. Off leaves the stylised one. */
    b8            ssao_on;
    b8            crt_on;
    b8            grade_on;
    b8            grayscale_on;

    /** The CRT pass's uniform, kept alive for the frame the pass reads it. */
    NYA_ShaderCrtUniform crt_uniform;
    NYA_ShaderLutUniform grade_uniform;

    /** Registered flag, so the meshes and the pipelines are set up once (and again after a reload). */
    b8 registered;

    /** Free-fly camera state; when off, the camera flies its own automatic path. */
    b8    freefly;
    f32x3 eye;
    f32   yaw;
    f32   pitch;

    /** The camera last frame, for the speed-driven effects and to measure motion. */
    f32x3 previous_eye;
    f32   previous_s;

    /** Which debug overlay page is shown. */
    u32 overlay_page;

    /** Timer feeding the fluid and particle emitters at a frame-rate-independent rate. */
    f32 emit_timer_s;

    /** When non-zero (from NYA_STRESS_FRAMES), the scene quits after this many frames, for a timed run. */
    u32 max_frames;
    u32 frame_count;

    /** The last frame's readings, captured in on_render and logged at teardown so a headless run reports. */
    f32 last_fps;
    f64 last_work_ms;
    NYA_Render3DFrameStats last_stats;
} Stress;

NYA_INTERNAL Stress* stress(void) {
    return nya_world_user_data();
}

/*
 * The scale table. Instance counts at `large` and `insane` deliberately exceed NYA_RENDER3D_MAX_INSTANCES
 * (1024) so the dropped-draw counter climbs — that ceiling is one of the limits this example is here to find.
 */
NYA_INTERNAL const StressScale STRESS_SCALES[] = {
    {
        .name            = "small",
        .instances       = 600,
        .dust_pool       = 800,
        .spark_pool      = 400,
        .smoke_pool      = 600,
        .fluid_width     = 24,
        .fluid_height    = 40,
        .fluid_depth     = 24,
        .shadow_cascades = 2,
        .shadow_map_size = 1024,
        .heavy_post      = false,
    },
    {
        .name            = "large",
        .instances       = 2500,
        .dust_pool       = 4000,
        .spark_pool      = 2000,
        .smoke_pool      = 3000,
        .fluid_width     = 32,
        .fluid_height    = 56,
        .fluid_depth     = 32,
        .shadow_cascades = 3,
        .shadow_map_size = 2048,
        .heavy_post      = true,
    },
    {
        .name            = "insane",
        .instances       = 9000,
        .dust_pool       = 16000,
        .spark_pool      = 8000,
        .smoke_pool      = 12000,
        .fluid_width     = 48,
        .fluid_height    = 80,
        .fluid_depth     = 48,
        .shadow_cascades = 4,
        .shadow_map_size = 4096,
        .heavy_post      = true,
    },
};

#define STRESS_SCALE_COUNT ((u32)nya_carray_length(STRESS_SCALES))

NYA_INTERNAL const StressScale* scale_of(const Stress* state) {
    return &STRESS_SCALES[state->scale % STRESS_SCALE_COUNT];
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TERRAIN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The ground height at a world xz, a sum of sines standing in for eroded hills. */
NYA_INTERNAL f32 terrain_height(f32 x, f32 z) {
    return (sinf(x * 0.055F) * 4.5F) + (cosf(z * 0.048F) * 3.5F) + (sinf((x + z) * 0.11F) * 1.4F)
         + (cosf((x - z) * 0.17F) * 0.7F);
}

/** The surface normal from the height field's finite differences. */
NYA_INTERNAL f32x3 terrain_normal(f32 x, f32 z) {
    const f32 e  = 0.5F;
    f32       dx = terrain_height(x + e, z) - terrain_height(x - e, z);
    f32       dz = terrain_height(x, z + e) - terrain_height(x, z - e);

    return nya_vector_normalize((f32x3){ -dx, 2.0F * e, -dz });
}

/** A muted band colour by height, so the ground reads as terrain without needing a texture. */
NYA_INTERNAL NYA_Color terrain_color(f32 height) {
    f32 t = nya_clamp((height + 8.0F) / 16.0F, 0.0F, 1.0F);

    NYA_Color low  = { 0.10F, 0.16F, 0.11F, 1.0F };
    NYA_Color high = { 0.42F, 0.40F, 0.34F, 1.0F };

    return nya_color_mix(low, high, t);
}

/** Builds the terrain grid into `out` and returns the vertex count. */
NYA_INTERNAL u32 terrain_build(NYA_Vertex3D* out) {
    u32 at = 0;

    for (u32 j = 0; j < TERRAIN_CELLS; j++) {
        for (u32 i = 0; i < TERRAIN_CELLS; i++) {
            f32 x0 = ((f32)i * TERRAIN_CELL_SIZE) - TERRAIN_HALF;
            f32 z0 = ((f32)j * TERRAIN_CELL_SIZE) - TERRAIN_HALF;
            f32 x1 = x0 + TERRAIN_CELL_SIZE;
            f32 z1 = z0 + TERRAIN_CELL_SIZE;

            f32x3 a = { x0, terrain_height(x0, z0), z0 };
            f32x3 b = { x1, terrain_height(x1, z0), z0 };
            f32x3 c = { x1, terrain_height(x1, z1), z1 };
            f32x3 d = { x0, terrain_height(x0, z1), z1 };

            // Per-corner normals and colours, so the lighting follows the relief rather than facets.
            NYA_Vertex3D va = nya_vertex3d(a, terrain_color(a.y), terrain_normal(x0, z0), f32x2_zero);
            NYA_Vertex3D vb = nya_vertex3d(b, terrain_color(b.y), terrain_normal(x1, z0), f32x2_zero);
            NYA_Vertex3D vc = nya_vertex3d(c, terrain_color(c.y), terrain_normal(x1, z1), f32x2_zero);
            NYA_Vertex3D vd = nya_vertex3d(d, terrain_color(d.y), terrain_normal(x0, z1), f32x2_zero);

            out[at++] = va;
            out[at++] = vb;
            out[at++] = vc;
            out[at++] = va;
            out[at++] = vc;
            out[at++] = vd;
        }
    }

    return at;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PROP MESHES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * A crystal spire: a many-sided prism narrowing toward the top, the same shape at three resolutions so the
 * LOD chain has distinct silhouettes to pick between. Built once, uploaded once, drawn thousands of times.
 */

/** Writes two triangles for quad `a b c d` with the flat normal the winding gives. Returns six. */
NYA_INTERNAL u32 prop_face(NYA_Vertex3D* out, f32x3 a, f32x3 b, f32x3 c, f32x3 d, NYA_Color color) {
    f32x3 normal = nya_vector_normalize(nya_vector_cross(b - a, c - a));

    out[0] = nya_vertex3d(a, color, normal, f32x2_zero);
    out[1] = nya_vertex3d(b, color, normal, f32x2_zero);
    out[2] = nya_vertex3d(c, color, normal, f32x2_zero);
    out[3] = nya_vertex3d(a, color, normal, f32x2_zero);
    out[4] = nya_vertex3d(c, color, normal, f32x2_zero);
    out[5] = nya_vertex3d(d, color, normal, f32x2_zero);

    return 6;
}

/** Builds a `sides`-sided spire of `segments` stacked rings into `out`, returning the vertex count. */
NYA_INTERNAL u32 prop_build(NYA_Vertex3D* out, u32 sides, u32 segments) {
    nya_assert(sides >= 3);
    nya_assert(segments >= 1);

    u32 at = 0;

    NYA_Color color = NYA_COLOR_WHITE; // tinted per draw; the mesh is left white.

    for (u32 s = 0; s < segments; s++) {
        f32 y0 = (f32)s / (f32)segments;
        f32 y1 = (f32)(s + 1) / (f32)segments;

        // narrows to a quarter of the base by the top.
        f32 r0 = nya_lerp(1.0F, 0.25F, y0);
        f32 r1 = nya_lerp(1.0F, 0.25F, y1);

        for (u32 k = 0; k < sides; k++) {
            f32 a0 = ((f32)k / (f32)sides) * 2.0F * (f32)M_PI;
            f32 a1 = ((f32)(k + 1) / (f32)sides) * 2.0F * (f32)M_PI;

            f32x3 lower_here = { cosf(a0) * r0, y0, sinf(a0) * r0 };
            f32x3 lower_next = { cosf(a1) * r0, y0, sinf(a1) * r0 };
            f32x3 upper_here = { cosf(a0) * r1, y1, sinf(a0) * r1 };
            f32x3 upper_next = { cosf(a1) * r1, y1, sinf(a1) * r1 };

            at += prop_face(&out[at], lower_here, lower_next, upper_next, upper_here, color);
        }
    }

    // the base cap, wound so it faces down.
    for (u32 k = 0; k < sides; k++) {
        f32 a0 = ((f32)k / (f32)sides) * 2.0F * (f32)M_PI;
        f32 a1 = ((f32)(k + 1) / (f32)sides) * 2.0F * (f32)M_PI;

        f32x3 here = { cosf(a0), 0.0F, sinf(a0) };
        f32x3 next = { cosf(a1), 0.0F, sinf(a1) };

        f32x3 normal = { 0.0F, -1.0F, 0.0F };

        out[at++] = nya_vertex3d(f32x3_zero, color, normal, f32x2_zero);
        out[at++] = nya_vertex3d(here, color, normal, f32x2_zero);
        out[at++] = nya_vertex3d(next, color, normal, f32x2_zero);
    }

    nya_assert_le(at, PROP_VERTICES_MAX);

    return at;
}

/** Registers the three prop LODs and chains them, and registers the terrain. Called once, and after a reload. */
NYA_INTERNAL void meshes_register(NYA_Window* window) {
    Stress* state = stress();

    if (state->registered) return;

    // the prop LODs: fewer sides and segments as detail drops. Stack sized, since each is small.
    struct {
        NYA_ConstCString handle;
        u32              sides;
        u32              segments;
        f32              max_distance;
    } levels[] = {
        { PROP_MESH_L0, 8, 3, 45.0F },
        { PROP_MESH_L1, 6, 2, 100.0F },
        { PROP_MESH_L2, 4, 1, 190.0F },
    };

    NYA_Render3DLodLevel chain[nya_carray_length(levels)];

    for (u64 i = 0; i < nya_carray_length(levels); i++) {
        NYA_Vertex3D vertices[PROP_VERTICES_MAX];
        u32          count = prop_build(vertices, levels[i].sides, levels[i].segments);

        if (!nya_render3d_mesh_register(window, levels[i].handle, vertices, count)) {
            nya_log_error("Could not register prop LOD '%s'; the scene will draw without it.", levels[i].handle);
            return;
        }

        chain[i] = (NYA_Render3DLodLevel){ .handle = levels[i].handle, .max_distance = levels[i].max_distance };
    }

    if (!nya_render3d_lod_register(PROP_MESH_L0, chain, (u32)nya_carray_length(chain))) {
        nya_log_error("Could not register the prop LOD chain; props will always draw at full detail.");
    }

    // the terrain: too large for the stack, so a scratch arena that dies with this call.
    NYA_Arena* scratch = nya_arena_create(.name = "stress_terrain_build");
    defer nya_arena_destroy(scratch);

    NYA_Vertex3D* vertices = nya_arena_alloc(scratch, sizeof(NYA_Vertex3D) * TERRAIN_VERTICES);
    u32           count    = terrain_build(vertices);

    if (!nya_render3d_mesh_register(window, "nya_stress_terrain", vertices, count)) {
        nya_log_error("Could not register the terrain mesh; the ground will be missing.");
    }

    state->registered = true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SCATTER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Prop placement is derived from the index by hashing rather than stored, so the count is a pure knob and no
 * per-instance memory grows with it.
 */

/** Where prop `index` stands, on the terrain. */
NYA_INTERNAL f32x3 prop_position(u32 index) {
    f32 x = nya_ihash2((s32)index, 0, SCATTER_SEED) * (TERRAIN_HALF - 4.0F);
    f32 z = nya_ihash2((s32)index, 1, SCATTER_SEED) * (TERRAIN_HALF - 4.0F);

    return (f32x3){ x, terrain_height(x, z), z };
}

/** Prop `index`'s full height in world units. */
NYA_INTERNAL f32 prop_height(u32 index) {
    f32 unit = (nya_ihash2((s32)index, 2, SCATTER_SEED) * 0.5F) + 0.5F;
    return nya_lerp(1.2F, 4.5F, unit);
}

/** A megalith's centre and half-extents: big cuboids used as both scenery and occluders. */
NYA_INTERNAL void megalith_box(u32 index, OUT f32x3* out_center, OUT f32x3* out_half) {
    f32 angle = ((f32)index / (f32)MEGALITH_COUNT) * 2.0F * (f32)M_PI;
    f32 radius = 26.0F + (nya_ihash2((s32)index, 7, SCATTER_SEED) * 12.0F);

    f32 x = cosf(angle) * radius;
    f32 z = sinf(angle) * radius;

    f32 height = 8.0F + (nya_ihash2((s32)index, 8, SCATTER_SEED) * 5.0F);
    f32 width  = 3.0F + (nya_ihash2((s32)index, 9, SCATTER_SEED) * 1.5F);

    *out_half   = (f32x3){ width, height * 0.5F, width };
    *out_center = (f32x3){ x, terrain_height(x, z) + (height * 0.5F), z };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PARTICLES AND FLUID
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The world point the fire and its simulated column sit over. */
NYA_INTERNAL f32x3 hearth(void) {
    f32 ground = terrain_height(0.0F, 0.0F);
    return (f32x3){ 0.0F, ground, 0.0F };
}

/** Builds (or rebuilds) the particle systems and fluid volume for the current scale. */
NYA_INTERNAL void systems_build(void) {
    Stress*           state = stress();
    const StressScale* sc   = scale_of(state);

    if (state->built_scale == state->scale) return;

    NYA_Arena* allocator = nya_world()->allocator;

    // From the world's arena, so they die with the world rather than needing their own teardown. A previous
    // set is simply abandoned into the arena; the scene is a stress test, not a long-lived app.
    state->dust   = nya_particles_create(allocator, sc->dust_pool);
    state->sparks = nya_particles_create(allocator, sc->spark_pool);
    state->smoke  = nya_particles_create(allocator, sc->smoke_pool);

    nya_particles_space_set(state->dust, NYA_PARTICLE_SPACE_3D);
    nya_particles_space_set(state->sparks, NYA_PARTICLE_SPACE_3D);
    nya_particles_space_set(state->smoke, NYA_PARTICLE_SPACE_3D);

    // the dust is opaque grit whose billboard is its real shadow; the soft sprites stay out of the shadow pass.
    nya_particles_casts_shadow_set(state->dust, true);

    nya_particles_texture_set(state->sparks, PARTICLE_TEXTURE);
    nya_particles_texture_set(state->smoke, PARTICLE_TEXTURE);

    f32x3 fire = hearth();

    state->plume = nya_fluid_create(allocator, (NYA_FluidOptions){
                                                   .space       = NYA_FLUID_SPACE_3D,
                                                   .width       = sc->fluid_width,
                                                   .height      = sc->fluid_height,
                                                   .depth       = sc->fluid_depth,
                                                   .cell_size   = 0.35F,
                                                   .buoyancy    = 6.0F,
                                                   .vorticity   = 2.0F,
                                                   .dissipation = 0.55F,
                                                   .cooling     = 1.4F,
                                                   .origin      = {
                                                       fire.x - ((f32)sc->fluid_width * 0.35F * 0.5F),
                                                       fire.y,
                                                       fire.z - ((f32)sc->fluid_depth * 0.35F * 0.5F),
                                                   },
                                               });

    state->built_scale = state->scale;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * POST PIPELINES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Queues the custom grade / CRT / grayscale post pipelines. Safe to call more than once. */
NYA_INTERNAL void post_pipelines_ensure(NYA_Window* window) {
    // grayscale: one sampler, no uniform.
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
                   .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
                   .handle    = NYA_ASSET_SHADER_EFFECT_GRAYSCALE_FRAG,
                   .as_shader = { .num_samplers = 1 },
               }),
               "while queueing the grayscale shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
                   .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
                   .handle               = PIPELINE_GRAYSCALE,
                   .as_graphics_pipeline = {
                       .window                 = window,
                       .vertex_shader_handle   = NYA_ASSET_SHADER_BATCH2D_VERT,
                       .fragment_shader_handle = NYA_ASSET_SHADER_EFFECT_GRAYSCALE_FRAG,
                       .blend                  = true,
                       .vertex_layout          = NYA_VERTEX_LAYOUT_2D,
                   },
               }),
               "while queueing the grayscale pipeline");

    // CRT: one sampler and one uniform buffer (curvature, scanlines, aberration).
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
                   .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
                   .handle    = NYA_ASSET_SHADER_EFFECT_CRT_FRAG,
                   .as_shader = { .num_samplers = 1, .num_uniform_buffers = 1 },
               }),
               "while queueing the CRT shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
                   .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
                   .handle               = PIPELINE_CRT,
                   .as_graphics_pipeline = {
                       .window                 = window,
                       .vertex_shader_handle   = NYA_ASSET_SHADER_BATCH2D_VERT,
                       .fragment_shader_handle = NYA_ASSET_SHADER_EFFECT_CRT_FRAG,
                       .blend                  = true,
                       .vertex_layout          = NYA_VERTEX_LAYOUT_2D,
                   },
               }),
               "while queueing the CRT pipeline");

    // grade / LUT: two samplers (image and table) and one uniform buffer (strength).
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
                   .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
                   .handle    = NYA_ASSET_SHADER_EFFECT_LUT_FRAG,
                   .as_shader = { .num_samplers = 2, .num_uniform_buffers = 1 },
               }),
               "while queueing the grade shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
                   .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
                   .handle               = PIPELINE_GRADE,
                   .as_graphics_pipeline = {
                       .window                 = window,
                       .vertex_shader_handle   = NYA_ASSET_SHADER_BATCH2D_VERT,
                       .fragment_shader_handle = NYA_ASSET_SHADER_EFFECT_LUT_FRAG,
                       .blend                  = true,
                       .vertex_layout          = NYA_VERTEX_LAYOUT_2D,
                   },
               }),
               "while queueing the grade pipeline");

    // the grade table itself, sampled by the grade pass.
    (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_LUT, .handle = GRADE_LUT });

    // the shared textures.
    (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = DECAL_TEXTURE });
    (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = PARTICLE_TEXTURE });
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON CREATE / DESTROY / EVENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void stress_layer_on_create(NYA_Window* window) {
    nya_assert(window != nullptr);

    Stress* state = stress();

    // a dim sky-blue, so the terrain's rim reads against it before the sky pass paints over it.
    nya_render_clear_color_set(window, (NYA_Color){ 0.05F, 0.06F, 0.09F, 1.0F });

    nya_input_action_bind(STRESS_ACTION_SCALE_SMALL, NYA_KEY_1);
    nya_input_action_bind(STRESS_ACTION_SCALE_LARGE, NYA_KEY_2);
    nya_input_action_bind(STRESS_ACTION_SCALE_INSANE, NYA_KEY_3);
    nya_input_action_bind(STRESS_ACTION_OVERLAY_PAGE, NYA_KEY_TAB);
    nya_input_action_bind(STRESS_ACTION_TOGGLE_FREEFLY, NYA_KEY_F);
    nya_input_action_bind(STRESS_ACTION_TOGGLE_POST, NYA_KEY_P);
    nya_input_action_bind(STRESS_ACTION_TOGGLE_SHADOWS, NYA_KEY_H);
    nya_input_action_bind(STRESS_ACTION_TOGGLE_OCCLUSION, NYA_KEY_O);

    meshes_register(window);
    systems_build();
    post_pipelines_ensure(window);

    // the occlusion buffer is tens of kilobytes; it lives on the world's arena, cleared and refilled each frame.
    if (state->occlusion == nullptr) {
        state->occlusion = nya_arena_alloc(nya_world()->allocator, sizeof(NYA_OcclusionBuffer));
    }

    nya_render3d_decals_set(window, (NYA_Render3DDecals){ .enabled = true });

    // the shadow atlas at this scale's size.
    const StressScale* sc = scale_of(state);
    nya_render3d_shadow_options_set(window, (NYA_Render3DShadowOptions){
                                                .cascades = sc->shadow_cascades,
                                                .map_size = sc->shadow_map_size,
                                                .color    = { 0.10F, 0.12F, 0.20F, 0.6F },
                                            });

    // the camera starts on its automatic path, above and outside the ring.
    state->eye   = (f32x3){ 0.0F, 24.0F, 70.0F };
    state->yaw   = -(f32)M_PI * 0.5F;
    state->pitch = -0.25F;
}

void stress_layer_on_destroy(NYA_Window* window) {
    Stress* state = stress();

    // the scene features this layer turned on, off again, so a re-enter starts clean.
    nya_post_bloom_set(window, (NYA_PostBloom){ 0 });
    nya_post_depth_of_field_set(window, (NYA_PostDepthOfField){ 0 });
    nya_post_motion_blur_set(window, (NYA_PostMotionBlur){ 0 });
    nya_post_eye_adaptation_set(window, (NYA_PostEyeAdaptation){ 0 });
    nya_post_light_shafts_set(window, (NYA_PostLightShafts){ 0 });
    nya_post_ambient_occlusion_set(window, (NYA_PostAmbientOcclusion){ 0 });
    nya_render3d_decals_set(window, (NYA_Render3DDecals){ 0 });

    nya_post_chain_destroy(&state->post);

    // the last frame's readings, so a headless timed run leaves the numbers in the log.
    const StressScale* sc = scale_of(state);
    nya_log_info("renderer_stress[%s]: %.1f fps, %.2f ms work, draws %u, instances %u, dropped %u, culled %u, occluded %u.",
                 sc->name, (f64)state->last_fps, state->last_work_ms, state->last_stats.draw_calls, state->last_stats.instances,
                 state->last_stats.dropped_draws, state->last_stats.culled, state->last_stats.occluded);

    // the meshes' GPU buffers; the particle pools and fluid live on the world arena and go with it.
    nya_render3d_lod_unregister(PROP_MESH_L0);
    nya_render3d_mesh_release(window, PROP_MESH_L0);
    nya_render3d_mesh_release(window, PROP_MESH_L1);
    nya_render3d_mesh_release(window, PROP_MESH_L2);
    nya_render3d_mesh_release(window, "nya_stress_terrain");

    state->registered = false;
}

/** Switches to `scale`, rebuilding the size-dependent systems and the shadow atlas. */
NYA_INTERNAL void scale_set(NYA_Window* window, u32 scale) {
    Stress* state = stress();

    state->scale = scale % STRESS_SCALE_COUNT;
    systems_build();

    const StressScale* sc = scale_of(state);
    nya_render3d_shadow_options_set(window, (NYA_Render3DShadowOptions){
                                                .cascades = sc->shadow_cascades,
                                                .map_size = sc->shadow_map_size,
                                                .color    = { 0.10F, 0.12F, 0.20F, 0.6F },
                                            });

    // the heavy passes follow the level, so `small` is a light baseline and `insane` turns everything on.
    state->crt_on       = sc->heavy_post;
    state->grade_on     = sc->heavy_post;
    state->grayscale_on = false;

    nya_log_info("renderer_stress: scale '%s' — %u instances, %u/%u/%u particles, %ux%ux%u fluid, %u cascades at %u.",
                 sc->name, sc->instances, sc->dust_pool, sc->spark_pool, sc->smoke_pool, sc->fluid_width, sc->fluid_height,
                 sc->fluid_depth, sc->shadow_cascades, sc->shadow_map_size);
}

void stress_layer_on_event(NYA_Window* window, NYA_Event* event) {
    nya_assert(event != nullptr);

    Stress* state = stress();

    if (event->type == NYA_EVENT_KEY_DOWN) {
        NYA_KeyEvent* key = &event->as_key_event;
        if (key->is_repeat) return;

        if (key->key == NYA_KEY_ESCAPE) {
            nya_app_get()->should_quit = true;
            event->was_handled         = true;
            return;
        }

        if (nya_input_action_matches(STRESS_ACTION_SCALE_SMALL, key->key, key->modifier_flags)) {
            scale_set(window, 0);
            event->was_handled = true;
        } else if (nya_input_action_matches(STRESS_ACTION_SCALE_LARGE, key->key, key->modifier_flags)) {
            scale_set(window, 1);
            event->was_handled = true;
        } else if (nya_input_action_matches(STRESS_ACTION_SCALE_INSANE, key->key, key->modifier_flags)) {
            scale_set(window, 2);
            event->was_handled = true;
        } else if (nya_input_action_matches(STRESS_ACTION_OVERLAY_PAGE, key->key, key->modifier_flags)) {
            state->overlay_page = (state->overlay_page + 1) % NYA_DEBUG_OVERLAY_PAGE_COUNT;
            event->was_handled  = true;
        } else if (nya_input_action_matches(STRESS_ACTION_TOGGLE_FREEFLY, key->key, key->modifier_flags)) {
            state->freefly     = !state->freefly;
            event->was_handled = true;
        } else if (nya_input_action_matches(STRESS_ACTION_TOGGLE_POST, key->key, key->modifier_flags)) {
            state->post_on     = !state->post_on;
            event->was_handled = true;
        } else if (nya_input_action_matches(STRESS_ACTION_TOGGLE_SHADOWS, key->key, key->modifier_flags)) {
            state->shadows_on  = !state->shadows_on;
            event->was_handled = true;
        } else if (nya_input_action_matches(STRESS_ACTION_TOGGLE_OCCLUSION, key->key, key->modifier_flags)) {
            state->occlusion_on = !state->occlusion_on;
            event->was_handled  = true;
        }
        return;
    }

    // free-fly look: the right mouse button turns the camera.
    if (event->type == NYA_EVENT_MOUSE_MOVED && state->freefly && nya_input_mouse_button_pressed(NYA_MOUSE_BUTTON_RIGHT)) {
        NYA_MouseMovedEvent* mouse = &event->as_mouse_moved_event;

        state->yaw   += mouse->delta_x * 0.003F;
        state->pitch -= mouse->delta_y * 0.003F;
        state->pitch  = nya_clamp(state->pitch, -1.5F, 1.5F);

        event->was_handled = true;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON UPDATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The camera's forward from its yaw and pitch. */
NYA_INTERNAL f32x3 camera_forward(const Stress* state) {
    return (f32x3){
        cosf(state->pitch) * cosf(state->yaw),
        sinf(state->pitch),
        cosf(state->pitch) * sinf(state->yaw),
    };
}

void stress_layer_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window);

    Stress*            state = stress();
    const StressScale* sc    = scale_of(state);

    if (state->freefly) {
        // WASD across the ground, Q/E down and up, in the camera's own frame.
        f32x3 forward = camera_forward(state);
        f32x3 right   = nya_vector_normalize(nya_vector_cross(forward, (f32x3){ 0.0F, 1.0F, 0.0F }));

        f32 speed = 30.0F * delta_time_s;

        if (nya_input_key_pressed(NYA_KEY_W)) state->eye = state->eye + (forward * speed);
        if (nya_input_key_pressed(NYA_KEY_S)) state->eye = state->eye - (forward * speed);
        if (nya_input_key_pressed(NYA_KEY_D)) state->eye = state->eye + (right * speed);
        if (nya_input_key_pressed(NYA_KEY_A)) state->eye = state->eye - (right * speed);
        if (nya_input_key_pressed(NYA_KEY_E)) state->eye.y += speed;
        if (nya_input_key_pressed(NYA_KEY_Q)) state->eye.y -= speed;
    } else {
        // the automatic path: a slow orbit that also rises and falls, so the whole scene is exercised.
        f32 t      = (f32)nya_app_uptime_s() * 0.12F;
        f32 radius = 58.0F + (sinf(t * 0.6F) * 18.0F);

        state->eye = (f32x3){
            cosf(t) * radius,
            26.0F + (sinf(t * 0.9F) * 10.0F),
            sinf(t) * radius,
        };

        // look at the hearth in the middle, over the terrain.
        f32x3 target  = { 0.0F, terrain_height(0.0F, 0.0F) + 6.0F, 0.0F };
        f32x3 forward = nya_vector_normalize(target - state->eye);

        state->yaw   = atan2f(forward.z, forward.x);
        state->pitch = asinf(nya_clamp(forward.y, -1.0F, 1.0F));
    }

    // the particle systems, once a tick.
    nya_particles_update(state->dust, delta_time_s);
    nya_particles_update(state->sparks, delta_time_s);
    nya_particles_update(state->smoke, delta_time_s);

    // the simulated column over the fire.
    f32x3 fire = hearth();

    nya_fluid_emit(state->plume, (NYA_FluidEmitter){
                                     .position    = { fire.x, fire.y + 0.5F, fire.z },
                                     .radius      = 1.4F,
                                     .density     = 3.0F * delta_time_s,
                                     .temperature = 8.0F * delta_time_s,
                                     .velocity    = { 0.0F, 3.0F, 0.0F },
                                 });

    nya_fluid_step(state->plume, delta_time_s);

    // the emitters, fed on a timer so their rate does not depend on frame rate.
    state->emit_timer_s += delta_time_s;

    while (state->emit_timer_s >= 0.05F) {
        state->emit_timer_s -= 0.05F;

        // ambient dust drifting across the whole field.
        (void)nya_particles_emit(state->dust, (NYA_ParticleBurst){
                                                  .shape      = NYA_PARTICLE_SHAPE_BOX,
                                                  .position   = { 0.0F, 14.0F, 0.0F },
                                                  .volume     = { TERRAIN_HALF, 3.0F, TERRAIN_HALF },
                                                  .count      = nya_max(sc->dust_pool / 60u, 1u),
                                                  .direction  = { 0.0F, -1.0F, 0.0F },
                                                  .spread     = 3.14F,
                                                  .speed      = { 0.5F, 2.0F },
                                                  .lifetime_s = { 2.0F, 5.0F },
                                                  .size       = { 0.05F, 0.12F },
                                                  .size_end   = { 0.0F, 0.03F },
                                                  .color_start = { 0.7F, 0.7F, 0.75F, 0.5F },
                                                  .color_end   = { 0.6F, 0.6F, 0.65F, 0.0F },
                                                  .gravity     = { 0.0F, -0.5F, 0.0F },
                                                  .damping     = 0.6F,
                                              });

        // sparks off the fire, fast and additive.
        (void)nya_particles_emit(state->sparks, (NYA_ParticleBurst){
                                                    .shape       = NYA_PARTICLE_SHAPE_CONE,
                                                    .position    = { fire.x, fire.y + 0.5F, fire.z },
                                                    .count       = nya_max(sc->spark_pool / 40u, 1u),
                                                    .direction   = { 0.0F, 1.0F, 0.0F },
                                                    .spread      = 0.9F,
                                                    .speed       = { 4.0F, 9.0F },
                                                    .lifetime_s  = { 0.4F, 1.1F },
                                                    .size        = { 0.06F, 0.14F },
                                                    .size_end    = { 0.0F, 0.02F },
                                                    .color_start = { 3.0F, 1.6F, 0.4F, 1.0F },
                                                    .color_end   = { 1.2F, 0.3F, 0.05F, 0.0F },
                                                    .gravity     = { 0.0F, -3.0F, 0.0F },
                                                    .damping     = 0.9F,
                                                });

        // smoke above the flame.
        (void)nya_particles_emit(state->smoke, (NYA_ParticleBurst){
                                                   .shape       = NYA_PARTICLE_SHAPE_CONE,
                                                   .position    = { fire.x, fire.y + 2.0F, fire.z },
                                                   .count       = nya_max(sc->smoke_pool / 50u, 1u),
                                                   .direction   = { 0.0F, 1.0F, 0.0F },
                                                   .spread      = 1.4F,
                                                   .speed       = { 1.0F, 3.0F },
                                                   .lifetime_s  = { 1.5F, 4.0F },
                                                   .size        = { 0.5F, 1.2F },
                                                   .size_end    = { 1.5F, 3.0F },
                                                   .color_start = { 0.25F, 0.25F, 0.27F, 0.7F },
                                                   .color_end   = { 0.15F, 0.15F, 0.16F, 0.0F },
                                                   .gravity     = { 0.0F, 0.6F, 0.0F },
                                                   .damping     = 0.5F,
                                               });
    }

    // Kept a no-op after the first frame, except after a code reload, which empties the mesh and LOD registries.
    meshes_register(window);

    // a timed run quits itself once it has drawn its frames, so a headless CI run terminates.
    state->frame_count++;
    if (state->max_frames > 0 && state->frame_count >= state->max_frames) nya_app_get()->should_quit = true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DRAW
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The colour prop `index` is tinted, from a small palette by its material bucket. */
NYA_INTERNAL NYA_Color prop_color(u32 index) {
    static const NYA_Color palette[MATERIAL_COUNT] = {
        { 0.60F, 0.20F, 0.24F, 1.0F },
        { 0.20F, 0.42F, 0.55F, 1.0F },
        { 0.70F, 0.62F, 0.24F, 1.0F },
        { 0.32F, 0.55F, 0.30F, 1.0F },
        { 0.48F, 0.34F, 0.62F, 1.0F },
        { 0.75F, 0.75F, 0.80F, 1.0F },
    };

    return palette[index % MATERIAL_COUNT];
}

/** One opaque material per bucket, so the buckets differ visibly and each is its own flush. */
NYA_INTERNAL NYA_Render3DMaterial material_of(u32 bucket) {
    return (NYA_Render3DMaterial){
        .metallic  = ((f32)bucket / (f32)MATERIAL_COUNT),
        .roughness = nya_lerp(0.3F, 1.0F, (f32)bucket / (f32)MATERIAL_COUNT),
        .edge      = 0.4F,
        // the last bucket glows, so bloom and eye adaptation have something bright to work on.
        .emission = bucket + 1u == MATERIAL_COUNT ? 0.6F : 0.0F,
    };
}

/** Draws the 3D scene between begin and end. Separate so the post chain can capture it without the HUD. */
NYA_INTERNAL void draw_scene(NYA_Window* window) {
    Stress*            state = stress();
    const StressScale* sc    = scale_of(state);

    f32x3 forward = camera_forward(state);
    f32x3 target  = state->eye + forward;

    nya_render3d_begin(window, (NYA_Camera3DPerspective){
                                   .position   = state->eye,
                                   .target     = target,
                                   .far_plane  = 400.0F,
                               });

    // occlusion culling: the megaliths are the occluders, so smaller props behind them are rejected.
    if (state->occlusion_on && state->occlusion != nullptr) {
        nya_occlusion_begin(state->occlusion, nya_render3d_view_projection(window));

        for (u32 i = 0; i < MEGALITH_COUNT; i++) {
            f32x3 center;
            f32x3 half;
            megalith_box(i, &center, &half);
            (void)nya_occlusion_box(state->occlusion, center, half, state->eye);
        }

        nya_render3d_occlusion(window, state->occlusion);
    } else {
        nya_render3d_occlusion(window, nullptr);
    }

    // the sun, low and warm, so the shadows are long.
    f32x3 sun_direction = nya_vector_normalize((f32x3){ -0.5F, -0.55F, -0.35F });

    nya_render3d_light_set(window, (NYA_Render3DLight){
                                       .direction = sun_direction,
                                       .color     = { 1.0F, 0.93F, 0.80F, 1.0F },
                                       .ambient   = 0.30F,
                                       .intensity = 1.1F,
                                       .sky       = { 0.45F, 0.55F, 0.75F, 1.0F },
                                       .ground    = { 0.30F, 0.26F, 0.20F, 1.0F },
                                   });

    // the sky behind everything, its sun aligned with the directional light.
    nya_render3d_sky_draw(window, (NYA_Render3DSky){
                                      .zenith        = { 0.18F, 0.30F, 0.55F, 1.0F },
                                      .horizon       = { 0.55F, 0.60F, 0.68F, 1.0F },
                                      .ground        = { 0.10F, 0.10F, 0.12F, 1.0F },
                                      .sun_direction = -sun_direction,
                                      .sun_color     = { 1.0F, 0.85F, 0.6F, 1.0F },
                                      .sun_intensity = 1.4F,
                                  });

    // distance and height fog with aerial perspective, the depth cue a flat-shaded scene otherwise lacks.
    nya_render3d_fog_set(window, (NYA_Render3DFog){
                                     .color          = { 0.55F, 0.60F, 0.68F, 1.0F },
                                     .density        = 0.006F,
                                     .height_falloff = 0.03F,
                                     .sun_amount     = 0.4F,
                                     .aerial         = 0.8F,
                                 });

    // the four point lights (the per-draw cap), orbiting so they sweep the props.
    nya_render3d_point_lights_clear(window);

    f32 phase = (f32)nya_app_uptime_s() * 0.5F;

    for (u32 i = 0; i < POINT_LIGHT_COUNT; i++) {
        f32 angle = phase + ((f32)i * (2.0F * (f32)M_PI / (f32)POINT_LIGHT_COUNT));
        f32 lx    = cosf(angle) * 22.0F;
        f32 lz    = sinf(angle) * 22.0F;

        static const NYA_Color colors[POINT_LIGHT_COUNT] = {
            { 1.0F, 0.4F, 0.3F, 1.0F },
            { 0.3F, 0.6F, 1.0F, 1.0F },
            { 0.4F, 1.0F, 0.5F, 1.0F },
            { 1.0F, 0.9F, 0.4F, 1.0F },
        };

        nya_render3d_point_light_add(window, (NYA_Render3DPointLight){
                                                 .position  = { lx, terrain_height(lx, lz) + 6.0F, lz },
                                                 .color     = colors[i],
                                                 .range     = 30.0F,
                                                 .intensity = 2.0F,
                                             });
    }

    // the ground.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .roughness = 1.0F });
    nya_render3d_mesh(window, "nya_stress_terrain", f32x3_zero, (f32x3){ 1.0F, 1.0F, 1.0F }, nya_quaternion_identity, NYA_COLOR_WHITE);

    // the megaliths, drawn as cubes in a dark stone.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .roughness = 0.9F, .edge = 0.3F });

    for (u32 i = 0; i < MEGALITH_COUNT; i++) {
        f32x3 center;
        f32x3 half;
        megalith_box(i, &center, &half);
        nya_render3d_cube(window, center, half * 2.0F, nya_quaternion_identity, (NYA_Color){ 0.22F, 0.22F, 0.26F, 1.0F });
    }

    // the opaque props, bucketed by material so instances of the same LOD mesh batch into few draws. The LOD
    // chain on PROP_MESH_L0 swaps the drawn mesh by distance, and the surplus past the instance ceiling is
    // counted as dropped draws — one of the limits this example exists to surface.
    for (u32 bucket = 0; bucket < MATERIAL_COUNT; bucket++) {
        nya_render3d_material_set(window, material_of(bucket));

        for (u32 i = bucket; i < sc->instances; i += MATERIAL_COUNT) {
            if ((i % GLASS_EVERY) == 0) continue; // glass is drawn in the translucent pass below.

            f32x3 base   = prop_position(i);
            f32   height = prop_height(i);
            f32   spin   = nya_ihash2((s32)i, 3, SCATTER_SEED) * (f32)M_PI;

            nya_render3d_mesh(window, PROP_MESH_L0, base, (f32x3){ height * 0.4F, height, height * 0.4F },
                              nya_quaternion_from_axis_angle((f32x3){ 0.0F, 1.0F, 0.0F }, spin), prop_color(i));
        }
    }

    // the translucent pass: every GLASS_EVERY-th prop drawn refractive, so it bends the captured opaque scene.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){
                                          .metallic    = 0.2F,
                                          .roughness   = 0.1F,
                                          .reflectance = 0.4F,
                                          .refraction  = 0.6F,
                                          .blur        = 0.2F,
                                      });

    for (u32 i = 0; i < sc->instances; i += GLASS_EVERY) {
        f32x3 base   = prop_position(i);
        f32   height = prop_height(i);
        f32   spin   = nya_ihash2((s32)i, 3, SCATTER_SEED) * (f32)M_PI;

        nya_render3d_mesh(window, PROP_MESH_L0, base, (f32x3){ height * 0.4F, height, height * 0.4F },
                          nya_quaternion_from_axis_angle((f32x3){ 0.0F, 1.0F, 0.0F }, spin),
                          (NYA_Color){ 0.6F, 0.85F, 0.9F, 0.4F });
    }

    // scattered decals stamped flat on the ground.
    for (u32 i = 0; i < DECAL_COUNT; i++) {
        f32 x = nya_ihash2((s32)i, 4, SCATTER_SEED) * (TERRAIN_HALF - 6.0F);
        f32 z = nya_ihash2((s32)i, 5, SCATTER_SEED) * (TERRAIN_HALF - 6.0F);

        nya_render3d_decal(window, (NYA_Render3DDecal){
                                       .texture  = DECAL_TEXTURE,
                                       .center   = { x, terrain_height(x, z), z },
                                       .size     = { 2.0F, 1.0F, 2.0F },
                                       .rotation = nya_ihash2((s32)i, 6, SCATTER_SEED) * (f32)M_PI,
                                       .color    = { 0.1F, 0.1F, 0.1F, 0.6F },
                                       .columns  = 2,
                                       .rows     = 2,
                                       .cell     = (u8)(i % 4),
                                   });
    }

    // TODO(wind/foliage/water): the swaying foliage, the wind field and the water surface slot in here, drawn
    // after the opaque props and before the additive passes. They land separately; see render_wind.* and the
    // gnyame foliage work.

    // the particles: opaque dust first (through the sorted transparent pass), then smoke, then additive sparks.
    nya_particles_draw(window, state->dust);
    nya_particles_draw(window, state->smoke);

    nya_render3d_blend_set(window, NYA_RENDER3D_BLEND_ADDITIVE);
    nya_particles_draw(window, state->sparks);

    // the simulated column, additive, over the same fire.
    nya_fluid_render_options_set(window, (NYA_FluidRenderOptions){
                                             .enabled         = true,
                                             .opacity         = 0.7F,
                                             .cool            = { 0.3F, 0.05F, 0.02F, 1.0F },
                                             .hot             = { 3.0F, 1.5F, 0.4F, 1.0F },
                                             .hot_temperature = 4.0F,
                                         });
    nya_fluid_draw(window, state->plume);

    nya_render3d_blend_set(window, NYA_RENDER3D_BLEND_ALPHA);

    nya_render3d_end(window);
}

/** Fills `passes` with the caller-side post passes that are on, and returns how many. */
NYA_INTERNAL u32 build_passes(Stress* state, OUT NYA_PostPass* passes) {
    u32 count = 0;

    if (state->grade_on) {
        state->grade_uniform = (NYA_ShaderLutUniform){ .strength = 0.85F };

        passes[count++] = (NYA_PostPass){
            .pipeline     = PIPELINE_GRADE,
            .texture      = GRADE_LUT,
            .uniform      = &state->grade_uniform,
            .uniform_size = sizeof(state->grade_uniform),
            .trace        = NYA_TRACE_GRADE,
        };
    }

    if (state->grayscale_on) {
        passes[count++] = (NYA_PostPass){ .pipeline = PIPELINE_GRAYSCALE, .trace = NYA_TRACE_POST };
    }

    if (state->crt_on) {
        state->crt_uniform = (NYA_ShaderCrtUniform){
            .curvature         = 0.12F,
            .scanline_count    = (f32)WINDOW_HEIGHT,
            .scanline_strength = 0.25F,
            .aberration        = 1.5F,
        };

        passes[count++] = (NYA_PostPass){
            .pipeline     = PIPELINE_CRT,
            .uniform      = &state->crt_uniform,
            .uniform_size = sizeof(state->crt_uniform),
            .trace        = NYA_TRACE_POST,
        };
    }

    return count;
}

/** Applies the window's scene-pass toggles for this frame, from the scale and the runtime flags. */
NYA_INTERNAL void apply_scene_features(NYA_Window* window, Stress* state) {
    const StressScale* sc = scale_of(state);

    // the sun's cascaded shadows, fitted around the scene's centre.
    if (state->shadows_on) {
        f32 distance = nya_vector_length((f32x3){ 0.0F, terrain_height(0.0F, 0.0F), 0.0F } - state->eye);

        nya_render3d_shadow_set(window, (NYA_Render3DShadowFit){
                                            .near_distance = nya_max(distance - 60.0F, 0.1F),
                                            .range         = distance + 60.0F,
                                            .strength      = 0.5F,
                                        });
    } else {
        nya_render3d_shadow_set(window, (NYA_Render3DShadowFit){ .strength = 0.0F });
    }

    if (!state->post_on) {
        // everything off: the scene still draws, straight to the window.
        nya_post_bloom_set(window, (NYA_PostBloom){ 0 });
        nya_post_depth_of_field_set(window, (NYA_PostDepthOfField){ 0 });
        nya_post_motion_blur_set(window, (NYA_PostMotionBlur){ 0 });
        nya_post_eye_adaptation_set(window, (NYA_PostEyeAdaptation){ 0 });
        nya_post_light_shafts_set(window, (NYA_PostLightShafts){ 0 });
        nya_post_ambient_occlusion_set(window, (NYA_PostAmbientOcclusion){ 0 });
        nya_post_ssao_set(window, (NYA_PostSsao){ 0 });
        return;
    }

    // bloom and ambient occlusion at every level; the rest only at the heavy levels.
    nya_post_bloom_set(window, (NYA_PostBloom){ .enabled = true, .threshold = 0.9F, .intensity = 1.0F });

    // classic SSAO stands in for the stylised occlusion when asked; the two share the half resolution buffer, so only
    // one runs at a time.
    if (state->ssao_on) {
        nya_post_ambient_occlusion_set(window, (NYA_PostAmbientOcclusion){ 0 });
        nya_post_ssao_set(window, (NYA_PostSsao){ .enabled = true, .strength = 0.6F });
    } else {
        nya_post_ssao_set(window, (NYA_PostSsao){ 0 });
        nya_post_ambient_occlusion_set(window, (NYA_PostAmbientOcclusion){ .enabled = true, .strength = 0.45F });
    }

    if (sc->heavy_post) {
        // motion blur reads the camera's motion between frames; the speed comes from the previous eye.
        nya_post_motion_blur_set(window, (NYA_PostMotionBlur){ .enabled = true, .strength = 0.6F });
        nya_post_eye_adaptation_set(window, (NYA_PostEyeAdaptation){ .enabled = true });
        nya_post_light_shafts_set(window, (NYA_PostLightShafts){ .enabled = true });
        nya_post_depth_of_field_set(window, (NYA_PostDepthOfField){ .focus = NYA_POST_FOCUS_DISTANCE, .focus_distance = 40.0F });
    } else {
        nya_post_motion_blur_set(window, (NYA_PostMotionBlur){ 0 });
        nya_post_eye_adaptation_set(window, (NYA_PostEyeAdaptation){ 0 });
        nya_post_light_shafts_set(window, (NYA_PostLightShafts){ 0 });
        nya_post_depth_of_field_set(window, (NYA_PostDepthOfField){ 0 });
    }
}

void stress_layer_on_render(NYA_Window* window) {
    Stress*            state = stress();
    const StressScale* sc    = scale_of(state);

    apply_scene_features(window, state);

    // HDR output at the heavy levels, so the emissive props and fire lift above one.
    nya_render_output_set(window, (NYA_RenderOutput){ .hdr = sc->heavy_post });

    // the scene target needs its depth kept, both for the 3D pass and for the refraction glass reads.
    state->post.scene = (NYA_RenderTextureOptions){ .depth = NYA_RENDER_TEXTURE_DEPTH_ATTACHED };

    NYA_PostPass passes[3];
    u32          pass_count = build_passes(state, passes);

    // through the chain when a pass or a scene feature wants it; otherwise straight to the window.
    if (!state->post_on || !(pass_count > 0 || nya_post_enabled(window)) || !nya_post_begin(window, &state->post)) {
        draw_scene(window);
        nya_render_output_scene_end(window);
    } else {
        draw_scene(window);
        nya_post_end(window, &state->post, passes, pass_count);
    }

    // what the culls and the batch did this frame; read here because nya_render_begin resets it.
    NYA_Render3DFrameStats drawn  = nya_render3d_frame_stats(window);
    NYA_OcclusionStats     hidden = state->occlusion != nullptr ? nya_occlusion_stats(state->occlusion) : (NYA_OcclusionStats){ 0 };

    const NYA_FrameStats* frame      = &nya_app_get()->frame_stats;
    f64                   work_ms    = (f64)frame->work_ns / 1.0e6;
    f64                   physics_ms = (f64)nya_physics3d_last_step_time_s() * 1000.0;
    f64                   fluid_ms   = (f64)nya_fluid_step_time_s(state->plume) * 1000.0;

    // kept for the teardown summary a headless run prints.
    state->last_fps     = frame->fps;
    state->last_work_ms = work_ms;
    state->last_stats   = drawn;

    // the HUD, in screen pixels over the flushed scene.
    f32 y = 12.0F;
    f32 step = HUD_FONT_SIZE * 1.35F;

    nya_render2d_textf_with_font(window, HUD_FONT, HUD_FONT_SIZE, 12.0F, y, NYA_COLOR_WHITE,
                                 "scale %s  (1/2/3)   %.1f fps   frame %.2f ms (work)", sc->name, (f64)frame->fps, work_ms);
    y += step;
    nya_render2d_textf_with_font(window, HUD_FONT, HUD_FONT_SIZE, 12.0F, y, NYA_COLOR_LIGHT_GRAY,
                                 "draws %u   instances %u   passes %u   dropped %u", drawn.draw_calls, drawn.instances,
                                 drawn.passes, drawn.dropped_draws);
    y += step;
    nya_render2d_textf_with_font(window, HUD_FONT, HUD_FONT_SIZE, 12.0F, y, NYA_COLOR_LIGHT_GRAY,
                                 "frustum-culled %u   occlusion-culled %u   (occluders %u, tests %u)", drawn.culled,
                                 drawn.occluded, hidden.occluders, hidden.tests);
    y += step;
    nya_render2d_textf_with_font(window, HUD_FONT, HUD_FONT_SIZE, 12.0F, y, NYA_COLOR_LIGHT_GRAY,
                                 "particles %u/%u/%u   physics %.2f ms   fluid %.2f ms", nya_particles_count(state->dust),
                                 nya_particles_count(state->sparks), nya_particles_count(state->smoke), physics_ms, fluid_ms);
    y += step;
    nya_render2d_textf_with_font(window, HUD_FONT, HUD_FONT_SIZE, 12.0F, y, NYA_COLOR_GRAY,
                                 "Tab overlay   F free-fly   P post %s   H shadows %s   O occlusion %s   Esc quit",
                                 state->post_on ? "on" : "off", state->shadows_on ? "on" : "off",
                                 state->occlusion_on ? "on" : "off");

    // the engine's own overlay: the trace table, VRAM by kind and the fixed-capacity ceilings. This is the
    // instrumentation the task asks for — it names which resource is fullest, not just the frame time.
    nya_debug_overlay_draw(window, (NYA_DebugOverlayStyle){
                                       .x                   = 12.0F,
                                       .y                   = y + (step * 1.5F),
                                       .page                = (NYA_DebugOverlayPage)state->overlay_page,
                                       .sort                = NYA_TRACE_SORT_GPU,
                                       .show_batch_breakdown = true,
                                       .font                = HUD_FONT,
                                       .font_size           = HUD_FONT_SIZE,
                                   });
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MAIN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The scale named on the command line or in NYA_STRESS_SCALE, or `large` when neither says. */
NYA_INTERNAL u32 initial_scale(s32 argc, NYA_CString* argv) {
    NYA_ConstCString wanted = nullptr;

    for (s32 i = 1; i + 1 < argc; i++) {
        if (nya_string_equals(argv[i], "--scale")) wanted = argv[i + 1];
    }

    if (wanted == nullptr) wanted = getenv("NYA_STRESS_SCALE");

    if (wanted != nullptr) {
        for (u32 i = 0; i < STRESS_SCALE_COUNT; i++) {
            if (nya_string_equals(wanted, STRESS_SCALES[i].name)) return i;
        }
        nya_log_warn("renderer_stress: unknown scale '%s'; using 'large'.", wanted);
    }

    return 1; // large.
}

s32 main(s32 argc, NYA_CString* argv) {
    nya_backtrace_init();

    NYA_EXPECT(nya_app_init(.app_id = "renderer_stress"), "while starting the engine");

    // The scene's root lives on the world, so it shares the world's arena and lifetime.
    Stress* state = nya_arena_alloc(nya_world()->allocator, sizeof(Stress));
    *state        = (Stress){
        .window       = NYA_WINDOW_HANDLE_NONE,
        .scale        = initial_scale(argc, argv),
        .built_scale  = U32_MAX,
        .post_on      = true,
        .shadows_on   = true,
        .occlusion_on = true,
    };

    // an optional frame budget, so a headless or CI run draws a fixed number of frames and then quits.
    NYA_ConstCString frames = getenv("NYA_STRESS_FRAMES");
    if (frames != nullptr) state->max_frames = (u32)strtoul(frames, nullptr, 10);

    // classic SSAO in place of the stylised occlusion, for comparing the two or a headless SSAO run.
    state->ssao_on = getenv("NYA_STRESS_SSAO") != nullptr;

    nya_world_user_data_set(state);

    state->window = nya_window_create(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_RESIZABLE);
    nya_assert(nya_window_is_valid(state->window), "the window could not be created");

    // pushing the layer runs its on_create, which builds the scene.
    nya_layer_push(state->window, nya_layer_of(stress_layer, LAYER_ID));

    nya_app_run();

    nya_app_deinit();

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}

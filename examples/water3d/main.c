/**
 * @file examples/water3d/main.c
 *
 * A flowing river surface. One water draw path (nya_render3d_water) lays travelling waves over a registered
 * strip mesh, refracts the riverbed captured behind it, blends deep channel to shallow bank, and foams the
 * shoreline from the true water depth over the bed (the scene distance buffer the render texture carries) so
 * the foam wraps the banks and rings the submerged boulders, plus the wave crests. The one analytic wind field
 * (render_wind.h) that drives the foliage example drives the water's chop here too, so water, foliage and
 * particles all read one wind.
 *
 * ```
 * ./build run example water3d
 * ```
 *
 * Left/right arrows turn the current, up/down change its speed, `w` toggles the wind driving the chop,
 * `escape` quits. Set NYA_WATER3D_FRAMES=N to draw N frames and quit, for a headless CI run.
 *
 * ## The shape of it
 *
 * The riverbed is a heightfield mesh: a channel dipping below the water plane (y = 0) between banks that rise
 * above it. The water is a second flat strip at y = 0 spanning the channel, registered once; each vertex
 * carries a shore weight in its colour alpha — 0 down the middle, 1 at the banks — which drives both the
 * deep-to-shallow colour and the shoreline foam. Every frame the wind field is advanced once and sampled at
 * the surface, and the sampled push plus the flow parameters are handed to nya_render3d_water, whose vertex
 * stage lifts the strip into waves and whose fragment stage refracts, tints and foams it. The scene is drawn
 * into a render texture (through the post chain) so the refraction has a resolved image to sample.
 * */
#include "genyarated/assets.h"
#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/* CONSTANTS */

#define WINDOW_TITLE  "nyangine — water"
#define WINDOW_WIDTH  1024
#define WINDOW_HEIGHT 720

#define LAYER_ID "water"

/** Registered mesh handles. Reserved names, not asset paths: these meshes are built here, not loaded. */
#define MESH_TERRAIN "water3d/terrain"
#define MESH_RIVER   "water3d/river"

/** The river runs along x; the banks stand out along z. */
#define RIVER_LENGTH 24.0F
#define RIVER_WIDTH  10.0F

/** How far out from the centre line the water strip reaches, inside the rising banks. */
#define WATER_HALF 3.6F

/** How deep the channel bed sits below the water plane, and how high the banks stand above it. */
#define BED_DEPTH  1.2F
#define BANK_HEIGHT 1.6F

/** Grid resolution of each mesh: cells along the river by cells across it. The water is finer, for the waves. */
#define TERRAIN_COLS 48
#define TERRAIN_ROWS 28
#define RIVER_COLS   72
#define RIVER_ROWS   28

/** The most vertices either built mesh uses, so both stage through one fixed scratch buffer. */
#define MESH_VERTICES_MAX 24000

/** How far a key press turns the current, and steps its speed. */
#define FLOW_TURN_STEP  0.2618F /* fifteen degrees */
#define FLOW_SPEED_STEP 0.4F
#define FLOW_SPEED_MAX  4.0F

enum {
    ENTITY_NONE = 0,
};

/* STATE */

typedef struct Water Water;

struct Water {
    NYA_WindowHandle window;

    /** The one wind field the chop reads, the same kind the foliage example steers. */
    NYA_WindField wind;
    b8            wind_on;

    /** Where the current points, as an angle about +y, and how fast it flows, so a key press can step them. */
    f32 flow_azimuth;
    f32 flow_speed;

    /** Total seconds, for the slow camera orbit. */
    f32 elapsed_s;

    /** The post chain: it renders the scene into a texture, which is what the water refraction samples. */
    NYA_PostChain post;

    /** When non-zero (from NYA_WATER3D_FRAMES), the scene quits after this many frames, for a timed run. */
    u32 max_frames;
    u32 frame_count;

    b8 meshes_ready;
};

NYA_INTERNAL Water* water(void) {
    return nya_world_user_data();
}

/* GEOMETRY */

/** A smooth 0..1 ramp between two edges, the Hermite the shaders use; the math module has no scalar one. */
NYA_INTERNAL f32 example_smoothstep(f32 edge0, f32 edge1, f32 x) {
    f32 t = nya_clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);

    return t * t * (3.0F - (2.0F * t));
}

/** The riverbed height at a point: a channel below the water plane between banks that rise above it. */
NYA_INTERNAL f32 terrain_height(f32 x, f32 z) {
    // 0 down the centre line, 1 out at the banks.
    f32 across = nya_clamp(fabsf(z) / (RIVER_WIDTH * 0.5F), 0.0F, 1.0F);

    // the bed climbs from the channel floor up to the bank top across the middle band.
    f32 profile = example_smoothstep(0.30F, 0.72F, across);
    f32 height  = nya_lerp(-BED_DEPTH, BANK_HEIGHT, profile);

    // gentle undulation so the bed and banks are not glassy flat, and something for the refraction to bend over.
    height += 0.12F * sinf(x * 0.6F) * cosf(z * 0.5F);

    return height;
}

/** The bed colour at a point: sandy in the channel, grassy up the banks. */
NYA_INTERNAL f32x3 terrain_color(f32 z) {
    f32 across = nya_clamp(fabsf(z) / (RIVER_WIDTH * 0.5F), 0.0F, 1.0F);

    f32x3 bed  = { 0.52F, 0.44F, 0.30F };
    f32x3 bank = { 0.24F, 0.40F, 0.18F };

    return nya_lerp(bed, bank, example_smoothstep(0.35F, 0.75F, across));
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

/** The riverbed heightfield: banks and a channel, coloured sand to grass, no sway weight in the alpha. */
NYA_INTERNAL u32 build_terrain(NYA_Vertex3D* vertices) {
    u32 count = 0;

    for (u32 row = 0; row < TERRAIN_ROWS; row++) {
        for (u32 col = 0; col < TERRAIN_COLS; col++) {
            f32 x0 = nya_lerp(-RIVER_LENGTH * 0.5F, RIVER_LENGTH * 0.5F, (f32)col / (f32)TERRAIN_COLS);
            f32 x1 = nya_lerp(-RIVER_LENGTH * 0.5F, RIVER_LENGTH * 0.5F, (f32)(col + 1) / (f32)TERRAIN_COLS);
            f32 z0 = nya_lerp(-RIVER_WIDTH * 0.5F, RIVER_WIDTH * 0.5F, (f32)row / (f32)TERRAIN_ROWS);
            f32 z1 = nya_lerp(-RIVER_WIDTH * 0.5F, RIVER_WIDTH * 0.5F, (f32)(row + 1) / (f32)TERRAIN_ROWS);

            f32x3 p00 = { x0, terrain_height(x0, z0), z0 };
            f32x3 p10 = { x1, terrain_height(x1, z0), z0 };
            f32x3 p11 = { x1, terrain_height(x1, z1), z1 };
            f32x3 p01 = { x0, terrain_height(x0, z1), z1 };

            NYA_Color c00 = { terrain_color(z0).x, terrain_color(z0).y, terrain_color(z0).z, 1.0F };
            NYA_Color c10 = c00;
            NYA_Color c11 = { terrain_color(z1).x, terrain_color(z1).y, terrain_color(z1).z, 1.0F };
            NYA_Color c01 = c11;

            grid_triangle(vertices, &count, p00, p10, p11, c00, c10, c11);
            grid_triangle(vertices, &count, p00, p11, p01, c00, c11, c01);
        }
    }

    return count;
}

/** The water strip at y = 0, with the shore weight (0 mid-channel, 1 at the banks) in each vertex's alpha. */
NYA_INTERNAL u32 build_river(NYA_Vertex3D* vertices) {
    u32 count = 0;

    for (u32 row = 0; row < RIVER_ROWS; row++) {
        for (u32 col = 0; col < RIVER_COLS; col++) {
            f32 x0 = nya_lerp(-RIVER_LENGTH * 0.5F, RIVER_LENGTH * 0.5F, (f32)col / (f32)RIVER_COLS);
            f32 x1 = nya_lerp(-RIVER_LENGTH * 0.5F, RIVER_LENGTH * 0.5F, (f32)(col + 1) / (f32)RIVER_COLS);
            f32 z0 = nya_lerp(-WATER_HALF, WATER_HALF, (f32)row / (f32)RIVER_ROWS);
            f32 z1 = nya_lerp(-WATER_HALF, WATER_HALF, (f32)(row + 1) / (f32)RIVER_ROWS);

            // the still surface is flat at y = 0; the water vertex shader lifts it into waves.
            f32x3 p00 = { x0, 0.0F, z0 };
            f32x3 p10 = { x1, 0.0F, z0 };
            f32x3 p11 = { x1, 0.0F, z1 };
            f32x3 p01 = { x0, 0.0F, z1 };

            // shore weight in alpha: how close to the bank this vertex sits. rgb is unused (the colours come from the water uniform), so leave it white.
            f32 s0 = nya_clamp(fabsf(z0) / WATER_HALF, 0.0F, 1.0F);
            f32 s1 = nya_clamp(fabsf(z1) / WATER_HALF, 0.0F, 1.0F);

            NYA_Color c00 = { 1.0F, 1.0F, 1.0F, s0 };
            NYA_Color c10 = c00;
            NYA_Color c11 = { 1.0F, 1.0F, 1.0F, s1 };
            NYA_Color c01 = c11;

            grid_triangle(vertices, &count, p00, p10, p11, c00, c10, c11);
            grid_triangle(vertices, &count, p00, p11, p01, c00, c11, c01);
        }
    }

    return count;
}

/* LAYER */

void water_layer_on_create(NYA_Window* window) {
    nya_assert(window != nullptr);

    Water* state = water();

    NYA_Arena* scratch = nya_arena_create(.name = "water_build");
    defer nya_arena_destroy(scratch);

    NYA_Vertex3D* vertices = nya_arena_alloc(scratch, MESH_VERTICES_MAX * sizeof(NYA_Vertex3D));

    u32 terrain_count   = build_terrain(vertices);
    state->meshes_ready = nya_render3d_mesh_register(window, MESH_TERRAIN, vertices, terrain_count);

    u32 river_count     = build_river(vertices);
    state->meshes_ready = nya_render3d_mesh_register(window, MESH_RIVER, vertices, river_count) && state->meshes_ready;

    // a gentle glow on the sunlit surf.
    nya_post_bloom_set(window, (NYA_PostBloom){ .enabled = true, .threshold = 0.95F, .intensity = 0.8F });
}

void water_layer_on_destroy(NYA_Window* window) {
    Water* state = water();

    nya_render3d_mesh_release(window, MESH_TERRAIN);
    nya_render3d_mesh_release(window, MESH_RIVER);

    nya_post_chain_destroy(&state->post);
}

void water_layer_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window, event);
}

void water_layer_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window);

    Water* state = water();

    if (nya_input_key_pressed(NYA_KEY_ESCAPE)) nya_app_get()->should_quit = true;

    // turn and speed the current on a key press, then re-point the field the chop reads. nya_wind_set leaves the field's clock alone, so the chop does not jump when the current changes.
    b8 changed = false;

    if (nya_input_key_pressed(NYA_KEY_LEFT)) { state->flow_azimuth -= FLOW_TURN_STEP; changed = true; }
    if (nya_input_key_pressed(NYA_KEY_RIGHT)) { state->flow_azimuth += FLOW_TURN_STEP; changed = true; }
    if (nya_input_key_pressed(NYA_KEY_UP)) { state->flow_speed = nya_min(state->flow_speed + FLOW_SPEED_STEP, FLOW_SPEED_MAX); }
    if (nya_input_key_pressed(NYA_KEY_DOWN)) { state->flow_speed = nya_max(state->flow_speed - FLOW_SPEED_STEP, 0.0F); }
    if (nya_input_key_pressed(NYA_KEY_W)) state->wind_on = !state->wind_on;

    if (changed) {
        f32x3 direction = { cosf(state->flow_azimuth), 0.0F, sinf(state->flow_azimuth) };
        nya_wind_set(&state->wind, direction, 2.5F, 0.7F);
    }

    // the one per-frame advance the chop animates from.
    nya_wind_advance(&state->wind, delta_time_s);
    state->elapsed_s += delta_time_s;

    // a timed run quits itself once it has drawn its frames, so a headless CI run terminates.
    state->frame_count++;
    if (state->max_frames > 0 && state->frame_count >= state->max_frames) nya_app_get()->should_quit = true;
}

/** Draws the terrain, the props on the bed, and the flowing water over them. */
NYA_INTERNAL void draw_scene(NYA_Window* window) {
    Water* state = water();

    f32 orbit = state->elapsed_s * 0.12F;

    nya_render3d_begin(window, (NYA_Camera3DPerspective){
                                   .position = { sinf(orbit) * 16.0F, 7.0F, cosf(orbit) * 16.0F },
                                   .target   = { 0.0F, 0.0F, 0.0F },
                               });

    nya_render3d_sky_draw(window, (NYA_Render3DSky){ .sun_direction = nya_vector_normalize((f32x3){ 0.4F, 0.8F, 0.3F }), .sun_angle = 0.03F });

    nya_render3d_light_set(window, (NYA_Render3DLight){
                                       .direction = nya_vector_normalize((f32x3){ -0.4F, -1.0F, -0.3F }),
                                       .color     = NYA_COLOR_WHITE,
                                       .ambient   = 0.45F,
                                       .intensity = 1.0F,
                                   });

    if (!state->meshes_ready) {
        nya_render3d_end(window);
        return;
    }

    // the riverbed, opaque, drawn first so the water's refraction capture sees it behind the surface.
    nya_render3d_mesh(window, MESH_TERRAIN, f32x3_zero, (f32x3){ 1, 1, 1 }, nya_quaternion_identity, NYA_COLOR_WHITE);

    // a few boulders on the bed, so the refraction bends over something with shape and colour.
    for (u32 i = 0; i < 6; i++) {
        f32   fx    = ((f32)i - 2.5F) * 3.4F;
        f32   fz    = sinf((f32)i * 1.7F) * 1.6F;
        f32   y     = terrain_height(fx, fz);
        NYA_Color c = { 0.45F + 0.1F * (f32)(i % 3), 0.40F, 0.34F, 1.0F };

        nya_render3d_sphere(window, (f32x3){ fx, y + 0.25F, fz }, 0.4F, c);
    }

    // a glossy, reflective surface for the sun glint, set just before the water so it applies to it alone.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.85F, .roughness = 0.25F, .reflectance = 0.4F });

    NYA_Render3DWater river = {
        .flow_direction = { cosf(state->flow_azimuth), 0.0F, sinf(state->flow_azimuth) },
        .flow_speed     = state->flow_speed,
        .wave_amplitude = 0.12F,
        .wave_frequency = 0.5F,
        .choppiness     = 0.45F,
        .deep_color     = { 0.02F, 0.10F, 0.16F, 0.90F },
        .shallow_color  = { 0.10F, 0.34F, 0.40F, 0.35F },
        .opacity        = 0.85F,
        .refraction     = 0.55F,
        .foam           = 0.18F,

        // true depth-difference shoreline foam: the foam follows where the riverbed sits close beneath the surface, so it wraps the banks and rings the boulders that rise near the waterline, instead of only the authored shore band. Needs the scene distance buffer the render texture carries (normals, below); drawn to the window it falls back to that band. Zero would keep the authored band alone.
        .depth_foam     = 0.85F,

        // a real planar reflection: the sky and sun glint mirror in the river, rendered from a camera mirrored about the surface into a bounded capture and Fresnel-blended in. Zero would keep the old flat tint.
        .reflection     = 0.9F,
    };

    // share the one wind field with the chop: its push at the surface centre, when the wind is switched on.
    river.wind           = nya_wind_sample(&state->wind, f32x3_zero);
    river.wind_influence = state->wind_on ? 0.6F : 0.0F;

    nya_render3d_water(window, MESH_RIVER, f32x3_zero, (f32x3){ 1, 1, 1 }, nya_quaternion_identity, river);

    nya_render3d_end(window);
}

void water_layer_on_render(NYA_Window* window) {
    nya_assert(window != nullptr);

    Water* state = water();

    // the scene target keeps its depth, both for the 3D pass and so the water refraction reads a resolved image, and its normal/distance buffer, which the water's depth-difference shoreline foam reads to tell how deep it sits over the bed drawn behind it.
    state->post.scene = (NYA_RenderTextureOptions){ .depth = NYA_RENDER_TEXTURE_DEPTH_ATTACHED, .normals = true };

    // through the chain (a render texture, so the refraction has something to sample); straight to the window only if the chain cannot be set up this frame, where the water falls back to its colour. Bloom is a scene feature nya_post_end runs itself, so there are no caller passes here.
    if (nya_post_begin(window, &state->post)) {
        draw_scene(window);
        nya_post_end(window, &state->post, nullptr, 0);
    } else {
        draw_scene(window);
        nya_render_output_scene_end(window);
    }

    nya_render2d_textf_with_font(window, NYA_ASSET_FONTS_ALDRICH_TTF, 20.0F, 16.0F, 16.0F, NYA_COLOR_WHITE,
                                 "flow %.1f  wind %s  ·  arrows steer/speed the current  ·  w wind chop", (double)state->flow_speed,
                                 state->wind_on ? "on" : "off");
}

/* MAIN */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();

    NYA_EXPECT(nya_app_init(.app_id = "water3d"), "while starting the engine");

    Water* state = nya_arena_alloc(nya_world()->allocator, sizeof(Water));

    *state = (Water){
        .window       = NYA_WINDOW_HANDLE_NONE,
        .flow_azimuth = 0.0F,
        .flow_speed   = 1.2F,
        .wind_on      = true,
    };

    state->wind = nya_wind_field((NYA_WindOptions){ .direction = { 1, 0, 0 }, .strength = 2.5F, .gustiness = 0.7F });

    // an optional frame budget, so a headless or CI run draws a fixed number of frames and then quits.
    NYA_ConstCString frames = getenv("NYA_WATER3D_FRAMES");
    if (frames != nullptr) state->max_frames = (u32)strtoul(frames, nullptr, 10);

    nya_world_user_data_set(state);

    state->window = nya_window_create(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_RESIZABLE);
    nya_assert(nya_window_is_valid(state->window), "the window could not be created");

    nya_layer_push(state->window, nya_layer_of(water_layer, LAYER_ID));

    nya_app_run();

    nya_app_deinit();

    nya_backtrace_deinit();
    return 0;
}

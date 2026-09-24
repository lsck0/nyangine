/**
 * @file examples/gpu_scene/main.c
 *
 * A small, readable 3D plaza that shows off four renderer features that landed together, each doing the
 * one job it is best at, in the flat, stylized look the other 3D demos share (a restrained palette, one
 * soft shading step, never photoreal):
 *
 *   - **Screen-space reflections** (render_post.h, `nya_post_ssr_set`): a polished floor mirrors the ring
 *     of pillars standing on it, and the pillars catch each other at grazing angles. This is the
 *     "polished floor" case the SSR pass is written for — a scene-wide reflection with no planar mirror.
 *   - **SSAO** (render_post.h, `nya_post_ssao_set`): the textbook hemisphere-kernel ambient occlusion
 *     darkens the contact seams where the pillars meet the floor, the shading a flat renderer otherwise
 *     misses. SSR and SSAO both read the chain's scene normal buffer, so the whole scene is drawn through
 *     the post chain (a render texture) rather than straight to the window.
 *   - **The composable force-field primitive** (render_force.h): a *CPU* particle column spirals up the
 *     central monolith, driven not by a bespoke updater but by a NYA_ForceSet composed out of orthogonal
 *     pieces — a vortex about the column's axis, a drag that keeps the swirl from running away, and a
 *     gentle uniform lift. The particle system's `on_update` hook samples the summed field per particle,
 *     which is the whole point of the primitive: behaviour built by adding small fields, not by writing a
 *     new one.
 *   - **GPU-compute particles** (render_compute_particles.h): when the device has a compute stage, a
 *     field whose positions and velocities never leave the GPU is overlaid in the corner. It is desktop
 *     only (WebGL2 has no compute), and a device without compute simply returns null from create — the
 *     scene then draws without the overlay, so this piece degrades gracefully rather than branching the
 *     rest of the frame.
 *
 * ```
 * ./build run example gpu_scene
 * NYA_GPU_SCENE_FRAMES=6 ./build run example gpu_scene   # draw six frames and quit, for a headless run
 * ```
 *
 * It only calls the public renderer API and registers no engine systems. The CPU force-driven column and
 * the optional GPU field are deliberately side by side: one shows the composition seam a gameplay system
 * reads on the CPU, the other shows the same idea (a field of particles integrated each frame) living
 * entirely on the GPU through the compute pipeline.
 * */
#include "genyarated/assets.h"
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define WINDOW_TITLE  "nyangine — gpu scene"
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/** The layer's id, compared by content so it survives a code reload. */
#define LAYER_ID "gpu_scene"

/** The polished floor: a square plane at y = 0, wide enough that its reflection fills the lower frame. */
#define FLOOR_HALF 26.0F

/** The ring of reflective pillars the floor mirrors: how many, how far out, and how tall. */
#define PILLAR_COUNT  9u
#define PILLAR_RADIUS 9.0F
#define PILLAR_HEIGHT 5.0F
#define PILLAR_WIDTH  1.1F

/** The central monolith the particle column climbs, and the axis the vortex swirls about. */
#define MONOLITH_HEIGHT 8.0F
#define MONOLITH_WIDTH  1.6F

/** The CPU particle column: a fixed pool, refilled on a timer near the base so its rate is frame-rate free. */
#define COLUMN_POOL      4000u
#define COLUMN_PER_BURST 40u
#define COLUMN_TEXTURE   NYA_ASSET_TEXTURES_PUFF_PNG

/** The GPU-compute field's particle count and texture resolution, and where the overlay sits, in pixels. */
#define COMPUTE_PARTICLES 1024u
#define COMPUTE_RESOLUTION 192u
#define COMPUTE_OVERLAY   192.0F
#define COMPUTE_MARGIN    16.0F

/** The raymarched volumetric's image resolution: the fog is composited full-frame, so it is upscaled to the window. */
#define VOLUMETRIC_RESOLUTION 256u

#define HUD_FONT      NYA_ASSET_FONTS_ALDRICH_TTF
#define HUD_FONT_SIZE 16.0F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct GpuScene GpuScene;

struct GpuScene {
    NYA_WindowHandle window;

    /** The post chain: the scene draws into its texture so SSR and SSAO have a scene and a normal buffer to read. */
    NYA_PostChain post;

    /** The CPU particle column. Lives on the world arena, so it goes with the world's lifetime. */
    NYA_ParticleSystem* column;

    /**
     * The one force field the column reads: a vortex, a drag and a lift, summed. The on_update hook is handed a
     * pointer to this, so every mote is steered by the composition rather than by a hand-written updater.
     * */
    NYA_ForceSet forces;

#if !OS_WASM
    /**
     * The GPU-compute particle field, or null on a device with no compute stage (some offscreen and software
     * backends). Null is the graceful case: step and draw ignore it and the rest of the frame is unchanged.
     * */
    NYA_GPUParticleField* compute_field;

    /**
     * The raymarched volumetric fog, composited full-frame over the scene, or null with no compute stage. Its
     * begin marches the field on its own command buffer in on_update; its end composites it in on_render.
     * */
    NYA_GPUVolumetric* volume;
#endif

    /** Total seconds, driving the slow camera orbit. */
    f32 elapsed_s;

    /** Fed on a timer so the column's emission rate does not ride the frame rate. */
    f32 emit_timer_s;

    /** Logged once, on the first frame, so a headless run's log proves each feature's path ran. */
    b8 logged_paths;

    /** When non-zero (from NYA_GPU_SCENE_FRAMES), the scene quits after this many frames, for a headless run. */
    u32 max_frames;
    u32 frame_count;

    /** The last frame's fps, captured in on_render and logged at teardown so a headless run reports a number. */
    f32 last_fps;
};

NYA_INTERNAL GpuScene* gpu_scene(void) {
    return nya_world_user_data();
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FORCE FIELD
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Composes the column's motion out of three orthogonal fields, in place, the way render_force.h intends: a
 * consumer holds one NYA_ForceSet and the caller fills it field by field, no allocation and no bespoke builder.
 *
 *   - a VORTEX about the +y axis through the monolith, so motes swirl around the column;
 *   - a DRAG, the one kind that reads velocity, so the swirl settles to a steady spin rather than flinging out;
 *   - a UNIFORM lift, the plain directional push, so the spiral climbs the monolith instead of pooling at the base.
 *
 * Sampling the set is a pure function of its bytes, so the column looks the same on any run.
 * */
NYA_INTERNAL NYA_ForceSet column_forces(void) {
    NYA_ForceSet set = { 0 };

    set.fields[set.count++] = nya_force_field((NYA_ForceOptions){
        .kind     = NYA_FORCE_VORTEX,
        .direction = { 0.0F, 1.0F, 0.0F },
        .center   = { 0.0F, 0.0F, 0.0F },
        .strength = 16.0F,
        .radius   = 7.0F,
        .falloff  = NYA_FORCE_FALLOFF_LINEAR,
    });

    set.fields[set.count++] = nya_force_field((NYA_ForceOptions){
        .kind     = NYA_FORCE_DRAG,
        .strength = 0.9F,
    });

    set.fields[set.count++] = nya_force_field((NYA_ForceOptions){
        .kind      = NYA_FORCE_UNIFORM,
        .direction = { 0.0F, 1.0F, 0.0F },
        .strength  = 2.4F,
    });

    return set;
}

/**
 * Steers one mote after its integration: read the summed force at the mote's own position and velocity, and
 * ease that acceleration into its velocity. `user_data` is the scene's NYA_ForceSet, advanced once a frame.
 * */
NYA_INTERNAL void column_ride_forces(NYA_Particle* particle, f32 t, f32 delta_time_s, void* user_data) {
    nya_unused(t);

    const NYA_ForceSet* forces = user_data;

    f32x3 acceleration = nya_forces_sample(forces, particle->position, particle->velocity);

    particle->velocity = particle->velocity + (acceleration * delta_time_s);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYER: CREATE / DESTROY / EVENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gpu_scene_layer_on_create(NYA_Window* window) {
    nya_assert(window != nullptr);

    GpuScene* state = gpu_scene();

    // a bright dusk-blue behind everything, so the plaza reads against it before the sky pass paints over it, and
    // so nothing renders as a black frame — the readability the other demos learned the hard way.
    nya_render_clear_color_set(window, (NYA_Color){ 0.12F, 0.16F, 0.24F, 1.0F });

    // the CPU column, from the world's arena so it shares the world's lifetime. Its on_update reads the force set,
    // which is why every mote is carried by the composed vortex/drag/lift rather than by its own code.
    state->forces = column_forces();

    state->column = nya_particles_create(nya_world()->allocator, COLUMN_POOL);
    nya_particles_space_set(state->column, NYA_PARTICLE_SPACE_3D);
    nya_particles_texture_set(state->column, COLUMN_TEXTURE);
    nya_particles_on_update_set(state->column, column_ride_forces, &state->forces);

    (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = COLUMN_TEXTURE });

#if !OS_WASM
    // the GPU-compute field, built once (and again after a code reload, which destroyed it). Create checks the
    // device for a compute stage itself and returns null when there is none, logging why; the scene carries on.
    state->compute_field = nya_gpu_particle_field_create(window, COMPUTE_PARTICLES, COMPUTE_RESOLUTION);

    nya_log_info("gpu_scene: GPU compute particle field %s.",
                 state->compute_field != nullptr ? "created (device has a compute stage)"
                                                  : "unavailable (no compute on this device) — drawing without it");

    // the raymarched volumetric, its parameters set through the window the way SSR's are, and on so the demo shows
    // it; nya_settings_graphics_apply would gate it against the player's fog switch. Create degrades to null the
    // same way the field does when the device has no compute.
    nya_volumetric_params_set(window, (NYA_VolumetricParams){
                                          .enabled         = true,
                                          .density         = 1.6F,
                                          .absorption      = 1.2F,
                                          .steps           = 56,
                                          .light_direction = { -0.45F, -0.62F, -0.30F },
                                      });

    state->volume = nya_gpu_volumetric_create(window, VOLUMETRIC_RESOLUTION);
#endif

    // a small shadow atlas, so the pillars cast onto the floor and the contact reads even before SSAO deepens it.
    nya_render3d_shadow_options_set(window, (NYA_Render3DShadowOptions){
                                                .cascades = NYA_RENDER3D_SHADOW_CASCADES,
                                                .map_size = 2048,
                                                .color    = { 0.10F, 0.12F, 0.20F, 0.5F },
                                            });
}

void gpu_scene_layer_on_destroy(NYA_Window* window) {
    GpuScene* state = gpu_scene();

    // every scene feature this layer switched on, off again, so a re-enter (a hot reload) starts clean.
    nya_post_ssr_set(window, (NYA_PostSsr){ 0 });
    nya_post_ssao_set(window, (NYA_PostSsao){ 0 });

    nya_post_chain_destroy(&state->post);

#if !OS_WASM
    // the compute field's buffer, texture and pipelines. Null when the device had no compute, which destroy ignores.
    nya_gpu_particle_field_destroy(window, state->compute_field);
    state->compute_field = nullptr;

    // the volumetric's pipeline and image, and its parameters off again so a hot reload starts clean.
    nya_gpu_volumetric_destroy(window, state->volume);
    state->volume = nullptr;
    nya_volumetric_params_set(window, (NYA_VolumetricParams){ 0 });
#endif

    // leave the frame count in the log so a headless run reports it verified something. The column lives on the
    // world arena and goes with it.
    nya_log_info("gpu_scene: shutting down after %u frames, last %.1f fps.", state->frame_count, (f64)state->last_fps);
}

void gpu_scene_layer_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window);
    nya_assert(event != nullptr);

    if (event->type == NYA_EVENT_KEY_DOWN && event->as_key_event.key == NYA_KEY_ESCAPE) {
        nya_app_get()->should_quit = true;
        event->was_handled         = true;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYER: UPDATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gpu_scene_layer_on_update(NYA_Window* window, f32 delta_time_s) {
    GpuScene* state = gpu_scene();

    state->elapsed_s += delta_time_s;

    // the force set's shared clock, once a frame, so its drag and turbulence read a consistent time; then the
    // column, whose on_update samples the set per mote.
    nya_forces_advance(&state->forces, delta_time_s);
    nya_particles_update(state->column, delta_time_s);

#if !OS_WASM
    // the GPU-compute field, once a tick, on its own command buffer (it opens no render pass). Ignored when null.
    // The frame draws the texture it leaves behind; see gpu_scene_layer_on_render.
    nya_gpu_particle_field_step(window, state->compute_field, delta_time_s);

    // the volumetric's march, likewise on its own command buffer; on_render composites the volume it leaves behind.
    // Ignored when null or switched off.
    nya_gpu_volumetric_begin(window, state->volume, delta_time_s);
#endif

    // release motes near the base of the monolith on a timer, so the column's rate does not ride the frame rate.
    // They are given a small outward-and-up kick; the force set does the rest, curling them into the spiral.
    state->emit_timer_s += delta_time_s;

    while (state->emit_timer_s >= 0.03F) {
        state->emit_timer_s -= 0.03F;

        (void)nya_particles_emit(state->column, (NYA_ParticleBurst){
                                                    .shape       = NYA_PARTICLE_SHAPE_BOX,
                                                    .position    = { 0.0F, 0.4F, 0.0F },
                                                    .volume      = { 4.4F, 0.4F, 4.4F },
                                                    .count       = COLUMN_PER_BURST,
                                                    .direction   = { 0.0F, 1.0F, 0.0F },
                                                    .spread      = 0.6F,
                                                    .speed       = { 1.5F, 3.0F },
                                                    .lifetime_s  = { 2.4F, 4.0F },
                                                    .size        = { 0.10F, 0.20F },
                                                    .size_end    = { 0.02F, 0.06F },
                                                    .color_start = { 0.55F, 0.85F, 1.0F, 0.9F },
                                                    .color_end   = { 0.30F, 0.45F, 0.85F, 0.0F },
                                                    .damping     = 0.1F,
                                                });
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

/** Which way the sun's light travels. One place, so the sky disc and the shading agree. */
NYA_INTERNAL f32x3 sun_travel(void) {
    return nya_vector_normalize((f32x3){ -0.45F, -0.62F, -0.30F });
}

/** Where pillar `index` stands, evenly around the ring. */
NYA_INTERNAL f32x3 pillar_position(u32 index) {
    f32 angle = ((f32)index / (f32)PILLAR_COUNT) * 2.0F * (f32)M_PI;

    return (f32x3){ cosf(angle) * PILLAR_RADIUS, PILLAR_HEIGHT * 0.5F, sinf(angle) * PILLAR_RADIUS };
}

/** The whole 3D scene, between begin and end, so the post chain can capture it for SSR and SSAO to read. */
NYA_INTERNAL void draw_scene(NYA_Window* window) {
    GpuScene* state = gpu_scene();

    // a slow orbit at a modest height, so the polished floor is seen at a grazing angle where reflections read,
    // and the ring, the column and the sun all pass through frame.
    f32   t      = state->elapsed_s * 0.12F;
    f32   radius = 24.0F;
    f32x3 eye    = { cosf(t) * radius, 9.0F + (sinf(t * 0.7F) * 2.0F), sinf(t) * radius };

    nya_render3d_begin(window, (NYA_Camera3DPerspective){
                                   .position  = eye,
                                   .target    = { 0.0F, 3.5F, 0.0F },
                                   .far_plane = 200.0F,
                               });

    f32x3 sun = sun_travel();

    // a warm sun with a generous hemispheric ambient (sky on tops, bounce on undersides), so a surface turned
    // from the sun is still lit rather than black — the flat look wants readable shade, not darkness.
    nya_render3d_light_set(window, (NYA_Render3DLight){
                                       .direction = sun,
                                       .color     = { 1.0F, 0.94F, 0.82F, 1.0F },
                                       .ambient   = 0.45F,
                                       .intensity = 1.1F,
                                       .sky       = { 0.50F, 0.62F, 0.85F, 1.0F },
                                       .ground    = { 0.32F, 0.30F, 0.28F, 1.0F },
                                   });

    // the sky behind everything, its sun aligned with the light (the sky wants a direction toward the sun).
    nya_render3d_sky_draw(window, (NYA_Render3DSky){
                                      .zenith        = { 0.20F, 0.34F, 0.60F, 1.0F },
                                      .horizon       = { 0.62F, 0.70F, 0.82F, 1.0F },
                                      .ground        = { 0.14F, 0.16F, 0.20F, 1.0F },
                                      .sun_direction = -sun,
                                      .sun_color     = { 1.0F, 0.88F, 0.66F, 1.0F },
                                      .sun_intensity = 1.4F,
                                  });

    // the polished floor: low roughness and a metallic highlight, so the SSR pass has a near-mirror to reflect the
    // ring into. A cool mid grey-blue, bright enough to read as stone, not a black slab.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.6F, .roughness = 0.12F, .reflectance = 0.2F });
    nya_render3d_plane(window, (f32x3){ 0.0F, 0.0F, 0.0F }, (f32x2){ FLOOR_HALF * 2.0F, FLOOR_HALF * 2.0F },
                       (NYA_Color){ 0.34F, 0.40F, 0.50F, 1.0F });

    // the ring of reflective pillars: a metallic material with a rim so they catch the light and each other in the
    // SSR pass, and cast into the shadow atlas onto the floor for the SSAO seam to deepen.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.8F, .roughness = 0.2F, .reflectance = 0.35F, .edge = 0.2F });

    static const NYA_Color pillar_palette[] = {
        { 0.86F, 0.42F, 0.38F, 1.0F }, { 0.42F, 0.66F, 0.88F, 1.0F }, { 0.92F, 0.80F, 0.44F, 1.0F },
        { 0.56F, 0.82F, 0.52F, 1.0F }, { 0.80F, 0.56F, 0.86F, 1.0F }, { 0.46F, 0.82F, 0.80F, 1.0F },
    };

    for (u32 i = 0; i < PILLAR_COUNT; i++) {
        nya_render3d_cube(window, pillar_position(i), (f32x3){ PILLAR_WIDTH, PILLAR_HEIGHT, PILLAR_WIDTH },
                          nya_quaternion_identity, pillar_palette[i % nya_carray_length(pillar_palette)]);
    }

    // the central monolith the column climbs: a tall chrome slab, the brightest mirror in the scene.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 1.0F, .roughness = 0.08F, .reflectance = 0.4F });
    nya_render3d_cube(window, (f32x3){ 0.0F, MONOLITH_HEIGHT * 0.5F, 0.0F },
                      (f32x3){ MONOLITH_WIDTH, MONOLITH_HEIGHT, MONOLITH_WIDTH }, nya_quaternion_identity,
                      (NYA_Color){ 0.72F, 0.78F, 0.84F, 1.0F });

    // the CPU force-driven column, drawn additive so the spiral glows against the metal.
    nya_render3d_blend_set(window, NYA_RENDER3D_BLEND_ADDITIVE);
    nya_particles_draw(window, state->column);
    nya_render3d_blend_set(window, NYA_RENDER3D_BLEND_ALPHA);

    nya_render3d_end(window);
}

void gpu_scene_layer_on_render(NYA_Window* window) {
    GpuScene* state = gpu_scene();

    // the two scene passes this example is about: a fresnel-weighted screen-space reflection over the whole scene,
    // and the textbook hemisphere SSAO. Both read the chain's scene normal buffer, so the scene is drawn through
    // the post chain below rather than straight to the window.
    nya_post_ssr_set(window, (NYA_PostSsr){ .enabled = true, .strength = 0.7F, .max_distance = 24.0F });
    nya_post_ssao_set(window, (NYA_PostSsao){ .enabled = true, .strength = 0.6F, .radius = 0.8F });

    // the scene target keeps its depth, both for the 3D pass and for the reflections' march.
    state->post.scene = (NYA_RenderTextureOptions){ .depth = NYA_RENDER_TEXTURE_DEPTH_ATTACHED };

    // through the chain when a scene pass wants it (it does — SSR and SSAO are on); otherwise straight to the
    // window. nya_post_begin can fail to build its targets, so the fallback still has to draw the scene.
    if (nya_post_enabled(window) && nya_post_begin(window, &state->post)) {
        draw_scene(window);
        nya_post_end(window, &state->post, nullptr, 0);
    } else {
        draw_scene(window);
        nya_render_output_scene_end(window);
    }

    const NYA_FrameStats* frame = &nya_app_get()->frame_stats;
    state->last_fps             = frame->fps;

    // once, on the first drawn frame, name the paths that ran so a headless log proves each feature executed.
    if (!state->logged_paths) {
        state->logged_paths = true;

#if !OS_WASM
        NYA_ConstCString compute = state->compute_field != nullptr ? "on" : "absent";
#else
        NYA_ConstCString compute = "absent";
#endif

        nya_log_info("gpu_scene: paths — SSR on, SSAO on, force-driven column %u motes, GPU compute %s.",
                     nya_particles_count(state->column), compute);
    }

#if !OS_WASM
    // the volumetric fog, composited full-frame over the flushed scene before the HUD, so the text stays readable
    // above it. The march that filled this image ran in on_update; here it is only sampled. Ignored when null or off.
    {
        u32 fog_width = 0, fog_height = 0;
        nya_render2d_target_size(window, &fog_width, &fog_height);
        nya_gpu_volumetric_end(window, state->volume, 0.0F, 0.0F, (f32)fog_width, (f32)fog_height);
    }
#endif

    // the HUD, in screen pixels over the flushed scene.
    f32 y    = 12.0F;
    f32 step = HUD_FONT_SIZE * 1.35F;

    nya_render2d_textf_with_font(window, HUD_FONT, HUD_FONT_SIZE, 12.0F, y, NYA_COLOR_WHITE,
                                 "gpu scene   %.1f fps   SSR + SSAO + force-field column", (f64)frame->fps);
    y += step;
    nya_render2d_textf_with_font(window, HUD_FONT, HUD_FONT_SIZE, 12.0F, y, NYA_COLOR_LIGHT_GRAY,
                                 "CPU column %u motes (vortex + drag + lift)", nya_particles_count(state->column));

#if !OS_WASM
    // the GPU-compute field, drawn in the bottom-right corner over the flushed scene. The compute passes that
    // filled this texture ran in on_update on a command buffer of their own; here it is only sampled. Ignored
    // when the device had no compute.
    if (state->compute_field != nullptr) {
        u32 target_width = 0, target_height = 0;
        nya_render2d_target_size(window, &target_width, &target_height);

        nya_gpu_particle_field_draw(window, state->compute_field, (f32)target_width - COMPUTE_OVERLAY - COMPUTE_MARGIN,
                                    (f32)target_height - COMPUTE_OVERLAY - COMPUTE_MARGIN, COMPUTE_OVERLAY, COMPUTE_OVERLAY);

        nya_render2d_textf_with_font(window, HUD_FONT, HUD_FONT_SIZE, (f32)target_width - COMPUTE_OVERLAY - COMPUTE_MARGIN,
                                     (f32)target_height - COMPUTE_OVERLAY - COMPUTE_MARGIN - step, NYA_COLOR_LIGHT_GRAY,
                                     "GPU compute field");
    }
#endif
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MAIN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc);
    nya_unused(argv);

    nya_backtrace_init();

    NYA_EXPECT(nya_app_init(.app_id = "gpu_scene"), "while starting the engine");

    // the scene's root lives on the world, so it shares the world's arena and lifetime.
    GpuScene* state = nya_arena_alloc(nya_world()->allocator, sizeof(GpuScene));
    *state          = (GpuScene){ .window = NYA_WINDOW_HANDLE_NONE };

    // an optional frame budget, so a headless or CI run draws a fixed number of frames and then quits.
    NYA_ConstCString frames = getenv("NYA_GPU_SCENE_FRAMES");
    if (frames != nullptr) state->max_frames = (u32)strtoul(frames, nullptr, 10);

    nya_world_user_data_set(state);

    state->window = nya_window_create(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_RESIZABLE);
    nya_assert(nya_window_is_valid(state->window), "the window could not be created");

    // pushing the layer runs its on_create, which builds the scene.
    nya_layer_push(state->window, nya_layer_of(gpu_scene_layer, LAYER_ID));

    nya_app_run();

    nya_app_deinit();

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}

/**
 * @file layer_cube3d.c
 *
 * The 3D scene: noise-generated terrain, a pile of falling cubes, and one you can click and spin. It
 * is the example that exercises what is new in three dimensions versus a 2D layer:
 *
 * - **NYA_Camera3DPerspective** instead of position and zoom: placed and aimed, so orbiting is
 *   spherical coordinates rather than a pan.
 * - **nya_physics3d_body_attach**: bodies carry a full quaternion, read straight off the entity, with
 *   no angle to pick out of it.
 * - **nya_render3d_cube**, batched with a PBR material — ground and cube are two draw calls because
 *   they are two *materials*, not two objects.
 * - **A ray, not a point**: a 3D click names a line, so picking goes through
 *   nya_render3d_screen_ray into nya_physics3d_raycast.
 * - **render2d over the top**, same frame and render pass, no camera needed for the HUD.
 *
 * The ground is a heightmap rather than a flat plane: a flat floor exercises one contact normal and
 * settles in the first second, saying nothing about the solver, batching or shading under load. See
 * system_terrain3d.c.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where the camera sits, from its orbit angles. Spherical to cartesian, y up. */
NYA_INTERNAL f32x3 _gny_cube3d_camera_position(const GNY_Cube3DScene* scene);

/** The scene's state, hung off the world so it survives a hot reload. */
NYA_INTERNAL GNY_Cube3DScene* _gny_cube3d_scene(void);

/** Where the draggable cube starts: above the middle of the terrain, whatever height that is. */
NYA_INTERNAL f32x3 _gny_cube3d_drop_point(void);

/** How blocked a sound at `source` is from the camera. Installed as the engine's occlusion callback. */
NYA_INTERNAL f32 _gny_cube3d_occlusion(f32x3 source, void* user_data);

/**
 * Where the pile's `index`-th cube starts, hashed from the index alone — deterministic, so R replays the
 * same pile and a recycled cube follows the same rule as a fresh one.
 * */
NYA_INTERNAL f32x3 _gny_cube3d_cube_placement(u32 index);

/** Full edge length of the pile's `index`-th cube, metres. Hashed, like its placement. */
NYA_INTERNAL f32 _gny_cube3d_cube_size(u32 index);

/** Spawns one of the pile, sized, coloured and placed from `index`. Zeroed when the spawn fails. */
NYA_INTERNAL GNY_FallingCube _gny_cube3d_cube_spawn(u32 index);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON CREATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_cube3d_on_create(NYA_Window* window) {

    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    *scene = (GNY_Cube3DScene){
        .orbit_yaw   = GNY_CUBE3D_ORBIT_YAW,
        .orbit_pitch = GNY_CUBE3D_ORBIT_PITCH,
        .orbit_range = GNY_CUBE3D_ORBIT_RANGE,

        // From the world's arena, so it dies with the world rather than needing its own teardown.
        .dust = nya_particles_create(nya_world()->allocator, GNY_CUBE3D_DUST_POOL),

        // Two systems: a system draws in one blend mode and these need different ones. See GNY_Cube3DScene.
        .fire  = nya_particles_create(nya_world()->allocator, GNY_CUBE3D_FIRE_POOL),
        .smoke = nya_particles_create(nya_world()->allocator, GNY_CUBE3D_SMOKE_POOL),
    };

    // Queued rather than waited on: nya_asset_load returns once queued and the read lands at frame end,
    // so early frames draw without it (nya_render3d_mesh draws nothing until loaded) instead of stalling
    // the opening frame. Not fatal — a missing model leaves the scene with its primitives. Same pipeline
    // the 2D world composites through; see gny_bloom_pipeline_ensure.
    gny_bloom_pipeline_ensure(window);

    // Queued here too — not a duplicate, since nya_asset_load is keyed on the handle and a second
    // request is a no-op — so this scene works regardless of visit order. Predecoded: decoding on the
    // audio thread at the moment of impact is exactly when a hitch is audible.
    NYA_Error sound = nya_asset_load((NYA_AssetLoadParameters){
        .type     = NYA_ASSET_TYPE_SOUND,
        .handle   = NYA_ASSET_SOUNDS_HIT_WAV,
        .as_sound = { .predecode = true },
    });

    // Not fatal. A machine with no audio device still shows the scene, and the play call below already
    // treats a missing asset as a no-op.
    if (!sound.ok) nya_log_warn("%s", (NYA_ConstCString)sound.message);

    NYA_AssetHandle models[] = { GNY_CUBE3D_MODEL, GNY_CUBE3D_PILL };

    for (u64 i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        NYA_Error queued = nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_MESH, .handle = models[i] });

        if (!queued.ok) {
            nya_log_error("Could not queue the 3D model '%s' (%s); the scene will draw without it.", models[i],
                          (NYA_ConstCString)queued.message);
        }
    }

    // The one line that makes this system 3D. Everything above it — the pool, the shapes, the
    // integration — is identical to the 2D one; only the draw differs. See render_particles.h.
    nya_particles_space_set(scene->dust, NYA_PARTICLE_SPACE_3D);
    nya_particles_space_set(scene->fire, NYA_PARTICLE_SPACE_3D);
    nya_particles_space_set(scene->smoke, NYA_PARTICLE_SPACE_3D);

    // A soft radial sprite on the plume: an untextured billboard is a hard-edged square, and this alpha
    // falls off smoothly to nothing at the rim so overlapping puffs blend into a mass instead of showing
    // outlines. Not on the dust — impact chips are meant to look small, hard and countable.
    NYA_Error puff = nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = GNY_CUBE3D_PUFF_TEXTURE });

    // Not fatal: nya_render3d_billboard draws untextured when the handle names nothing loaded, so a
    // missing sprite is a plume of squares rather than no plume.
    if (!puff.ok) nya_log_warn("%s", (NYA_ConstCString)puff.message);

    nya_particles_texture_set(scene->fire, GNY_CUBE3D_PUFF_TEXTURE);
    nya_particles_texture_set(scene->smoke, GNY_CUBE3D_PUFF_TEXTURE);

    // Gravity is negative y here, not positive: the 2D world's is positive because the screen's y grows
    // downward, while every 3D convention puts y up — the two solvers disagree about which way is down,
    // which costs nothing since nothing is ever simulated in both.
    nya_physics3d_gravity_set(NYA_PHYSICS3D_GRAVITY_DEFAULT);

    // The ground, before anything that lands on it: a static triangle mesh from fBm noise (see the file
    // header, and system_terrain3d.c for how), in the world's arena since the sample grid must outlive
    // this function. Seeded from the launch seed like the 2D terrain, mixed with a constant so the two
    // don't generate the same shape from the same number.
    gny_terrain3d_generate(window, nya_world()->allocator, GNY_LAUNCH.world_seed ^ GNY_TERRAIN3D_SEED);

    // The cube, dropped from a height so the first thing the scene does is show the solver working.
    // Rotation is never touched by this file after the spawn — it's the solver's, written back onto the
    // entity every tick as a full quaternion, and the drag handler adds to it via angular impulse rather
    // than assignment, since writing a simulated body's transform fights the solver.
    scene->cube = nya_entity_spawn(
        .name     = "cube",
        .type     = GNY_ENTITY_CUBE3D,
        .position     = _gny_cube3d_drop_point(),
        .state        = NYA_ENTITY_STATE_ACTIVE,
        .on_collision = nya_callback(gny_layer_cube3d_on_collision),
        .on_click     = nya_callback(gny_layer_cube3d_on_cube_click)
    );

    (void)nya_physics3d_body_attach(
        scene->cube,
        .type            = NYA_PHYSICS_BODY_DYNAMIC,
        .shape           = NYA_PHYSICS3D_SHAPE_BOX,
        .size            = { GNY_CUBE3D_SIZE, GNY_CUBE3D_SIZE, GNY_CUBE3D_SIZE },
        .density         = 400.0F,
        .friction        = 0.5F,
        .restitution     = 0.2F,
        // Otherwise the solver parks it after a few seconds and the drag does nothing until it is
        // poked. A demo whose one interactive object goes to sleep reads as broken.
        .never_sleep     = true,
        .angular_damping = 1.5F
    );

    // The two models, as entities now rather than two draw calls at fixed offsets. Spawned here without
    // bodies: a model's size comes from its vertices, still in the asset queue at this point, so the
    // attach happens in gny_layer_cube3d_models_attach on the first update that finds them loaded.
    scene->model = nya_entity_spawn(
        .name         = "cubie",
        .type         = GNY_ENTITY_CUBE3D,
        .position     = { GNY_CUBE3D_MODEL_OFFSET, gny_terrain3d()->max_height + GNY_CUBE3D_MODEL_LIFT, 0.0F },
        .state        = NYA_ENTITY_STATE_ACTIVE,
        .on_collision = nya_callback(gny_layer_cube3d_on_collision)
    );

    scene->pill = nya_entity_spawn(
        .name         = "pill",
        .type         = GNY_ENTITY_CUBE3D,
        .position     = { GNY_CUBE3D_PILL_OFFSET, gny_terrain3d()->max_height + GNY_CUBE3D_PILL_LIFT, 0.0F },
        .state        = NYA_ENTITY_STATE_ACTIVE,
        .on_collision = nya_callback(gny_layer_cube3d_on_collision)
    );

    // Occlusion, driven by the solver this scene already has: core_audio takes a callback rather than
    // raycasting itself (see nya_audio_occlusion_set), since it has no business knowing about physics3d.
    // Worth it here specifically — the terrain is a bowl with a raised rim, so a cube on the far slope
    // genuinely has ground between it and the camera.
    //
    // Reverb on the effects bus, not master, so music isn't put in the impacts' room; it runs *after* the
    // occlusion filter on the same bus, so a sound muffled by a hill reverberates muffled too. See
    // _nya_audio_group_mix_callback.
    nya_audio_bus_reverb_set(
        NYA_AUDIO_BUS_SOUND,
        (NYA_AudioReverb){
            .room_size = GNY_CUBE3D_REVERB_ROOM,
            .damping   = GNY_CUBE3D_REVERB_DAMPING,
            .wet       = GNY_CUBE3D_REVERB_WET,
            .dry       = GNY_CUBE3D_REVERB_DRY,
        }
    );

    nya_audio_occlusion_set(
        _gny_cube3d_occlusion,
        nullptr,
        (NYA_AudioOcclusion){
            .lowpass_hz = GNY_CUBE3D_OCCLUSION_HZ,
            .gain       = GNY_CUBE3D_OCCLUSION_GAIN,
            .glide_ms   = GNY_CUBE3D_OCCLUSION_GLIDE_MS,
        }
    );

    // The pile, last, so it falls past what is already there rather than into empty space.
    gny_layer_cube3d_cubes_drop();
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON DESTROY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_cube3d_on_destroy(NYA_Window* window) {
    nya_unused(window);

    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    // The offscreen bloom target used not to be released here. GNY_World.scene is shared with the 2D
    // game layer (one window-sized texture, since the two screens are never both up), and the release
    // lived only in gny_layer_game_on_destroy — wrong for main menu straight into the 3D scene and back
    // out, which never runs that on_destroy, leaking the texture and its Vulkan views at exit.
    // LeakSanitizer reports it as a block from SDL_BeginGPURenderPass, which names the allocation site
    // not the owner. Guarded on the texture, not on which layer got there first, so both releases are
    // safe in either order; safe on shutdown too since nya_app_deinit destroys windows before the
    // renderer, leaving the device alive here.
    GNY_World* world = gny_world();
    if (world != nullptr) nya_post_chain_destroy(&world->post);

    // Despawning destroys the 3D body with it, same contract as 2D. Deferred, since this can run from
    // inside the layer stack's own iteration.
    // Uninstalled before the solver it raycasts goes away; passing null also clears whatever it was
    // applying, so a voice muffled here isn't left muffled forever.
    nya_audio_occlusion_set(nullptr, nullptr, (NYA_AudioOcclusion){ 0 });

    // The room goes with the scene. A zeroed room size switches it off without cutting the tail dead,
    // so whatever is still ringing rings out rather than clicking.
    nya_audio_bus_reverb_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioReverb){ 0 });

    nya_entity_despawn_deferred(scene->cube);
    nya_entity_despawn_deferred(scene->model);
    nya_entity_despawn_deferred(scene->pill);

    gny_layer_cube3d_cubes_clear();

    // Takes the terrain body, and with it the triangle mesh Box3D built. The sample grid stays: it came
    // from the world's arena and is reused if this scene is opened again.
    gny_terrain3d_destroy(window);

    // Zeroed *after* the terrain teardown, and the terrain carried across: the sample grid came from the
    // world's arena and is reused if this scene is opened again, so dropping the pointer would rebuild it.
    NYA_Terrain3D* keep = scene->terrain;

    *scene = (GNY_Cube3DScene){ 0 };

    scene->terrain = keep;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON EVENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_cube3d_on_event(NYA_Window* window, NYA_Event* event) {
    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    switch (event->type) {
        case NYA_EVENT_MOUSE_BUTTON_DOWN: {
            NYA_MouseButtonEvent* mouse = &event->as_mouse_button_event;
            if (mouse->button != NYA_MOUSE_BUTTON_LEFT) break;

            // A ray, not a point: unlike 2D's nya_physics2d_entity_at (the screen *is* the world plane
            // there), the pixel under the cursor here is every point along a line.
            NYA_Render3DRay ray = nya_render3d_screen_ray(window, (f32x2){ mouse->x, mouse->y });

            // Through nya_entity_click, not a raw raycast — a raw raycast against the cube's handle used
            // to mean its on_click never ran. Both dimensions dispatch the same way now, so "what happens
            // when clicked" is a property of the entity, same as a crate in 2D; the ground just has no
            // on_click.
            NYA_EntityHandle hit = nya_entity_click(ray.origin, ray.direction * GNY_CUBE3D_PICK_RANGE, mouse->button);

            scene->grabbed_once = scene->grabbed_once || nya_entity_is_valid(hit);

            event->was_handled = true;
        } break;

        // See the note in layer_game.c: two different ways for the cursor to stop being anywhere.
        case NYA_EVENT_WINDOW_MOUSE_LEAVE:
        case NYA_EVENT_WINDOW_FOCUS_LOST: {
            nya_entity_hover_clear();
        } break;

        case NYA_EVENT_MOUSE_BUTTON_UP: {
            if (event->as_mouse_button_event.button != NYA_MOUSE_BUTTON_LEFT) break;

            scene->dragging    = false;
            event->was_handled = true;
        } break;

        case NYA_EVENT_MOUSE_MOVED: {
            NYA_MouseMovedEvent* mouse = &event->as_mouse_moved_event;

            // The same hover the 2D scene drives, via nya_entity_hover's ray overload, so on_hover
            // doesn't need to know which solver is carrying it. Done before the drag below, so a frame
            // spinning the cube still reports as hovered — the drag reads the same cursor without
            // consuming it.
            NYA_Render3DRay hover_ray = nya_render3d_screen_ray(window, (f32x2){ mouse->x, mouse->y });

            (void)nya_entity_hover(hover_ray.origin, hover_ray.direction * GNY_CUBE3D_PICK_RANGE);

            if (scene->dragging) {
                // An angular impulse, not a written rotation: assignment fights the solver and shivers, an
                // impulse keeps spinning after the mouse stops and slows under angular damping. Horizontal
                // motion turns it about the world's up axis, vertical about the camera's right.
                NYA_Entity* cube = nya_entity_get(scene->cube);

                if (cube != nullptr) {
                    f32x3 impulse = {
                        mouse->delta_y * GNY_CUBE3D_SPIN_STRENGTH,
                        mouse->delta_x * GNY_CUBE3D_SPIN_STRENGTH,
                        0.0F,
                    };

                    nya_physics3d_apply_angular_impulse(cube, impulse);
                }

                event->was_handled = true;
                break;
            }

            // Not dragging the cube: right button orbits the camera instead.
            if (nya_input_mouse_button_pressed(NYA_MOUSE_BUTTON_RIGHT)) {
                scene->orbit_yaw   += mouse->delta_x * GNY_CUBE3D_ORBIT_SENSITIVITY;
                scene->orbit_pitch -= mouse->delta_y * GNY_CUBE3D_ORBIT_SENSITIVITY;

                // Stopped short of the poles: straight up, the view direction is parallel to the up
                // vector, there's no unique roll, and nya_matrix_look_at returns identity — the camera
                // snapping to the origin.
                scene->orbit_pitch = nya_clamp(scene->orbit_pitch, -1.5F, 1.5F);

                event->was_handled = true;
            }
        } break;

        case NYA_EVENT_MOUSE_WHEEL_MOVED: {
            f32 factor = event->as_mouse_wheel_event.amount_y > 0.0F ? (1.0F / GNY_CUBE3D_ZOOM_STEP) : GNY_CUBE3D_ZOOM_STEP;

            scene->orbit_range = nya_clamp(scene->orbit_range * factor, GNY_CUBE3D_RANGE_MIN, GNY_CUBE3D_RANGE_MAX);
            event->was_handled = true;
        } break;

        case NYA_EVENT_KEY_DOWN: {
            NYA_KeyEvent* key = &event->as_key_event;
            if (key->is_repeat) break;

            if (nya_input_action_matches(NYA_INPUT_ACTION_CANCEL, key->key, key->modifier_flags)) {
                gny_screen_main_menu();
                event->was_handled = true;
                break;
            }

            // A new landscape, with everything on it put back: the pile is dropped again rather than left,
            // since its bodies rest on a surface about to vanish (Box3D would wake them into a hilltop
            // crate inside a new valley). Seed advances by one, deterministically, same as the 2D world's
            // R.
            if (nya_input_action_matches(GNY_ACTION_REGENERATE_TERRAIN, key->key, key->modifier_flags)) {
                gny_layer_cube3d_cubes_clear();

                gny_terrain3d_generate(window, nya_world()->allocator, gny_terrain3d()->seed + 1);

                // Teleported, not despawned/respawned, so the handle — and its on_click — stay valid.
                NYA_Entity* cube = nya_entity_get(scene->cube);

                if (cube != nullptr) {
                    nya_physics3d_teleport(cube, _gny_cube3d_drop_point(), nya_quaternion_identity);
                    nya_physics3d_velocity_set(cube, f32x3_zero);
                    nya_physics3d_angular_velocity_set(cube, f32x3_zero);
                }

                // The two models likewise: a body left where it was is a body embedded in a new hill.
                struct {
                    NYA_EntityHandle handle;
                    f32              x;
                    f32              lift;
                } models[] = {
                    { scene->model, GNY_CUBE3D_MODEL_OFFSET, GNY_CUBE3D_MODEL_LIFT },
                    { scene->pill, GNY_CUBE3D_PILL_OFFSET, GNY_CUBE3D_PILL_LIFT },
                };

                for (u64 i = 0; i < nya_carray_length(models); i++) {
                    NYA_Entity* entity = nya_entity_get(models[i].handle);
                    if (entity == nullptr) continue;

                    nya_physics3d_teleport(entity, (f32x3){ models[i].x, gny_terrain3d()->max_height + models[i].lift, 0.0F },
                                           nya_quaternion_identity);
                    nya_physics3d_velocity_set(entity, f32x3_zero);
                    nya_physics3d_angular_velocity_set(entity, f32x3_zero);
                }

                gny_layer_cube3d_cubes_drop();

                event->was_handled = true;
                break;
            }

            // Another pile on top: the quickest way to see the solver handle a stack, not a scatter.
            if (nya_input_action_matches(GNY_ACTION_SPAWN_BURST, key->key, key->modifier_flags)) {
                gny_layer_cube3d_cubes_drop();

                event->was_handled = true;
                break;
            }

            if (nya_input_action_matches(GNY_ACTION_CLEAR_BOXES, key->key, key->modifier_flags)) {
                gny_layer_cube3d_cubes_clear();

                event->was_handled = true;
                break;
            }

            // Same flag and key the 2D world uses, so B means the same thing in both scenes.
            if (nya_input_action_matches(GNY_ACTION_TOGGLE_BLOOM, key->key, key->modifier_flags)) {
                gny_world()->bloom_enabled = !gny_world()->bloom_enabled;

                event->was_handled = true;
            }
        } break;

        default: break;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON COLLISION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_cube3d_on_cube_click(NYA_Entity* entity, f32x3 world_point, u8 button) {
    nya_unused(entity, world_point);

    if (button != NYA_MOUSE_BUTTON_LEFT) return;

    _gny_cube3d_scene()->dragging = true;
}

void gny_layer_cube3d_on_collision(NYA_Entity* entity, NYA_Entity* other, const NYA_PhysicsHit* hit) {
    nya_unused(entity, other);

    // Sensor overlaps have no closing speed, so scaling a burst by one would divide by nothing.
    if (hit->kind != NYA_PHYSICS_HIT_IMPACT) return;

    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    // Scaled by how hard it landed against the slowest possible impact — same shape as the 2D sparks.
    f32 threshold = nya_physics3d_hit_threshold();
    f32 strength  = nya_clamp((hit->approach_speed / threshold) - 1.0F, 0.0F, 1.0F);

    (void)nya_particles_emit(
        scene->dust,
        (NYA_ParticleBurst){
            .shape    = NYA_PARTICLE_SHAPE_CONE,
            .position = hit->point,
            .count    = (u32)nya_lerp((f32)GNY_CUBE3D_DUST_MIN, (f32)GNY_CUBE3D_DUST_MAX, strength),

            // `up`, not the hit normal — the normal points A toward B, and which of those is the cube
            // depends on the pair, while `up` is right whichever way round the pair came.
            .direction = { 0.0F, 1.0F, 0.0F },
            .spread    = 1.1F,

            .speed      = { GNY_CUBE3D_DUST_SPEED.x, nya_lerp(GNY_CUBE3D_DUST_SPEED.x, GNY_CUBE3D_DUST_SPEED.y, strength) },
            .lifetime_s = { 0.3F, 0.9F },
            .size       = GNY_CUBE3D_DUST_SIZE,
            .size_end   = { 0.0F, 0.02F },

            .color_start = GNY_CUBE3D_DUST_COLOR,
            // Zero alpha, so it settles out rather than vanishing at full brightness.
            .color_end = { 0.72F, 0.68F, 0.60F, 0.0F },

            .gravity = { 0.0F, GNY_CUBE3D_DUST_GRAVITY, 0.0F },
            .damping = 0.8F,
        }
    );

    // nya_audio_play_sound_at_3d, not 2D's nya_audio_play_sound_at — the 2D listener is a point plus a
    // fixed world-to-audio mapping that can't describe an orbiting camera. Listener set once a frame in
    // _gny_cube3d_draw_scene from the same orbit, so ear and eye can't drift apart. Scaled by the same
    // `strength` as the dust: harder landings are louder, lower, and detuned so a pile doesn't sound
    // like one recording.
    nya_audio_play_sound_at_3d(
        NYA_ASSET_SOUNDS_HIT_WAV,
        hit->point,
        (NYA_SoundParams){
            .gain              = nya_lerp(GNY_HIT_GAIN_MIN, GNY_HIT_GAIN_MAX, strength) * nya_settings_volume_effective(NYA_VOLUME_CHANNEL_SOUND),
            .gain_variation_db = 1.5F,

            .pitch                     = nya_lerp(1.12F, 0.88F, strength),
            .pitch_variation_semitones = 0.6F,

            // Sixteen voices isn't many — ranking by impact strength keeps a heavy landing audible when
            // six light ones arrive the same frame.
            .priority = (s32)(strength * 100.0F),
        }
    );
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE PILE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_cube3d_cubes_drop(void) {
    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    // Replaced, not appended to: the pool is fixed, and an appending drop would silently stop working
    // once full.
    gny_layer_cube3d_cubes_clear();

    for (u32 i = 0; i < GNY_TERRAIN3D_CUBE_COUNT; i++) {
        GNY_FallingCube cube = _gny_cube3d_cube_spawn(i);

        // A failed spawn means a full entity table — not this layer's problem, not worth abandoning the
        // rest of the pile over.
        if (!nya_entity_is_valid(cube.entity)) continue;

        scene->cubes[scene->cube_count++] = cube;
    }
}

void gny_layer_cube3d_cubes_clear(void) {
    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    // Deferred: runs from a key handler and from on_destroy, which can be inside the layer stack's own
    // iteration. Costs a frame where cubes are still in the entity table but not this array, hence
    // zeroing count here.
    for (u32 i = 0; i < scene->cube_count; i++) nya_entity_despawn_deferred(scene->cubes[i].entity);

    scene->cube_count = 0;
}

void gny_layer_cube3d_models_attach(NYA_Window* window) {
    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    struct {
        NYA_EntityHandle    handle;
        NYA_ConstCString    mesh;
        f32                 scale;
        NYA_Physics3DShape  shape;
    } models[] = {
        { scene->model, GNY_CUBE3D_MODEL, GNY_CUBE3D_MODEL_SCALE, NYA_PHYSICS3D_SHAPE_BOX },
        { scene->pill, GNY_CUBE3D_PILL, GNY_CUBE3D_PILL_SCALE, NYA_PHYSICS3D_SHAPE_CAPSULE },
    };

    for (u64 i = 0; i < nya_carray_length(models); i++) {
        NYA_Entity* entity = nya_entity_get(models[i].handle);

        // Already bodies, or gone — the common case every frame but one, cheap enough to call unguarded.
        if (entity == nullptr || nya_physics3d_body_attached(entity)) continue;

        f32x3 min;
        f32x3 max;

        // Still loading — the ordinary answer for the first frame or two, not a failure.
        if (!nya_render3d_mesh_bounds(window, models[i].mesh, &min, &max)) continue;

        // Same scale the draw applies, so the body matches what's on screen; bounds are in the file's own
        // units, see nya_render3d_mesh_bounds.
        f32x3 size = (max - min) * models[i].scale;

        // A convex stand-in, not a triangle mesh: NYA_PHYSICS3D_SHAPE_MESH is a *static* shape with no
        // interior or mass, wrong for anything that falls, so a rounded cube gets a box and a capsule gets
        // a capsule. Visible cost: the collider is the bounding shape, so Cubie's rounded corners collide
        // square. Convex decomposition is the real fix and is a tool, not a few lines.
        b8 attached = false;

        if (models[i].shape == NYA_PHYSICS3D_SHAPE_BOX) {
            attached = nya_physics3d_body_attach(
                models[i].handle,
                .type        = NYA_PHYSICS_BODY_DYNAMIC,
                .shape       = NYA_PHYSICS3D_SHAPE_BOX,
                .size        = size,
                .density     = GNY_TERRAIN3D_CUBE_DENSITY,
                .friction    = GNY_TERRAIN3D_CUBE_FRICTION,
                .restitution = GNY_TERRAIN3D_CUBE_RESTITUTION
            );
        } else {
            /*
             * Capsule upright about y: radius is half the *wider* horizontal extent (so the model fits
             * inside rather than poking out), length is what's left of the height once both caps are
             * accounted for, clamped at zero (a negative length is rejected by the attach).
             */
            f32 radius = nya_max(size.x, size.z) * 0.5F;
            f32 length = nya_max(size.y - (radius * 2.0F), 0.01F);

            attached = nya_physics3d_body_attach(
                models[i].handle,
                .type        = NYA_PHYSICS_BODY_DYNAMIC,
                .shape       = NYA_PHYSICS3D_SHAPE_CAPSULE,
                .radius      = radius,
                .length      = length,
                .density     = GNY_TERRAIN3D_CUBE_DENSITY,
                .friction    = GNY_TERRAIN3D_CUBE_FRICTION,
                .restitution = GNY_TERRAIN3D_CUBE_RESTITUTION
            );
        }

        if (!attached) {
            nya_log_error("Could not give '%s' a body; it will hang in the air.", models[i].mesh);
            continue;
        }

        nya_log_info("Fitted a body to '%s' from its bounds (" FMTf32x3 ").", models[i].mesh, FMTf32x3_ARG(size));
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON UPDATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_cube3d_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window);

    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    /*
     * The terrain's detail levels, from where the camera is.
     *
     * In on_update rather than on_render because re-levelling a chunk *uploads geometry*, and the
     * render is called once per pass — the shadow cascades alone would run it four times a frame and
     * three of those would find nothing to do. The camera position is derived from the orbit the same
     * way the draw derives it.
     */
    gny_terrain3d_update(_gny_cube3d_camera_position(scene));

    // Once a tick, here not in on_render — the camera is derived from orbit angles at draw time, so this
    // is the only state in this layer that needs advancing.
    nya_particles_update(scene->dust, delta_time_s);
    nya_particles_update(scene->fire, delta_time_s);
    nya_particles_update(scene->smoke, delta_time_s);

    // The plume is fed on a timer, not once per frame — per-frame emission ties fire density to frame
    // rate (a bonfire at 165Hz, an ember at 30). The while loop, not an if, keeps puffs/second constant
    // across a long frame too.
    scene->plume_timer_s += delta_time_s;

    while (scene->plume_timer_s >= GNY_CUBE3D_PLUME_INTERVAL_S) {
        scene->plume_timer_s -= GNY_CUBE3D_PLUME_INTERVAL_S;

        f32x3 base = { GNY_CUBE3D_PLUME_X, gny_terrain3d_height_at(GNY_CUBE3D_PLUME_X, GNY_CUBE3D_PLUME_Z), GNY_CUBE3D_PLUME_Z };

        (void)nya_particles_emit(
            scene->fire,
            (NYA_ParticleBurst){
                .shape     = NYA_PARTICLE_SHAPE_CONE,
                .position  = base,
                .count     = GNY_CUBE3D_FIRE_COUNT,
                .direction = { 0.0F, 1.0F, 0.0F },
                .spread    = GNY_CUBE3D_PLUME_SPREAD,

                .speed      = GNY_CUBE3D_FIRE_SPEED,
                .lifetime_s = GNY_CUBE3D_FIRE_LIFETIME,
                .size       = GNY_CUBE3D_FIRE_SIZE,
                .size_end   = GNY_CUBE3D_FIRE_SIZE_END,

                .color_start = GNY_CUBE3D_FIRE_COLOR_START,
                .color_end   = GNY_CUBE3D_FIRE_COLOR_END,

                // Upward: the acceleration on hot air is buoyancy, not weight.
                .gravity = GNY_CUBE3D_FIRE_GRAVITY,
                .damping = 1.2F,
            }
        );

        // Started a little above the flame, where a real plume's smoke becomes visible.
        (void)nya_particles_emit(
            scene->smoke,
            (NYA_ParticleBurst){
                .shape     = NYA_PARTICLE_SHAPE_CONE,
                .position  = base + (f32x3){ 0.0F, 0.55F, 0.0F },
                .count     = GNY_CUBE3D_SMOKE_COUNT,
                .direction = { 0.0F, 1.0F, 0.0F },
                .spread    = GNY_CUBE3D_PLUME_SPREAD * 1.6F,

                .speed      = GNY_CUBE3D_SMOKE_SPEED,
                .lifetime_s = GNY_CUBE3D_SMOKE_LIFETIME,
                .size       = GNY_CUBE3D_SMOKE_SIZE,
                .size_end   = GNY_CUBE3D_SMOKE_SIZE_END,

                .color_start = GNY_CUBE3D_SMOKE_COLOR_START,
                .color_end   = GNY_CUBE3D_SMOKE_COLOR_END,

                .gravity = GNY_CUBE3D_SMOKE_GRAVITY,
                .damping = 0.6F,
            }
        );
    }

    // Gives the two models bodies once their meshes finish loading; a no-op every frame after.
    gny_layer_cube3d_models_attach(window);

    // Once a tick, not per sound: re-evaluating every live voice against the camera is what lets a sound
    // un-muffle as the view swings clear of a hill.
    nya_audio_occlusion_update();

    /*
     * Anything that has left the world (fallen past the bounded terrain's rim, else it falls forever,
     * costing a solver body for nothing) is recycled rather than despawned, since the pool is fixed.
     * Teleported, not respawned, so the handle and its collision hook stay valid — velocity is cleared
     * by hand, since a teleport doesn't touch it and terminal velocity would send it straight back
     * through.
     */
    for (u32 i = 0; i < scene->cube_count; i++) {
        NYA_Entity* cube = nya_entity_get(scene->cubes[i].entity);

        if (cube == nullptr || cube->position.y > GNY_TERRAIN3D_CUBE_KILL_Y) continue;

        // Placed by the same rule a fresh one is, from an ever-climbing counter, so a cube recycled twice
        // doesn't come back to the same spot.
        nya_physics3d_teleport(cube, _gny_cube3d_cube_placement(GNY_TERRAIN3D_CUBE_COUNT + scene->cubes_recycled),
                               nya_quaternion_identity);

        nya_physics3d_velocity_set(cube, f32x3_zero);
        nya_physics3d_angular_velocity_set(cube, f32x3_zero);

        scene->cubes_recycled++;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON RENDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Draws the 3D half of the scene: everything between nya_render3d_begin and nya_render3d_end. Split out
 * so it can be aimed at an offscreen target for bloom without the HUD text going through it too —
 * glowing text would be a different decision from a glowing lamp.
 * */
NYA_INTERNAL void _gny_cube3d_draw_scene(NYA_Window* window) {

    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    NYA_Entity* cube = nya_entity_get(scene->cube);

    f32x3 eye = _gny_cube3d_camera_position(scene);

    // Aimed a little above the ground, so the basin's middle sits in the frame's middle, not its bottom
    // edge.
    f32x3 target = { 0.0F, gny_terrain3d_height_at(0.0F, 0.0F) + GNY_CUBE3D_SIZE, 0.0F };

    nya_render3d_begin(window, (NYA_Camera3DPerspective){ .position = eye, .target = target });

    /*
     * The ear, on the camera, derived here (not set in on_update) from the same two vectors the view is
     * built from, so the two can't disagree. core_audio.h warns the camera is usually the wrong ear, but
     * it's right here — there's no avatar, the camera *is* the point of view. Idempotent and cheap, so
     * setting it every frame is intended, not something needing a change check.
     */
    nya_audio_listener_3d_set((NYA_AudioListener3D){
        .position           = eye,
        .forward            = target - eye,
        .up                 = { 0.0F, 1.0F, 0.0F },
        .reference_distance = GNY_CUBE3D_EAR_DISTANCE,
    });

    GNY_SkyState sky = gny_sky_state();

    nya_render3d_light_set(
        window,
        (NYA_Render3DLight){
            .direction = sky.direction,
            .color     = sky.light,
            .ambient   = sky.ambient,
            .intensity = sky.intensity,
        }
    );

    /*
     * The sky, drawn first from the same GNY_SkyState the light above uses — shaded from a view ray so it
     * turns with the camera (system_sky.c's 2D backdrop stays put and used to leave the sun in the corner
     * on orbit).
     *
     * `sun_direction` is the *negation* of the light's direction: GNY_SkyState.direction is the way light
     * travels, this wants the way to look — passing it unnegated puts the sun opposite where the scene is
     * lit from, the one mistake this API can make.
     *
     * The disc is drawn several times life-size — half a degree reads as a dot, but this is a cartoon sky
     * and the sun in it is a shape.
     */
    nya_render3d_sky_draw(
        window,
        (NYA_Render3DSky){
            .zenith  = sky.top,
            .horizon = sky.bottom,

            // Darker than the horizon and unlit, so the world reads as sitting *on* something, not
            // floating. Not derived from the terrain's colour.
            .ground = GNY_SKY3D_GROUND,

            .sun_direction = -sky.direction,
            .sun_color     = sky.disc,
            .sun_angle     = GNY_SKY3D_SUN_ANGLE,
            .sun_intensity = sky.is_night ? GNY_SKY3D_MOON_INTENSITY : GNY_SKY3D_SUN_INTENSITY,

            // Wide halo by day, tight at night — a moon with a sunset's glow reads as a second sun.
            .sun_halo = sky.is_night ? GNY_SKY3D_MOON_HALO : GNY_SKY3D_SUN_HALO,

            .horizon_softness = GNY_SKY3D_HORIZON_SOFTNESS,
            .ground_blend     = GNY_SKY3D_GROUND_BLEND,
        }
    );

    // Two materials, two draw calls — not two because there are two objects (a hundred cubes of the same
    // material is still one draw call, base colour being per vertex), but because material is per flush,
    // not per primitive.
    //
    // The lamps, before anything else, since adding one flushes: cleared and re-added every frame because
    // they orbit, and the clear keeps a re-entered scene from accumulating a second set.
    nya_render3d_point_lights_clear(window);

    f32 lamp_phase = nya_app_uptime_s() * GNY_CUBE3D_LAMP_SPEED;

    f32x3 lamp_positions[GNY_CUBE3D_LAMP_COUNT];

    for (u32 i = 0; i < GNY_CUBE3D_LAMP_COUNT; i++) {
        // Spread evenly around the circle, so two lamps sit opposite each other, not together.
        f32 angle = lamp_phase + ((f32)i * (2.0F * (f32)M_PI / (f32)GNY_CUBE3D_LAMP_COUNT));

        f32 lamp_x = cosf(angle) * GNY_CUBE3D_LAMP_RADIUS;
        f32 lamp_z = sinf(angle) * GNY_CUBE3D_LAMP_RADIUS;

        // Above the ground it's over, so a lamp crossing a hill rises with it rather than sinking in —
        // same correction the two models need above.
        lamp_positions[i] = (f32x3){ lamp_x, gny_terrain3d_height_at(lamp_x, lamp_z) + GNY_CUBE3D_LAMP_HEIGHT, lamp_z };

        nya_render3d_point_light_add(
            window,
            (NYA_Render3DPointLight){
                .position  = lamp_positions[i],
                .color     = i == 0 ? GNY_CUBE3D_LAMP_A_COLOR : GNY_CUBE3D_LAMP_B_COLOR,
                .range     = GNY_CUBE3D_LAMP_RANGE,
                .intensity = GNY_CUBE3D_LAMP_INTENSITY,
            }
        );
    }

    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.0F, .roughness = 0.9F, .edge = GNY_CUBE3D_EDGE });

    // The landscape draws as a few thousand flat triangles in the same batch as everything else. No grid
    // over it any more — a wireframe grid on the xz plane reads a flat floor but becomes a mess of lines
    // through hillsides with relief; the facets do that job now.
    gny_terrain3d_draw(window);

    // The pile, between the ground and the draggable cube, same material as the terrain — so they batch
    // into one draw call: twenty-four boxes and two thousand ground triangles for the price of one, true
    // only because colour is per vertex.
    for (u32 i = 0; i < scene->cube_count; i++) {
        const NYA_Entity* box = nya_entity_get(scene->cubes[i].entity);

        if (box == nullptr || scene->cubes[i].glass) continue;

        f32 size = scene->cubes[i].size;

        // Rotation read straight off the entity as a full quaternion — no angle to extract, and none
        // could be: three degrees of angular freedom don't reduce to one.
        nya_render3d_cube(window, box->position, (f32x3){ size, size, size }, box->rotation, scene->cubes[i].color);
    }

    /*
     * Glass cubes drawn after opaque ones, grouped by blur level: one run per level rather than a
     * material change per cube, since material is per flush — a draw call per cube versus per *level* is
     * why there are three discrete levels rather than a continuous range. Run order doesn't affect
     * blending correctness (the renderer sorts the transparent stream by alpha); grouping only saves draw
     * calls.
     *
     * `refraction` is what turns these into glass rather than tinted panes: the surface samples the scene
     * instead of blending over it. See NYA_Render3DMaterial.refraction for its limits.
     */
    static const f32 glass_blurs[] = GNY_CUBE3D_GLASS_BLURS;

    for (u64 level = 0; level < nya_carray_length(glass_blurs); level++) {
        // Counted before the material is set, so a level nothing uses costs no draw call at all.
        u32 drawn = 0;

        for (u32 i = 0; i < scene->cube_count; i++) {
            if (!scene->cubes[i].glass || scene->cubes[i].blur != glass_blurs[level]) continue;
            if (nya_entity_get(scene->cubes[i].entity) == nullptr) continue;

            drawn++;
        }

        if (drawn == 0) continue;

        nya_render3d_material_set(
            window,
            (NYA_Render3DMaterial){
                .metallic    = GNY_CUBE3D_GLASS_METALLIC,
                .roughness   = GNY_CUBE3D_GLASS_ROUGHNESS,
                .reflectance = GNY_CUBE3D_GLASS_REFLECTANCE,
                .refraction  = GNY_CUBE3D_GLASS_REFRACTION,
                .blur        = glass_blurs[level],

                // Off: edge darkening stands in for occlusion, and something you can see through doesn't
                // occlude itself.
                .edge = 0.0F,
            }
        );

        for (u32 i = 0; i < scene->cube_count; i++) {
            const NYA_Entity* box = nya_entity_get(scene->cubes[i].entity);

            if (box == nullptr || !scene->cubes[i].glass || scene->cubes[i].blur != glass_blurs[level]) continue;

            f32 size = scene->cubes[i].size;

            nya_render3d_cube(window, box->position, (f32x3){ size, size, size }, box->rotation, scene->cubes[i].color);
        }
    }

    // Back to the pile's own material — leaving glass set would put a hard highlight and full rim on the
    // models below.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.0F, .roughness = 0.9F, .edge = GNY_CUBE3D_EDGE });

    if (cube != nullptr) {
        // Brushed metal while held, matte plastic otherwise — shows material is a property of the *draw*,
        // not the object.
        nya_render3d_material_set(
            window,
            scene->dragging ? (NYA_Render3DMaterial){ .metallic = 1.0F, .roughness = 0.28F, .edge = GNY_CUBE3D_EDGE }
                            : (NYA_Render3DMaterial){ .metallic = 0.0F, .roughness = 0.55F, .edge = GNY_CUBE3D_EDGE }
        );

        // Rotation comes straight off the entity — no angle extracted, none could be: three degrees of
        // angular freedom don't reduce to one.
        nya_render3d_cube(
            window,
            cube->position,
            (f32x3){ GNY_CUBE3D_SIZE, GNY_CUBE3D_SIZE, GNY_CUBE3D_SIZE },
            cube->rotation,
            scene->dragging ? GNY_CUBE3D_HELD_COLOR : GNY_CUBE3D_COLOR
        );
    }

    // The loaded model, beside the primitives, not instead of them: ground plane, generated cube, and
    // disk-read mesh all go into the same batch, same light, same material, one draw call — a model
    // needing its own pass would show as a second draw with different shading. Smooth shaded, unlike the
    // cube beside it, since its normals come from the file. See nya_render3d_mesh.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.1F, .roughness = 0.45F, .edge = GNY_CUBE3D_EDGE });

    // Ink outline around the two models only, which the material's edge term can't draw — that term finds
    // curvature from screen-space derivatives (fillets, grazing angles; see mesh3d_shading.hlsli), so an
    // inverted hull gives the silhouette instead. Not on the primitives: they have no second copy to
    // expand, and an outlined pile would be a mesh of black lines.
    nya_render3d_outline_set(window, GNY_CUBE3D_OUTLINE_THICKNESS, GNY_CUBE3D_OUTLINE_COLOR);

    // Placed and turned by the solver, not the clock — both used to sit at a fixed offset spinning at a
    // constant rate about y (a turntable, which showed only that a mesh batches). Reading the transform
    // off the entity makes them part of the same simulation: they fall, roll, and get knocked about like
    // the cubes. The rotation is a full quaternion, which a turntable's single angle could never produce.
    const NYA_Entity* model_entity = nya_entity_get(scene->model);

    if (model_entity != nullptr) {
        nya_render3d_mesh(
            window,
            GNY_CUBE3D_MODEL,
            model_entity->position,
            (f32x3){ GNY_CUBE3D_MODEL_SCALE, GNY_CUBE3D_MODEL_SCALE, GNY_CUBE3D_MODEL_SCALE },
            model_entity->rotation,
            GNY_CUBE3D_MODEL_COLOR
        );
    }

    // The pill gets its own material too, a different roughness on purpose: the models and the cube then
    // differ by material as well as shape, showing material is per *draw* within one batch — the same
    // point the cube's held/idle switch makes, across objects instead of across time.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.55F, .roughness = 0.3F, .edge = GNY_CUBE3D_EDGE });

    const NYA_Entity* pill_entity = nya_entity_get(scene->pill);

    if (pill_entity != nullptr) {
        nya_render3d_mesh(
            window,
            GNY_CUBE3D_PILL,
            pill_entity->position,
            (f32x3){ GNY_CUBE3D_PILL_SCALE, GNY_CUBE3D_PILL_SCALE, GNY_CUBE3D_PILL_SCALE },
            pill_entity->rotation,
            GNY_CUBE3D_PILL_COLOR
        );
    }

    // Off again before the lamps: an emissive bead with an ink outline reads as a hole, not a glow.
    nya_render3d_outline_set(window, 0.0F, GNY_CUBE3D_OUTLINE_COLOR);

    /*
     * Water is the scene's only translucent surface — without one there's nothing for transparency
     * ordering to be right or wrong about. Two overlapping panes show the difference: in call order the
     * nearer writes depth and hides the far one, sorted back-to-front they tint each other instead. See
     * NYA_Render3DStream. Three panes, drawn *nearest first* on purpose, to prove the renderer reorders
     * them rather than relying on the caller.
     */
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.35F, .roughness = 0.15F, .edge = 0.0F });

    for (u32 i = 0; i < GNY_CUBE3D_WATER_LAYERS; i++) {
        f32 height = GNY_CUBE3D_WATER_LEVEL + ((f32)(GNY_CUBE3D_WATER_LAYERS - 1 - i) * GNY_CUBE3D_WATER_GAP);

        nya_render3d_plane(window, (f32x3){ 0.0F, height, 0.0F }, (f32x2){ GNY_CUBE3D_WATER_SIZE, GNY_CUBE3D_WATER_SIZE },
                           GNY_CUBE3D_WATER_COLOR);
    }

    // A glowing bead at each lamp, emissive rather than merely bright: emission is added unlit, so the
    // bead reads the same colour from every side instead of having a dark half — what a light source
    // looks like, and what the bloom pass needs to find it. See NYA_Render3DMaterial.emission.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .roughness = 0.4F, .emission = GNY_CUBE3D_LAMP_EMISSION });

    for (u32 i = 0; i < GNY_CUBE3D_LAMP_COUNT; i++) {
        nya_render3d_sphere(window, lamp_positions[i], GNY_CUBE3D_LAMP_MARKER_RADIUS,
                            i == 0 ? GNY_CUBE3D_LAMP_A_COLOR : GNY_CUBE3D_LAMP_B_COLOR);
    }

    // Inside the 3D scene, before it's flushed: a 3D particle system draws through render3d and is
    // skipped when no 3D camera is active, so drawing it after nya_render3d_end would silently produce
    // nothing. Being between begin/end also puts the dust in the same depth buffer as the cube.
    nya_particles_draw(window, scene->dust);

    // Smoke before fire: smoke goes through the ordinary sorted transparent pass so its puffs layer
    // correctly. Fire is switched to additive, so overlapping tongues brighten toward white instead of
    // averaging into a grey-brown smear. Its colours are deliberately above one (see
    // GNY_CUBE3D_FIRE_COLOR_START), which the tonemap's shoulder absorbs and bloom finds.
    nya_particles_draw(window, scene->smoke);

    nya_render3d_blend_set(window, NYA_RENDER3D_BLEND_ADDITIVE);
    nya_particles_draw(window, scene->fire);
    nya_render3d_blend_set(window, NYA_RENDER3D_BLEND_ALPHA);

    nya_render3d_end(window);
}

void gny_layer_cube3d_on_render(NYA_Window* window) {
    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    NYA_Entity* cube = nya_entity_get(scene->cube);

    GNY_World* bloom_world = gny_world();

    GNY_SkyState sky = gny_sky_state();

    // The sun must be set before the shadow pass: nya_render3d_shadow_begin builds its matrix from the
    // light currently on the batch, so otherwise the shadow pass uses the default sun (upper front left)
    // while the scene pass uses the sky's, and shadows land in the wrong place.
    nya_render3d_light_set(
        window,
        (NYA_Render3DLight){
            .direction = sky.direction,
            .color     = sky.light,
            .ambient   = sky.ambient,
            .intensity = sky.intensity,
        }
    );

    /*
     * The shadow pass, before the camera draw: the scene is drawn twice (once from the sun into the depth
     * map, once from the camera sampling it) because the batch flushes as it fills and keeps no geometry
     * to replay — why _gny_cube3d_draw_scene must be callable twice with no state of its own. See
     * nya_render3d_shadow_begin.
     *
     * The cascade loop is here, not in the engine, because cascades just mean drawing the scene three or
     * four times instead of two, and only the caller can draw its own scene.
     *
     * ⭐ **Fitted to the camera rather than centred on the origin.** This used to pin every cascade at
     * the world origin, which is correct for a fixed ground plane and wastes most of the near map the
     * moment the camera orbits away from the middle. nya_render3d_shadow_for_camera pushes each
     * cascade's volume a half-extent down the view direction and snaps it to the shadow map's texel
     * grid — the second of which is what stops shadow edges crawling while the camera orbits, which
     * this scene does continuously.
     */
    NYA_Camera3DPerspective shadow_camera = {
        .position = _gny_cube3d_camera_position(scene),
        .target   = { 0.0F, gny_terrain3d_height_at(0.0F, 0.0F) + GNY_CUBE3D_SIZE, 0.0F },
    };

    for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
        nya_render3d_shadow_begin(
            window,
            nya_render3d_shadow_for_camera(shadow_camera, sky.direction, cascade,
                                           (NYA_Render3DShadowFit){
                                               .near_extent = GNY_CUBE3D_SHADOW_EXTENT,
                                               .strength    = GNY_CUBE3D_SHADOW_STRENGTH,
                                           })
        );

        if (nya_render3d_active(window)) _gny_cube3d_draw_scene(window);

        nya_render3d_shadow_end(window);
    }

    /*
     * Through an offscreen target and the bloom pipeline, or straight to the window — the same
     * arrangement and shared target _gny_camera_render_primary uses for the 2D world (the two screens are
     * never up at once). Emission is why it's worth it here: the lamps' beads are drawn past the bloom
     * threshold on purpose, and without this pass they're just small bright spheres.
     *
     * The `bloom` *available* check is not defensive padding: render2d silently drops a batch whose
     * pipeline isn't loaded (the ordinary case for the first frame or two), so if that pipeline instead
     * fails *permanently*, the cost isn't the glow — it's the entire scene, rendered into a texture that
     * is then never blitted back, showing nothing at all. That is exactly what happened on Windows, where
     * the bloom pipeline was rejected by D3D12 and the 3D scene simply did not appear with nothing in the
     * frame saying why. The underlying cause is fixed in gny_bloom_pipeline_ensure; this guard is what
     * keeps the next backend disagreement from being invisible.
     */
    if (!bloom_world->bloom_enabled) {
        _gny_cube3d_draw_scene(window);
    } else if (!nya_post_begin(window, &bloom_world->post)) {
        // Minimised or mid resize: draw straight to the window, the same fallback the 2D path takes
        // rather than skipping the frame.
        _gny_cube3d_draw_scene(window);
    } else {
        nya_perf_time_this_scope("gny_cube3d_bloom_pass");

        _gny_cube3d_draw_scene(window);

        // nya_post_end skips a pass whose pipeline isn't loaded and blits the captured scene straight
        // back — so the failure above costs only the glow, not the entire scene.
        nya_post_end(
            window, &bloom_world->post,
            (NYA_PostPass[]){
                {
                    .pipeline = GNY_PIPELINE_BLOOM,
                    .uniform =
                        &(NYA_ShaderBloomUniform){
                            // This scene's own numbers, not the 2D world's; see GNY_BLOOM_3D_THRESHOLD.
                            .texel_x   = GNY_BLOOM_3D_SPREAD / (f32)bloom_world->post.width,
                            .texel_y   = GNY_BLOOM_3D_SPREAD / (f32)bloom_world->post.height,
                            .threshold = GNY_BLOOM_3D_THRESHOLD,
                            .intensity = GNY_BLOOM_3D_INTENSITY,
                        },
                    .uniform_size = sizeof(NYA_ShaderBloomUniform),
                },
            },
            1
        );
    }


    // Screen pixels over the top, same frame and render pass: nothing switched off or reset, render2d
    // never had a camera, its pipelines don't test depth, and nya_render3d_end already flushed the scene.
    // "In front" is decided purely by which batch flushed last.
    nya_render2d_font_set(GNY_UI_FONT, GNY_UI_FONT_SIZE);

    f32 line = GNY_UI_PADDING;

    nya_render2d_text(window, nya_string_cube3d_title(), GNY_UI_PADDING, line, GNY_UI_TEXT);
    line += nya_render2d_font_line_height();

    nya_render2d_text(window, scene->grabbed_once ? nya_string_cube3d_hint_drag() : nya_string_cube3d_hint_click(), GNY_UI_PADDING, line,
                      GNY_UI_DIM);
    line += nya_render2d_font_line_height();

    nya_render2d_text(window, nya_string_cube3d_hint_camera(), GNY_UI_PADDING, line, GNY_UI_DIM);
    line += nya_render2d_font_line_height();

    // Not through i18n: key letters aren't translated, and a line mostly key names would be four more
    // strings per locale saying the same thing.
    nya_render2d_textf(window, GNY_UI_PADDING, line, GNY_UI_DIM, "space drops cubes, c clears, b bloom %s",
                       gny_world()->bloom_enabled ? "on" : "off");
    line += nya_render2d_font_line_height();

    if (cube != nullptr) {
        nya_render2d_textf(window, GNY_UI_PADDING, line, GNY_UI_DIM, "cube at " FMTf32x3, FMTf32x3_ARG(cube->position));
        line += nya_render2d_font_line_height();
    }

    // Seed shown because it's the only way to ask for the same landscape again; recycle count because a
    // pile quietly falling off the edge and coming back looks identical to one at rest, until this moves.
    nya_render2d_textf(window, GNY_UI_PADDING, line, GNY_UI_DIM, "terrain seed %llu, %u bodies, %u recycled",
                       (unsigned long long)gny_terrain3d()->seed, nya_physics3d_body_count(), scene->cubes_recycled);
    line += nya_render2d_font_line_height();

    // These frame-stats counters existed but were never reset or read; see nya_render3d_frame_stats.
    // `culled` is worth watching — frustum culling costs when everything's on screen, saves when it's not.
    // Read before nya_render_end, which clears them.
    NYA_Render3DFrameStats stats = nya_render3d_frame_stats(window);

    nya_render2d_textf(window, GNY_UI_PADDING, line, GNY_UI_DIM, "3d: %u draws, %u verts, %u instances, %u culled", stats.draw_calls,
                       stats.vertices, stats.instances, stats.culled);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

GNY_Cube3DScene* _gny_cube3d_scene(void) {
    // On the world, not a static, so it survives a hot reload — same reason the menus keep state there.
    // See layers.h.
    return &gny_world()->cube3d;
}

f32 _gny_cube3d_occlusion(f32x3 source, void* user_data) {
    nya_unused(user_data);

    f32x3 ear = nya_audio_listener_3d_get().position;

    f32x3 to_source = source - ear;

    f32 distance = nya_vector_length(to_source);

    // A sound at the listener's own position has no direction to cast along; clear, not blocked, is the
    // safe answer for a degenerate case.
    if (distance < NYA_EPSILON) return 0.0F;

    // Three rays, not one, spread around the direct line: one ray is a boolean, snapping audibly as a
    // cube rolls behind a ridge, while three give thirds the engine interpolates into a fade. The spread
    // is perpendicular to the line and scaled by distance, so it's angular rather than a fixed offset.
    f32x3 direction = to_source / distance;

    f32x3 reference = fabsf(direction.y) < 0.9F ? (f32x3){ 0.0F, 1.0F, 0.0F } : (f32x3){ 1.0F, 0.0F, 0.0F };

    f32x3 side = nya_vector_normalize(nya_vector_cross(direction, reference)) * (distance * GNY_CUBE3D_OCCLUSION_SPREAD);

    f32x3 targets[] = { source, source + side, source - side };

    u32 blocked = 0;

    for (u64 i = 0; i < nya_carray_length(targets); i++) {
        // Cast from the ear toward the source, not the other way: the raycast reports the *first* thing
        // it meets, and starting at the source would hit the ground the sound sits on, reading every
        // landing as fully occluded.
        f32x3 ray = targets[i] - ear;

        NYA_EntityHandle hit = nya_physics3d_raycast(ear, ray, nullptr, nullptr);

        if (nya_entity_is_valid(hit)) blocked++;
    }

    return (f32)blocked / (f32)nya_carray_length(targets);
}

f32x3 _gny_cube3d_drop_point(void) {
    // Above the middle of the terrain, not a fixed height — the middle isn't at zero any more, and a
    // constant would put the cube underground on a seed with a hill there.
    return (f32x3){ 0.0F, gny_terrain3d_height_at(0.0F, 0.0F) + GNY_CUBE3D_DROP_HEIGHT, 0.0F };
}

f32x3 _gny_cube3d_cube_placement(u32 index) {
    // Two channels of the engine's integer hash, mapped onto a square around the origin. The channel
    // argument, not two different seeds — nya_ihash2 mixes y with a stride of 57, so offsetting the seed
    // instead would make cube i's z equal cube i+1's x, landing the pile along a diagonal.
    f32 x = nya_ihash2((s32)index, 0, GNY_TERRAIN3D_CUBE_SEED) * GNY_TERRAIN3D_CUBE_SPREAD;
    f32 z = nya_ihash2((s32)index, 1, GNY_TERRAIN3D_CUBE_SEED) * GNY_TERRAIN3D_CUBE_SPREAD;

    // Above the *highest* ground, not the ground beneath it: a fixed height over each cube's own column
    // would let a valley cube and a hilltop cube reach ground at the same time from different distances,
    // reading as gravity varying across the map. Staggered per index so they arrive as a stream, not a
    // sheet of twenty-four simultaneous contacts.
    f32 ceiling = gny_terrain3d()->max_height + GNY_TERRAIN3D_CUBE_DROP;

    return (f32x3){ x, ceiling + ((f32)index * GNY_TERRAIN3D_CUBE_STAGGER), z };
}

f32 _gny_cube3d_cube_size(u32 index) {
    // A third channel, so size does not correlate with either coordinate.
    f32 unit = (nya_ihash2((s32)index, 2, GNY_TERRAIN3D_CUBE_SEED) * 0.5F) + 0.5F;

    return nya_lerp(GNY_TERRAIN3D_CUBE_MIN_SIZE, GNY_TERRAIN3D_CUBE_MAX_SIZE, unit);
}

GNY_FallingCube _gny_cube3d_cube_spawn(u32 index) {
    static const NYA_Color palette[] = GNY_TERRAIN3D_CUBE_COLORS;

    const u32 palette_size = (u32)(sizeof(palette) / sizeof(palette[0]));

    f32 size = _gny_cube3d_cube_size(index);

    NYA_EntityHandle handle = nya_entity_spawn(
        .name         = "falling cube",
        .type         = GNY_ENTITY_CUBE3D,
        .position     = _gny_cube3d_cube_placement(index),
        .state        = NYA_ENTITY_STATE_ACTIVE,
        // The same hook the draggable cube uses, so a landing anywhere in the scene kicks up dust.
        .on_collision = nya_callback(gny_layer_cube3d_on_collision)
    );

    if (!nya_entity_is_valid(handle)) return (GNY_FallingCube){ 0 };

    // An initial tumble, so no two land on the same face — set on the entity before the attach, since
    // nya_physics3d_body_attach seeds the body from the entity's velocity and angular velocity, one call
    // instead of guessing an impulse afterward.
    NYA_Entity* entity = nya_entity_get(handle);

    entity->angular_velocity = (f32x3){
        nya_ihash2((s32)index, 3, GNY_TERRAIN3D_CUBE_SEED) * GNY_TERRAIN3D_CUBE_SPIN,
        nya_ihash2((s32)index, 4, GNY_TERRAIN3D_CUBE_SEED) * GNY_TERRAIN3D_CUBE_SPIN,
        nya_ihash2((s32)index, 5, GNY_TERRAIN3D_CUBE_SEED) * GNY_TERRAIN3D_CUBE_SPIN,
    };

    b8 attached = nya_physics3d_body_attach(
        handle,
        .type        = NYA_PHYSICS_BODY_DYNAMIC,
        .shape       = NYA_PHYSICS3D_SHAPE_BOX,
        .size        = { size, size, size },
        .density     = GNY_TERRAIN3D_CUBE_DENSITY,
        .friction    = GNY_TERRAIN3D_CUBE_FRICTION,
        .restitution = GNY_TERRAIN3D_CUBE_RESTITUTION
    );

    if (!attached) {
        nya_entity_despawn(handle);
        return (GNY_FallingCube){ 0 };
    }

    // Every fourth cube is glass, by index (deterministic like the rest of the pile, so R replays the
    // same arrangement) rather than by chance — a random selection clusters, and a clump of glass with no
    // opaque cubes behind it has nothing to be seen *against*.
    b8 glass = (index % GNY_CUBE3D_GLASS_EVERY) == 0;

    static const f32 blurs[] = GNY_CUBE3D_GLASS_BLURS;

    // Which frosting this one gets, cycled through the levels by how many glass cubes came before it —
    // so the three levels alternate rather than clustering.
    f32 blur = glass ? blurs[(index / GNY_CUBE3D_GLASS_EVERY) % (u32)nya_carray_length(blurs)] : 0.0F;

    // Allowed to sleep, unlike the draggable cube (which must move on any drag): letting the solver park
    // a settled pile is most of what keeps twenty-four bodies cheap, and a sleeping body wakes on contact.
    return (GNY_FallingCube){
        .entity = handle,
        .size   = size,
        .color  = glass ? GNY_CUBE3D_GLASS_COLOR : palette[index % palette_size],
        .glass  = glass,
        .blur   = blur,
    };
}

f32x3 _gny_cube3d_camera_position(const GNY_Cube3DScene* scene) {
    // Spherical to cartesian, y up: yaw turns about the vertical axis, pitch lifts above the horizon —
    // the orbit every 3D viewer has, and why the camera is placed and aimed rather than positioned and
    // zoomed.
    f32 horizontal = cosf(scene->orbit_pitch) * scene->orbit_range;

    // Lifted by the ground height under the terrain's middle, not a constant — a constant was right for a
    // flat plane at y=0, but puts the camera underground on any seed with a hill in the centre.
    f32 pivot = gny_terrain3d_height_at(0.0F, 0.0F) + GNY_CUBE3D_SIZE;

    return (f32x3){
        sinf(scene->orbit_yaw) * horizontal,
        (sinf(scene->orbit_pitch) * scene->orbit_range) + pivot,
        cosf(scene->orbit_yaw) * horizontal,
    };
}

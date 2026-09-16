/**
 * @file layer_cube3d.c
 *
 * The 3D demo: an orbit camera over a noise terrain, a draggable cube, two loaded models fitted with
 * bodies, a pile of cubes, fire and smoke particles, occluded 3D sound, shadows and bloom.
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
 * Where the pile's `index`-th cube starts, hashed from the index so R replays the same pile and a recycled
 * cube follows the same rule.
 * */
NYA_INTERNAL f32x3 _gny_cube3d_cube_placement(u32 index);

/** Full edge length of the pile's `index`-th cube, metres. Hashed, like its placement. */
NYA_INTERNAL f32 _gny_cube3d_cube_size(u32 index);

/** Spawns one of the pile, sized, coloured and placed from `index`. Zeroed when the spawn fails. */
NYA_INTERNAL GNY_FallingCube _gny_cube3d_cube_spawn(u32 index);

/** Teleports a body to `position`, upright and at rest. No-op for a handle that does not resolve. */
NYA_INTERNAL void _gny_cube3d_body_reset(NYA_EntityHandle handle, f32x3 position);

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

        // two systems, since each draws in one blend mode. see GNY_Cube3DScene.
        .fire  = nya_particles_create(nya_world()->allocator, GNY_CUBE3D_FIRE_POOL),
        .smoke = nya_particles_create(nya_world()->allocator, GNY_CUBE3D_SMOKE_POOL),
    };

    // queued, not waited on: the model draws once it loads. a missing model leaves the primitives.
    gny_bloom_pipeline_ensure(window);

    // queued here too so the scene works in any visit order; a second load is a no-op. predecoded, since
    // decoding at the moment of impact is when a hitch is audible.
    NYA_Error sound = nya_asset_load((NYA_AssetLoadParameters){
        .type     = NYA_ASSET_TYPE_SOUND,
        .handle   = NYA_ASSET_SOUNDS_HIT_WAV,
        .as_sound = { .predecode = true },
    });

    // not fatal: without an audio device the scene still runs and play calls are no-ops.
    if (!sound.ok) nya_log_warn("%s", (NYA_ConstCString)sound.message);

    NYA_AssetHandle models[] = { GNY_CUBE3D_MODEL, GNY_CUBE3D_PILL };

    for (u64 i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        NYA_Error queued = nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_MESH, .handle = models[i] });

        if (!queued.ok) {
            nya_log_error("Could not queue the 3D model '%s' (%s); the scene will draw without it.", models[i],
                          (NYA_ConstCString)queued.message);
        }
    }

    // the only 3D-specific line; the pool, shapes and integration are shared with 2D. see render_particles.h.
    nya_particles_space_set(scene->dust, NYA_PARTICLE_SPACE_3D);
    nya_particles_space_set(scene->fire, NYA_PARTICLE_SPACE_3D);
    nya_particles_space_set(scene->smoke, NYA_PARTICLE_SPACE_3D);

    // a soft radial sprite on the plume, so overlapping puffs blend instead of showing square edges. the dust stays
    // hard and countable.
    NYA_Error puff = nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = GNY_CUBE3D_PUFF_TEXTURE });

    // not fatal: a missing sprite draws untextured squares.
    if (!puff.ok) nya_log_warn("%s", (NYA_ConstCString)puff.message);

    nya_particles_texture_set(scene->fire, GNY_CUBE3D_PUFF_TEXTURE);
    nya_particles_texture_set(scene->smoke, GNY_CUBE3D_PUFF_TEXTURE);

    // negative y: 3D is y up, while the 2D world's y grows down the screen.
    nya_physics3d_gravity_set(NYA_PHYSICS3D_GRAVITY_DEFAULT);

    // the ground first: a static triangle mesh from fBm noise (see system_terrain3d.c), in the world's arena so the
    // sample grid outlives this call. seeded from the launch seed, mixed with a constant so 2D and 3D differ.
    gny_terrain3d_generate(window, nya_world()->allocator, GNY_LAUNCH.world_seed ^ GNY_TERRAIN3D_SEED);

    // the cube, dropped from a height so the solver shows immediately. its rotation belongs to the solver; the drag
    // spins it with angular impulses.
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
        // never sleeps, or the drag would do nothing after a few idle seconds.
        .never_sleep     = true,
        .angular_damping = 1.5F
    );

    // the two models, spawned without bodies: their size comes from vertices still loading, so
    // gny_layer_cube3d_models_attach attaches them once loaded.
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

    // occlusion through a callback (core_audio knows nothing about physics). the terrain is a bowl with a rim, so a
    // cube on the far slope really is behind ground. reverb goes on the effects bus, after the occlusion filter, so
    // a muffled sound reverberates muffled and music is left dry.
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

    // the pile last, so it falls onto what is there.
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

    // the bloom target is shared with the 2D game layer. released here too, since going from the menu into this
    // scene and out never runs that layer's on_destroy. guarded on the texture, so either release order is safe.
    GNY_World* world = gny_world();
    if (world != nullptr) nya_post_chain_destroy(&world->post);

    // despawning destroys the body. deferred, since this can run inside the layer stack's iteration. the occlusion
    // callback is removed before the solver it raycasts goes; null also clears what it applied.
    nya_audio_occlusion_set(nullptr, nullptr, (NYA_AudioOcclusion){ 0 });

    // a zero room size switches the reverb off and lets the tail ring out.
    nya_audio_bus_reverb_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioReverb){ 0 });

    nya_entity_despawn_deferred(scene->cube);
    nya_entity_despawn_deferred(scene->model);
    nya_entity_despawn_deferred(scene->pill);

    gny_layer_cube3d_cubes_clear();

    // takes the terrain body and its triangle mesh. the sample grid is kept for reuse.
    gny_terrain3d_destroy(window);

    // zeroed after the terrain teardown, keeping the terrain's sample grid for the next visit.
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

            // a ray: in 3D the pixel under the cursor is a line.
            NYA_Render3DRay ray = nya_render3d_screen_ray(window, (f32x2){ mouse->x, mouse->y });

            // through nya_entity_click, so a click runs the entity's on_click as in 2D. the ground has none.
            NYA_EntityHandle hit = nya_entity_click(ray.origin, ray.direction * GNY_CUBE3D_PICK_RANGE, mouse->button);

            scene->grabbed_once = scene->grabbed_once || nya_entity_is_valid(hit);

            event->was_handled = true;
        } break;

        // see layer_game.c: two ways for the cursor to stop being anywhere.
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

            // the same hover as 2D, before the drag, so a spinning cube still counts as hovered.
            NYA_Render3DRay hover_ray = nya_render3d_screen_ray(window, (f32x2){ mouse->x, mouse->y });

            (void)nya_entity_hover(hover_ray.origin, hover_ray.direction * GNY_CUBE3D_PICK_RANGE);

            if (scene->dragging) {
                // an angular impulse, not a written rotation, which would fight the solver. horizontal motion turns about world
                // up, vertical about the camera's right.
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

            // not dragging the cube: the right button orbits.
            if (nya_input_mouse_button_pressed(NYA_MOUSE_BUTTON_RIGHT)) {
                scene->orbit_yaw   += mouse->delta_x * GNY_CUBE3D_ORBIT_SENSITIVITY;
                scene->orbit_pitch -= mouse->delta_y * GNY_CUBE3D_ORBIT_SENSITIVITY;

                // stopped short of the poles, where the view is parallel to up and nya_matrix_look_at degenerates.
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
                gny_screen_request(GNY_SCREEN_MAIN_MENU);
                event->was_handled = true;
                break;
            }

            // a new landscape with everything put back. the pile is dropped again, since its bodies rest on ground about to
            // change. the seed advances by one, as in 2D.
            if (nya_input_action_matches(GNY_ACTION_REGENERATE_TERRAIN, key->key, key->modifier_flags)) {
                gny_layer_cube3d_cubes_clear();

                gny_terrain3d_generate(window, nya_world()->allocator, gny_terrain3d()->seed + 1);

                // teleported rather than respawned, so handles and on_click stay valid. the models too, or they would be
                // embedded in a new hill.
                f32 top = gny_terrain3d()->max_height;
                _gny_cube3d_body_reset(scene->cube, _gny_cube3d_drop_point());
                _gny_cube3d_body_reset(scene->model, (f32x3){ GNY_CUBE3D_MODEL_OFFSET, top + GNY_CUBE3D_MODEL_LIFT, 0.0F });
                _gny_cube3d_body_reset(scene->pill, (f32x3){ GNY_CUBE3D_PILL_OFFSET, top + GNY_CUBE3D_PILL_LIFT, 0.0F });

                gny_layer_cube3d_cubes_drop();

                event->was_handled = true;
                break;
            }

            // another pile on top, to show stacking.
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

            // the 2D world's key and flag, so B means the same in both scenes.
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

    // sensor overlaps have no closing speed.
    if (hit->kind != NYA_PHYSICS_HIT_IMPACT) return;

    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    // scaled by landing strength, like the 2D sparks.
    f32 threshold = nya_physics3d_hit_threshold();
    f32 strength  = nya_clamp((hit->approach_speed / threshold) - 1.0F, 0.0F, 1.0F);

    (void)nya_particles_emit(
        scene->dust,
        (NYA_ParticleBurst){
            .shape    = NYA_PARTICLE_SHAPE_CONE,
            .position = hit->point,
            .count    = (u32)nya_lerp((f32)GNY_CUBE3D_DUST_MIN, (f32)GNY_CUBE3D_DUST_MAX, strength),

            // `up`, not the hit normal, whose direction depends on the pair's order.
            .direction = { 0.0F, 1.0F, 0.0F },
            .spread    = 1.1F,

            .speed      = { GNY_CUBE3D_DUST_SPEED.x, nya_lerp(GNY_CUBE3D_DUST_SPEED.x, GNY_CUBE3D_DUST_SPEED.y, strength) },
            .lifetime_s = { 0.3F, 0.9F },
            .size       = GNY_CUBE3D_DUST_SIZE,
            .size_end   = { 0.0F, 0.02F },

            .color_start = GNY_CUBE3D_DUST_COLOR,
            // zero alpha, so it fades out.
            .color_end = { 0.72F, 0.68F, 0.60F, 0.0F },

            .gravity = { 0.0F, GNY_CUBE3D_DUST_GRAVITY, 0.0F },
            .damping = 0.8F,
        }
    );

    // the 3D listener, which can follow an orbiting camera. set each frame in _gny_cube3d_draw_scene from the same
    // orbit as the view. harder landings are louder, lower and detuned so a pile does not sound like one recording.
    nya_audio_play_sound_at_3d(
        NYA_ASSET_SOUNDS_HIT_WAV,
        hit->point,
        (NYA_SoundParams){
            .gain              = nya_lerp(GNY_HIT_GAIN_MIN, GNY_HIT_GAIN_MAX, strength) * nya_settings_volume_effective(NYA_VOLUME_CHANNEL_SOUND),
            .gain_variation_db = 1.5F,

            .pitch                     = nya_lerp(1.12F, 0.88F, strength),
            .pitch_variation_semitones = 0.6F,

            // ranked by strength, so a heavy landing is heard over six light ones in sixteen voices.
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

    // replaced: the pool is fixed.
    gny_layer_cube3d_cubes_clear();

    for (u32 i = 0; i < GNY_TERRAIN3D_CUBE_COUNT; i++) {
        GNY_FallingCube cube = _gny_cube3d_cube_spawn(i);

        // a failed spawn is a full entity table; the rest of the pile still spawns.
        if (!nya_entity_is_valid(cube.entity)) continue;

        scene->cubes[scene->cube_count++] = cube;
    }
}

void gny_layer_cube3d_cubes_clear(void) {
    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    // deferred, since this runs from a key handler and from on_destroy. count is zeroed now; the cubes leave the
    // table next frame.
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

        // already attached or gone: the common case, cheap to call unguarded.
        if (entity == nullptr || nya_physics3d_body_attached(entity)) continue;

        f32x3 min;
        f32x3 max;

        // still loading.
        if (!nya_render3d_mesh_bounds(window, models[i].mesh, &min, &max)) continue;

        // the draw's scale, so the body matches what is on screen.
        f32x3 size = (max - min) * models[i].scale;

        // a convex stand-in: a mesh shape is static and has no mass. Cubie's rounded corners collide square; convex
        // decomposition would fix it.
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
             * Capsule upright on y: radius is half the wider horizontal extent so the model fits inside, length is the
             * height left after both caps, clamped at zero.
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
     */
    gny_terrain3d_update(_gny_cube3d_camera_position(scene));

    // once a tick; the camera derives from orbit angles at draw time.
    nya_particles_update(scene->dust, delta_time_s);
    nya_particles_update(scene->fire, delta_time_s);
    nya_particles_update(scene->smoke, delta_time_s);

    // fed on a timer, so plume density does not depend on frame rate. a loop keeps the rate through long frames.
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

                // upward: hot air rises.
                .gravity = GNY_CUBE3D_FIRE_GRAVITY,
                .damping = 1.2F,
            }
        );

        // a little above the flame, where smoke becomes visible.
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

    // once a tick, so a sound un-muffles as the view swings clear of a hill.
    nya_audio_occlusion_update();

    /*
     * Cubes that fall off the terrain's rim are recycled: the pool is fixed, and an escaped body costs solver time
     * forever. Teleported so handles stay valid, with velocity cleared by hand.
     */
    for (u32 i = 0; i < scene->cube_count; i++) {
        NYA_Entity* cube = nya_entity_get(scene->cubes[i].entity);
        if (cube == nullptr || cube->position.y > GNY_TERRAIN3D_CUBE_KILL_Y) continue;

        // placed by an ever-climbing counter, so a cube recycled twice lands somewhere new.
        _gny_cube3d_body_reset(cube->handle, _gny_cube3d_cube_placement(GNY_TERRAIN3D_CUBE_COUNT + scene->cubes_recycled));
        scene->cubes_recycled++;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON RENDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Draws the 3D half of the scene between nya_render3d_begin and nya_render3d_end. Separate so it can go through
 * the bloom target without the HUD.
 * */
NYA_INTERNAL void _gny_cube3d_draw_scene(NYA_Window* window) {

    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    NYA_Entity* cube = nya_entity_get(scene->cube);

    f32x3 eye = _gny_cube3d_camera_position(scene);

    // aimed a little above the ground, so the basin sits mid-frame.
    f32x3 target = { 0.0F, gny_terrain3d_height_at(0.0F, 0.0F) + GNY_CUBE3D_SIZE, 0.0F };

    nya_render3d_begin(window, (NYA_Camera3DPerspective){ .position = eye, .target = target });

    /*
     * The ear on the camera, from the same vectors as the view, so they cannot disagree. There is no avatar, so the
     * camera is the right ear here. Cheap, so it is set every frame.
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

    /* The sky first, from the same GNY_SkyState as the light, shaded from the view ray so it turns with the camera. */
    nya_render3d_sky_draw(
        window,
        (NYA_Render3DSky){
            .zenith  = sky.top,
            .horizon = sky.bottom,

            // darker than the horizon and unlit, so the world sits on something.
            .ground = GNY_SKY3D_GROUND,

            .sun_direction = -sky.direction,
            .sun_color     = sky.disc,
            .sun_angle     = GNY_SKY3D_SUN_ANGLE,
            .sun_intensity = sky.is_night ? GNY_SKY3D_MOON_INTENSITY : GNY_SKY3D_SUN_INTENSITY,

            // a wide halo by day, tight at night, so the moon does not look like a second sun.
            .sun_halo = sky.is_night ? GNY_SKY3D_MOON_HALO : GNY_SKY3D_SUN_HALO,

            .horizon_softness = GNY_SKY3D_HORIZON_SOFTNESS,
            .ground_blend     = GNY_SKY3D_GROUND_BLEND,
        }
    );

    // fog in the sky's horizon colour, so the terrain's rim dissolves into the sky. tinted toward the light, which
    // matters at dawn.
    nya_render3d_fog_set(
        window,
        (NYA_Render3DFog){
            .color          = sky.bottom,
            .density        = GNY_SKY3D_FOG_DENSITY,
            .height_falloff = GNY_SKY3D_FOG_HEIGHT_FALLOFF,
            .sun_amount     = GNY_SKY3D_FOG_SUN_AMOUNT,
        }
    );

    // material is per flush, so two materials cost two draw calls however many objects use them. lamps are
    // re-added every frame because they orbit; the clear keeps a re-entered scene from doubling them.
    nya_render3d_point_lights_clear(window);

    f32 lamp_phase = nya_app_uptime_s() * GNY_CUBE3D_LAMP_SPEED;

    f32x3 lamp_positions[GNY_CUBE3D_LAMP_COUNT];

    for (u32 i = 0; i < GNY_CUBE3D_LAMP_COUNT; i++) {
        // evenly spread, so two lamps sit opposite each other.
        f32 angle = lamp_phase + ((f32)i * (2.0F * (f32)M_PI / (f32)GNY_CUBE3D_LAMP_COUNT));

        f32 lamp_x = cosf(angle) * GNY_CUBE3D_LAMP_RADIUS;
        f32 lamp_z = sinf(angle) * GNY_CUBE3D_LAMP_RADIUS;

        // above the ground beneath, so a lamp rises over hills.
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

    // the landscape is a few thousand flat triangles in the shared batch. the facets show relief better than a
    // grid.
    gny_terrain3d_draw(window);

    // the pile shares the terrain's material, so both batch into one draw call.
    for (u32 i = 0; i < scene->cube_count; i++) {
        const NYA_Entity* box = nya_entity_get(scene->cubes[i].entity);

        if (box == nullptr || scene->cubes[i].glass) continue;

        f32 size = scene->cubes[i].size;

        // rotation straight off the entity as a quaternion.
        nya_render3d_cube(window, box->position, (f32x3){ size, size, size }, box->rotation, scene->cubes[i].color);
    }

    /*
     * Glass cubes after opaque ones, grouped by blur level. Material is per flush, so levels are discrete: a draw
     * call per level instead of per cube. The renderer sorts transparency itself, so grouping only saves draws.
     */
    static const f32 glass_blurs[] = GNY_CUBE3D_GLASS_BLURS;

    for (u64 level = 0; level < nya_carray_length(glass_blurs); level++) {
        // counted first, so an unused level costs no draw call.
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

                // off: edge darkening stands in for occlusion, which see-through things do not have.
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

    // back to the pile's material, or the models below get glass highlights.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.0F, .roughness = 0.9F, .edge = GNY_CUBE3D_EDGE });

    if (cube != nullptr) {
        // brushed metal while held, matte plastic otherwise: material belongs to the draw, not the object.
        nya_render3d_material_set(
            window,
            scene->dragging ? (NYA_Render3DMaterial){ .metallic = 1.0F, .roughness = 0.28F, .edge = GNY_CUBE3D_EDGE }
                            : (NYA_Render3DMaterial){ .metallic = 0.0F, .roughness = 0.55F, .edge = GNY_CUBE3D_EDGE }
        );

        // rotation straight off the entity.
        nya_render3d_cube(
            window,
            cube->position,
            (f32x3){ GNY_CUBE3D_SIZE, GNY_CUBE3D_SIZE, GNY_CUBE3D_SIZE },
            cube->rotation,
            scene->dragging ? GNY_CUBE3D_HELD_COLOR : GNY_CUBE3D_COLOR
        );
    }

    // the loaded model shares the batch, light and material with the primitives. smooth shaded, since its normals
    // come from the file.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.1F, .roughness = 0.45F, .edge = GNY_CUBE3D_EDGE });

    // an ink outline on the models only. the material's edge term finds curvature, not silhouettes. the primitives
    // have no hull to expand.
    nya_render3d_outline_set(window, GNY_CUBE3D_OUTLINE_THICKNESS, GNY_CUBE3D_OUTLINE_COLOR);

    // placed and turned by the solver, so the models fall and roll like the cubes.
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

    // the pill gets a different roughness, showing material varies per draw within one batch.
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

    // outline off before the lamps: an outlined glowing bead reads as a hole.
    nya_render3d_outline_set(window, 0.0F, GNY_CUBE3D_OUTLINE_COLOR);

    /*
     * Water is the scene's translucent surface. The three panes are drawn nearest first on purpose, to show the
     * renderer sorts them: unsorted, the nearer pane would hide the far one. See NYA_Render3DStream.
     */
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.35F, .roughness = 0.15F, .edge = 0.0F });

    for (u32 i = 0; i < GNY_CUBE3D_WATER_LAYERS; i++) {
        f32 height = GNY_CUBE3D_WATER_LEVEL + ((f32)(GNY_CUBE3D_WATER_LAYERS - 1 - i) * GNY_CUBE3D_WATER_GAP);

        nya_render3d_plane(window, (f32x3){ 0.0F, height, 0.0F }, (f32x2){ GNY_CUBE3D_WATER_SIZE, GNY_CUBE3D_WATER_SIZE },
                           GNY_CUBE3D_WATER_COLOR);
    }

    // an emissive bead at each lamp, lit the same from every side and bright enough for bloom. see
    // NYA_Render3DMaterial.emission.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .roughness = 0.4F, .emission = GNY_CUBE3D_LAMP_EMISSION });

    for (u32 i = 0; i < GNY_CUBE3D_LAMP_COUNT; i++) {
        nya_render3d_sphere(window, lamp_positions[i], GNY_CUBE3D_LAMP_MARKER_RADIUS,
                            i == 0 ? GNY_CUBE3D_LAMP_A_COLOR : GNY_CUBE3D_LAMP_B_COLOR);
    }

    // inside the scene: 3D particles draw through render3d and need an active camera, and they share the cube's
    // depth buffer.
    nya_particles_draw(window, scene->dust);

    // smoke first, through the sorted transparent pass. fire is additive so overlapping tongues brighten. its colours
    // exceed one on purpose (GNY_CUBE3D_FIRE_COLOR_START) for the tonemap and bloom.
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

    // the sun is set before the shadow pass, which builds its matrix from the current light.
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
     * The shadow pass before the camera draw. The batch keeps no geometry, so the scene is drawn once per pass, and
     * _gny_cube3d_draw_scene must be callable repeatedly with no state of its own.
     */
    NYA_Camera3DPerspective shadow_camera = {
        .position = _gny_cube3d_camera_position(scene),
        .target   = { 0.0F, gny_terrain3d_height_at(0.0F, 0.0F) + GNY_CUBE3D_SIZE, 0.0F },
    };

    /* The target's aspect, since the fit measures the camera's frustum. */
    u32 target_width = 0, target_height = 0;
    nya_render2d_target_size(window, &target_width, &target_height);

    f32 aspect = target_height > 0 ? (f32)target_width / (f32)target_height : 0.0F;

    /*
     * Where casters start and end down the view. The camera orbits outside the terrain, so casters lie within the
     * terrain's reach of the target distance. Using the near plane put the sharp cascades in empty air. See
     * NYA_Render3DShadowFit.near_distance.
     */
    f32 subject_distance = nya_vector_length(shadow_camera.target - shadow_camera.position);
    f32 subject_reach    = GNY_CUBE3D_SHADOW_SUBJECT_REACH;

    f32 shadow_near = nya_max(subject_distance - subject_reach, 0.1F);
    f32 shadow_far  = subject_distance + subject_reach;

    for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
        nya_render3d_shadow_begin(
            window,
            nya_render3d_shadow_for_camera(shadow_camera, sky.direction, cascade,
                                           (NYA_Render3DShadowFit){
                                               .near_distance = shadow_near,
                                               .range    = shadow_far,
                                               .aspect   = aspect,
                                               .strength = GNY_CUBE3D_SHADOW_STRENGTH,
                                           })
        );

        if (nya_render3d_active(window)) _gny_cube3d_draw_scene(window);

        nya_render3d_shadow_end(window);
    }

    /*
     * Through the bloom target like the 2D world, or straight to the window. The lamp beads are past the bloom
     * threshold on purpose.
     */
    // minimised or mid resize, nya_post_begin fails and the scene goes straight to the window like the
    // 2D path does, rather than skipping the frame.
    if (!bloom_world->bloom_enabled || !nya_post_begin(window, &bloom_world->post)) {
        _gny_cube3d_draw_scene(window);
    } else {
        nya_perf_time_this_scope("gny_cube3d_bloom_pass");

        _gny_cube3d_draw_scene(window);

        // nya_post_end blits the scene back when a pipeline is not loaded, so a failure costs only the glow.
        nya_post_end(
            window, &bloom_world->post,
            (NYA_PostPass[]){
                {
                    .pipeline = GNY_PIPELINE_BLOOM,
                    .uniform =
                        &(NYA_ShaderBloomUniform){
                            // this scene's numbers; see GNY_BLOOM_3D_THRESHOLD.
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


    // screen pixels over the top in the same pass. render2d has no camera or depth test, and the scene has flushed,
    // so the HUD lands in front.
    nya_render2d_font_set(GNY_UI_FONT, GNY_UI_FONT_SIZE);

    f32 line = GNY_UI_PADDING;

    nya_render2d_text(window, nya_string_cube3d_title(), GNY_UI_PADDING, line, GNY_UI_TEXT);
    line += nya_render2d_font_line_height();

    nya_render2d_text(window, scene->grabbed_once ? nya_string_cube3d_hint_drag() : nya_string_cube3d_hint_click(), GNY_UI_PADDING, line,
                      GNY_UI_DIM);
    line += nya_render2d_font_line_height();

    nya_render2d_text(window, nya_string_cube3d_hint_camera(), GNY_UI_PADDING, line, GNY_UI_DIM);
    line += nya_render2d_font_line_height();

    // not translated: key letters are the same in every locale.
    nya_render2d_textf(window, GNY_UI_PADDING, line, GNY_UI_DIM, "space drops cubes, c clears, b bloom %s",
                       gny_world()->bloom_enabled ? "on" : "off");
    line += nya_render2d_font_line_height();

    if (cube != nullptr) {
        nya_render2d_textf(window, GNY_UI_PADDING, line, GNY_UI_DIM, "cube at " FMTf32x3, FMTf32x3_ARG(cube->position));
        line += nya_render2d_font_line_height();
    }

    // the seed is how to ask for this landscape again. the recycle count shows cubes falling off the edge.
    nya_render2d_textf(window, GNY_UI_PADDING, line, GNY_UI_DIM, "terrain seed %llu, %u bodies, %u recycled",
                       (unsigned long long)gny_terrain3d()->seed, nya_physics3d_body_count(), scene->cubes_recycled);
    line += nya_render2d_font_line_height();

    // `culled` shows what frustum culling saves. read before nya_render_end clears the counters.
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
    // on the world, so it survives a hot reload. see layers.h.
    return &gny_world()->cube3d;
}

f32 _gny_cube3d_occlusion(f32x3 source, void* user_data) {
    nya_unused(user_data);

    f32x3 ear = nya_audio_listener_3d_get().position;

    f32x3 to_source = source - ear;

    f32 distance = nya_vector_length(to_source);

    // a sound at the listener has no direction; treated as clear.
    if (distance < NYA_EPSILON) return 0.0F;

    // three rays around the direct line, so occlusion fades in thirds instead of snapping. spread perpendicular and
    // scaled by distance, so the spread is angular.
    f32x3 direction = to_source / distance;

    f32x3 reference = fabsf(direction.y) < 0.9F ? (f32x3){ 0.0F, 1.0F, 0.0F } : (f32x3){ 1.0F, 0.0F, 0.0F };

    f32x3 side = nya_vector_normalize(nya_vector_cross(direction, reference)) * (distance * GNY_CUBE3D_OCCLUSION_SPREAD);

    f32x3 targets[] = { source, source + side, source - side };

    u32 blocked = 0;

    for (u64 i = 0; i < nya_carray_length(targets); i++) {
        // from the ear toward the source: the raycast reports the first hit, and from the source it would hit the ground
        // the sound sits on.
        f32x3 ray = targets[i] - ear;

        NYA_EntityHandle hit = nya_physics3d_raycast(ear, ray, nullptr, nullptr);

        if (nya_entity_is_valid(hit)) blocked++;
    }

    u64 target_count = nya_carray_length(targets);

    return (f32)blocked / (f32)target_count;
}

f32x3 _gny_cube3d_drop_point(void) {
    // above the terrain's middle, not a fixed height, which could be underground.
    return (f32x3){ 0.0F, gny_terrain3d_height_at(0.0F, 0.0F) + GNY_CUBE3D_DROP_HEIGHT, 0.0F };
}

f32x3 _gny_cube3d_cube_placement(u32 index) {
    // two channels of the integer hash. offsetting the seed instead would correlate cube i's z with cube i+1's x
    // (nya_ihash2's stride is 57) and line the pile up diagonally.
    f32 x = nya_ihash2((s32)index, 0, GNY_TERRAIN3D_CUBE_SEED) * GNY_TERRAIN3D_CUBE_SPREAD;
    f32 z = nya_ihash2((s32)index, 1, GNY_TERRAIN3D_CUBE_SEED) * GNY_TERRAIN3D_CUBE_SPREAD;

    // above the highest ground, so every cube falls the same distance, staggered so they arrive as a stream.
    f32 ceiling = gny_terrain3d()->max_height + GNY_TERRAIN3D_CUBE_DROP;

    return (f32x3){ x, ceiling + ((f32)index * GNY_TERRAIN3D_CUBE_STAGGER), z };
}

f32 _gny_cube3d_cube_size(u32 index) {
    // a third channel, so size does not correlate with position.
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
        // the draggable cube's hook, so every landing kicks up dust.
        .on_collision = nya_callback(gny_layer_cube3d_on_collision)
    );

    if (!nya_entity_is_valid(handle)) return (GNY_FallingCube){ 0 };

    // an initial tumble, set on the entity since the attach seeds the body from it.
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

    // every fourth cube is glass, by index, so R replays it and glass never clumps without opaque cubes behind it.
    b8 glass = (index % GNY_CUBE3D_GLASS_EVERY) == 0;

    static const f32 blurs[] = GNY_CUBE3D_GLASS_BLURS;

    // blur levels cycle through the glass cubes so they alternate.
    f32 blur = glass ? blurs[(index / GNY_CUBE3D_GLASS_EVERY) % (u32)nya_carray_length(blurs)] : 0.0F;

    // allowed to sleep, which keeps a settled pile cheap. contact wakes it.
    return (GNY_FallingCube){
        .entity = handle,
        .size   = size,
        .color  = glass ? GNY_CUBE3D_GLASS_COLOR : palette[index % palette_size],
        .glass  = glass,
        .blur   = blur,
    };
}

f32x3 _gny_cube3d_camera_position(const GNY_Cube3DScene* scene) {
    // spherical to cartesian, y up: yaw about the vertical axis, pitch above the horizon.
    f32 horizontal = cosf(scene->orbit_pitch) * scene->orbit_range;

    // lifted by the ground height at the terrain's middle, so the camera is never underground.
    f32 pivot = gny_terrain3d_height_at(0.0F, 0.0F) + GNY_CUBE3D_SIZE;

    return (f32x3){
        sinf(scene->orbit_yaw) * horizontal,
        (sinf(scene->orbit_pitch) * scene->orbit_range) + pivot,
        cosf(scene->orbit_yaw) * horizontal,
    };
}

void _gny_cube3d_body_reset(NYA_EntityHandle handle, f32x3 position) {
    NYA_Entity* entity = nya_entity_get(handle);
    if (entity == nullptr) return;

    nya_physics3d_teleport(entity, position, nya_quaternion_identity);
    nya_physics3d_velocity_set(entity, f32x3_zero);
    nya_physics3d_angular_velocity_set(entity, f32x3_zero);
}

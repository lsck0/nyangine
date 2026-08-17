/**
 * @file layer_cube3d.c
 *
 * The 3D scene: a noise-generated landscape, a pile of cubes falling onto it, and one you can click and
 * spin.
 *
 * Here as an example rather than as a feature, and it is the example on purpose — between them these
 * few hundred lines exercise every seam that is new in three dimensions, and each of them is
 * somewhere a 2D habit gives the wrong answer:
 *
 * - **NYA_Camera3DPerspective** instead of a position and a zoom. It is placed and aimed, so the
 *   orbit below is spherical coordinates rather than a pan.
 * - **nya_physics3d_body_attach**, whose bodies carry a whole quaternion. The cube's rotation is the
 *   solver's, read straight off the entity, and there is no angle to pick out of it.
 * - **nya_render3d_cube**, batched, with a PBR material — so the ground and the cube are two draw
 *   calls because they are two *materials*, not because they are two objects.
 * - **A ray, not a point.** A click in 2D names a world position; in 3D it names a line, which is
 *   why picking goes through nya_render3d_screen_ray into nya_physics3d_raycast.
 * - **render2d over the top**, in the same frame and the same render pass, with no camera at all.
 *   That is the HUD case and it needed nothing special.
 *
 * ## Why the ground is a heightmap
 *
 * It was a flat plane over a static box, which was honest about what it was — a backdrop — and useless
 * as a test. A cube landing on a flat floor exercises one contact normal, never rolls, and settles in
 * the first second; nothing about that says whether the solver, the batching or the shading works on
 * anything harder.
 *
 * The landscape gives all three something to fail at: thousands of triangles with normals pointing
 * everywhere, a shadow receiver with real relief on it, and slopes a cube lands on, tips off, and rolls
 * down to somewhere different on every seed. See system_terrain3d.c.
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
 * Where the pile's `index`-th cube starts, hashed from the index alone.
 *
 * A pure function of the index, not an RNG, for the reason _gny_sky_random is: the same press of R has
 * to give the same pile, and a recycled cube has to be placed by the same rule as a fresh one without
 * either of them carrying state to do it with.
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

        // Two systems, because a system draws in one blend mode and these need different ones. See
        // GNY_Cube3DScene.
        .fire  = nya_particles_create(nya_world()->allocator, GNY_CUBE3D_FIRE_POOL),
        .smoke = nya_particles_create(nya_world()->allocator, GNY_CUBE3D_SMOKE_POOL),
    };

    /*
     * The model, queued rather than waited on.
     *
     * nya_asset_load returns as soon as the request is in the queue and the read happens at the end of
     * the frame, so the first frame or two of this scene draw without it — nya_render3d_mesh answers
     * "not loaded yet" by drawing nothing. That is the right trade for a few hundred kilobytes of FBX:
     * blocking here would stall the frame that opens the scene.
     *
     * Failure is not fatal. A missing or unreadable model leaves the scene with its primitives, which
     * is a worse-looking demo rather than a broken one.
     */
    // NYA_AssetHandle, not NYA_ConstCString: the handle typedef is a mutable char* and taking the
    // narrower type here only moved the cast to the call.
    // The same pipeline the 2D world composites through; see gny_bloom_pipeline_ensure.
    gny_bloom_pipeline_ensure(window);

    /*
     * The impact sound, queued here as well as by the 2D game layer.
     *
     * Not a duplicate load: nya_asset_load is keyed on the handle, so the second request for a handle
     * already loaded is a no-op. Asking here is what makes this scene independent of whether anyone has
     * been through the 2D one first — which they have not, coming straight from the main menu.
     *
     * Predecoded because it is short and played often, and decoding on the audio thread at the moment of
     * an impact is exactly when a hitch is audible.
     */
    NYA_Error sound = nya_asset_load((NYA_AssetLoadParameters){
        .type     = NYA_ASSET_TYPE_SOUND,
        .handle   = NYA_ASSET_SOUNDS_HIT_WAV,
        .as_sound = { .predecode = true },
    });

    // Not fatal. A machine with no audio device still shows the scene, and the play call below already
    // treats a missing asset as a no-op.
    if (!sound.ok) nya_warn("%s", (NYA_ConstCString)sound.message);

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

    /*
     * A soft radial sprite on the plume, which is what turns it from a stack of squares into smoke.
     *
     * An untextured billboard is a hard-edged square — the shape *is* the geometry, so no arrangement of
     * colours softens it. This is the difference between the two, and it is one texture rather than any
     * code: the alpha falls off smoothly to nothing at the rim, so overlapping puffs blend into a mass
     * instead of showing their outlines.
     *
     * Not on the dust. Impact chips are meant to look like chips — small, hard, and countable — and a soft
     * sprite would turn them into a puff of the same smoke.
     */
    NYA_Error puff = nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = GNY_CUBE3D_PUFF_TEXTURE });

    // Not fatal: nya_render3d_billboard draws untextured when the handle names nothing loaded, so a
    // missing sprite is a plume of squares rather than no plume.
    if (!puff.ok) nya_warn("%s", (NYA_ConstCString)puff.message);

    nya_particles_texture_set(scene->fire, GNY_CUBE3D_PUFF_TEXTURE);
    nya_particles_texture_set(scene->smoke, GNY_CUBE3D_PUFF_TEXTURE);

    /*
     * Gravity is negative y here, not positive.
     *
     * The 2D world's is positive because the screen's y grows downward. A 3D scene has no such
     * constraint and every 3D convention puts y up, so the two solvers genuinely disagree about
     * which way down is — which costs nothing, because nothing is ever simulated in both.
     */
    nya_physics3d_gravity_set(NYA_PHYSICS3D_GRAVITY_DEFAULT);

    /*
     * The ground, before anything that lands on it.
     *
     * A static triangle mesh from fBm noise rather than the flat box this used to be — see the note at
     * the top of the file for why, and system_terrain3d.c for how. The world's arena, because the sample
     * grid has to outlive this function and die with the world.
     *
     * Seeded from the launch seed like the 2D terrain, so `--world-seed` reproduces both scenes, and
     * mixed with a constant so the two do not generate the same shape from the same number.
     */
    gny_terrain3d_generate(window, nya_world()->allocator, GNY_LAUNCH.world_seed ^ GNY_TERRAIN3D_SEED);

    /*
     * The cube, dropped from a height so the first thing the scene does is show the solver working.
     *
     * Rotation is never touched by this file after the spawn. It is the solver's, written back onto
     * the entity every tick as a full quaternion, and the drag handler below adds to it through an
     * angular impulse rather than by assignment — writing a simulated body's transform fights the
     * solver, exactly as it does in 2D.
     */
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

    /*
     * The two models, as entities now rather than as two draw calls at fixed offsets.
     *
     * Spawned here without bodies. A model's size comes from its vertices and the vertices are still in
     * the asset queue at this point, so the attach happens in gny_layer_cube3d_models_attach on the first
     * update that finds them loaded.
     */
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

    /*
     * Occlusion, driven by the solver this scene already has.
     *
     * The engine deliberately does not raycast for itself — core_audio has no business knowing about
     * physics3d, and "what is between these two points" has a different answer in every game. It takes a
     * callback instead; see nya_audio_occlusion_set.
     *
     * Worth having in *this* scene specifically: the terrain is a bowl with a raised rim, so a cube
     * landing on the far slope genuinely has ground between it and the camera, and the effect is audible
     * rather than theoretical.
     */
    /*
     * A room on the effects bus, because the scene is one.
     *
     * The terrain is a bowl with a raised rim — geometrically a small hard-walled space — so a tail is
     * what a cube landing in it would actually produce. On the effects bus rather than the master, so the
     * music is not put in the same room as the impacts.
     *
     * It runs *after* the occlusion filter on the same bus, which is the order that means something: a
     * sound muffled by a hill reverberates muffled. See _nya_audio_group_mix_callback.
     */
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

    /*
     * The offscreen bloom target, which this layer creates and used not to release.
     *
     * GNY_World.scene is shared with the 2D game layer — the two screens are never up at once, so one
     * window sized texture serves both — and the release lived only in gny_layer_game_on_destroy. That
     * is correct for every path through the 2D game and wrong for the one this layer opens: main menu
     * into the 3D scene and straight out of the program never runs the game layer's on_destroy at all,
     * and the texture and its Vulkan views are still allocated at exit. LeakSanitizer reports it as a
     * block from SDL_BeginGPURenderPass with the textures hanging off it, which names the allocation
     * site and not the owner, so it is worth saying here which one this is.
     *
     * Guarded on the texture rather than on which layer got there first, so both releases are safe in
     * either order: nya_render_texture_destroy clears the handle, and whichever layer goes second finds
     * nothing to do.
     *
     * Safe on the shutdown path for the reason the game layer documents: nya_app_deinit destroys the
     * windows before the renderer, so the device is still alive here.
     */
    GNY_World* world = gny_world();
    if (world != nullptr && world->scene.texture != nullptr) nya_render_texture_destroy(&world->scene);

    // Despawning destroys the 3D body with it, the same contract the 2D one has. Deferred, because
    // this can run from inside the layer stack's own iteration.
    // Uninstalled before the solver it raycasts goes away. Passing null also clears whatever it was
    // applying, so a voice muffled at this moment is not left muffled for the rest of its life.
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

    /*
     * Zeroed *after* the terrain teardown, and this order matters.
     *
     * GNY_Terrain3D lives inside this struct, so clearing it first would drop the handle of the body
     * that still has to be despawned and the pointer to the grid that is worth keeping.
     */
    GNY_Terrain3D keep = scene->terrain;

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

            /*
             * A ray, because a click in three dimensions names a line rather than a point.
             *
             * This is the one part of picking that has no 2D counterpart: nya_physics2d_entity_at
             * takes a world position, because in 2D the screen *is* the world plane. Here the screen
             * is a window onto a volume and the pixel under the cursor is every point along a line.
             */
            NYA_Render3DRay ray = nya_render3d_screen_ray(window, (f32x2){ mouse->x, mouse->y });

            /*
             * Through nya_entity_click rather than raycasting and reacting here.
             *
             * This scene used to call nya_physics3d_raycast itself and compare the handle against
             * the cube's, which meant the cube's on_click was never run — and could not have been,
             * because nya_entity_click was 2D only. Both dimensions now dispatch the same way, so
             * "what happens when this is clicked" is a property of the entity here exactly as it is
             * for a crate in the 2D scene.
             *
             * The ground has no on_click, which is how it declines: no flag, no check, and no way
             * for a picker to disagree with what the entity actually does.
             */
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

            /*
             * The same hover the 2D scene drives, through the ray overload.
             *
             * Picking is the one part of this that genuinely differs by dimension — a point there, a
             * line here — and nya_entity_hover has an overload for each precisely so that an entity's
             * on_hover does not have to know which solver is carrying it.
             *
             * Before the drag below rather than after, so that a frame in which the cube is being spun
             * still reports it as hovered — the drag reads the same cursor and does not consume it.
             */
            NYA_Render3DRay hover_ray = nya_render3d_screen_ray(window, (f32x2){ mouse->x, mouse->y });

            (void)nya_entity_hover(hover_ray.origin, hover_ray.direction * GNY_CUBE3D_PICK_RANGE);

            if (scene->dragging) {
                /*
                 * Spun with an angular impulse rather than by writing the rotation.
                 *
                 * Assignment would fight the solver — it resolves the contact with the ground, this
                 * puts the cube back, and the result shivers. An impulse is a request the solver
                 * integrates, so the cube keeps spinning when the mouse stops and slows under the
                 * angular damping it was given.
                 *
                 * Horizontal mouse motion turns it about the world's up axis and vertical motion
                 * about the camera's right, which is what makes a drag feel like it is turning the
                 * object rather than the axes.
                 */
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

                // Stopped just short of the poles. At exactly straight up the view direction is
                // parallel to the up vector, there is no unique roll, and nya_matrix_look_at gives
                // back the identity — which reads as the camera snapping to the origin.
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

            /*
             * A new landscape, and everything standing on it put back.
             *
             * The pile is dropped again rather than left, because the bodies it is made of are resting
             * on a surface that is about to stop existing — Box3D would wake them into whatever the new
             * terrain does at that point, which for a crate that was on a hilltop is inside a valley
             * floor. Redropping is both the honest answer and the more interesting one to watch.
             *
             * The seed advances by one, which is what the 2D world's R does. Deterministic on purpose:
             * the same number of presses from the same launch seed gives the same landscape, so a
             * problem seen on the third one can be got back to.
             */
            if (nya_input_action_matches(GNY_ACTION_REGENERATE_TERRAIN, key->key, key->modifier_flags)) {
                gny_layer_cube3d_cubes_clear();

                gny_terrain3d_generate(window, nya_world()->allocator, gny_terrain3d()->seed + 1);

                // A teleport rather than a despawn and respawn, so the handle the scene holds — and the
                // on_click registered on it — stay valid.
                NYA_Entity* cube = nya_entity_get(scene->cube);

                if (cube != nullptr) {
                    nya_physics3d_teleport(cube, _gny_cube3d_drop_point(), nya_quaternion_identity);
                    nya_physics3d_velocity_set(cube, f32x3_zero);
                    nya_physics3d_angular_velocity_set(cube, f32x3_zero);
                }

                // The two models with them, for the same reason: they are resting on a surface that has
                // just been replaced, and a body left where it was is a body embedded in a new hill.
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

            // Another pile on top of whatever is already there, which is the quickest way to see the
            // solver handle a stack rather than a scatter.
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

            // The same flag and the same key the 2D world uses, so B means the same thing in both
            // scenes. It is read every frame by the composite below; nothing has to be rebuilt.
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

    /*
     * Scaled by how hard it landed, against the slowest impact that can appear at all.
     *
     * The same shape the 2D sparks use, and the same reason: one number driving the whole burst is
     * what makes a hard landing read as hard rather than as a fixed puff that happens to coincide.
     */
    f32 threshold = nya_physics3d_hit_threshold();
    f32 strength  = nya_clamp((hit->approach_speed / threshold) - 1.0F, 0.0F, 1.0F);

    (void)nya_particles_emit(
        scene->dust,
        (NYA_ParticleBurst){
            .shape    = NYA_PARTICLE_SHAPE_CONE,
            .position = hit->point,
            .count    = (u32)nya_lerp((f32)GNY_CUBE3D_DUST_MIN, (f32)GNY_CUBE3D_DUST_MAX, strength),

            // Away from the surface. The hit normal points from A toward B, and which of those the
            // cube is depends on the pair — so `up` is used instead, which for a cube landing on
            // flat ground is the same thing and is right whichever way round the pair came.
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

    /*
     * The landing, heard from where the camera is.
     *
     * Through nya_audio_play_sound_at_3d rather than the 2D nya_audio_play_sound_at the sparks use,
     * and that is the whole difference: the 2D listener is a point plus a fixed statement about which
     * world axis maps to which audio axis, which cannot describe a camera that orbits. Here it can, so
     * a cube landing behind the viewer swings across the speakers as the camera turns past it.
     *
     * The listener itself is set once a frame in _gny_cube3d_draw_scene, from the same orbit the
     * camera is built from — so the ear and the eye cannot drift apart.
     *
     * Scaled by the same `strength` the dust is, and shaped like the 2D world's impact for the same
     * reasons: harder landings are louder and lower, and the detune keeps a pile of them from sounding
     * like one recording played twenty times.
     */
    nya_audio_play_sound_at_3d(
        NYA_ASSET_SOUNDS_HIT_WAV,
        hit->point,
        (NYA_SoundParams){
            .gain              = nya_lerp(GNY_HIT_GAIN_MIN, GNY_HIT_GAIN_MAX, strength) * nya_settings_volume_effective(NYA_VOLUME_CHANNEL_SOUND),
            .gain_variation_db = 1.5F,

            .pitch                     = nya_lerp(1.12F, 0.88F, strength),
            .pitch_variation_semitones = 0.6F,

            // A pile lands in a burst, and sixteen voices is not many. Ranking by how hard each one hit
            // is what keeps the heavy landing audible when six light ones arrive in the same frame.
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

    // Replaced rather than added to. The pool is fixed, and a drop that appended would silently do
    // nothing once it was full — which reads as the key having stopped working.
    gny_layer_cube3d_cubes_clear();

    for (u32 i = 0; i < GNY_TERRAIN3D_CUBE_COUNT; i++) {
        GNY_FallingCube cube = _gny_cube3d_cube_spawn(i);

        // A failed spawn is a full entity table, which is not this layer's problem to solve and not
        // worth abandoning the rest of the pile over.
        if (!nya_entity_is_valid(cube.entity)) continue;

        scene->cubes[scene->cube_count++] = cube;
    }
}

void gny_layer_cube3d_cubes_clear(void) {
    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    /*
     * Deferred, because this runs from a key handler and from on_destroy.
     *
     * The second of those can be inside the layer stack's own iteration, and an immediate despawn there
     * is the trap gny_layer_cube3d_on_destroy already documents. Deferring costs a frame in which the
     * cubes are still in the entity table but no longer in this array — which is why the count is
     * zeroed here rather than after the despawns land.
     */
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

        // Already bodies, or gone. The first is the case on every frame but one, which is why this
        // function is cheap enough to call from on_update without a flag guarding it.
        if (entity == nullptr || nya_physics3d_body_attached(entity)) continue;

        f32x3 min;
        f32x3 max;

        // Still loading. nya_asset_load queues and the read lands at the end of the frame, so this is
        // the ordinary answer for the first frame or two of the scene rather than a failure.
        if (!nya_render3d_mesh_bounds(window, models[i].mesh, &min, &max)) continue;

        // The same scale the draw applies, so the body is the size of what is on screen. The bounds are
        // in the file's own units; see nya_render3d_mesh_bounds.
        f32x3 size = (max - min) * models[i].scale;

        /*
         * A convex stand-in, not a triangle mesh, and this is the interesting part of giving a model a body.
         *
         * NYA_PHYSICS3D_SHAPE_MESH exists and the vertices are right there — and it is exactly the wrong
         * choice here, because a triangle mesh has no interior and therefore no mass. It is a *static*
         * shape. Anything that has to fall needs a volume, which means a convex shape or a compound of
         * them, and for these two models the primitive that fits is obvious from what they are: a rounded
         * cube is a box, a capsule is a capsule.
         *
         * The cost is honest and visible: the collider is the model's bounding shape rather than its
         * silhouette, so Cubie's rounded corners collide square. Convex decomposition is the real answer
         * and is a tool, not a few lines here.
         */
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
             * The capsule is upright about y and described by a radius and the gap between the cap centres.
             *
             * The radius is half the *wider* of the two horizontal extents, so the model fits inside rather
             * than poking out of the narrower side. The length is what is left of the height once both caps
             * are accounted for, and it clamps at zero: a model shorter than it is wide is a sphere with a
             * capsule's name, and asking for a negative length is rejected by the attach.
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

        nya_info("Fitted a body to '%s' from its bounds (" FMTf32x3 ").", models[i].mesh, FMTf32x3_ARG(size));
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

    // Once a tick, here rather than in on_render — the solver moves the cube and writes its transform
    // onto the entity, and the camera is derived from the orbit angles at draw time, so this is the
    // only thing in this layer with state to advance.
    nya_particles_update(scene->dust, delta_time_s);
    nya_particles_update(scene->fire, delta_time_s);
    nya_particles_update(scene->smoke, delta_time_s);

    /*
     * The plume, fed on a timer rather than once per frame.
     *
     * A per-frame emission ties the fire's density to the frame rate, so the same scene is a bonfire at
     * 165 Hz and an ember at 30. A fixed interval emits the same number of puffs per second whatever the
     * frame rate is, and the while loop rather than an if is what keeps that true across a long frame.
     */
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

        // Started a little above the flame, where a real plume's smoke becomes visible — at the base it
        // is still burning.
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

    // The two models get their bodies on whichever frame their meshes finish loading. A no-op on every
    // frame after that; see the note at its definition for why it does not need a flag.
    gny_layer_cube3d_models_attach(window);

    // Once a tick, not once per sound: the callback is a raycast, and re-evaluating every live voice
    // against the current camera is what makes a sound un-muffle as the view swings clear of a hill.
    nya_audio_occlusion_update();

    /*
     * Anything that has left the world, put back at the top.
     *
     * The terrain is bounded, so a cube that catches the rim on the way down goes over the edge and
     * falls forever — which costs a body in the solver and shows nothing. Recycling rather than
     * despawning, because the pool is fixed and a scene that empties itself over a minute is a worse
     * demo than one that keeps going.
     *
     * Teleported rather than respawned, so the handle stays valid and the collision hook registered on
     * it keeps working. The velocity has to be cleared by hand: a teleport moves a body without
     * touching what it was doing, so a cube arriving at the top still carrying terminal velocity falls
     * straight back through.
     */
    for (u32 i = 0; i < scene->cube_count; i++) {
        NYA_Entity* cube = nya_entity_get(scene->cubes[i].entity);

        if (cube == nullptr || cube->position.y > GNY_TERRAIN3D_CUBE_KILL_Y) continue;

        // Placed by the same rule a fresh one is, from a counter that keeps climbing — so a cube that
        // goes over the edge twice does not come back to the spot it fell from.
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
 * Draws the 3D half of the scene: everything between nya_render3d_begin and nya_render3d_end.
 *
 * Split out so it can be aimed at an offscreen target for the bloom pass without the HUD text going
 * through it too — glowing text would be a different decision from a glowing lamp.
 * */
NYA_INTERNAL void _gny_cube3d_draw_scene(NYA_Window* window) {

    GNY_Cube3DScene* scene = _gny_cube3d_scene();

    NYA_Entity* cube = nya_entity_get(scene->cube);

    f32x3 eye = _gny_cube3d_camera_position(scene);

    // Aimed a little above the ground rather than at it, so the middle of the basin sits in the middle
    // of the frame instead of along its bottom edge.
    f32x3 target = { 0.0F, gny_terrain3d_height_at(0.0F, 0.0F) + GNY_CUBE3D_SIZE, 0.0F };

    nya_render3d_begin(window, (NYA_Camera3DPerspective){ .position = eye, .target = target });

    /*
     * The ear, on the camera, from the same two vectors the view is built from.
     *
     * Derived here rather than set alongside the orbit in on_update, so there is no way for the two to
     * disagree: whatever this frame is drawn from is what it is heard from. core_audio.h warns that the
     * camera is usually the wrong ear, and it is right here for the reason the 2D scene gives — there is
     * no avatar, and the camera *is* the point of view.
     *
     * Idempotent and cheap, so setting it every frame is the intended use rather than something to
     * guard with a change check.
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
     * The sky, first, and from the same GNY_SkyState the light above came from.
     *
     * This is what system_sky.c's own note said was missing: the 2D backdrop it draws serves both scenes
     * and stays put when a 3D camera orbits, so turning the world used to leave the sun in the corner of
     * the screen. Shaded from a view ray, it turns with everything else.
     *
     * `sun_direction` is the *negation* of the light's direction. GNY_SkyState.direction is the way light
     * travels — down from the sun — and this wants the way to look to find it. Passing it unnegated puts
     * the sun exactly opposite where the scene is lit from, which is the one mistake this API can make.
     *
     * The disc is drawn several times life-size. Half a degree is correct and reads as a bright dot; this
     * is a cartoon sky, and the sun in it is a shape.
     */
    nya_render3d_sky_draw(
        window,
        (NYA_Render3DSky){
            .zenith  = sky.top,
            .horizon = sky.bottom,

            // Darker than the horizon and unlit, so the world reads as sitting *on* something rather than
            // floating in the gradient. Not derived from the terrain, which is a different scene's colour.
            .ground = GNY_SKY3D_GROUND,

            .sun_direction = -sky.direction,
            .sun_color     = sky.disc,
            .sun_angle     = GNY_SKY3D_SUN_ANGLE,
            .sun_intensity = sky.is_night ? GNY_SKY3D_MOON_INTENSITY : GNY_SKY3D_SUN_INTENSITY,

            // A wide halo by day and a tight one at night: the moon has no atmosphere lit around it, and
            // a moon with a sunset's glow reads as a second sun.
            .sun_halo = sky.is_night ? GNY_SKY3D_MOON_HALO : GNY_SKY3D_SUN_HALO,

            .horizon_softness = GNY_SKY3D_HORIZON_SOFTNESS,
            .ground_blend     = GNY_SKY3D_GROUND_BLEND,
        }
    );

    /*
     * Two materials, therefore two draw calls, and that is the whole cost model.
     *
     * Not two because there are two objects — a hundred cubes of the same material are still one
     * draw call, because base colour is per vertex. Sorting a scene by material is what a renderer
     * built this way rewards, and is why the material is per flush rather than per primitive.
     */
    /*
     * The lamps, before anything is drawn: they are frame state, and adding one flushes.
     *
     * Cleared and re-added every frame rather than moved, because they orbit — and because the clear is
     * what keeps a scene that is re-entered from accumulating a second set. Two flushes a frame is the
     * honest price of the light set being a fragment uniform.
     */
    nya_render3d_point_lights_clear(window);

    f32 lamp_phase = nya_app_uptime_s() * GNY_CUBE3D_LAMP_SPEED;

    f32x3 lamp_positions[GNY_CUBE3D_LAMP_COUNT];

    for (u32 i = 0; i < GNY_CUBE3D_LAMP_COUNT; i++) {
        // Spread evenly around the circle, so two lamps sit opposite each other rather than together.
        f32 angle = lamp_phase + ((f32)i * (2.0F * (f32)M_PI / (f32)GNY_CUBE3D_LAMP_COUNT));

        f32 lamp_x = cosf(angle) * GNY_CUBE3D_LAMP_RADIUS;
        f32 lamp_z = sinf(angle) * GNY_CUBE3D_LAMP_RADIUS;

        // Above the ground it is over, so a lamp passing across a hill rises with it instead of
        // disappearing into it. Same correction the two models need above, for the same reason.
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

    /*
     * The landscape, as a few thousand flat triangles in the same batch as everything else.
     *
     * No grid over it any more: a wireframe grid on the xz plane is a reading aid for a flat floor and
     * becomes a mess of lines floating through hillsides the moment the ground has relief. The facets
     * do that job now — they are what shows where the surface is and which way it faces.
     */
    gny_terrain3d_draw(window);

    /*
     * The pile, between the ground and the draggable cube.
     *
     * Same material, so all of them go into the batch the terrain is already in and come out in the
     * same draw call — twenty-four boxes and two thousand ground triangles for the price of one. That
     * is the claim the scene exists to make, and it is only true because the colour is per vertex.
     */
    for (u32 i = 0; i < scene->cube_count; i++) {
        const NYA_Entity* box = nya_entity_get(scene->cubes[i].entity);

        if (box == nullptr || scene->cubes[i].glass) continue;

        f32 size = scene->cubes[i].size;

        // The rotation is read straight off the entity, where the solver wrote it as a full quaternion.
        // There is no angle to extract and none could be: three degrees of angular freedom do not
        // reduce to one.
        nya_render3d_cube(window, box->position, (f32x3){ size, size, size }, box->rotation, scene->cubes[i].color);
    }

    /*
     * The glass ones after the opaque ones, grouped by how frosted they are.
     *
     * One run per blur level rather than one loop with a material change inside it. The material is per
     * flush, so switching it per cube would be a draw call per cube; grouping makes it a draw call per
     * *level*, which is the same cost model the two materials at the top of this function have and the
     * reason there are three levels rather than a continuous range.
     *
     * The order between the runs is not what makes the blending correct — the renderer routes these into
     * the sorted transparent stream by their alpha and orders them there. What the grouping buys is draw
     * calls.
     *
     * `refraction` is what turns these from tinted panes into glass: the surface stops blending over the
     * scene and starts sampling it. See NYA_Render3DMaterial.refraction for what that cannot do.
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

                // Off: edge darkening stands in for occlusion on a curved surface, and something you can
                // see through does not occlude itself.
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

    // Back to the pile's own material for whatever is drawn next. Leaving glass set would put a hard
    // highlight and a full rim on the models below.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.0F, .roughness = 0.9F, .edge = GNY_CUBE3D_EDGE });

    if (cube != nullptr) {
        // Brushed metal while held, matte plastic otherwise — the clearest way to show that the
        // material is a property of the *draw* and not of the object.
        nya_render3d_material_set(
            window,
            scene->dragging ? (NYA_Render3DMaterial){ .metallic = 1.0F, .roughness = 0.28F, .edge = GNY_CUBE3D_EDGE }
                            : (NYA_Render3DMaterial){ .metallic = 0.0F, .roughness = 0.55F, .edge = GNY_CUBE3D_EDGE }
        );

        // The rotation comes straight off the entity, where the solver wrote it. No angle is
        // extracted and none could be: three degrees of angular freedom do not reduce to one.
        nya_render3d_cube(
            window,
            cube->position,
            (f32x3){ GNY_CUBE3D_SIZE, GNY_CUBE3D_SIZE, GNY_CUBE3D_SIZE },
            cube->rotation,
            scene->dragging ? GNY_CUBE3D_HELD_COLOR : GNY_CUBE3D_COLOR
        );
    }

    /*
     * The loaded model, beside the primitives rather than instead of them.
     *
     * Which is the point of it being here: the ground is a plane, the box is a generated cube and this
     * is a mesh read off disk, and all three go into the same batch, take the same light and the same
     * material, and come out in one draw call. A model that needed its own pass would show up as a
     * second draw and a different shading.
     *
     * Smooth shaded, unlike the cube next to it — the normals come from the file. See
     * nya_render3d_mesh.
     */
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.1F, .roughness = 0.45F, .edge = GNY_CUBE3D_EDGE });

    /*
     * Ink around the two models, which the material's edge term cannot draw.
     *
     * The edge term finds curvature from screen-space derivatives, so it picks out fillets and grazing
     * angles and nothing else — see mesh3d_shading.hlsli. An inverted hull gives the silhouette a real
     * line, which is what makes a low-poly model read as drawn rather than as shaded.
     *
     * Only the models. The generated primitives have no second copy to expand, and a pile of outlined
     * cubes would be a mesh of black lines rather than a pile.
     */
    nya_render3d_outline_set(window, GNY_CUBE3D_OUTLINE_THICKNESS, GNY_CUBE3D_OUTLINE_COLOR);

    /*
     * Placed and turned by the solver, not by the clock.
     *
     * Both used to sit at a fixed offset and spin at a constant rate about y — a model on a turntable,
     * which showed that a mesh batches with the primitives and nothing else. Reading the transform off the
     * entity instead makes them part of the same simulation the cubes are in: they fall onto the terrain,
     * roll down it, and are knocked about by anything that lands on them.
     *
     * The rotation is a full quaternion off the body. A turntable only ever needed one angle, which is
     * precisely why it could not show what a 3D solver actually produces.
     */
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

    /*
     * The pill, with its own material as well as its own transform.
     *
     * A different roughness on purpose: the two models and the cube then differ by material rather than
     * only by shape, which is what shows that the material is per *draw* within one batch — the same
     * point the cube's held-versus-idle switch makes, made across objects instead of across time.
     */
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

    // Off again before the lamps: an emissive bead is a light source, and a light source with an ink
    // outline reads as a hole rather than as a glow.
    nya_render3d_outline_set(window, 0.0F, GNY_CUBE3D_OUTLINE_COLOR);

    /*
     * Water in the bottom of the basin, and the reason it is here.
     *
     * It is the scene's only translucent surface, and without one the transparency ordering has nothing to
     * be right or wrong about. Two overlapping panes are what shows the difference: drawn in call order
     * the nearer one writes depth and the further one vanishes behind it, while sorted back to front they
     * tint each other. See NYA_Render3DStream.
     *
     * Three panes at slightly different heights rather than one, so the sort has work to do — and they are
     * drawn *nearest first* on purpose, to prove the renderer reorders them rather than relying on the
     * caller having got it right.
     */
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 0.35F, .roughness = 0.15F, .edge = 0.0F });

    for (u32 i = 0; i < GNY_CUBE3D_WATER_LAYERS; i++) {
        f32 height = GNY_CUBE3D_WATER_LEVEL + ((f32)(GNY_CUBE3D_WATER_LAYERS - 1 - i) * GNY_CUBE3D_WATER_GAP);

        nya_render3d_plane(window, (f32x3){ 0.0F, height, 0.0F }, (f32x2){ GNY_CUBE3D_WATER_SIZE, GNY_CUBE3D_WATER_SIZE },
                           GNY_CUBE3D_WATER_COLOR);
    }

    /*
     * A glowing bead at each lamp, so a light has something to look at.
     *
     * Emissive rather than merely bright: emission is added unlit, so the bead reads as the same colour
     * from every side instead of having a dark half — which is what a light source looks like, and what a
     * bloom pass needs in order to find it. See NYA_Render3DMaterial.emission.
     */
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .roughness = 0.4F, .emission = GNY_CUBE3D_LAMP_EMISSION });

    for (u32 i = 0; i < GNY_CUBE3D_LAMP_COUNT; i++) {
        nya_render3d_sphere(window, lamp_positions[i], GNY_CUBE3D_LAMP_MARKER_RADIUS,
                            i == 0 ? GNY_CUBE3D_LAMP_A_COLOR : GNY_CUBE3D_LAMP_B_COLOR);
    }

    /*
     * Inside the 3D scene, before it is flushed.
     *
     * A 3D particle system draws through render3d and is skipped entirely when no 3D camera is
     * active — so drawing it after nya_render3d_end would silently produce nothing. Being between
     * begin and end is also what puts the dust in the same depth buffer as the cube, so a chip
     * behind the cube is hidden by it.
     */
    nya_particles_draw(window, scene->dust);

    /*
     * Smoke before fire, and fire additive.
     *
     * The smoke goes through the ordinary sorted transparent pass, so its own puffs layer correctly over
     * each other. The fire is switched to additive first: overlapping tongues then brighten toward white
     * the way overlapping light does, where alpha would average them into a grey-brown smear.
     *
     * Its colours are deliberately above one — see GNY_CUBE3D_FIRE_COLOR_START — which the tonemap's
     * shoulder absorbs and the bloom pass finds.
     */
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

    /*
     * The sun, before the shadow pass.
     *
     * nya_render3d_shadow_begin builds its matrix from the light currently on the batch, so the sun
     * direction has to be set here — otherwise the shadow pass uses the default sun (upper front left)
     * while the scene pass uses the sky's sun, and the shadows land in the wrong place.
     */
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
     * The shadow pass, before anything is drawn for the camera.
     *
     * The scene is drawn twice — once from the sun into the depth map, once from the camera sampling it —
     * because the batch flushes as it fills and keeps no frame of geometry to replay. That is the whole
     * cost of the design, and it is why _gny_cube3d_draw_scene has to be callable twice with no state of
     * its own: see nya_render3d_shadow_begin.
     *
     * Centred on the origin rather than on the camera. This scene is a fixed ground plane a few units
     * across, so a following volume would only spend resolution on ground nobody is looking at. A scrolling
     * world would centre it ahead of the camera instead.
     */
    /*
     * One pass per cascade, and the loop is here rather than inside the engine.
     *
     * It has to be: this renderer keeps no geometry between flushes, so nothing but the caller can draw
     * the scene again. That was already true of the single shadow map — the note above is about the scene
     * being drawn twice — and cascades make it three times or four rather than changing the shape.
     *
     * `extent` names the *nearest* cascade. Each one after it is NYA_RENDER3D_SHADOW_CASCADE_RATIO wider
     * over the same resolution, so the near map is dense around the middle of the basin and the far one
     * reaches the rim.
     */
    for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
        nya_render3d_shadow_begin(
            window,
            (NYA_Render3DShadow){
                .center   = f32x3_zero,
                .extent   = GNY_CUBE3D_SHADOW_EXTENT,
                .strength = GNY_CUBE3D_SHADOW_STRENGTH,
                .cascade  = cascade,
            }
        );

        if (nya_render3d_active(window)) _gny_cube3d_draw_scene(window);

        nya_render3d_shadow_end(window);
    }

    /*
     * Through an offscreen target and the bloom pipeline, or straight to the window.
     *
     * The same arrangement _gny_camera_render_primary uses for the 2D world, and the same shared target:
     * the two screens are never up at once, so one window sized texture serves both. Emission is what
     * makes this worth doing here — the lamps' beads are drawn past the bloom threshold on purpose, and
     * without this pass they are simply small bright spheres.
     */
    /*
     * Straight to the window unless bloom is both wanted and *available*.
     *
     * The availability half is not defensive padding. The composite draws the offscreen target through
     * the bloom pipeline, and render2d drops a batch whose pipeline is not loaded — silently, because a
     * pipeline still loading is the ordinary case on the first frame. So a pipeline that failed
     * permanently does not cost the glow, it costs *the entire scene*: the world renders into a texture
     * that is then never blitted back, and the window shows nothing at all.
     *
     * That is exactly what happened on Windows, where the bloom pipeline was rejected by D3D12 — the 3D
     * scene simply did not appear, and nothing in the frame said why. The underlying cause is fixed in
     * gny_bloom_pipeline_ensure; this is what keeps the next backend disagreement from being invisible.
     */
    b8 bloom_ready = nya_asset_status(GNY_PIPELINE_BLOOM) == NYA_ASSET_STATUS_LOADED;

    if (!bloom_world->bloom_enabled || !bloom_ready) {
        _gny_cube3d_draw_scene(window);
    } else {
        gny_scene_target_ensure(window);

        if (bloom_world->scene.texture == nullptr) {
            // Minimised or mid resize. Drawing straight to the window is the same fallback the 2D path
            // takes rather than skipping the frame.
            _gny_cube3d_draw_scene(window);
        } else {
            nya_perf_time_this_scope("gny_cube3d_bloom_pass");

            // Transparent, because the background layer has already drawn the sky underneath and this
            // target is composited over it. Clearing to a colour would paint a rectangle over the stars.
            nya_render_texture_begin(window, &bloom_world->scene, NYA_COLOR_TRANSPARENT);
            _gny_cube3d_draw_scene(window);
            nya_render_texture_end(window);

            nya_render2d_shader_begin(window, GNY_PIPELINE_BLOOM);

            nya_render2d_shader_set_uniform(
                window,
                &(NYA_ShaderBloomUniform){
                    // This scene's own numbers, not the 2D world's. See GNY_BLOOM_3D_THRESHOLD for why
                    // one pair cannot serve a dim side-on scene and a daylit landscape at once.
                    .texel_x   = GNY_BLOOM_3D_SPREAD / (f32)bloom_world->scene.width,
                    .texel_y   = GNY_BLOOM_3D_SPREAD / (f32)bloom_world->scene.height,
                    .threshold = GNY_BLOOM_3D_THRESHOLD,
                    .intensity = GNY_BLOOM_3D_INTENSITY,
                },
                sizeof(NYA_ShaderBloomUniform)
            );

            // White: the shader multiplies by the vertex colour, and anything else tints the whole scene.
            nya_render2d_render_texture(window, &bloom_world->scene, 0.0F, 0.0F, (f32)window->screen_width,
                                        (f32)window->screen_height, NYA_COLOR_WHITE);

            nya_render2d_shader_end(window);
        }
    }


    /*
     * Screen pixels, over the top, in the same frame and the same render pass.
     *
     * Nothing was switched off to get here and no camera was reset: render2d never had one, its
     * pipelines do not test depth, and nya_render3d_end already flushed the scene. "In front" is
     * decided by which batch flushed last, and that is the whole of the interop rule.
     */
    nya_render2d_font_set(GNY_UI_FONT, GNY_UI_FONT_SIZE);

    f32 line = GNY_UI_PADDING;

    nya_render2d_text(window, nya_string_cube3d_title(), GNY_UI_PADDING, line, GNY_UI_TEXT);
    line += nya_render2d_font_line_height();

    nya_render2d_text(window, scene->grabbed_once ? nya_string_cube3d_hint_drag() : nya_string_cube3d_hint_click(), GNY_UI_PADDING, line,
                      GNY_UI_DIM);
    line += nya_render2d_font_line_height();

    nya_render2d_text(window, nya_string_cube3d_hint_camera(), GNY_UI_PADDING, line, GNY_UI_DIM);
    line += nya_render2d_font_line_height();

    // Not through i18n: the key letters are not translated, and a line that is three quarters key names
    // would be four more strings in every locale to say the same thing.
    nya_render2d_textf(window, GNY_UI_PADDING, line, GNY_UI_DIM, "space drops cubes, c clears, b bloom %s",
                       gny_world()->bloom_enabled ? "on" : "off");
    line += nya_render2d_font_line_height();

    if (cube != nullptr) {
        nya_render2d_textf(window, GNY_UI_PADDING, line, GNY_UI_DIM, "cube at " FMTf32x3, FMTf32x3_ARG(cube->position));
        line += nya_render2d_font_line_height();
    }

    /*
     * The numbers that say whether the test is doing anything.
     *
     * The seed is here because it is the only way to ask for a landscape again, and the recycle count
     * because a pile that is quietly falling off the edge and coming back looks identical to one that is
     * resting until this number moves.
     */
    nya_render2d_textf(window, GNY_UI_PADDING, line, GNY_UI_DIM, "terrain seed %llu, %u bodies, %u recycled",
                       (unsigned long long)gny_terrain3d()->seed, nya_physics3d_body_count(), scene->cubes_recycled);
    line += nya_render2d_font_line_height();

    /*
     * What the 3D batch actually did, which was previously unknowable.
     *
     * These counters existed and were never reset or read — see nya_render3d_frame_stats. `culled` is the
     * one worth watching: frustum culling is a cost when everything is on screen and a saving when it is
     * not, and the ratio between it and the vertex count is what says which this frame was.
     *
     * Read before nya_render_end, which is where the frame's counters are cleared.
     */
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
    // On the world rather than in a static, so it survives a hot reload — the same reason the menus
    // keep their state there. See layers.h.
    return &gny_world()->cube3d;
}

f32 _gny_cube3d_occlusion(f32x3 source, void* user_data) {
    nya_unused(user_data);

    f32x3 ear = nya_audio_listener_3d_get().position;

    f32x3 to_source = source - ear;

    f32 distance = nya_vector_length(to_source);

    // A sound at the listener's own position has no direction to cast along, and nothing can be between
    // a point and itself. Clear rather than blocked, which is the safe answer for a degenerate case.
    if (distance < NYA_EPSILON) return 0.0F;

    /*
     * Three rays rather than one, spread around the direct line.
     *
     * One ray is a boolean, and a boolean occlusion switches audibly as a cube rolls behind a ridge —
     * the sound snaps from open to muffled in a single frame. Three gives thirds, which the engine
     * interpolates into a partial cutoff, and a source passing behind an edge fades instead of jumping.
     *
     * The spread is perpendicular to the line and scaled by distance, so it stays an angular spread
     * rather than a fixed offset that would be enormous up close and meaningless far away.
     */
    f32x3 direction = to_source / distance;

    f32x3 reference = fabsf(direction.y) < 0.9F ? (f32x3){ 0.0F, 1.0F, 0.0F } : (f32x3){ 1.0F, 0.0F, 0.0F };

    f32x3 side = nya_vector_normalize(nya_vector_cross(direction, reference)) * (distance * GNY_CUBE3D_OCCLUSION_SPREAD);

    f32x3 targets[] = { source, source + side, source - side };

    u32 blocked = 0;

    for (u64 i = 0; i < nya_carray_length(targets); i++) {
        /*
         * Cast from the ear toward the source, not the other way.
         *
         * The raycast reports the *first* thing it meets. Starting at the source would report the
         * geometry the sound is sitting on — the ground under a cube is between it and everything — so
         * every landing would read as fully occluded.
         */
        f32x3 ray = targets[i] - ear;

        NYA_EntityHandle hit = nya_physics3d_raycast(ear, ray, nullptr, nullptr);

        if (nya_entity_is_valid(hit)) blocked++;
    }

    return (f32)blocked / (f32)nya_carray_length(targets);
}

f32x3 _gny_cube3d_drop_point(void) {
    // Above the middle of the terrain rather than at a fixed height, because the middle of the terrain is
    // not at zero any more and a constant would put the cube underground on a seed with a hill there.
    return (f32x3){ 0.0F, gny_terrain3d_height_at(0.0F, 0.0F) + GNY_CUBE3D_DROP_HEIGHT, 0.0F };
}

f32x3 _gny_cube3d_cube_placement(u32 index) {
    /*
     * Two independent channels of the engine's integer hash, mapped onto a square around the origin.
     *
     * The channel argument rather than two different seeds, for the reason spelled out in
     * _gny_sky_random: nya_ihash2 mixes y with a stride of 57, and offsetting the seed by one instead
     * would make cube i's z the same number as cube i+1's x — a pile that lands along a diagonal.
     */
    f32 x = nya_ihash2((s32)index, 0, GNY_TERRAIN3D_CUBE_SEED) * GNY_TERRAIN3D_CUBE_SPREAD;
    f32 z = nya_ihash2((s32)index, 1, GNY_TERRAIN3D_CUBE_SEED) * GNY_TERRAIN3D_CUBE_SPREAD;

    /*
     * Above the *highest* ground, not above the ground beneath it.
     *
     * Dropping each cube from a fixed height over its own column would start one over a valley below the
     * hilltop next to it, and the two would reach the ground at the same time from different distances —
     * which reads as gravity varying across the map. A common ceiling keeps the fall honest.
     *
     * The stagger is per index, so they arrive as a stream rather than a sheet. A sheet of twenty-four
     * simultaneous contacts is also the least interesting thing to ask a solver for.
     */
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

    /*
     * An initial tumble, so no two land on the same face.
     *
     * Set on the entity before the attach rather than as an impulse afterwards: nya_physics3d_body_attach
     * seeds the body from the entity's velocity and angular velocity, which is one call instead of two
     * and does not have to guess at an impulse that would produce the spin wanted.
     */
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

    /*
     * Every fourth one is glass, by index rather than by chance.
     *
     * The index, so the same press of R gives the same arrangement — the whole pile is deterministic from
     * its index and this has no reason to be the exception. Spreading them evenly also matters more than
     * it sounds: a random selection clusters, and a clump of glass cubes with no opaque ones behind them
     * has nothing to be seen *against*.
     */
    b8 glass = (index % GNY_CUBE3D_GLASS_EVERY) == 0;

    static const f32 blurs[] = GNY_CUBE3D_GLASS_BLURS;

    // Which frosting this one gets, cycled through the levels by how many glass cubes came before it —
    // so the three levels alternate rather than clustering.
    f32 blur = glass ? blurs[(index / GNY_CUBE3D_GLASS_EVERY) % (u32)nya_carray_length(blurs)] : 0.0F;

    /*
     * Allowed to sleep, unlike the draggable cube.
     *
     * That one never sleeps because a drag has to be able to move it at any moment. These are only ever
     * pushed by each other and by gravity, so letting the solver park a settled pile is most of what
     * keeps twenty-four bodies cheap — and a sleeping body is woken by a contact from one that is not.
     */
    return (GNY_FallingCube){
        .entity = handle,
        .size   = size,
        .color  = glass ? GNY_CUBE3D_GLASS_COLOR : palette[index % palette_size],
        .glass  = glass,
        .blur   = blur,
    };
}

f32x3 _gny_cube3d_camera_position(const GNY_Cube3DScene* scene) {
    // Spherical to cartesian with y up. Yaw turns about the vertical axis and pitch lifts above the
    // horizon, which is the orbit every 3D viewer has and the reason the camera is placed and aimed
    // rather than positioned and zoomed.
    f32 horizontal = cosf(scene->orbit_pitch) * scene->orbit_range;

    // Lifted by where the ground is under the middle of the terrain, not by a constant. The constant was
    // right when the ground was a plane at y = 0; on a landscape it puts the camera underground on any
    // seed with a hill in the centre.
    f32 pivot = gny_terrain3d_height_at(0.0F, 0.0F) + GNY_CUBE3D_SIZE;

    return (f32x3){
        sinf(scene->orbit_yaw) * horizontal,
        (sinf(scene->orbit_pitch) * scene->orbit_range) + pivot,
        cosf(scene->orbit_yaw) * horizontal,
    };
}

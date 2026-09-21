/**
 * @file layer_game.c
 *
 * The 2D scene: terrain and tilemap, crates spawned by clicking, the camera entities, input actions,
 * music and the bloom post chain.
 * */
#include "gnyame/gnyame.h"
#include "generated/assets.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Drops a small burst of crates above the camera, for filling the world without clicking. */
NYA_INTERNAL void _gny_box_burst(u32 count);




/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON CREATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_game_on_create(NYA_Window* window) {
    nya_unused(window);

    // The 2D game's tick, which the engine's registry runs between the layer stack and the tweens.
    // On here rather than at registration, so the menu and the 3D demo do not tick a world they have
    // nothing to do with. See gny_systems_register_all.
    gny_systems_gameplay_enable();

    // Queued, not loaded: the asset system resolves it at the end of the frame. Predecoded because
    // it is short and played often, and decoding on the audio thread at the moment of an impact is
    // exactly when a hitch is audible.
    NYA_Error sound = nya_asset_load((NYA_AssetLoadParameters){
        .type     = NYA_ASSET_TYPE_SOUND,
        .handle   = NYA_ASSET_SOUNDS_HIT_WAV,
        .as_sound = { .predecode = true },
    });

    // Not fatal. A machine with no audio device still plays the demo, and nya_audio_play_sound
    // already treats a missing asset as a no-op.
    if (!sound.ok) nya_log_warn("%s", (NYA_ConstCString)sound.message);

    // Shared with the 3D scene, which composites through the same pipeline. See layers.c.
    gny_post_pipelines_ensure(window);


    // guarded, because hot reload re-resolves callbacks without re-running on_create. Regenerating the
    // terrain under a settled pile would bury every crate.
    GNY_World* world = gny_world();
    if (nya_entity_is_valid(world->terrain)) return;

    gny_entity_camera_create((f32x2){ GNY_CAMERA_START_X, GNY_CAMERA_START_Y }, GNY_CAMERA_START_ZOOM);

    gny_terrain_generate(world->terrain_seed);

    /*
     * The Tiled map, loaded from the world's arena so it dies with the world.
     */
    NYA_Error map_error = nya_tilemap_load(world->allocator, NYA_ASSET_MAPS_DEMO_TOPDOWN_TMJ, &world->tilemap);

    if (!map_error.ok) {
        u8 message[256];
        (void)nya_error_format(&map_error, message, sizeof(message));
        nya_log_warn("Could not load the demo tilemap: %s", (NYA_CString)message);
    } else {
        // placed first: draw, collision and conversions are all relative to the map origin.
        world->tilemap->origin = GNY_TILEMAP_ORIGIN;

        // The invisible "collision" layer becomes static bodies. Merged into runs, so the map's three
        // solid rows are three wide boxes rather than sixty one-tile ones a crate could catch on.
        (void)nya_tilemap_collision_build(world->tilemap, "collision", GNY_TILEMAP_COLLIDER_KIND);

        // the same layer the terrain chain is on: to everything that queries, the map's solid cells
        // and the ground are the same thing.
        NYA_PhysicsLayerMask terrain = nya_physics_layer(GNY_LAYER_TERRAIN);

        nya_entity_foreach_kind (GNY_TILEMAP_COLLIDER_KIND, collider) nya_physics2d_layers_set(collider, terrain, NYA_PHYSICS_LAYER_ALL);
    }

    /*
     * Three one-way ledges above the terrain, the middle one patrolling.
     */
    gny_entity_ledge_create((f32x2){ GNY_LEDGE_LEFT_X, GNY_TERRAIN_BASE_Y - GNY_LEDGE_BASE_LIFT }, GNY_LEDGE_SIZE, 0.0F);

    gny_entity_ledge_create((f32x2){ GNY_LEDGE_MIDDLE_X, GNY_TERRAIN_BASE_Y - GNY_LEDGE_BASE_LIFT - GNY_LEDGE_STEP_LIFT },
                            GNY_LEDGE_SIZE, GNY_LEDGE_PATROL_DISTANCE);

    gny_entity_ledge_create((f32x2){ GNY_LEDGE_RIGHT_X, GNY_TERRAIN_BASE_Y - GNY_LEDGE_BASE_LIFT }, GNY_LEDGE_SIZE, 0.0F);

    _gny_box_burst(12);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON DESTROY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_game_on_destroy(NYA_Window* window) {
    nya_unused(window);

    // the partner of the enable in on_create: registered and initialized still, simply not ticking.
    gny_systems_gameplay_disable();

    /*
     * The offscreen target, and nothing else.
     */
    GNY_World* world = gny_world();
    if (world != nullptr) nya_post_chain_destroy(&world->post);

    /*
     * The world itself is torn down by the screen change that pops this layer, in gny_world_clear,
     * not here.
     */
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON EVENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_game_on_event(NYA_Window* window, NYA_Event* event) {
    GNY_World* world = gny_world();

    switch (event->type) {
        case NYA_EVENT_MOUSE_BUTTON_DOWN: {
            NYA_MouseButtonEvent* mouse = &event->as_mouse_button_event;

            f32x2 point = gny_screen_to_world(window, (f32x2){ mouse->x, mouse->y });

            /*
             * Handled here rather than by polling in on_update.
             */
            if (mouse->button == NYA_MOUSE_BUTTON_LEFT) {
                (void)gny_entity_box_create(point, GNY_ENTITY_BOX_DEFAULT_FLAGS);
                event->was_handled = true;
            }

            /*
             * Every other button goes to whatever is under the cursor.
             */
            else {
                // crates only: without the layer this resolves to the terrain chain under the cursor
                // and the click is swallowed by a body that has no on_click.
                (void)nya_entity_click(point, mouse->button, nya_physics_layer(GNY_LAYER_CRATE));

                event->was_handled = true;
            }
        } break;

        case NYA_EVENT_MOUSE_MOVED: {
            NYA_MouseMovedEvent* moved = &event->as_mouse_moved_event;

            /*
             * Where the cursor is, every time it moves. nya_entity_hover does the rest.
             */
            (void)nya_entity_hover(gny_screen_to_world(window, (f32x2){ moved->x, moved->y }), nya_physics_layer(GNY_LAYER_CRATE));
        } break;

        /*
         * The cursor is gone, so nothing is under it.
         */
        case NYA_EVENT_WINDOW_MOUSE_LEAVE:
        case NYA_EVENT_WINDOW_FOCUS_LOST: {
            nya_entity_hover_clear();
        } break;

        case NYA_EVENT_MOUSE_WHEEL_MOVED: {
            NYA_MouseWheelEvent* wheel = &event->as_mouse_wheel_event;

            f32 factor = wheel->amount_y > 0.0F ? GNY_CAMERA_ZOOM_STEP : (1.0F / GNY_CAMERA_ZOOM_STEP);

            // the wheel is an event, not a held key, so the layer forwards it instead of the camera polling it.
            gny_entity_camera_zoom_by(factor);
            event->was_handled = true;
        } break;

        case NYA_EVENT_KEY_DOWN: {
            NYA_KeyEvent* key = &event->as_key_event;

            // Auto repeat is the keyboard's, not the player's. Without this, holding space empties
            // the entity table in about a second.
            if (key->is_repeat) break;

            /*
             * Matched against actions rather than switched on keycodes.
             */
            if (nya_input_action_matches(GNY_ACTION_SPAWN_BURST, key->key, key->modifier_flags)) {
                _gny_box_burst(24);
                event->was_handled = true;
            } else if (nya_input_action_matches(GNY_ACTION_CLEAR_BOXES, key->key, key->modifier_flags)) {
                gny_entity_box_destroy_all();
                event->was_handled = true;
            } else if (nya_input_action_matches(GNY_ACTION_REGENERATE_TERRAIN, key->key, key->modifier_flags)) {
                // The crates go first: regenerating under them would leave anything resting on
                // the old surface embedded in the new one, and the solver pushes it out hard.
                gny_entity_box_destroy_all();
                gny_terrain_generate(world->terrain_seed + 1);
                event->was_handled = true;
            } else if (nya_input_action_matches(GNY_ACTION_TOGGLE_PHYSICS, key->key, key->modifier_flags)) {
                nya_physics2d_enabled_set(!nya_physics2d_enabled());
                event->was_handled = true;
            } else if (nya_input_action_matches(GNY_ACTION_TOGGLE_BLOOM, key->key, key->modifier_flags)) {
                world->bloom_enabled = !world->bloom_enabled;
                event->was_handled   = true;
            } else if (nya_input_action_matches(GNY_ACTION_TOGGLE_GRADE, key->key, key->modifier_flags)) {
                world->grade_enabled = !world->grade_enabled;
                event->was_handled   = true;
            } else if (nya_input_action_matches(GNY_ACTION_TOGGLE_FLUID, key->key, key->modifier_flags)) {
                // the volume keeps stepping either way, so turning it back on shows the plume it
                // would have had rather than an empty grid. Only the draw is switched.
                world->fluid_enabled = !world->fluid_enabled;
                event->was_handled   = true;
            } else if (nya_input_action_matches(GNY_ACTION_DROP_THROUGH, key->key, key->modifier_flags)) {
                // every crate, on a ledge or not. The window is harmless off a ledge, and checking would mean
                // walking contacts. See gny_entity_ledge_drop_everything_through.
                (void)gny_entity_ledge_drop_everything_through(GNY_LEDGE_DROP_SECONDS);
                event->was_handled = true;
            } else if (nya_input_action_matches(GNY_ACTION_TOGGLE_MUSIC, key->key, key->modifier_flags)) {
                // Paused rather than stopped, so it resumes where it was instead of restarting
                // the track every time the key is pressed.
                if (nya_audio_music_playing()) {
                    nya_audio_pause_music();
                } else {
                    nya_audio_resume_music();
                }

                event->was_handled = true;
            }
        } break;

        default: break;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON UPDATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_game_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window);

    /*
     * The gameplay systems are not run from here any more: they are registered with the engine's
     * registry and run by it, in the same place in the tick this call used to sit. See
     * gny_systems_register_all.
     */

    /*
     * The map's animation clock.
     */
    nya_tilemap_animate(gny_world()->tilemap, delta_time_s);

    // The startup script, and its once-a-second hook. See gny_world_script_tick.
    gny_world_script_tick(delta_time_s);

    // once per tick here, not in on_render, which runs once per camera and would age them several times.
    nya_particles_update(gny_world()->sparks, delta_time_s);

    /*
     * The steam vent, for the same reason: one source and one step per tick, whatever the frame rate
     * does. The emitter takes amounts rather than rates, so the per-second constants are multiplied by
     * this tick's own step here; see nya_fluid_emit.
     */
    nya_fluid_emit(gny_world()->steam, (NYA_FluidEmitter){
                                           .position    = GNY_FLUID2D_VENT,
                                           .radius      = GNY_FLUID2D_VENT_RADIUS,
                                           .density     = GNY_FLUID2D_VENT_DENSITY * delta_time_s,
                                           .temperature = GNY_FLUID2D_VENT_TEMPERATURE * delta_time_s,
                                           .velocity    = GNY_FLUID2D_VENT_VELOCITY,
                                       });

    nya_fluid_step(gny_world()->steam, delta_time_s);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON RENDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_game_on_render(NYA_Window* window) {
    /*
     * Smoothing, before anything is drawn.
     */
    // `elapsed_ns` is the frame period, sleep included, which is the real time interpolation advances
    // by. `delta_time_s` is the fixed tick and would tie smoothing to the simulation rate.
    nya_net_client_interpolate((f32)nya_time_ns_to_s(nya_app_get()->frame_stats.elapsed_ns));

    /*
     * The window's fluid look, before anything draws through it. Set every frame rather than at
     * creation because `9` flips it and because the 3D scene sets its own: whichever layer rendered
     * last owns the window's copy, which is what makes one options struct per window enough.
     */
    nya_fluid_render_options_set(window, (NYA_FluidRenderOptions){
                                             .enabled         = gny_world()->fluid_enabled,
                                             .opacity         = GNY_FLUID2D_OPACITY,
                                             .threshold       = GNY_FLUID_DRAW_THRESHOLD,
                                             .density_full    = GNY_FLUID_DENSITY_FULL,
                                             .cool            = GNY_FLUID2D_COOL_COLOR,
                                             .hot             = GNY_FLUID2D_HOT_COLOR,
                                             .hot_temperature = GNY_FLUID_HOT_TEMPERATURE,
                                         });

    /*
     * One call, because how many cameras there are and what order they draw in is the camera
     * system's problem rather than this layer's.
     */
    gny_system_camera_render(window);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _gny_box_burst(u32 count) {
    // Spread across the width of the view and dropped from above the top of it, so they are already
    // falling by the time they come into shot.
    for (u32 i = 0; i < count; i++) {
        f32 spread = ((f32)i / (f32)nya_max(count - 1, 1U)) - 0.5F;

        NYA_Camera2DTopDown camera = gny_entity_camera_get();

        f32x2 position = {
            camera.position.x + (spread * 900.0F),
            camera.position.y - 500.0F - ((f32)i * 26.0F),
        };

        (void)gny_entity_box_create(position, GNY_ENTITY_BOX_DEFAULT_FLAGS);
    }
}





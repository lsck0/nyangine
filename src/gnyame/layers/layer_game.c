/**
 * @file layer_game.c
 *
 * The world: terrain, crates, camera and the input that drops things into it.
 *
 * Everything here is drawn in **world** coordinates, between nya_render2d_camera_set and
 * nya_render2d_camera_reset. The layers on either side of this one draw in screen pixels, so the camera
 * is set and unset within this one render call rather than left standing.
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
    gny_bloom_pipeline_ensure(window);


    // Guarded, because a hot reload re-resolves this layer's callbacks but does not re-run the
    // window's on_create — and if it ever does, regenerating the terrain under a settled pile would
    // leave every crate inside the ground.
    GNY_World* world = gny_world();
    if (nya_entity_is_valid(world->terrain)) return;

    gny_entity_camera_create((f32x2){ GNY_CAMERA_START_X, GNY_CAMERA_START_Y }, GNY_CAMERA_START_ZOOM);

    gny_terrain_generate(world->terrain_seed);

    /*
     * The Tiled map, loaded from the world's arena so it dies with the world.
     *
     * Not fatal if it fails. A map is content, and a demo that refuses to start because one asset is
     * missing is worse than a demo with no map in it — the terrain is still a floor.
     */
    NYA_Error map_error = nya_tilemap_load(world->allocator, NYA_ASSET_MAPS_DEMO_TOPDOWN_TMJ, &world->tilemap);

    if (!map_error.ok) {
        u8 message[256];
        (void)nya_error_format(&map_error, message, sizeof(message));
        nya_log_warn("Could not load the demo tilemap: %s", (NYA_CString)message);
    } else {
        // Placed before anything reads it, because the origin is what every coordinate on the map is
        // relative to — draw, collision and the conversions all go through it.
        world->tilemap->origin = GNY_TILEMAP_ORIGIN;

        // The invisible "collision" layer becomes static bodies. Merged into runs, so the map's three
        // solid rows are three wide boxes rather than sixty one-tile ones a crate could catch on.
        (void)nya_tilemap_collision_build(world->tilemap, "collision", GNY_TILEMAP_COLLIDER_KIND);
    }

    /*
     * Three one-way ledges above the terrain, the middle one patrolling.
     *
     * Not decoration: they are the only thing in the game that drives one-way surfaces, a tween-backed
     * kinematic move, or the transform hierarchy — see entity_ledge.c. Crates thrown up through them
     * pass; crates falling onto them land; `drop_through` lets go.
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

    /*
     * The offscreen target, and nothing else.
     *
     * GPU resources are the one thing this callback *can* safely release: nya_app_deinit destroys
     * the windows before it destroys the renderer, so the device is still alive here even on the
     * shutdown path. That is exactly not true of the entity system, which is why the world teardown
     * below is somebody else's job.
     */
    GNY_World* world = gny_world();
    if (world != nullptr) nya_post_chain_destroy(&world->post);

    /*
     * The world itself is torn down by the screen change that pops this layer, in gny_world_clear,
     * not here.
     *
     * on_destroy cannot do it, because it does not only run when a screen changes — nya_app_deinit
     * destroys the windows on the way out, and destroying a window runs on_destroy for every layer
     * still on it. By that point nya_system_entity_deinit has already zeroed the entity table, so
     * nya_entity_is_valid dereferences a null `occupied` array. That is a segfault at address 0x0
     * inside nya_window_destroy, on the ordinary quit path.
     *
     * The entity system clears itself on shutdown anyway, and each despawn it does destroys the
     * physics body that entity carried, so nothing is leaked by leaving this empty.
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
             *
             * on_update runs once per *fixed tick* and a slow frame runs several of them, while
             * nya_input_mouse_button_just_pressed stays true for the whole frame — so one click
             * would spawn one crate per tick that frame, which is between one and eight of them.
             * An event fires exactly once.
             */
            if (mouse->button == NYA_MOUSE_BUTTON_LEFT) {
                (void)gny_entity_box_create(point, GNY_ENTITY_BOX_DEFAULT_FLAGS);
                event->was_handled = true;
            }

            /*
             * Every other button goes to whatever is under the cursor.
             *
             * Not just the right one. This layer used to name NYA_MOUSE_BUTTON_RIGHT explicitly,
             * which meant middle click never reached an entity at all — `gny_entity_box_on_click`
             * has handled it since it was written, and the branch that would have dispatched it did
             * not exist. The symptom was a feature that looked implemented and did nothing.
             *
             * The layer has no business enumerating which buttons an entity cares about: it forwards
             * the button and the entity decides. An entity with no on_click — the terrain — simply is
             * not clickable, with no flag needed to say so.
             */
            else {
                (void)nya_entity_click(point, mouse->button);

                event->was_handled = true;
            }
        } break;

        case NYA_EVENT_MOUSE_MOVED: {
            NYA_MouseMovedEvent* moved = &event->as_mouse_moved_event;

            /*
             * Where the cursor is, every time it moves. nya_entity_hover does the rest.
             *
             * On the event rather than in on_update, for a different reason than the click above: a
             * click must not repeat, while a hover is idempotent and would survive being called per
             * tick. What it would not survive is *not* being called — the cursor moving while the
             * simulation is paused still has to update the highlight, and on_update does not run then.
             *
             * Deliberately not marked handled. Hovering is an observation, not a consumption: a layer
             * underneath is still entitled to see the pointer move.
             */
            (void)nya_entity_hover(gny_screen_to_world(window, (f32x2){ moved->x, moved->y }));
        } break;

        /*
         * The cursor is gone, so nothing is under it.
         *
         * Both events, because they are different departures: the pointer can leave the window while
         * the window keeps focus, and the window can lose focus with the pointer still inside it — the
         * second is what alt-tabbing away does. Without both, whatever was hovered at that moment keeps
         * its highlight until the cursor comes back and moves.
         */
        case NYA_EVENT_WINDOW_MOUSE_LEAVE:
        case NYA_EVENT_WINDOW_FOCUS_LOST: {
            nya_entity_hover_clear();
        } break;

        case NYA_EVENT_MOUSE_WHEEL_MOVED: {
            NYA_MouseWheelEvent* wheel = &event->as_mouse_wheel_event;

            f32 factor = wheel->amount_y > 0.0F ? GNY_CAMERA_ZOOM_STEP : (1.0F / GNY_CAMERA_ZOOM_STEP);

            // The wheel is an event rather than a held key, so it cannot be polled in the camera's
            // own update the way panning is — the layer forwards it instead of owning the zoom.
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
             *
             * Still an event and not a poll, for the reason above — nya_input_action_matches is the
             * event-shaped question, nya_input_action_pressed is the held-key one. What changes is
             * only that the keys are the player's to rebind and are written into the settings file
             * by name. See actions.h.
             *
             * An if-chain rather than a switch because a switch needs constants and an action's key
             * is a runtime value; the chain stops at the first match either way.
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
            } else if (nya_input_action_matches(GNY_ACTION_DROP_THROUGH, key->key, key->modifier_flags)) {
                // Every crate, whether or not it is on a ledge — the window is harmless on one that is
                // not, and asking which are would mean walking contacts. See
                // gny_entity_ledge_drop_everything_through.
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
     * The per-frame systems, in the order gny_systems_register_all gave them.
     *
     * That order is still a decision made out loud, just read here instead of written here — see
     * systems.h for where it is now spelled out, and gny_world_create for where it is finalized.
     *
     * Everything else that used to be here has moved to where it belongs: panning and following to
     * the movement systems, the listener to the camera's own update, impacts to the crate's
     * on_collision.
     */
    nya_system_registry_run_update(delta_time_s);

    /*
     * The map's animation clock.
     *
     * The whole of what animated tiles cost per frame: nothing is stored per cell, and drawing
     * resolves each tile's frame from this — so a map with none is a single add and a compare. In
     * on_update rather than on_render for the same reason the particles are: several cameras draw the
     * world, and advancing a clock in the draw ages it once per camera.
     */
    nya_tilemap_animate(gny_world()->tilemap, delta_time_s);

    // The startup script, and its once-a-second hook. See gny_world_script_tick.
    gny_world_script_tick(delta_time_s);

    // Once a tick, here rather than in on_render — drawing can happen more than once a frame with
    // several cameras, and advancing them there would age them once per camera.
    nya_particles_update(gny_world()->sparks, delta_time_s);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON RENDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_game_on_render(NYA_Window* window) {
    /*
     * Smoothing, before anything is drawn.
     *
     * Snapshots arrive at the snapshot rate; this runs per frame. Without it every remote player steps —
     * still for a few frames, jump, still, jump — which is the most visible artefact a networked game
     * can have and is visible at any latency including none. See nya_net_replica_interpolate.
     *
     * In on_render rather than on_update because the whole point is to fill the gaps *between* fixed
     * ticks, and on_update runs at the fixed rate. Does nothing in single player or on a listen server,
     * where there are no snapshots being applied.
     */
    // `elapsed_ns` is the frame *period* — start to start, sleep included — which is exactly the real
    // time interpolation has to advance by. `delta_time_s` is the fixed tick and would make smoothing
    // run at the simulation rate, which is the thing it exists to decouple from.
    nya_net_client_interpolate((f32)nya_time_ns_to_s(nya_app_get()->frame_stats.elapsed_ns));

    /*
     * One call, because how many cameras there are and what order they draw in is the camera
     * system's problem rather than this layer's.
     *
     * It sets and resets a camera per pass, so this layer opens none of its own — the HUD above it
     * still gets screen space, exactly as it did when there was one camera and this set it here.
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





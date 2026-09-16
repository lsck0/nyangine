/**
 * @file layer_game.c
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
                (void)nya_entity_click(point, mouse->button);

                event->was_handled = true;
            }
        } break;

        case NYA_EVENT_MOUSE_MOVED: {
            NYA_MouseMovedEvent* moved = &event->as_mouse_moved_event;

            /*
             * Where the cursor is, every time it moves. nya_entity_hover does the rest.
             */
            (void)nya_entity_hover(gny_screen_to_world(window, (f32x2){ moved->x, moved->y }));
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
     */
    nya_system_registry_run_update(delta_time_s);

    /*
     * The map's animation clock.
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
     */
    // `elapsed_ns` is the frame *period* — start to start, sleep included — which is exactly the real
    // time interpolation has to advance by. `delta_time_s` is the fixed tick and would make smoothing
    // run at the simulation rate, which is the thing it exists to decouple from.
    nya_net_client_interpolate((f32)nya_time_ns_to_s(nya_app_get()->frame_stats.elapsed_ns));

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





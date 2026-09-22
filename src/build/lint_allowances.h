/**
 * @file lint_allowances.h
 *
 * What each rule in lint.c knowingly lets through today, and why. Every entry is debt with a reason, reviewed by
 * hand, and none of them may grow: a new violation fails the check, and so does an entry that is no longer needed,
 * so the list only ever shrinks. Included by lint.c alone.
 * */
#pragma once

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYERING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Include edges against the module order, with the number of `#include` lines making each today. All of them are
 * Phase 1's to remove: base stops including math and platform, net splits into a transport below the app loop and
 * replication above it, http_metrics and nn's drawing move out, and core stops reaching up into what it drives.
 * */
NYA_INTERNAL const _LintEdge _LINT_LAYERING_ALLOWED[] = {
    { "base",     "math",     5 },
    { "base",     "platform", 4 },
    { "net",      "core",     4 },
    { "http",     "core",     5 },
    { "core",     "renderer", 7 },
    { "core",     "physics",  5 },
    { "core",     "ui",       1 },
    { "nn",       "renderer", 2 },
    { "renderer", "debug",    3 },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * VERB PAIRS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Public verbs with no partner in their header when the rule landed, grouped by why. */
NYA_INTERNAL const _LintAllowed _LINT_VERB_PAIRS_ALLOWED[] = {
    { "nya_audio_voice_stop", "started by a verb other than start (play, rumble, move_to); rename or pair" },
    { "nya_entity_move_stop", "started by a verb other than start (play, rumble, move_to); rename or pair" },
    { "nya_gamepad_rumble_stop", "started by a verb other than start (play, rumble, move_to); rename or pair" },
    { "nya_integrity_start", "started by a verb other than start (play, rumble, move_to); rename or pair" },
    { "nya_skeleton_player_layer_stop", "started by a verb other than start (play, rumble, move_to); rename or pair" },
    { "nya_sprite_animator_stop", "started by a verb other than start (play, rumble, move_to); rename or pair" },
    { "nya_blend_tree_init", "fills the caller's struct and holds nothing; the style still wants an empty deinit" },
    { "nya_skeleton_inertializer_init", "fills the caller's struct and holds nothing; the style still wants an empty deinit" },
    { "nya_skeleton_player_init", "fills the caller's struct and holds nothing; the style still wants an empty deinit" },
    { "nya_cache_remove", "inserted with _set, so the pair is set/remove, which the vocabulary does not list" },
    { "nya_dict_remove", "inserted with _set, so the pair is set/remove, which the vocabulary does not list" },
    { "nya_hmap_remove", "inserted with _set, so the pair is set/remove, which the vocabulary does not list" },
    { "nya_host_environment_remove", "inserted with _set, so the pair is set/remove, which the vocabulary does not list" },
    { "nya_hset_remove", "inserted with _set, so the pair is set/remove, which the vocabulary does not list" },
    { "nya_object_remove", "inserted with _set, so the pair is set/remove, which the vocabulary does not list" },
    { "nya_string_remove", "inserted with _set, so the pair is set/remove, which the vocabulary does not list" },
    { "nya_command_destroy", "made as a struct literal, then run; there is no create" },
    { "nya_gpu_buffer_create", "created and released: the pair is create/release, which the vocabulary does not list" },
    { "nya_gpu_buffer_release", "created and released: the pair is create/release, which the vocabulary does not list" },
    { "nya_gpu_texture_create", "created and released: the pair is create/release, which the vocabulary does not list" },
    { "nya_gpu_texture_release", "created and released: the pair is create/release, which the vocabulary does not list" },
    { "nya_gpu_transfer_buffer_create", "created and released: the pair is create/release, which the vocabulary does not list" },
    { "nya_gpu_transfer_buffer_release", "created and released: the pair is create/release, which the vocabulary does not list" },
    { "nya_input_source_release", "created and released: the pair is create/release, which the vocabulary does not list" },
    { "nya_memory_release", "created and released: the pair is create/release, which the vocabulary does not list" },
    { "nya_render3d_mesh_release", "created and released: the pair is create/release, which the vocabulary does not list" },
    { "nya_terrain2d_release", "created and released: the pair is create/release, which the vocabulary does not list" },
    { "nya_terrain3d_release", "created and released: the pair is create/release, which the vocabulary does not list" },
    { "nya_http_challenge_create", "a value: nothing is held, so there is nothing to destroy" },
    { "nya_matrix_create", "a value: nothing is held, so there is nothing to destroy" },
    { "nya_net_key_pair_create", "a value: nothing is held, so there is nothing to destroy" },
    { "nya_noise_create", "a value: nothing is held, so there is nothing to destroy" },
    { "nya_quaternion_create", "a value: nothing is held, so there is nothing to destroy" },
    { "nya_rng_create", "a value: nothing is held, so there is nothing to destroy" },
    { "nya_uuid_v4_create", "a value: nothing is held, so there is nothing to destroy" },
    { "nya_uuid_v7_create", "a value: nothing is held, so there is nothing to destroy" },
    { "nya_log_directory_open", "opens something the platform closes (an overlay, a socket's peer) or reopens with nothing to close" },
    { "nya_social_invite_open", "opens something the platform closes (an overlay, a socket's peer) or reopens with nothing to close" },
    { "nya_steam_lobby_invite_open", "opens something the platform closes (an overlay, a socket's peer) or reopens with nothing to close" },
    { "nya_steam_p2p_close", "opens something the platform closes (an overlay, a socket's peer) or reopens with nothing to close" },
    { "nya_websocket_close", "opens something the platform closes (an overlay, a socket's peer) or reopens with nothing to close" },
    { "nya_nav_flow_create", "lives in the caller's arena and goes with it; the style still wants an empty destroy" },
    { "nya_nav_grid_create", "lives in the caller's arena and goes with it; the style still wants an empty destroy" },
    { "nya_nn_dqn_create", "lives in the caller's arena and goes with it; the style still wants an empty destroy" },
    { "nya_nn_graph_create", "lives in the caller's arena and goes with it; the style still wants an empty destroy" },
    { "nya_nn_neat_network_create", "lives in the caller's arena and goes with it; the style still wants an empty destroy" },
    { "nya_nn_sequential_create", "lives in the caller's arena and goes with it; the style still wants an empty destroy" },
    { "nya_nn_tensor_create", "lives in the caller's arena and goes with it; the style still wants an empty destroy" },
    { "nya_particles_create", "lives in the caller's arena and goes with it; the style still wants an empty destroy" },
    { "nya_save_database_open", "lives in the caller's arena and goes with it; the style still wants an empty destroy" },
    { "nya_terrain2d_create", "lives in the caller's arena and goes with it; the style still wants an empty destroy" },
    { "nya_terrain3d_create", "lives in the caller's arena and goes with it; the style still wants an empty destroy" },
    { "nya_net_client_attach", "registered for the life of the run; nothing removes one yet" },
    { "nya_nn_optimizer_add", "registered for the life of the run; nothing removes one yet" },
    { "nya_nn_sequential_push", "registered for the life of the run; nothing removes one yet" },
    { "nya_render3d_point_light_add", "registered for the life of the run; nothing removes one yet" },
    { "nya_session_action_add", "registered for the life of the run; nothing removes one yet" },
    { "nya_sim_observer_add", "registered for the life of the run; nothing removes one yet" },
    { "nya_simulation_action_add", "registered for the life of the run; nothing removes one yet" },
    { "nya_simulation_actions_reflect_add", "registered for the life of the run; nothing removes one yet" },
    { "nya_simulation_check_add", "registered for the life of the run; nothing removes one yet" },
    { "nya_simulation_fault_add", "registered for the life of the run; nothing removes one yet" },
    { "nya_net_message_begin", "half of a frame or tick bracket whose other half is implicit in the loop; pair it or rename it" },
    { "nya_occlusion_begin", "half of a frame or tick bracket whose other half is implicit in the loop; pair it or rename it" },
    { "nya_perf_frame_begin", "half of a frame or tick bracket whose other half is implicit in the loop; pair it or rename it" },
    { "nya_render_output_scene_end", "half of a frame or tick bracket whose other half is implicit in the loop; pair it or rename it" },
    { "nya_system_accounting_frame_end", "half of a frame or tick bracket whose other half is implicit in the loop; pair it or rename it" },
    { "nya_system_gamepad_frame_begin", "half of a frame or tick bracket whose other half is implicit in the loop; pair it or rename it" },
    { "nya_system_gamepad_tick_end", "half of a frame or tick bracket whose other half is implicit in the loop; pair it or rename it" },
    { "nya_trace_frame_end", "half of a frame or tick bracket whose other half is implicit in the loop; pair it or rename it" },
    { "nya_net_transport_destroy", "one destroy for every kind of transport, nya_net_transport_destroy" },
    { "nya_net_transport_loopback_create", "one destroy for every kind of transport, nya_net_transport_destroy" },
    { "nya_net_transport_steam_create", "one destroy for every kind of transport, nya_net_transport_destroy" },
    { "nya_net_transport_udp_create", "one destroy for every kind of transport, nya_net_transport_destroy" },
    { "nya_nn_add", "arithmetic, not the add/remove verb" },
    { "nya_quaternion_add", "arithmetic, not the add/remove verb" },
    { "nya_offsetof_end", "a noun, the offset where a field ends" },
    { "nya_post_chain_destroy", "created during another subsystem's init; the style wants the pair beside it" },
    { "nya_text_run_cache_destroy", "created during another subsystem's init; the style wants the pair beside it" },
    { "nya_sql_transaction_begin", "ends by itself or by another verb (a capture stops after its frames, a transaction commits or rolls back)" },
    { "nya_trace_capture_begin", "ends by itself or by another verb (a capture stops after its frames, a transaction commits or rolls back)" },
    { "nya_steam_lobby_create", "left rather than destroyed: nya_steam_lobby_leave" },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CALLERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Public functions nothing calls, found by the first run of the rule and grouped by header. Stricter than the audit
 * TODO.md recorded, which matched names as text and so counted a doc comment or a string naming a function as a
 * call. Each is Phase 0's to decide per cluster: a caller, a test, or deletion.
 * */
NYA_INTERNAL const _LintAllowed _LINT_CALLERS_ALLOWED[] = {
    // base_arena.h
    { "nya_arena_actions_set_callback", "no caller when the rule landed" },
    // base_build.h
    { "nya_build_last_failure", "no caller when the rule landed" },
    // base_diagnostics.h
    { "nya_crash_observer_clear", "no caller when the rule landed" },
    // base_object.h
    { "nya_object_destroy", "no caller when the rule landed" },
    // base_string.h
    { "nya_string_create_with_capacity_on_stack", "no caller when the rule landed" },
    { "nya_string_println", "no caller when the rule landed" },
    // core_asset.h
    { "nya_asset_blob_at", "no caller when the rule landed" },
    { "nya_asset_blob_count", "no caller when the rule landed" },
    { "nya_asset_blob_find", "no caller when the rule landed" },
    { "nya_asset_enumerate", "no caller when the rule landed" },
    // core_audio.h
    { "nya_audio_voice_filter_set", "no caller when the rule landed" },
    // core_control.h
    { "nya_control_expose_event", "no caller when the rule landed" },
    { "nya_control_hide_event", "no caller when the rule landed" },
    // core_entity.h
    { "nya_entity_query_ray", "no caller when the rule landed" },
    { "nya_entity_world_matrix", "no caller when the rule landed" },
    // core_event.h
    { "nya_event_hook_register_once", "no caller when the rule landed" },
    // core_i18n.h
    { "nya_i18n_load_bytes", "no caller when the rule landed" },
    // core_input.h
    { "nya_clipboard_has_text", "no caller when the rule landed" },
    { "nya_input_text_active", "no caller when the rule landed" },
    { "nya_input_text_composition_range", "no caller when the rule landed" },
    // core_nav.h
    { "nya_nav_grid_from_tilemap", "no caller when the rule landed" },
    // core_skeleton_blend.h
    { "nya_blend_tree_evaluate", "no caller when the rule landed" },
    // core_skeleton.h
    { "nya_skeleton_clip", "no caller when the rule landed" },
    // core_social.h
    { "nya_social_user_name", "no caller when the rule landed" },
    // core_system.h
    { "nya_system_registry_is_running", "no caller when the rule landed" },
    // http_message.h
    { "nya_http_response_json", "no caller when the rule landed" },
    // nn_dqn.h
    { "nya_nn_dqn_network", "no caller when the rule landed" },
    // nn_neat.h
    { "nya_nn_neat_step_for", "no caller when the rule landed" },
    // nn_tensor.h
    { "nya_nn_tensor_copy", "no caller when the rule landed" },
    // physics3d.h
    { "nya_physics3d_last_step_time_s", "no caller when the rule landed" },
    { "nya_physics3d_units_per_meter_set", "no caller when the rule landed" },
    { "nya_physics3d_wake", "no caller when the rule landed" },
    // lua.h
    { "nya_lua_nil", "no caller when the rule landed" },
    // render2d_sprite.h
    { "nya_sprite_from_atlas", "no caller when the rule landed" },
    // render2d_terminal.h
    { "nya_render2d_terminal_glyph", "no caller when the rule landed" },
    { "nya_render2d_terminal_image", "no caller when the rule landed" },
    // render3d.h
    { "nya_render3d_depth", "no caller when the rule landed" },
    { "nya_render3d_depth_set", "no caller when the rule landed" },
    // renderer.h
    { "nya_render_clear_color", "no caller when the rule landed" },
    { "nya_render_clear_color_set", "no caller when the rule landed" },
    // render_fluid.h
    { "nya_fluid_render_options", "no caller when the rule landed" },
    { "nya_fluid_step_time_s", "no caller when the rule landed" },
    { "nya_fluid_temperature_at", "no caller when the rule landed" },
    // render_particles.h
    { "nya_particles_casts_shadow_set", "no caller when the rule landed" },
    // testing_property.h
    { "nya_property_draw_f32_any", "no caller when the rule landed" },
};

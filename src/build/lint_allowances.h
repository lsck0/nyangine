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
 * Include edges against the module order, with the number of `#include` lines making each today. What is left is
 * Phase 1's to remove: core stops reaching up into what it drives. The rest of the phase has landed — base stopped
 * including math and platform, net split into a transport below the app loop and `replicate` above it, and
 * http_metrics and nn's drawing moved out to beside debug.
 * */
NYA_INTERNAL const _LintEdge _LINT_LAYERING_ALLOWED[] = {
    { "core",     "renderer", 7 },
    { "core",     "physics",  5 },
    { "core",     "ui",       1 },
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
    { "nya_reconnect_init", "fills the caller's struct and holds nothing; the style still wants an empty deinit" },
    { "nya_skeleton_inertializer_init", "fills the caller's struct and holds nothing; the style still wants an empty deinit" },
    { "nya_supervisor_init", "fills the caller's struct and holds nothing; the style still wants an empty deinit" },
    { "nya_skeleton_player_init", "fills the caller's struct and holds nothing; the style still wants an empty deinit" },
    { "nya_string_remove", "drops a substring wherever it occurs; there is no key to have added it at" },
    { "nya_command_destroy", "made as a struct literal, then run; there is no create" },
    { "nya_gpu_buffer_create", "create/release, AGENTS.md's pair for a resource owned outside the arena; create/destroy and acquire/release each still want their own half by name" },
    { "nya_gpu_buffer_release", "create/release, AGENTS.md's pair for a resource owned outside the arena; create/destroy and acquire/release each still want their own half by name" },
    { "nya_gpu_texture_create", "create/release, AGENTS.md's pair for a resource owned outside the arena; create/destroy and acquire/release each still want their own half by name" },
    { "nya_gpu_texture_release", "create/release, AGENTS.md's pair for a resource owned outside the arena; create/destroy and acquire/release each still want their own half by name" },
    { "nya_gpu_transfer_buffer_create", "create/release, AGENTS.md's pair for a resource owned outside the arena; create/destroy and acquire/release each still want their own half by name" },
    { "nya_gpu_transfer_buffer_release", "create/release, AGENTS.md's pair for a resource owned outside the arena; create/destroy and acquire/release each still want their own half by name" },
    { "nya_input_source_release", "create/release, AGENTS.md's pair for a resource owned outside the arena; create/destroy and acquire/release each still want their own half by name" },
    { "nya_os_page_release", "create/release, AGENTS.md's pair for a resource owned outside the arena; create/destroy and acquire/release each still want their own half by name" },
    { "nya_render3d_mesh_release", "create/release, AGENTS.md's pair for a resource owned outside the arena; create/destroy and acquire/release each still want their own half by name" },
    { "nya_terrain2d_release", "create/release, AGENTS.md's pair for a resource owned outside the arena; create/destroy and acquire/release each still want their own half by name" },
    { "nya_terrain3d_release", "create/release, AGENTS.md's pair for a resource owned outside the arena; create/destroy and acquire/release each still want their own half by name" },
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
    { "nya_nav_flow_create", "lives in the caller's arena and goes with it; AGENTS.md's decision is that this needs no destroy" },
    { "nya_nav_grid_create", "lives in the caller's arena and goes with it; AGENTS.md's decision is that this needs no destroy" },
    { "nya_nn_dqn_create", "lives in the caller's arena and goes with it; AGENTS.md's decision is that this needs no destroy" },
    { "nya_nn_graph_create", "lives in the caller's arena and goes with it; AGENTS.md's decision is that this needs no destroy" },
    { "nya_nn_neat_network_create", "lives in the caller's arena and goes with it; AGENTS.md's decision is that this needs no destroy" },
    { "nya_nn_sequential_create", "lives in the caller's arena and goes with it; AGENTS.md's decision is that this needs no destroy" },
    { "nya_nn_tensor_create", "lives in the caller's arena and goes with it; AGENTS.md's decision is that this needs no destroy" },
    { "nya_particles_create", "lives in the caller's arena and goes with it; AGENTS.md's decision is that this needs no destroy" },
    { "nya_save_database_open", "lives in the caller's arena and goes with it; AGENTS.md's decision is that this needs no destroy" },
    { "nya_terrain2d_create", "lives in the caller's arena and goes with it; AGENTS.md's decision is that this needs no destroy" },
    { "nya_terrain3d_create", "lives in the caller's arena and goes with it; AGENTS.md's decision is that this needs no destroy" },
    { "nya_weather_create", "lives in the caller's arena and goes with it; AGENTS.md's decision is that this needs no destroy" },
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
    { "nya_net_message_begin", "named for when in the frame or tick it runs, not for a bracket it closes; AGENTS.md excuses it from begin/end" },
    { "nya_occlusion_begin", "named for when in the frame or tick it runs, not for a bracket it closes; AGENTS.md excuses it from begin/end" },
    { "nya_perf_frame_begin", "named for when in the frame or tick it runs, not for a bracket it closes; AGENTS.md excuses it from begin/end" },
    { "nya_render_output_scene_end", "named for when in the frame or tick it runs, not for a bracket it closes; AGENTS.md excuses it from begin/end" },
    { "nya_system_accounting_frame_end", "named for when in the frame or tick it runs, not for a bracket it closes; AGENTS.md excuses it from begin/end" },
    { "nya_system_gamepad_frame_begin", "named for when in the frame or tick it runs, not for a bracket it closes; AGENTS.md excuses it from begin/end" },
    { "nya_system_gamepad_tick_end", "named for when in the frame or tick it runs, not for a bracket it closes; AGENTS.md excuses it from begin/end" },
    { "nya_trace_frame_end", "named for when in the frame or tick it runs, not for a bracket it closes; AGENTS.md excuses it from begin/end" },
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
 * Public functions nothing calls. Empty: the 81 the first run found were each given a caller or a test, or
 * deleted. An entry belongs here only for surface kept on purpose, with that purpose as its reason.
 * */
NYA_INTERNAL const _LintAllowed _LINT_CALLERS_ALLOWED[] = {
};

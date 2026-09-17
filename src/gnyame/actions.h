/**
 * @file actions.h
 *
 * ```c
 * // held, so polled from a system's update
 * if (nya_input_action_pressed(GNY_ACTION_MOVE_LEFT)) direction.x -= 1.0F;
 *
 * // discrete, so matched against the event that carried it
 * if (nya_input_action_matches(GNY_ACTION_SPAWN_BURST, key->key, key->modifier_flags)) { ... }
 * ```
 *
 * Movement and the menu actions also carry a d-pad button and a left stick direction, so the game plays
 * with a gamepad. Held actions read the pad through nya_input_action_pressed with no extra code.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The game's own actions. Continues the engine's numbering from NYA_INPUT_ACTION_USER.
 * */
enum {
    GNY_ACTION_MOVE_LEFT = NYA_INPUT_ACTION_USER,
    GNY_ACTION_MOVE_RIGHT,
    GNY_ACTION_MOVE_UP,
    GNY_ACTION_MOVE_DOWN,

    GNY_ACTION_SPAWN_BURST,
    GNY_ACTION_CLEAR_BOXES,
    GNY_ACTION_REGENERATE_TERRAIN,
    GNY_ACTION_TOGGLE_PHYSICS,
    GNY_ACTION_TOGGLE_BLOOM,
    GNY_ACTION_TOGGLE_MUSIC,
    GNY_ACTION_TOGGLE_OVERLAY,

    /** Opens a drop-through window on every crate, so anything on a one-way ledge falls off it. */
    GNY_ACTION_DROP_THROUGH,

    /** Stops and restarts the 3D scene's skinned animation clock. */
    GNY_ACTION_FREEZE_ANIMATION,

    GNY_ACTION_COUNT,
};

static_assert(GNY_ACTION_COUNT <= NYA_INPUT_ACTION_MAX, "the game has more actions than the binding table has room for");

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Names every action and binds the defaults, then loads the player's settings over the top.
 * */
void gny_actions_init(void);

/** Writes the settings back out, bindings and volumes together. Called from gnyame_deinit. */
void gny_actions_deinit(void);

/**
 * Whether a gamepad binding of `action` is held, ignoring its keys.
 *
 * Keys reach a menu as events, which a pad does not send, so a menu polls this once a tick and keeps the
 * previous answer to find the press. See gny_menu_update.
 * */
b8 gny_action_pad_held(NYA_InputAction action);

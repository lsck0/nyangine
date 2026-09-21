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

    /** The overlay's pages and the trace table's order, a log of the table, and a capture to disk. See debug_trace.h. */
    GNY_ACTION_CYCLE_OVERLAY_PAGE,
    GNY_ACTION_CYCLE_TRACE_SORT,
    GNY_ACTION_TRACE_REPORT,
    GNY_ACTION_TRACE_CAPTURE,

    /**
     * The overlay's systems page: move the cursor down the list, and switch the marked system off or
     * back on. Disabling `physics2d` is a freeze frame with everything else still running; disabling
     * `player_input` takes the keys away without touching the camera. See core_system.h.
     * */
    GNY_ACTION_SELECT_SYSTEM,
    GNY_ACTION_TOGGLE_SYSTEM,

    /*
     * The 3D demo's render features, on the number row. They flip NYA_CONFIG.engine.renderer, so the config file sets
     * where they start.
     */
    GNY_ACTION_TOGGLE_INK,
    GNY_ACTION_TOGGLE_OCCLUSION,
    GNY_ACTION_TOGGLE_ANTIALIAS,

    /** The colour grade, which the 2D world shares. */
    GNY_ACTION_TOGGLE_GRADE,
    GNY_ACTION_CYCLE_DEBUG_VIEW,
    GNY_ACTION_CYCLE_FOCUS,
    GNY_ACTION_TOGGLE_SPEED_LINES,
    GNY_ACTION_TOGGLE_DECALS,
    GNY_ACTION_TOGGLE_HDR,

    /** The fluid volume in whichever scene is up. Off costs nothing; see nya_fluid_render_options_set. */
    GNY_ACTION_TOGGLE_FLUID,

    /** Opens a drop-through window on every crate, so anything on a one-way ledge falls off it. */
    GNY_ACTION_DROP_THROUGH,

    /** Stops and restarts the 3D scene's skinned animation clock. */
    GNY_ACTION_FREEZE_ANIMATION,

    /**
     * Fails an assertion on purpose, so the crash reporter can be looked at without waiting for a real
     * bug. Bound to a key nothing else in the demo uses and needing a modifier, since it ends the process.
     * */
    GNY_ACTION_TEST_CRASH,

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

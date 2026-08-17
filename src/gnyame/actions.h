/**
 * @file actions.h
 *
 * What the player can ask for, named once, so nothing else spells a keycode.
 *
 * Every key in the game goes through here. A layer asks whether GNY_ACTION_SPAWN_BURST happened
 * rather than whether space was pressed, which buys three things at once: the keys are rebindable,
 * the bindings are persisted by name in the settings file (see core_settings.h), and the list of
 * what the game responds to is one screenful rather than scattered across five files.
 *
 * ```c
 * // held, so polled from a system's update
 * if (nya_input_action_pressed(GNY_ACTION_MOVE_LEFT)) direction.x -= 1.0F;
 *
 * // discrete, so matched against the event that carried it
 * if (nya_input_action_matches(GNY_ACTION_SPAWN_BURST, key->key, key->modifier_flags)) { ... }
 * ```
 *
 * ## Two halves, and the seam between them
 *
 * NYA_INPUT_ACTION_CONFIRM, CANCEL, PAUSE, UP, DOWN, LEFT and RIGHT are the engine's, and the menus
 * use those rather than declaring their own — a game that renamed "confirm" would find the engine's
 * defaults and the file's keys disagreeing about which action a rebind applied to.
 *
 * Everything below NYA_INPUT_ACTION_USER is the engine's and everything at or above it is the
 * game's. The gap is deliberate: adding an engine action later must not renumber a game's, because
 * bindings are written to disk. They are written *by name*, so a renumber would be survivable —
 * but the name only exists because gny_actions_init registered it, and an action nobody named is
 * not persisted at all.
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
 *
 * Movement is here rather than reusing the engine's UP/DOWN/LEFT/RIGHT because those are the menu's:
 * a player who rebinds "walk left" has not asked for the menu cursor to move with it, and sharing
 * one action would make that impossible to express.
 *
 * Anonymous, so its constants are plain ints and assign to an NYA_InputAction without a cast. A named
 * enum would make every call site an enum-to-enum conversion, which -Wimplicit-enum-enum-cast reports
 * — correctly, since the two really are different types describing one numbering. This is the
 * spelling core_input.h's own example uses, and the reason it uses it.
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
    GNY_ACTION_TOGGLE_TRACE,

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
 *
 * Order matters and is the reason this is one function rather than two: a settings file addresses
 * bindings by action name, so the names have to exist before the file is read, and the defaults have
 * to be in place before it so that an action the file does not mention keeps one.
 *
 * Called from gnyame_init, after nya_app_init and before the window — a layer's on_create is
 * entitled to ask what a key is bound to.
 * */
void gny_actions_init(void);

/** Writes the settings back out, bindings and volumes together. Called from gnyame_deinit. */
void gny_actions_deinit(void);

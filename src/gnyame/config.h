/**
 * @file config.h
 *
 * ```c
 * f32 speed    = NYA_CONFIG.game.player_speed;
 * u32 cascades = nya_config_engine()->renderer.shadow_cascades;
 * ```
 *
 * `engine.nya` has two objects at its top level, "engine" and "game", and each is owned by whoever
 * reads it: the engine's own half lives in the engine (core_config.h's nya_config_engine) and needs
 * nothing from this file. NYA_CONFIG here is the game's own half only, GNY_ConfigGame, and it is still a
 * global in this DLL: a code reload zeroes it and unmaps what its watch points at, which is what
 * gny_config_attach is still for. gnyame_run calls it once after every reload, reloading the file into
 * the new global and repointing the watch — the engine half needs no equivalent, since a code reload
 * never touches the engine that hosts this DLL.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The same file nya_config_engine's half was loaded from; see NYA_CONFIG_ENGINE_FILE in core_config.h,
 *  which this names rather than duplicates. */
#define GNY_CONFIG_FILE NYA_CONFIG_ENGINE_FILE

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct GNY_ConfigRobots GNY_ConfigRobots;
typedef struct GNY_ConfigGame   GNY_ConfigGame;
typedef struct GNY_Config       GNY_Config;

/**
 * The learning drones in the 2D scene. See robots.h. Off unless the file turns them on.
 * */
// @reflect
struct GNY_ConfigRobots {
    /** Drones, training and nav. Turning it off waits for the running job, saves, and frees everything. */
    b8 enabled;

    /** Genomes per NEAT generation. Read when training starts. Zero falls back to GNY_ROBOT_POPULATION. */
    u32 population;

    /** NEAT generations trained per second on a job. Zero falls back to GNY_ROBOT_GENERATIONS_PER_SECOND. */
    f32 generations_per_second;

    /** DQN gradient steps per second on the same job. Zero falls back to GNY_ROBOT_DQN_STEPS_PER_SECOND. */
    f32 dqn_steps_per_second;

    /** Draw the best genome under the HUD. */
    b8 show_brain;
};

/**
 * Gameplay tunables worth reaching without a rebuild. Mirrors constants.h's own GNY_PLAYER_* defaults,
 * which is what a field left out of the config file still behaves as.
 * */
// @reflect
struct GNY_ConfigGame {
    /** World units per second a networked player moves. Zero falls back to GNY_PLAYER_SPEED. */
    f32 player_speed;

    /** How far apart players spawn, so two joining at once do not overlap. Zero falls back to GNY_PLAYER_SPAWN_SPACING. */
    f32 player_spawn_spacing;

    /** Multiplies the demos' skeleton and sprite animation clocks. Zero falls back to GNY_ANIMATION_SPEED. */
    f32 animation_speed;

    /** The texture the pause menu's skins are cut from, by the regions in `engine.ui`. Empty draws the menu flat. */
    char menu_sheet[NYA_UI_SKIN_TEXTURE_MAX];

    GNY_ConfigRobots robots;
};

/**
 * gnyame's own half of `engine.nya`'s top level: the "game" object, and nothing of "engine", which
 * nya_config_engine owns instead. One field rather than GNY_ConfigGame itself, for the same reason
 * NYA_ConfigDocument in core_config.h is: nya_config_load wants "game" to be a field name it can match,
 * not a subtree the caller has already cut out.
 * */
// @reflect
struct GNY_Config {
    GNY_ConfigGame game;
};

/**
 * The game's own single instance: `NYA_CONFIG.game.player_speed`. See nya_config_engine for the
 * engine's own settings, which do not live here.
 * */
extern GNY_Config NYA_CONFIG;

/** Loads GNY_CONFIG_FILE into NYA_CONFIG and watches it. Called again after a code reload. */
void gny_config_attach(void);

/**
 * Hands the renderer knobs in NYA_CONFIG to `window`, then lays the player's graphics settings over them. Cheap, so a
 * scene calls it every frame, after setting its own, and edits show live.
 * */
void gny_config_renderer_apply(NYA_Window* window);

/** Hands each bus its effects from NYA_CONFIG. Cheap when nothing changed, so it runs every tick under every screen. */
void gny_config_audio_apply(void);

/**
 * nya_ui_begin with the style in NYA_CONFIG, so every menu and HUD pass follows edits to the file, and with the
 * window's presenter chosen: the shape one normally, the recording one while `gny_ui_record` is on.
 *
 * Paired with gny_ui_end, which is what writes a recorded pass out; a layer calls neither `nya_ui_begin` nor
 * `nya_ui_end` itself.
 * */
NYA_UI* gny_ui_begin(NYA_Window* window, NYA_UIPass pass);
void    gny_ui_end(NYA_Window* window, NYA_UI* ui);

/**
 * Sends the UI's draw passes to the recording presenter and logs each one, or stops. The same `nya_ui_*` calls, a
 * different backend: what proves the presenter seam from inside a running program rather than from a test.
 * */
void gny_ui_record_toggle(void);

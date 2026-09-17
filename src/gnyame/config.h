/**
 * @file config.h
 *
 * ```c
 * f32 speed = NYA_CONFIG.game.player_speed;
 * u32 cascades = NYA_CONFIG.engine.renderer.shadow_cascades;
 * ```
 *
 * NYA_CONFIG is a global in this DLL, so a code reload zeroes it and unmaps what the config watch
 * points at. gnyame_run then calls gny_config_attach, which reloads the file into the new global and
 * repoints the watch.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where the runtime config lives, an asset path exactly like a locale's. See core_i18n.h's own
 *  NYA_I18N_ASSET_DIRECTORY for the same reasoning: this is also what the file is registered under. */
#define GNY_CONFIG_FILE "./assets/config/engine.nya"

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

    GNY_ConfigRobots robots;
};

/**
 * The whole of NYA_CONFIG: the engine's own tunables plus gnyame's. See NYA_ConfigEngine in
 * core_config.h for the engine-owned half.
 * */
// @reflect
struct GNY_Config {
    NYA_ConfigEngine engine;
    GNY_ConfigGame   game;
};

/**
 * The single instance: `NYA_CONFIG.engine.renderer.shadow_bias`, `NYA_CONFIG.game.player_speed`.
 * */
extern GNY_Config NYA_CONFIG;

/** Loads GNY_CONFIG_FILE into NYA_CONFIG and watches it. Called again after a code reload. */
void gny_config_attach(void);

/** Hands the renderer knobs in NYA_CONFIG to `window`. Cheap, so a scene calls it every frame and edits show live. */
void gny_config_renderer_apply(NYA_Window* window);

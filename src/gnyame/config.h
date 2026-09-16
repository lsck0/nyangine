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

typedef struct GNY_ConfigGame GNY_ConfigGame;
typedef struct GNY_Config     GNY_Config;

/**
 * Gameplay tunables worth reaching without a rebuild. Mirrors constants.h's own GNY_PLAYER_* defaults,
 * which is what a field left out of the config file still behaves as.
 * */
// @reflect
struct GNY_ConfigGame {
    /**
     * World units per second a networked player moves. Not read yet: gny_net_apply_command still uses
     * GNY_PLAYER_SPEED.
     * */
    f32 player_speed;

    /** How far apart players spawn, so two joining at once do not start inside each other. See
     *  GNY_PLAYER_SPAWN_SPACING. */
    f32 player_spawn_spacing;
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

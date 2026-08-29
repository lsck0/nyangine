/**
 * @file config.h
 *
 * gnyame's own half of the runtime config, and NYA_CONFIG — the global both halves hang off.
 *
 * ```c
 * f32 speed = NYA_CONFIG.game.player_speed;
 * u32 cascades = NYA_CONFIG.engine.renderer.shadow_cascades;
 * ```
 *
 * Loaded once by gny_world_create from GNY_CONFIG_FILE and, under NYA_ASSET_HOT_RELOAD, kept in sync
 * with it from then on — see nya_config_watch in core_config.h for the mechanism, and
 * assets/config/engine.nya for the file itself.
 *
 * ⚠ **Does not survive a code hot reload.** NYA_CONFIG is a plain global in this DLL, exactly like
 * GNY_LAUNCH above, and gny_world_create runs exactly once regardless of how many times the DLL is
 * rebuilt and reloaded while the game keeps running — see main.c's reload loop, which calls
 * gnyame_run again but never gnyame_init. A rebuild of gnyame's *code* therefore resets this struct to
 * zero until the next full restart; only an edit to GNY_CONFIG_FILE itself is picked up live. See the
 * ownership note on core_config.h's file header for why fixing that needs the engine to own the
 * storage instead of pointing into the game's.
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
    /** World units per second a networked player moves. See GNY_PLAYER_SPEED and
     *  gny_net_apply_command, which is what actually reads a speed today — this field does not feed
     *  it yet; see the file header on core_config.h for why wiring that up is separate work. */
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
 * The single instance, reached with dotted field access the way the task that added this file
 * describes: `NYA_CONFIG.engine.renderer.shadow_bias`, `NYA_CONFIG.game.player_speed`.
 *
 * A plain global rather than something behind an accessor, for the same reason GNY_LAUNCH is: both
 * are read from ordinary game code all over gnyame, and a getter would only hide that this is
 * process-wide state, not make it any less so. See the file header for what does and does not survive
 * a reload.
 * */
extern GNY_Config NYA_CONFIG;

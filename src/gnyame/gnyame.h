/**
 * @file gnyame.h
 *
 * gnyame is the demo game, and the reference for how a game is built on nyangine. Read it in this order.
 *
 * ## Entry points
 *
 * A game exports three functions, and src/main.c calls them. Release builds link the game in; debug and
 * developer builds load it as a DLL and reload it when the file changes (gnyame.c).
 *
 * ```c
 * b8   gnyame_init(s32 argc, NYA_CString* argv);  // read the command line, then bring up its parts
 * void gnyame_run(void);                          // the frame loop, or the one shot the command asked for
 * void gnyame_deinit(void);                       // save, nya_app_deinit
 * ```
 *
 * `main` is a CLI: `gnyame` plays, `gnyame serve` runs headless and serves, `gnyame export <path>`
 * writes the world it would have generated and exits. Each is the same engine with a different set of
 * parts registered; see THE PARTS in gnyame.c.
 *
 * A code reload unloads nothing but zeroes every global in the new DLL and calls gnyame_run again. State
 * that must survive lives in GNY_World (world.h), which the engine world owns, and callbacks are passed
 * as nya_callback(fn) handles, which the engine re-resolves by name.
 *
 * ## The frame
 *
 * nya_app_run drains events into the layer stack, runs fixed update ticks (physics, systems, layers,
 * entities, then the simulation barrier) and renders each window's layers bottom to top.
 *
 * ## Where each engine feature is used
 *
 * | File                          | Shows                                                                   |
 * | :---------------------------- | :---------------------------------------------------------------------- |
 * | gnyame.c                      | app init options, locale loading, startup order, hot reload restore     |
 * | actions.c                     | named input actions, key and gamepad bindings, settings load and save   |
 * | config.h                      | a reflected config struct kept in sync with a file (nya_config_watch)   |
 * | world.c                       | game state in the engine world, Lua VM and scripts, fonts, 2D terrain   |
 * | screens.c                     | pushing and popping layers at the barrier                               |
 * | layers/layer_pause_menu.c     | UI buttons, sliders, a toggle and a selectable row, switching locale    |
 * | layers/layer_game.c           | the 2D scene: tilemap, crates, cameras, bloom post chain, music         |
 * | layers/layer_cube3d.c         | the 3D scene: meshes, 3D physics, picking, particles, 3D audio, shadows |
 * | layers/layer_cube3d_features.c| the switchboard over every NYA_RenderFeature, live in the 3D scene      |
 * | layers/layer_cube3d_stones.c  | built meshes, a detail chain, occluders, static bodies on a layer       |
 * | layers/layer_ui.c             | HUD panels, frame stats, perf span overlay, a NEAT genome drawn live    |
 * | layers/layer_background.c     | procedural immediate mode 2D drawing                                    |
 * | entities/entity_box.c         | entity kinds, spawn options, 2D bodies, collision, click, lights        |
 * | entities/entity_camera.c      | cameras as entities, following, render-to-texture views                |
 * | entities/entity_ledge.c       | one-way platforms, kinematic motion, parented entities                  |
 * | systems/                      | systems registered by name with ordering (nya_system_register)          |
 * | web.c                         | the HTTP server, its metrics resource and the generated OpenAPI schema  |
 * | sim.c                         | recording facts in callbacks and deciding once per frame in an observer |
 * | robots.c                      | NEAT and DQN trained on jobs, a nav flow field, saves, a sqlite history |
 * | net.c                         | single player, listen server, dedicated server and client in one path   |
 *
 * ## A new entity kind
 *
 * ```c
 * NYA_EntityHandle crate = nya_entity_spawn(
 *     .type      = GNY_ENTITY_BOX,
 *     .position  = { x, y, 0.0F },
 *     .on_update = nya_callback(gny_entity_box_on_update),
 *     .on_render = nya_callback(gny_entity_box_on_render)
 * );
 * (void)nya_physics2d_body_attach(crate, .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 24, 24 });
 * ```
 *
 * Add the kind to GNY_EntityKind (entities.h), give it a file under entities/, and include that file from
 * entities.c.
 *
 * ## A new layer
 *
 * Declare the id, the NYA_Layer and five hooks in layers/layers.h, build it in gny_layers_init with
 * nya_layer_of, and push it from a screen change in screens.c.
 *
 * ## Tuning
 *
 * Every number the game uses is a named constant in constants.h. Values meant to change without a
 * rebuild go in assets/config/engine.nya: the game's own in GNY_Config, the engine's own reached
 * through nya_config_engine.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "genyarated/strings.h"
// Before the layers, which size the terrain array from GNY_TERRAIN_POINT_COUNT.
#include "gnyame/constants.h"
/**/
// NYA_CONFIG, which the world and its systems may read, so it is declared before either.
#include "gnyame/config.h"
/**/
// What the player can ask for, before anything that asks whether they did.
#include "gnyame/actions.h"
/**/
// Entity kinds and flags, named before anything that spawns one.
#include "gnyame/entities/entities.h"
/**/
#include "gnyame/systems/systems.h"
#include "gnyame/sim.h"
#include "gnyame/robots.h"
/**/
#include "gnyame/guild.h"
#include "gnyame/world.h"
#include "gnyame/screens.h"
#include "gnyame/layers/layers.h"
#include "gnyame/windows.h"
/**/
// after the layers and the screens, which are what it reads to know where it is.
#include "gnyame/agent.h"

/** What the command line asked for, read once by gnyame_init and kept in the world. */
#define GNY_LAUNCH (gny_world()->launch)

/**
 * Which entities cross the wire.
 * */
#define GNY_FLAG_REPLICATED (1ULL << 20)

/**
 * Reads the command line and brings up the parts it asks for.
 *
 * False when there is nothing to run — `--help`, or a command line that could not be understood — and
 * then nothing was brought up and gnyame_deinit must not be called either.
 * */
b8 gnyame_init(s32 argc, NYA_CString* argv);

/** Runs the app. After a code reload, first rebuilds what this DLL's globals held. */
void gnyame_run(void);
void gnyame_deinit(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE PARTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * What gnyame brings up, each one a system in the engine's registry rather than a line in a hand
 * written startup sequence: one order shared with the engine's own subsystems, teardown that is the
 * reverse of it by construction, and a part that says what it needs refused at startup rather than at
 * its first frame. gnyame.c registers whichever of them a run is made of; see NYA_AppOptions.parts.
 *
 * Declared here because they are registered by name: a callback handle is re-resolved against the new
 * image after a code reload, and a name only the definition knows is a name dlsym cannot find.
 */
NYA_Error gny_part_actions_init(void);
void      gny_part_actions_deinit(void);
NYA_Error gny_part_locale_init(void);
NYA_Error gny_part_world_init(void);
NYA_Error gny_part_plugins_init(void);
NYA_Error gny_part_net_init(void);
void      gny_part_net_deinit(void);
NYA_Error gny_part_web_init(void);
void      gny_part_web_deinit(void);
NYA_Error gny_part_layers_init(void);
NYA_Error gny_part_window_init(void);
NYA_Error gny_part_social_init(void);
void      gny_part_social_deinit(void);
NYA_Error gny_part_screen_init(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * NETWORKING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Brings up whichever of the four modes GNY_LAUNCH names.
 * */
void gny_net_start(void);

void gny_net_stop(void);

/*
 * There is no gny_net_tick. The engine drives it.
 */

/** Turns a command into movement. Registered on both the server and the client. */
void gny_net_apply_command(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s);

/** Reads the local player's input into a command. The client's only way to know what the player wants. */
void gny_net_sample_command(OUT NYA_NetCommand* command);

/** Gives a joining player something to control. */
NYA_EntityHandle gny_net_spawn_player(NYA_NetPeerId peer, NYA_ConstCString name);

/**
 * Draws one player. Registered as the player entity's on_render by gny_net_spawn_player.
 * */
void gny_net_player_on_render(NYA_Entity* entity, NYA_Window* window);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE WEB INTERFACE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Starts the engine's HTTP server and mounts the metrics and schema resources, when
 * `GNYAME_WEB_PORT` names a port.
 *
 * Off unless that variable is set, which is the point: with it unset nothing here binds, allocates or
 * registers anything, and the game runs exactly as it did before the server existed.
 * */
void gny_web_start(void);

/** The pair. A no-op when the server was never started. */
void gny_web_stop(void);

/**
 * Tears the current session down and brings `config` up in its place. What accepting an invite does.
 * */
void gny_net_rejoin(NYA_NetLaunchConfig config);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRESENCE AND INVITES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Brings up presence and invites, and starts listening for joins. After nya_app_init. See social.c. */
void gny_social_start(void);

void gny_social_stop(void);

/** Rebuilds the presence card from real state. Registered as a per-tick system. */
void gny_social_update(f32 delta_time_s);

/** Whether somebody is waiting to be let in, which is what puts GNY_LAYER_SOCIAL on screen. */
b8 gny_social_request_pending(void);

/** Who is asking, for the prompt. Their display name, or their id when the provider gave no name. */
NYA_ConstCString gny_social_request_name(void);

/** Answers them and takes the prompt down. */
void gny_social_request_answer(b8 accept);

/*
 * The generated reflection tables.
 */
#include "genyarated/reflection.h"

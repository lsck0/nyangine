#pragma once

#include "nyangine/nyangine.h"
#include "generated/strings.h"
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
/**/
#include "gnyame/layers/layers.h"
#include "gnyame/windows.h"

/**
 * What the command line asked for, read once at startup by gnyame_init.
 * */
NYA_NetLaunchConfig GNY_LAUNCH;

/**
 * Which entities cross the wire.
 * */
#define GNY_FLAG_REPLICATED (1ULL << 20)

void gnyame_init(s32 argc, NYA_CString* argv);
void gnyame_run(void);
void gnyame_deinit(void);

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
 * The generated reflection tables.
 */
#include "generated/reflection.h"

#pragma once

#include "nyangine/nyangine.h"
#include "generated/strings.h"
// Before the layers, which size the terrain array from GNY_TERRAIN_POINT_COUNT.
#include "gnyame/constants.h"
/**/
// NYA_CONFIG, which the world and its systems may read once they exist — so named before either.
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
 *
 * A global because the answer is needed in three places that do not call each other — whether to
 * create a window, whether to start a server or a client, and what to call the player — and threading
 * it through all of them would be a parameter on functions that otherwise take none.
 *
 * Tentative definition rather than an extern, since this is a unity build. See cli.h in the build
 * system, which does the same for its parser.
 * */
NYA_NetLaunchConfig GNY_LAUNCH;

/**
 * Which entities cross the wire.
 *
 * A bit of the game's own NYA_Entity.flags, because the engine cannot know that a spark is not worth
 * replicating while a crate is. See net_snapshot.h.
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
 *
 * Single player, listen server, dedicated server and client all go through here, which is the point:
 * there is one path, so a bug cannot exist in one and not the others.
 * */
void gny_net_start(void);

void gny_net_stop(void);

/*
 * There is no gny_net_tick. The engine drives it.
 *
 * nya_net_server_tick and nya_net_client_tick are called from core_app's fixed update, after
 * everything that changes the world and before the simulation barrier — see the note there. A game
 * calling them as well would tick the network twice per frame, which on a listen server would apply
 * every command twice.
 */

/**
 * Turns a command into movement. Registered on **both** the server and the client.
 *
 * The same function on both sides is the only reason a client's prediction can agree with the server's
 * authority. Deterministic given (entity, command, dt) — see NYA_NetApplyCommandFn.
 * */
void gny_net_apply_command(NYA_Entity* entity, const NYA_NetCommand* command, f32 delta_time_s);

/** Reads the local player's input into a command. The client's only way to know what the player wants. */
void gny_net_sample_command(OUT NYA_NetCommand* command);

/** Gives a joining player something to control. */
NYA_EntityHandle gny_net_spawn_player(NYA_NetPeerId peer, NYA_ConstCString name);

/**
 * Draws one player. Registered as the player entity's on_render by gny_net_spawn_player.
 *
 * Not static, despite having one caller: on_render is stored as a NYA_CallbackHandle and re-resolved by
 * name after a hot reload, and dlsym cannot find a symbol with internal linkage.
 * */
void gny_net_player_on_render(NYA_Entity* entity, NYA_Window* window);

/*
 * The generated reflection tables.
 *
 * Here rather than in nyangine.h because they describe engine *and* game types, and this is the only
 * translation unit that always has both: the engine cannot name GNY_EntityFlags, and in a hot reload
 * build the executable never sees the game headers at all.
 */
#include "generated/reflection.h"

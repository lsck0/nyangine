/**
 * @file net_config.h
 *
 * What the command line says about how to run: one executable, four modes.
 *
 * ```
 * gnyame                                      single player
 * gnyame --server --port 27015                dedicated server, headless, no window
 * gnyame --connect 192.168.1.5 --port 27015   join somebody else's game
 * gnyame --name Luca                          any of the above, with a name
 * ```
 *
 * There is deliberately **no separate server binary**. A dedicated server is this executable with
 * `--server`, which is why the server code cannot drift from the code a listen server runs — there is
 * only one of it. It is also why "open to LAN" is a menu item: the running process is already a
 * server.
 *
 * ## Why this parses argv rather than taking a struct
 *
 * Because the mode has to be decided *before* anything is created. A dedicated server must not open a
 * window, and a window is created during startup — so something has to read argv before the game
 * decides what to bring up. Handing the game a parsed struct at that point is the smallest interface
 * that allows it.
 *
 * The engine's own argument parser (base_args.h) is deliberately not used here. It is built for a tool
 * with subcommands, and it *exits* on bad input after printing usage — which is right for `./build`
 * and wrong for a game, where an unrecognised argument should be ignored rather than fatal. A player
 * with a stale launch option in Steam should get their game, not a usage message they never see.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/net/net_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_NetLaunchConfig NYA_NetLaunchConfig;

/** The port used when none is given. Unassigned by IANA and conventional for a game server. */
#define NYA_NET_DEFAULT_PORT 27015

/** How long an address may be, buffer included. Comfortably fits an IPv6 literal or a hostname. */
#define NYA_NET_MAX_ADDRESS 128

/** What the command line asked for. */
struct NYA_NetLaunchConfig {
    /**
     * SERVER for single player and for `--server`; CLIENT for `--connect`.
     *
     * Single player and a dedicated server are the same role, distinguished by `dedicated` — which is
     * the whole architectural point. See net.h.
     * */
    NYA_NetRole role;

    /**
     * `--server` was given: run headless, with no window and no local player.
     *
     * What a game checks before creating a window. Not the same question as "is it listening": a
     * listen server listens too.
     * */
    b8 dedicated;

    /** `--connect <address>` was given, and this is the address. Empty otherwise. */
    char address[NYA_NET_MAX_ADDRESS];

    /** From `--port`, or NYA_NET_DEFAULT_PORT. Meaningful for a server to bind and a client to reach. */
    u16 port;

    /** From `--name`, or a platform default. What other players see. */
    char name[NYA_NET_MAX_NAME];

    /**
     * From `--max-players`. Zero means the engine's maximum.
     *
     * Only meaningful on a server, and ignored elsewhere rather than refused — a launch script shared
     * between a server and a client should not have to differ.
     * */
    u32 max_players;

    /**
     * From `--listen <port>` on a process that is otherwise single player.
     *
     * The command line spelling of "open to LAN", so a listen server can be started without a menu.
     * Zero means do not listen at startup.
     * */
    u16 listen_port;

    /** From `--tickrate`. Zero means the engine's default. Ignored on a client, which follows the server. */
    u32 tickrate;

    /** From `--seed`, for a game that generates its world. Zero means the game decides. */
    u64 world_seed;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Reads the command line. Never fails, and never exits.
 *
 * An unrecognised argument is ignored, and a malformed value falls back to the default with a warning.
 * That is deliberate: this runs in a shipped game, where a stale launch option should cost the player
 * nothing. A tool wants base_args.h instead — see the note in this file's header.
 *
 * `--server` and `--connect` together is contradictory; `--server` wins and the other is warned about,
 * because a launch script that says both more likely meant to host.
 * */
NYA_API NYA_NetLaunchConfig nya_net_config_from_args(s32 argc, NYA_CString* argv) __attr_no_discard;

/** Logs what the config resolved to, at info. What a dedicated server's first line of output should be. */
NYA_API void nya_net_config_report(const NYA_NetLaunchConfig* config);

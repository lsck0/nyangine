/**
 * @file net_config.h
 *
 * ```
 * gnyame                                      single player
 * gnyame --server --port 27015                dedicated server, headless, no window
 * gnyame --connect 192.168.1.5 --port 27015   join somebody else's game
 * gnyame --name Luca                          any of the above, with a name
 * gnyame --connect host --server-key 1f0c...  refuse any server but the one holding that key
 * gnyame --net-latency 60 --net-loss 5        and a bad network, to see how the game holds up
 * ```
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/net/net_crypto.h"
#include "nyangine/net/net_transport.h"
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
     * */
    NYA_NetRole role;

    /**
     * `--server` was given: run headless, with no window and no local player.
     * */
    b8 dedicated;

    /** `--connect <address>` was given, and this is the address. Empty otherwise. */
    char address[NYA_NET_MAX_ADDRESS];

    /** From `--port`, or NYA_NET_DEFAULT_PORT. Meaningful for a server to bind and a client to reach. */
    u16 port;

    /** From `--name`, or a platform default. What other players see. */
    char name[NYA_NET_MAX_NAME];

    /** Whether `--name` set it, so a game can put a saved name in place of the default. */
    b8 named;

    /**
     * From `--max-players`. Zero means the engine's maximum.
     * */
    u32 max_players;

    /**
     * From `--listen <port>` on a process that is otherwise single player.
     * */
    u16 listen_port;

    /** From `--tickrate`. Zero means the engine's default. Ignored on a client, which follows the server. */
    u32 tickrate;

    /** From `--seed`, for a game that generates its world. Zero means the game decides. */
    u64 world_seed;

    /** From `--server-key`, 64 hex digits: the only server a client will talk to. Zero trusts the first key it sees. */
    u8 server_key[NYA_NET_KEY_SIZE];

    /** From `--net-latency` and `--net-jitter` in milliseconds, `--net-loss`, `--net-duplicate` and `--net-reorder` in percent. */
    NYA_NetConditions conditions;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Reads the command line. Never fails, and never exits.
 * */
NYA_API NYA_NetLaunchConfig nya_net_config_from_args(s32 argc, NYA_CString* argv) __attr_no_discard;

/** Logs what the config resolved to, at info. What a dedicated server's first line of output should be. */
NYA_API void nya_net_config_report(const NYA_NetLaunchConfig* config);

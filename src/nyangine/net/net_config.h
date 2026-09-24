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
 * gnyame --connect 76561197960287930 \
 *        --transport steam                    join over Steam, where the address is an account
 * ```
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/net/net_crypto.h"
#include "nyangine/net/net_transport.h"
#include "nyangine/net/net_types.h"

// TYPES

typedef struct NYA_NetLaunchConfig NYA_NetLaunchConfig;

/** The port used when none is given. Unassigned by IANA and conventional for a game server. */
#define NYA_NET_DEFAULT_PORT 27015

/** How long an address may be, buffer included. Comfortably fits an IPv6 literal or a hostname. */
#define NYA_NET_MAX_ADDRESS 128

/**
 * The tag a join secret starts with. A version, so a secret written by a later build is rejected here
 * rather than half understood: the rule is reject by default, permit explicitly.
 * */
#define NYA_NET_JOIN_SECRET_TAG "nya1:"

/**
 * How a join secret names its transport. The scheme is written out rather than inferred from the
 * address: a Steam id is all digits and so is a bare IPv4 written without dots, and guessing which of
 * the two a string is would send a player to a stranger's account.
 * */
#define NYA_NET_JOIN_SCHEME_UDP   "udp"
#define NYA_NET_JOIN_SCHEME_STEAM "steam"

/**
 * How long a join secret is, buffer included: the tag, the scheme, the address, five port digits and
 * the server key in hex, each separated by a colon. The slack covers the separators and the longest
 * scheme. Discord truncates a secret at 128 bytes, so a secret at this size with a long hostname will
 * not survive it; see nya_net_config_to_join_secret.
 * */
#define NYA_NET_MAX_JOIN_SECRET (sizeof(NYA_NET_JOIN_SECRET_TAG) + NYA_NET_MAX_ADDRESS + NYA_NET_KEY_HEX_SIZE + 24)

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

    /**
     * From `--transport`, and from a join secret. NYA_NET_TRANSPORT_UDP unless something said otherwise.
     *
     * Over NYA_NET_TRANSPORT_STEAM, `address` is the host's Steam id in decimal, `port` means nothing,
     * and `server_key` is ignored: Valve's relay has already proved the account.
     * */
    NYA_NetTransportKind transport;
};

// FUNCTIONS

/**
 * What a launch is before anything is said about it: single player, on the default port, over sockets.
 * */
NYA_API NYA_NetLaunchConfig nya_net_config_default(void) __attr_no_discard;

/**
 * Applies one `--name value` pair, and answers whether it was a flag this vocabulary has.
 *
 * The one place that knows what a launch flag *means*. A program with a command line of its own — a
 * CLI over `nya_args`, as gnyame has — hands its parsed flags here one at a time, so what
 * `--tickrate` does lives here while what it is called and how it is described live in that program's
 * own command tree. `value` may be null for a flag that takes none.
 *
 * A value that is not usable is reported and the default kept rather than refused: somebody who
 * mistyped a port wants the game to start, and the report is how they find out.
 * */
NYA_API b8 nya_net_config_apply(NYA_NetLaunchConfig* config, NYA_ConstCString name, NYA_ConstCString value);

/**
 * Settles what the flags mean together, once they have all been applied: `--server` with `--connect`
 * is a server, a client is not dedicated, and a dedicated server listens.
 *
 * Apart from the applying because these answers depend on every flag rather than on one, and a
 * program that applies its own would otherwise have to know the rules.
 * */
NYA_API void nya_net_config_finish(NYA_NetLaunchConfig* config);

NYA_API NYA_NetLaunchConfig nya_net_config_from_args(s32 argc, NYA_CString* argv) __attr_no_discard;

/** Logs what the config resolved to, at info. What a dedicated server's first line of output should be. */
NYA_API void nya_net_config_report(const NYA_NetLaunchConfig* config);

// JOIN SECRETS

// A join secret is the launch config on the wire: the string a friend's client hands back when accepting an invite.

/**
 * Writes the address, port and server key of `config` as a join secret.
 *
 * False when the config has nothing to join (no listening port, or a Steam transport with no client
 * signed in) or the buffer is too small, in which case `out_secret` is left an empty string. The caller
 * is expected to be a listen server: a client's config names the server it joined, which is the same
 * thing, but a single player config names nothing.
 * */
NYA_API b8 nya_net_config_to_join_secret(const NYA_NetLaunchConfig* config, OUT char* out_secret, u64 capacity) __attr_no_discard;

/**
 * Parses a join secret into the client half of a launch config: role, address, port and server key.
 *
 * Every byte of `secret` came from another player's client, so this refuses anything it does not fully
 * understand rather than repairing it: a wrong tag, a scheme this build has no transport for, an empty
 * or overlong address, an address with a character no hostname or IP literal has, a port outside
 * 1..65535, or a key that is not 64 hex digits. False leaves `out_config` zeroed, and never logs on the
 * caller's behalf.
 * */
NYA_API b8 nya_net_config_from_join_secret(NYA_ConstCString secret, OUT NYA_NetLaunchConfig* out_config) __attr_no_discard;

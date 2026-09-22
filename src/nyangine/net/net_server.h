/**
 * @file net_server.h
 *
 * ```c
 * // single player: nobody listens and nothing is serialised.
 * NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = FLAG_REPLICATED }));
 *
 * // later, from a menu, open the same game to the LAN. The world does not change.
 * NYA_EXPECT(nya_net_server_listen(27015));
 * ```
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/core/core_entity.h"
#include "nyangine/net/net_command.h"
#include "nyangine/net/net_snapshot.h"
#include "nyangine/net/net_transport.h"
#include "nyangine/net/net_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_NetServerConfig NYA_NetServerConfig;
typedef struct NYA_NetServerPeer   NYA_NetServerPeer;

/** Violations a player may run up before being kicked, when the config does not say. */
#define NYA_NET_VIOLATION_LIMIT_DEFAULT 32

/**
 * The default hysteresis band, as a fraction of the relevance radius.
 * */
#define NYA_NET_RELEVANCE_HYSTERESIS 0.25F

/**
 * Called when a player joins, to give them something to control.
 * */
typedef NYA_EntityHandle (*NYA_NetSpawnPlayerFn)(NYA_NetPeerId peer, NYA_ConstCString name);

/** Called when a player leaves, before their entity is despawned. Optional. */
typedef void (*NYA_NetDespawnPlayerFn)(NYA_NetPeerId peer, NYA_EntityHandle entity);

/**
 * Whether `entity` is worth sending to `peer` this tick.
 * */
typedef b8 (*NYA_NetRelevanceFn)(NYA_NetPeerId peer, const NYA_Entity* peer_entity, const NYA_Entity* entity, b8 currently_relevant);

/**
 * Called when a client sends a game-defined event. The object dies when this returns.
 * */
typedef void (*NYA_NetServerEventFn)(NYA_NetPeerId peer, const NYA_Object* event);

struct NYA_NetServerConfig {
    /**
     * Which entities are replicated: a bit of the game's own NYA_Entity.flags.
     * */
    u64 replicated_flag;

    /** Refuses connections past this many. Clamped to NYA_NET_MAX_PEERS. Zero means the maximum. */
    u32 max_players;

    /**
     * How often to send snapshots, in ticks. One means every tick.
     * */
    u32 snapshot_interval_ticks;

    /*
     * the game's callbacks
     *
     * Handles built with nya_callback, not function pointers. The world, peers and sockets live in the
     * executable and survive a hot reload while the game DLL is swapped, so a raw pointer would dangle.
     * Handles are re-resolved by name, and shipping builds compile them down to plain pointers.
     *
     * Pass the bare function name: nya_callback stringizes its argument, so nya_callback(&fn) records
     * "&fn", which never resolves.
     */

    /** NYA_NetSpawnPlayerFn. What a joining player gets to control. */
    NYA_CallbackHandle on_spawn_player;

    /** NYA_NetDespawnPlayerFn. Optional. What happens to that entity when they leave. */
    NYA_CallbackHandle on_despawn_player;

    /** NYA_NetServerEventFn. Optional. Where a client's chat line or action request arrives. */
    NYA_CallbackHandle on_client_event;

    /**
     * NYA_NetApplyCommandFn. How a command becomes movement. Required as soon as anyone connects.
     * */
    NYA_CallbackHandle on_apply_command;

    /*
     * ── interest management ──
     */

    /**
     * NYA_NetRelevanceFn. Which entities each peer is told about. Unset sends everything to everyone.
     * */
    NYA_CallbackHandle on_relevance;

    /**
     * The built-in relevance rule: send an entity only within this distance of the player.
     * */
    f32 relevance_radius;

    /**
     * How much further than `relevance_radius` an entity must travel before it stops being sent.
     * */
    f32 relevance_hysteresis;

    /*
     * ── bandwidth ──
     */

    /**
     * The most this server will send one peer per second, in bytes. Zero is unlimited.
     * */
    u32 bandwidth_bytes_per_second;

    /*
     * ── authority ──
     */

    /**
     * The fastest any player's entity may move, in world units per second. Commands that would take it further are
     * cut short and counted as violations. Zero trusts the game's movement code.
     * */
    f32 max_speed;

    /** Violations a player may run up, decaying by one a second, before being kicked. Zero is NYA_NET_VIOLATION_LIMIT_DEFAULT. */
    u32 violation_limit;

    /** Fractional bits positions and velocities are sent with. Zero is NYA_NET_POSITION_BITS_DEFAULT. */
    u32 position_bits;

    /*
     * ── security ──
     */

    /** Who this server is. Players pin its public key. Zero generates a fresh identity when listening starts. */
    NYA_NetKeyPair identity;

    /** A bad network on purpose, for what this server sends to remote players. */
    NYA_NetConditions conditions;

    /*
     * ── lag compensation ──
     */

    /**
     * How many ticks of world history to keep for rewinding. Zero disables it.
     * */
    u32 lag_history_ticks;
};

/** One connected player, as the game sees them. */
struct NYA_NetServerPeer {
    NYA_NetPeerId peer;

    char name[NYA_NET_MAX_NAME];

    /** What this player controls, or NYA_ENTITY_HANDLE_NONE for a spectator. */
    NYA_EntityHandle entity;

    /** Whether the handshake completed. Nothing but HELLO is accepted before it does. */
    b8 accepted;

    /** True for a listen server's own player. See nya_net_server_local_peer. */
    b8 is_local;

    /** The long term key the player proved it holds, or all zero for an anonymous or local player. */
    u8 public_key[NYA_NET_KEY_SIZE];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Becomes the authority. Does not open a socket.
 * */
NYA_API NYA_Error nya_net_server_start(NYA_NetServerConfig config) __attr_no_discard;

NYA_API void nya_net_server_stop(void);

NYA_API b8 nya_net_server_running(void) __attr_no_discard;

/**
 * Starts accepting players over UDP on `port`.
 *
 * The convenience call on top of nya_net_server_listen_on, since UDP is what a LAN game and a dedicated
 * server want and what every existing caller asked for.
 * */
NYA_API NYA_Error nya_net_server_listen(u16 port) __attr_no_discard;

/**
 * Starts accepting players over `kind`.
 *
 * NYA_NET_TRANSPORT_UDP binds `port`. NYA_NET_TRANSPORT_STEAM has nothing to bind and ignores it:
 * players reach this process by Steam account, so what a friend needs is nya_steam_user_id, and the
 * game publishes it through a lobby or a join secret. NYA_NET_TRANSPORT_LOOPBACK cannot listen, and
 * says so; nya_net_server_attach_local is how a local player joins.
 * */
NYA_API NYA_Error nya_net_server_listen_on(NYA_NetTransportKind kind, u16 port) __attr_no_discard;

/** Whether a socket is open. False for single player, true once opened to the LAN. */
NYA_API b8 nya_net_server_is_listening(void) __attr_no_discard;

/**
 * The port players reach this server on, or zero when it is not listening.
 *
 * Not the number passed to nya_net_server_listen: zero there means "whichever port is free", so what was
 * asked for and what was bound are only the same when a port was named. This is the one to print, to show
 * in a HUD, and to put in whatever tells a player where the game is.
 * */
NYA_API u16 nya_net_server_port(void) __attr_no_discard;

/** The key players pin to be sure they reached this server, or null until it listens. */
NYA_API const u8* nya_net_server_public_key(void) __attr_no_discard;

/**
 * Attaches a local player over a loopback transport, and hands back the client end.
 * */
NYA_API NYA_Error nya_net_server_attach_local(OUT NYA_NetTransport** out_client_transport) __attr_no_discard;

/** The local player's peer id, or NYA_NET_PEER_NONE on a dedicated server. */
NYA_API NYA_NetPeerId nya_net_server_local_peer(void) __attr_no_discard;

/** Whether this server has no local player, i.e. it was started with `--server`. */
NYA_API b8 nya_net_server_is_dedicated(void) __attr_no_discard;

/**
 * One tick of networking: drain what arrived, apply commands, send snapshots.
 * */
NYA_API void nya_net_server_tick(u64 tick, f32 delta_time_s);

/** How many players are connected, the local one included. */
NYA_API u32 nya_net_server_peer_count(void) __attr_no_discard;

/** The player at `index` in the peer table, or null. Iterate 0..NYA_NET_MAX_PEERS. */
NYA_API const NYA_NetServerPeer* nya_net_server_peer_at(u32 index) __attr_no_discard;

NYA_API const NYA_NetServerPeer* nya_net_server_peer(NYA_NetPeerId peer) __attr_no_discard;

/** Drops a player. What a kick is, and what a game does about a cheater. */
NYA_API void nya_net_server_kick(NYA_NetPeerId peer, NYA_NetDisconnect reason);

/**
 * Sends a game-defined event to one peer, or to everyone when `peer` is NYA_NET_PEER_NONE.
 * */
NYA_API NYA_Error nya_net_server_send_event(NYA_NetPeerId peer, const NYA_Object* event) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * LAG COMPENSATION
 * ─────────────────────────────────────────────────────────
 */

/**
 * Moves the world back to what `peer` was looking at, so a hit test resolves against what they saw.
 *
 * ```c
 * if (nya_net_server_rewind_begin(shooter)) {
 *     NYA_EntityHandle hit = nya_physics3d_raycast(origin, direction, nullptr, nullptr);
 *     nya_net_server_rewind_end();
 *
 *     if (nya_entity_is_valid(hit)) apply_damage(hit);
 * }
 * ```
 * */
NYA_API b8 nya_net_server_rewind_begin(NYA_NetPeerId peer) __attr_no_discard;

/** Puts the world back. Only after nya_net_server_rewind_begin returned true. */
NYA_API void nya_net_server_rewind_end(void);

/**
 * How far back the last rewind went, in ticks. Zero when nothing is rewound.
 * */
NYA_API u64 nya_net_server_rewind_ticks(void) __attr_no_discard;

/** How long a line nya_net_stats_line writes may be, terminator included. */
#define NYA_NET_STATS_LINE_MAX 128

/**
 * One line summing up the network for a debug overlay: the connection to the server on a remote client, every remote
 * player on a server. False, writing nothing, when there is no network to describe.
 * */
NYA_API b8 nya_net_stats_line(OUT char* out, u64 capacity) __attr_no_discard;

/** What a player's connection is costing, and how often they broke the rules. Zeroes for a peer that is gone. */
NYA_API NYA_NetPeerStats nya_net_server_peer_stats(NYA_NetPeerId peer) __attr_no_discard;

/**
 * The most recent command applied for a peer.
 * */
NYA_API NYA_NetCommand nya_net_server_last_command(NYA_NetPeerId peer) __attr_no_discard;

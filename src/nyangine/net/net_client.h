/**
 * @file net_client.h
 *
 * ```c
 * NYA_EXPECT(nya_net_client_connect("192.168.1.5", 27015, "Luca", (NYA_NetClientConfig){
 *     .replicated_flag  = FLAG_REPLICATED,
 *     .on_apply_command = gny_apply_movement,   // the same function the server runs
 * }));
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

typedef struct NYA_NetClientConfig NYA_NetClientConfig;

/** How long replicas are extrapolated past the newest snapshot when the config does not say. */
#define NYA_NET_EXTRAPOLATION_LIMIT_MS_DEFAULT 100

/** The tick lengths a client accepts from a server's WELCOME: 240 down to 10 ticks a second. */
#define NYA_NET_TICK_NS_MIN (1000000000ULL / 240)
#define NYA_NET_TICK_NS_MAX (1000000000ULL / 10)
typedef enum NYA_NetClientState    NYA_NetClientState;

/**
 * Called once per tick to ask what the player is doing.
 * */
typedef void (*NYA_NetSampleCommandFn)(OUT NYA_NetCommand* command);

/** Called when the server sends a game event. The object dies when this returns. */
typedef void (*NYA_NetGameEventFn)(const NYA_Object* event);

/**
 * Called when another player joins or leaves. `name` dies when this returns; copy it to keep it.
 * */
typedef void (*NYA_NetPeerChangeFn)(NYA_NetPeerId peer, NYA_ConstCString name, b8 joined);

enum NYA_NetClientState {
    NYA_NET_CLIENT_DISCONNECTED = 0,

    /** The transport is reaching the server. Nothing has been agreed yet. */
    NYA_NET_CLIENT_CONNECTING,

    /** Connected at the transport level; HELLO sent, waiting for WELCOME or REJECT. */
    NYA_NET_CLIENT_HANDSHAKING,

    /** In the game. Snapshots are arriving and commands are going out. */
    NYA_NET_CLIENT_PLAYING,

    NYA_NET_CLIENT_STATE_COUNT,
};

struct NYA_NetClientConfig {
    /** Which entities the server replicates: the same bit the server was configured with. */
    u64 replicated_flag;

    /*
     * the game's callbacks
     *
     * Handles built with nya_callback rather than function pointers. The client keeps its socket through
     * a hot reload while the game DLL is replaced, so a raw pointer would dangle. Same as
     * NYA_NetServerConfig.
     */

    /** NYA_NetApplyCommandFn. Turns a command into movement. Must be the same function the server runs. */
    NYA_CallbackHandle on_apply_command;

    /** NYA_NetSampleCommandFn. How the client learns what the player is doing. Required. */
    NYA_CallbackHandle on_sample_command;

    /** NYA_NetGameEventFn. Optional. Where a chat line or an inventory change arrives. */
    NYA_CallbackHandle on_game_event;

    /** NYA_NetPeerChangeFn. Optional. Where another player joining or leaving arrives. */
    NYA_CallbackHandle on_peer_change;

    /**
     * How far a prediction may be wrong before it is corrected, in world units.
     * */
    f32 correction_threshold;

    /** How long a replica may be carried on by its velocity past the newest snapshot before it holds. Zero is NYA_NET_EXTRAPOLATION_LIMIT_MS_DEFAULT. */
    u32 extrapolation_limit_ms;

    /** The server's public key. Any other server is refused. Zero trusts the key the server presents. */
    u8 server_key[NYA_NET_KEY_SIZE];

    /** The player's own long term key, so the server can recognise them. Zero connects anonymously. */
    NYA_NetKeyPair identity;

    /** A bad network on purpose, for what this client sends. */
    NYA_NetConditions conditions;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Connects to a server over UDP. The handshake completes asynchronously; watch nya_net_client_state. */
NYA_API NYA_Error nya_net_client_connect(NYA_ConstCString address, u16 port, NYA_ConstCString name, NYA_NetClientConfig config) __attr_no_discard;

/** Attaches to a transport created elsewhere. This is how a listen server's host plays. */
NYA_API NYA_Error nya_net_client_attach(NYA_NetTransport* transport, NYA_ConstCString name, NYA_NetClientConfig config) __attr_no_discard;

NYA_API void nya_net_client_disconnect(void);

NYA_API NYA_NetClientState nya_net_client_state(void) __attr_no_discard;

/** Why the last disconnection happened. For showing a player something better than "connection lost". */
NYA_API NYA_NetDisconnect nya_net_client_disconnect_reason(void) __attr_no_discard;

/**
 * One tick: sample input, predict, send, and apply whatever arrived.
 * */
NYA_API void nya_net_client_tick(u64 tick, f32 delta_time_s);

/** The entity this client controls, in this process's handle space. */
NYA_API NYA_EntityHandle nya_net_client_entity(void) __attr_no_discard;

/**
 * The same entity as the *server* names it. What WELCOME carried.
 * */
NYA_API NYA_EntityHandle nya_net_client_entity_remote(void) __attr_no_discard;

/**
 * Translates a server handle into a local one, or NYA_ENTITY_HANDLE_NONE.
 * */
NYA_API NYA_EntityHandle nya_net_client_local_entity(NYA_EntityHandle remote) __attr_no_discard;

/** This client's peer id, as the server numbers it. */
NYA_API NYA_NetPeerId nya_net_client_peer(void) __attr_no_discard;

/** What the connection is costing. Zeroed round trip on a listen server, which is a fact rather than a placeholder. */
NYA_API NYA_NetPeerStats nya_net_client_stats(void) __attr_no_discard;

/** The newest server tick applied. What a debug overlay shows next to the local tick. */
NYA_API u64 nya_net_client_server_tick(void) __attr_no_discard;

/**
 * How many corrections have happened since connecting.
 * */
NYA_API u64 nya_net_client_correction_count(void) __attr_no_discard;

/** Sends a game-defined event to the server. Reliable and ordered. */
NYA_API NYA_Error nya_net_client_send_event(const NYA_Object* event) __attr_no_discard;

/**
 * Draws every replicated entity a little in the past, between the snapshots around that moment, with a delay that
 * follows how regularly snapshots arrive. Call once per frame with the frame's real duration.
 * */
NYA_API void nya_net_client_interpolate(f32 delta_time_s);

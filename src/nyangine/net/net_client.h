/**
 * @file net_client.h
 *
 * The replica side: it applies snapshots, predicts its own player, and reconciles when it was wrong.
 *
 * ```c
 * NYA_EXPECT(nya_net_client_connect("192.168.1.5", 27015, "Luca", (NYA_NetClientConfig){
 *     .replicated_flag  = FLAG_REPLICATED,
 *     .on_apply_command = gny_apply_movement,   // the same function the server runs
 * }));
 * ```
 *
 * ## Prediction, and why only the local player
 *
 * A client applies its own command the instant it samples it, rather than waiting a round trip to see
 * what the server made of it. Without that, every keypress feels as far away as the server is.
 *
 * When the snapshot for tick T arrives, the client compares the server's answer for its own entity
 * against what it predicted at T. If they agree, nothing happens. If they do not, it snaps to the
 * server's state and **replays** every command from T+1 to now — so the correction is applied to the
 * past and the player's current position is recomputed rather than jerked.
 *
 * **Only the local player is predicted.** Not the world. Rolling a whole Box2D or Box3D world back per
 * correction is not something either solver can do cheaply, and building that is how a netcode becomes
 * a rewrite of the physics engine. Everything else is interpolated between the last two snapshots,
 * which is smooth, slightly in the past, and what every player in every game of this kind has always
 * been looking at.
 *
 * This is the same choice Source and Overwatch make, and it is why prediction runs through a
 * game-supplied movement function rather than through the solver — see NYA_NetApplyCommandFn. The one
 * hard requirement is that the function be **deterministic given (entity state, command, dt)**. A
 * function that reads the wall clock, or an unseeded RNG, will disagree with the server on every tick
 * and the player will be corrected continuously.
 *
 * ## On a listen server there is no prediction at all
 *
 * The local client's transport reports itself local, there is no latency to hide, and the client reads
 * a world the server is writing directly. Predicting would be pure cost for an answer already exact.
 * See nya_net_transport_is_local.
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
typedef enum NYA_NetClientState    NYA_NetClientState;

/**
 * Called once per tick to ask what the player is doing.
 *
 * The engine does not read the input system itself: which of a game's actions map to which bits is
 * the game's numbering, and the engine has no way to know it. See net_command.h.
 *
 * Fill in `actions`, `aim` and `analog`; the tick is set by the caller.
 * */
typedef void (*NYA_NetSampleCommandFn)(OUT NYA_NetCommand* command);

/** Called when the server sends a game event. The object dies when this returns. */
typedef void (*NYA_NetGameEventFn)(const NYA_Object* event);

/**
 * Called when another player joins or leaves. `name` dies when this returns; copy it to keep it.
 *
 * What a player list is built from. The messages behind it are reliable and ordered, so a list assembled
 * from them cannot drift — which is the reason they are not inferred from snapshots.
 *
 * Not called for this client's own arrival: that is what WELCOME is, and reporting it here as well would
 * have a client add itself to its own roster twice.
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
     * ── the game's callbacks ──
     *
     * Handles rather than function pointers, built with nya_callback and passed the bare function name.
     * A client survives a hot reload with its socket open while the game DLL underneath it is replaced,
     * so a raw pointer into that DLL would be dangling on the next tick. See the same note on
     * NYA_NetServerConfig.
     */

    /**
     * NYA_NetApplyCommandFn. How a command becomes movement — the **same function the server runs**.
     *
     * If the two differ, prediction disagrees with authority on every tick and the player is corrected
     * continuously. That is why this is one callback a game passes to both rather than two
     * implementations that happen to match.
     * */
    NYA_CallbackHandle on_apply_command;

    /** NYA_NetSampleCommandFn. How the client learns what the player is doing. Required. */
    NYA_CallbackHandle on_sample_command;

    /** NYA_NetGameEventFn. Optional. Where a chat line or an inventory change arrives. */
    NYA_CallbackHandle on_game_event;

    /** NYA_NetPeerChangeFn. Optional. Where another player joining or leaving arrives. */
    NYA_CallbackHandle on_peer_change;

    /**
     * How far a prediction may be wrong before it is corrected, in world units.
     *
     * Not zero, deliberately. The server and the client compute the same movement in floating point on
     * possibly different hardware, so tiny disagreements are constant and correcting for them would
     * mean replaying every command every tick for no visible benefit. Zero uses a sensible default.
     * */
    f32 correction_threshold;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Connects to a server over UDP. The handshake completes asynchronously; watch nya_net_client_state. */
NYA_API NYA_Error nya_net_client_connect(NYA_ConstCString address, u16 port, NYA_ConstCString name, NYA_NetClientConfig config) __attr_no_discard;

/**
 * Attaches to a transport somebody else created — which is how a listen server's host plays.
 *
 * Takes the client end of the loopback pair from nya_net_server_attach_local. From here the host runs
 * exactly the same client code a remote player does.
 * */
NYA_API NYA_Error nya_net_client_attach(NYA_NetTransport* transport, NYA_ConstCString name, NYA_NetClientConfig config) __attr_no_discard;

NYA_API void nya_net_client_disconnect(void);

NYA_API NYA_NetClientState nya_net_client_state(void) __attr_no_discard;

/** Why the last disconnection happened. For showing a player something better than "connection lost". */
NYA_API NYA_NetDisconnect nya_net_client_disconnect_reason(void) __attr_no_discard;

/**
 * One tick: sample input, predict, send, and apply whatever arrived.
 *
 * Called from the app loop's fixed update, in the same place the server's tick runs — a listen server
 * calls both, and the order matters: the server ticks first, so the local client sees the world the
 * server just produced rather than last tick's.
 * */
NYA_API void nya_net_client_tick(u64 tick, f32 delta_time_s);

/**
 * The entity this client controls, in **this process's** handle space.
 *
 * NYA_ENTITY_HANDLE_NONE until the first snapshot has spawned it, which is a tick or two after the
 * handshake — a game must not assume it is there the moment nya_net_client_state reports PLAYING.
 *
 * ## Two handle spaces
 *
 * A handle is an index into an entity table plus a generation, and the client and the server have
 * different tables. The server's entity 7 is not this client's entity 7. So there are two names for
 * the player's entity: the server's, which is what crosses the wire, and this one, which is what
 * nya_entity_get can actually resolve. See NYA_NetReplicaMap.
 *
 * On a listen server the two are identical, because there is one table.
 * */
NYA_API NYA_EntityHandle nya_net_client_entity(void) __attr_no_discard;

/**
 * The same entity as the *server* names it. What WELCOME carried.
 *
 * For a game that needs to talk to the server about its own entity — an event naming it, say. Not
 * usable with nya_entity_get; see the note above.
 * */
NYA_API NYA_EntityHandle nya_net_client_entity_remote(void) __attr_no_discard;

/**
 * Translates a server handle into a local one, or NYA_ENTITY_HANDLE_NONE.
 *
 * What a game needs whenever the server refers to an entity — in a game event, a kill feed, a targeting
 * message. On a listen server this is the identity, so a game may call it unconditionally.
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
 *
 * The number to watch when prediction is misbehaving: a handful is normal, a steady stream every tick
 * means the movement function is not deterministic — it reads the clock, or an unseeded RNG, or state
 * the server does not have. See the note in this file's header.
 * */
NYA_API u64 nya_net_client_correction_count(void) __attr_no_discard;

/** Sends a game-defined event to the server. Reliable and ordered. */
NYA_API NYA_Error nya_net_client_send_event(const NYA_Object* event) __attr_no_discard;

/**
 * Smooths every replicated entity between the last two snapshots. Call once per **frame**.
 *
 * Snapshots arrive at the snapshot rate and a frame renders far more often, so writing each snapshot
 * straight onto an entity makes every remote player step rather than move. This fills the gaps. See
 * nya_net_replica_interpolate for the full reasoning and for what is deliberately skipped.
 *
 * Does nothing on a listen server, where the client reads the world the server is writing and there
 * are no snapshots being applied to smooth.
 *
 * Not called by the engine: it belongs in a frame, not a tick, and only the game knows whether it
 * wants smoothing at all — a strategy game drawing on a grid does not.
 * */
NYA_API void nya_net_client_interpolate(f32 delta_time_s);

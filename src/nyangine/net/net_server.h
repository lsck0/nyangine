/**
 * @file net_server.h
 *
 * The authoritative side: it simulates the world, and it is right about it.
 *
 * ```c
 * // Single player. Nobody is listening, and nothing is serialised.
 * NYA_EXPECT(nya_net_server_start((NYA_NetServerConfig){ .replicated_flag = FLAG_REPLICATED }));
 *
 * // Later, from a menu: open the same game to the LAN. Nothing about the world changes.
 * NYA_EXPECT(nya_net_server_listen(27015));
 * ```
 *
 * **Single player costs one branch per tick**: nya_net_server_tick returns immediately with no remote
 * peers, so nothing is captured, encoded or kept. That is why single player is a server rather than a
 * fourth mode — one simulation path, so a bug cannot exist in multiplayer and not in single player.
 *
 * **Per-client baselines.** Each peer's snapshot is delta'd against the newest one *it* acknowledged,
 * kept in a ring of NYA_NET_SNAPSHOT_HISTORY, so a peer on a bad connection is sent more without
 * making a good one pay for it. One whose acknowledgement has aged out of the ring gets a full
 * snapshot — both the join path and the recovery path.
 *
 * **The local player on a listen server** is a peer like any other with a real NYA_NetPeerId, so
 * nothing in the game has to ask "am I the host". It rides a loopback transport, so its snapshots skip
 * serialisation and its commands arrive with no latency. See nya_net_server_local_peer.
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

/**
 * The default hysteresis band, as a fraction of the relevance radius.
 *
 * A quarter: wide enough that ordinary movement does not cross both thresholds within a snapshot or two,
 * narrow enough that the extra entities being kept are genuinely nearby. What matters is that it is not
 * zero — see NYA_NetServerConfig.relevance_hysteresis.
 * */
#define NYA_NET_RELEVANCE_HYSTERESIS 0.25F

/**
 * Called when a player joins, to give them something to control.
 *
 * The engine spawns nothing on its own: what a player *is*, and where they start, is the game's
 * decision. Return NYA_ENTITY_HANDLE_NONE to admit a spectator with no body. The returned entity must
 * carry the config's `replicated_flag`, or the player will control something no client can see.
 * */
typedef NYA_EntityHandle (*NYA_NetSpawnPlayerFn)(NYA_NetPeerId peer, NYA_ConstCString name);

/** Called when a player leaves, before their entity is despawned. Optional. */
typedef void (*NYA_NetDespawnPlayerFn)(NYA_NetPeerId peer, NYA_EntityHandle entity);

/**
 * Whether `entity` is worth sending to `peer` this tick.
 *
 * The answer to "the world is bigger than the wire". Without one, every client is sent every replicated
 * entity every snapshot — ruinous for a large world, and quadratic in players since each pays for all
 * of it. Return false and the entity is absent from that peer's snapshot; the client's own sweep
 * despawns its copy, which is right — from that player's view it went out of range. It reappears in
 * full when relevant again, because it will not be in that peer's baseline.
 *
 * `peer_entity` is what that player controls, or null for a spectator; almost every rule is about
 * distance from it, so it is passed rather than looked up per entity.
 *
 * **`currently_relevant` is whether this entity is already being sent, and a rule that ignores it will
 * make entities on its boundary flicker.** Relevance is re-decided every snapshot, so an entity on the
 * edge of a sharp test flips several times a second, each flip a spawn and a despawn on the client — a
 * player near a doorway sees the next room strobe. Make the condition to *stay* looser than the one to
 * *start*: return true a little further out when it is set. The built-in radius rule does exactly this;
 * see NYA_NetServerConfig.relevance_hysteresis.
 *
 * Called once per replicated entity per peer per snapshot, so it wants to be cheap: distance-squared,
 * not distance. See NYA_NetServerConfig.relevance_radius for the built-in version.
 * */
typedef b8 (*NYA_NetRelevanceFn)(NYA_NetPeerId peer, const NYA_Entity* peer_entity, const NYA_Entity* entity, b8 currently_relevant);

/**
 * Called when a client sends a game-defined event. The object dies when this returns.
 *
 * **This is untrusted input.** A client can send any object with any contents at any time, so a server
 * that acts on one without checking has handed the client authority — which is the one thing this whole
 * architecture exists to prevent. Validate the peer is allowed to do what it is asking, and validate the
 * values, before changing anything.
 * */
typedef void (*NYA_NetServerEventFn)(NYA_NetPeerId peer, const NYA_Object* event);

struct NYA_NetServerConfig {
    /**
     * Which entities are replicated: a bit of the game's own NYA_Entity.flags.
     *
     * Zero replicates nothing, which is a legal and cheap configuration — a server with no clients
     * has nothing to send anyway.
     * */
    u64 replicated_flag;

    /** Refuses connections past this many. Clamped to NYA_NET_MAX_PEERS. Zero means the maximum. */
    u32 max_players;

    /**
     * How often to send snapshots, in ticks. One means every tick.
     *
     * The usual reason to raise it: a sixty hertz simulation does not need sixty snapshots a second
     * per player, and every client interpolates between them anyway. Three is a common choice and
     * cuts upstream bandwidth to a third for a latency cost most players cannot perceive.
     * */
    u32 snapshot_interval_ticks;

    /*
     * ── the game's callbacks ──
     *
     * Handles rather than function pointers, and every one of them has to be built with nya_callback.
     *
     * A server outlives a hot reload: the world, the peers and their open sockets all live in the
     * executable, and only the game DLL is swapped. A raw pointer into that DLL is dangling the moment
     * it is unloaded, so the first command applied after a reload jumped into freed address space. A
     * handle is re-resolved by name after the swap — see nya_callback — and in a shipping build the
     * whole indirection compiles down to the pointer it already was.
     *
     * Pass the bare function name: nya_callback stringizes its argument, so nya_callback(&fn) records
     * the name as "&fn" and nothing can find it again.
     */

    /** NYA_NetSpawnPlayerFn. What a joining player gets to control. */
    NYA_CallbackHandle on_spawn_player;

    /** NYA_NetDespawnPlayerFn. Optional. What happens to that entity when they leave. */
    NYA_CallbackHandle on_despawn_player;

    /** NYA_NetServerEventFn. Optional. Where a client's chat line or action request arrives. */
    NYA_CallbackHandle on_client_event;

    /**
     * NYA_NetApplyCommandFn. How a command becomes movement. Required as soon as anyone connects.
     *
     * Without it the server receives commands and does nothing with them, so every client's
     * prediction is immediately corrected back to a player who never moves.
     * */
    NYA_CallbackHandle on_apply_command;

    /*
     * ── interest management ──
     */

    /**
     * NYA_NetRelevanceFn. Which entities each peer is told about. Unset sends everything to everyone.
     *
     * Takes precedence over `relevance_radius`: a game supplying its own rule is not also asking for
     * the built-in one. Built with nya_callback like the rest — see the note above.
     * */
    NYA_CallbackHandle on_relevance;

    /**
     * The built-in relevance rule: send an entity only within this distance of the player.
     *
     * Zero disables it, which with no `on_relevance` means everything goes to everyone.
     *
     * Distance from the peer's own entity, in world units, compared squared. A spectator with no entity
     * is sent everything, because there is no centre to measure from and showing them nothing is worse
     * than showing them too much.
     *
     * The crude-but-effective version. It knows nothing about walls, rooms or line of sight, so a game
     * with any of those wants `on_relevance` — but it is what turns an unplayable open world into a
     * playable one for one field.
     * */
    f32 relevance_radius;

    /**
     * How much further than `relevance_radius` an entity must travel before it stops being sent.
     *
     * The entity enters at `relevance_radius` and only leaves at `relevance_radius + this`. Without the
     * gap, an entity sitting on the boundary flips between relevant and not on every snapshot — and each
     * flip is a spawn and a despawn on the client, several times a second, for something that barely
     * moved. A player standing near the edge of the radius watches the world strobe.
     *
     * **Zero does not mean off.** It means NYA_NET_RELEVANCE_HYSTERESIS of the radius, because no
     * hysteresis is never the right answer and a default that produces flicker is a trap rather than a
     * choice. Set it explicitly to widen or narrow the band; there is deliberately no way to ask for
     * none.
     *
     * Costs one bit per entity per peer to remember what is currently being sent, which is where the
     * "currently" in NYA_NetRelevanceFn's `currently_relevant` comes from.
     * */
    f32 relevance_hysteresis;

    /*
     * ── bandwidth ──
     */

    /**
     * The most this server will send one peer per second, in bytes. Zero is unlimited.
     *
     * Enforced with a token bucket per peer: a snapshot that does not fit in the remaining budget is
     * **skipped**, not queued. Skipping is the right answer for state — the next snapshot supersedes it
     * anyway — and queueing would build a backlog that only ever grows for a peer that cannot keep up.
     *
     * Reliable messages ignore the cap. They have to: they are the handshake, the roster and the game's
     * own events, and dropping one is not an option the channel offers. So this is a limit on *state*,
     * which is the part that scales with the world.
     *
     * A useful figure is a few tens of kilobytes a second. 64000 is generous for a small world and
     * still under what a poor connection tolerates.
     * */
    u32 bandwidth_bytes_per_second;

    /*
     * ── lag compensation ──
     */

    /**
     * How many ticks of world history to keep for rewinding. Zero disables it.
     *
     * See nya_net_server_rewind_begin. Capped at NYA_NET_LAG_HISTORY; a value below the worst round trip
     * you intend to support makes compensation silently partial, since a peer further behind than the
     * history reaches cannot be rewound to.
     *
     * Costs one snapshot per tick of history, kept for the whole server rather than per peer — so it is
     * a fixed cost, not a per-player one.
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
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Becomes the authority. Does not open a socket.
 *
 * This is what single player calls. The world is simulated, nothing is sent, and nya_net_server_listen
 * can be called later to admit other players without the world noticing.
 * */
NYA_API NYA_Error nya_net_server_start(NYA_NetServerConfig config) __attr_no_discard;

NYA_API void nya_net_server_stop(void);

NYA_API b8 nya_net_server_running(void) __attr_no_discard;

/**
 * Starts accepting players over UDP on `port`.
 *
 * Callable at any time after start, which is what makes "open to LAN" a menu item rather than a
 * restart. Errors if the port cannot be bound — a message a player can act on.
 * */
NYA_API NYA_Error nya_net_server_listen(u16 port) __attr_no_discard;

/** Whether a socket is open. False for single player, true once opened to the LAN. */
NYA_API b8 nya_net_server_is_listening(void) __attr_no_discard;

/**
 * Attaches a local player over a loopback transport, and hands back the client end.
 *
 * What turns a server into a listen server. The client end is passed to nya_net_client_attach; from
 * then on the host plays through exactly the same client code a remote player does, and the only
 * difference is that its transport reports itself local so prediction is skipped.
 * */
NYA_API NYA_Error nya_net_server_attach_local(OUT NYA_NetTransport** out_client_transport) __attr_no_discard;

/** The local player's peer id, or NYA_NET_PEER_NONE on a dedicated server. */
NYA_API NYA_NetPeerId nya_net_server_local_peer(void) __attr_no_discard;

/**
 * Whether this server has no local player — that is, whether it was started with `--server`.
 *
 * What a game checks before creating a window. Not the same question as "is it listening": a
 * dedicated server is always listening, but a listen server is too.
 * */
NYA_API b8 nya_net_server_is_dedicated(void) __attr_no_discard;

/**
 * One tick of networking: drain what arrived, apply commands, send snapshots.
 *
 * Called from the app loop's fixed update, after physics and before the simulation barrier — commands
 * have to move entities before the barrier applies deferred changes, and the snapshot has to describe
 * the world after everything that happened this tick.
 *
 * Returns immediately when there are no remote peers, which is what makes single player free.
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
 *
 * Reliable and ordered, so an event cannot be lost or arrive out of sequence with another. For
 * anything that happens once and cannot be inferred from a later snapshot.
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
 *
 * ## The problem it solves
 *
 * A client shoots at what it sees. What it sees is a snapshot that left the server a round trip ago,
 * interpolated slightly further into the past on top of that. So by the time the shot arrives everyone
 * else has moved — and a player who aimed dead centre misses, at every latency above nothing.
 *
 * The fix is not to make the client aim ahead. It is for the server to test the shot against the world
 * *as that client saw it*: rewind everyone else to the tick the client had applied, resolve, restore.
 *
 * ## What it moves, and what it will not
 *
 * The transforms of replicated entities the shooter does not control. Not the shooter — they predict
 * themselves and are already where they think they are — and **not anything with a physics body**,
 * because the solver owns those and would undo it within the tick.
 *
 * That exclusion is a real limit rather than an oversight: a game whose players are physics bodies
 * cannot be lag compensated this way and wants server-authoritative movement without bodies instead.
 * See the note in gny_net_apply_command.
 *
 * ## Rules
 *
 * Returns false when there is nothing to rewind to — compensation disabled, the peer has acknowledged
 * nothing, or it is further behind than the history reaches. The world is untouched in that case and
 * nya_net_server_rewind_end must **not** be called.
 *
 * Never nest, and always end before the tick does. The world is genuinely in the past between the two
 * calls, so anything else reading it in between sees history rather than the present.
 * */
NYA_API b8 nya_net_server_rewind_begin(NYA_NetPeerId peer) __attr_no_discard;

/** Puts the world back. Only after nya_net_server_rewind_begin returned true. */
NYA_API void nya_net_server_rewind_end(void);

/**
 * How far back the last rewind went, in ticks. Zero when nothing is rewound.
 *
 * For a debug overlay, and for a game that wants to refuse a shot compensated implausibly far — the cheap
 * defence against a client that lies about how far behind it is.
 * */
NYA_API u64 nya_net_server_rewind_ticks(void) __attr_no_discard;

/**
 * The most recent command received from a peer.
 *
 * For a game that wants to read a player's intent outside the movement function — to decide whether
 * they are holding the fire button, say. Zeroed for a peer that has sent nothing.
 * */
NYA_API NYA_NetCommand nya_net_server_last_command(NYA_NetPeerId peer) __attr_no_discard;

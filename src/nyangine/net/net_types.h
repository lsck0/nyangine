/**
 * @file net_types.h
 *
 * What the server, the client and the transports all have to agree on: roles, peers, channels and
 * the limits that size every buffer below.
 *
 * Separate from net.h for the reason core_types.h is separate from core.h — the server names a
 * client, the client names a server, and a transport names neither but carries messages for both.
 * Nothing here depends on anything else in net/.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIMITS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How many clients one server holds at once, the host included.
 *
 * Sizes the peer table, the per-peer snapshot baselines and the command rings, all of which are
 * allocated up front — so this is a memory decision as much as a gameplay one. Thirty-two is what a
 * shared-world game of this kind actually reaches; Factorio and Terraria both sit around here, and
 * the snapshot cost per peer is linear.
 * */
#define NYA_NET_MAX_PEERS 32

/**
 * The largest datagram this engine will put on the wire, headers included.
 *
 * Below the 1500 byte Ethernet MTU by enough to survive the usual tunnelling overheads — PPPoE takes
 * 8, an IPv6 header 40, a VPN more — because a datagram that exceeds the path MTU is fragmented by
 * IP itself, and an IP fragment lost anywhere costs the *whole* datagram. Staying under it means one
 * lost packet is one lost packet.
 *
 * Anything larger than this is split by net_packet.c into fragments the engine tracks itself, which
 * is the same trade made deliberately rather than by the network stack.
 * */
#define NYA_NET_MAX_DATAGRAM 1200

/**
 * How many unacknowledged reliable messages a peer may have outstanding.
 *
 * Reached only when a peer stops acknowledging, which is what a connection dying looks like before
 * the timeout notices. Past this the peer is dropped rather than the queue growing: a client that
 * cannot keep up is gone, and pretending otherwise costs the server memory for a peer that is not
 * coming back.
 * */
#define NYA_NET_MAX_RELIABLE_IN_FLIGHT 256

/** How many ticks of input a client keeps for replay after a correction. See net_client.h. */
#define NYA_NET_COMMAND_HISTORY 128

/**
 * How many snapshots a server keeps per peer to delta against.
 *
 * A delta is computed against the newest snapshot that peer has acknowledged, so this is how far
 * behind a peer may fall before it must be sent a full one. Thirty-two ticks is half a second at
 * sixty hertz, which comfortably covers ordinary jitter and a couple of lost packets.
 * */
#define NYA_NET_SNAPSHOT_HISTORY 32

/** Longest player name accepted, buffer included. */
#define NYA_NET_MAX_NAME 32

/**
 * How many ticks of world history the server can keep for lag compensation.
 *
 * Sixty-four ticks is about a second at sixty hertz, which covers any round trip a game is playable at —
 * a player 500 ms away is already having a bad time for reasons compensation cannot fix. It is a fixed
 * cost for the whole server rather than per peer, since everyone is rewound against the same history.
 * */
#define NYA_NET_LAG_HISTORY 64

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_NetRole      NYA_NetRole;
typedef enum NYA_NetChannel   NYA_NetChannel;
typedef enum NYA_NetDisconnect NYA_NetDisconnect;
typedef struct NYA_NetPeerId  NYA_NetPeerId;

/**
 * What this process is, with respect to the simulation.
 *
 * There is exactly one authoritative world and it belongs to the server. A client has a world too,
 * but it is a *replica*: it is written by snapshots rather than by simulation, except for the one
 * entity the local player predicts. That distinction is the whole architecture, and it is why this
 * is an engine-level concept rather than something a game tracks in a bool.
 *
 * Single player is not a fourth role. It is a server with no remote peers, which is what makes
 * "open to LAN" a runtime call rather than a different build — and what keeps single player on the
 * same code path as hosting, so a bug cannot hide in one and not the other.
 * */
enum NYA_NetRole {
    /**
     * No networking at all. What a tool, a test or an example is.
     *
     * The distinction from SERVER-with-no-peers is that this never starts a socket and never
     * allocates a peer table. A game does not normally use it.
     * */
    NYA_NET_ROLE_NONE = 0,

    /**
     * This process simulates the world and is right about it.
     *
     * Covers single player, "open to LAN" and a dedicated server alike. Whether anyone is listening
     * and whether there is a local player are separate questions — see nya_net_server_listen and
     * nya_net_server_is_dedicated.
     * */
    NYA_NET_ROLE_SERVER,

    /**
     * This process is connected to somebody else's world.
     *
     * It runs no authoritative simulation. It applies snapshots, predicts its own player forward
     * from them, and sends commands describing what that player is trying to do.
     * */
    NYA_NET_ROLE_CLIENT,

    NYA_NET_ROLE_COUNT,
};

/**
 * Which delivery guarantee a message wants.
 *
 * Two, because there are exactly two useful answers and a third would be a knob nobody can set
 * correctly. The choice is about what the data *is*, not about how important it feels: state that
 * is re-sent every tick must never block on a lost packet, and an event that happens once must
 * never be lost.
 * */
enum NYA_NetChannel {
    /**
     * Fire and forget. Dropped, duplicated and reordered packets are all normal.
     *
     * What every snapshot and every input command uses. A snapshot is a complete statement of the
     * world at a tick, so a lost one is superseded a sixteenth of a second later — resending it
     * would deliver stale truth *after* fresher truth, which is worse than the gap. Commands are
     * sent redundantly instead: each packet carries the last several ticks of them, so one lost
     * packet loses nothing.
     * */
    NYA_NET_CHANNEL_UNRELIABLE = 0,

    /**
     * Delivered, in order, exactly once, or the peer is dropped trying.
     *
     * For the things that happen once and cannot be inferred from a later snapshot: the handshake,
     * the world description, a chat line, "this player left". Costs a retransmit timer and holds
     * everything behind it while a lost message is recovered, which is precisely why state does not
     * use it.
     * */
    NYA_NET_CHANNEL_RELIABLE,

    NYA_NET_CHANNEL_COUNT,
};

/** Why a peer is no longer connected. Carried in the disconnect message where there was one to send. */
enum NYA_NetDisconnect {
    NYA_NET_DISCONNECT_NONE = 0,

    /** The peer said so. The ordinary case. */
    NYA_NET_DISCONNECT_REQUESTED,

    /** Nothing heard within the timeout. What a pulled cable and a crashed process both look like. */
    NYA_NET_DISCONNECT_TIMEOUT,

    /** The server is full. */
    NYA_NET_DISCONNECT_FULL,

    /** Protocol or game version mismatch. Refused at the handshake, before any state is exchanged. */
    NYA_NET_DISCONNECT_VERSION,

    /** The peer sent something malformed, or more than it is allowed to. */
    NYA_NET_DISCONNECT_PROTOCOL,

    /** The server shut down. */
    NYA_NET_DISCONNECT_SERVER_CLOSED,

    NYA_NET_DISCONNECT_COUNT,
};

/**
 * Identifies a connected peer for as long as it is connected.
 *
 * Generational, exactly like NYA_EntityHandle and for the same reason: peer slots are reused, and a
 * handle held across a disconnect must fail to resolve rather than silently addressing whoever
 * connected next. A game holding "the peer who owns this entity" across a reconnect is not a
 * hypothetical — it is what a scoreboard does.
 *
 * Slot zero with generation zero is "no peer". On a listen server the host is a real peer with a
 * real id, so nothing has to special case it.
 * */
// @reflect
struct NYA_NetPeerId {
    u32 index;
    u32 generation;
};

#define NYA_NET_PEER_NONE ((NYA_NetPeerId){ .index = 0, .generation = 0 })

/** Whether two peer ids name the same connection. */
NYA_API b8 nya_net_peer_equals(NYA_NetPeerId a, NYA_NetPeerId b) __attr_no_discard;

/** Whether an id names a peer at all. Does not say whether that peer is still connected. */
NYA_API b8 nya_net_peer_is_set(NYA_NetPeerId peer) __attr_no_discard;

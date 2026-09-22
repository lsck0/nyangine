/**
 * @file net_types.h
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
 * */
#define NYA_NET_MAX_PEERS 32

/**
 * The largest datagram this engine will put on the wire, headers included.
 * */
#define NYA_NET_MAX_DATAGRAM 1200

/**
 * How many unacknowledged reliable messages a peer may have outstanding.
 * */
#define NYA_NET_MAX_RELIABLE_IN_FLIGHT 256

/** How many ticks of input a client keeps for replay after a correction. See net_client.h. */
#define NYA_NET_COMMAND_HISTORY 128

/**
 * How many snapshots a server keeps per peer to delta against.
 * */
#define NYA_NET_SNAPSHOT_HISTORY 32

/** Longest player name accepted, buffer included. */
#define NYA_NET_MAX_NAME 32

/**
 * How many ticks of world history the server can keep for lag compensation.
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
 * */
enum NYA_NetRole {
    /**
     * No networking at all. What a tool, a test or an example is.
     * */
    NYA_NET_ROLE_NONE = 0,

    /**
     * This process simulates the world and is right about it.
     * */
    NYA_NET_ROLE_SERVER,

    /**
     * This process is connected to somebody else's world.
     * */
    NYA_NET_ROLE_CLIENT,

    NYA_NET_ROLE_COUNT,
};

/**
 * Which delivery guarantee a message wants.
 * */
enum NYA_NetChannel {
    /**
     * Fire and forget. Dropped, duplicated and reordered packets are all normal.
     * */
    NYA_NET_CHANNEL_UNRELIABLE = 0,

    /**
     * Delivered, in order, exactly once, or the peer is dropped trying.
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

    /** The server could not prove it holds the key the client was told to expect. */
    NYA_NET_DISCONNECT_IDENTITY,

    /** The server counted too many broken rules for movement or actions. */
    NYA_NET_DISCONNECT_CHEATING,

    /** A moderator dropped the player. Not cheating and not the player's own request; see permission.h. */
    NYA_NET_DISCONNECT_KICKED,

    NYA_NET_DISCONNECT_COUNT,
};

/**
 * Identifies a connected peer for as long as it is connected.
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

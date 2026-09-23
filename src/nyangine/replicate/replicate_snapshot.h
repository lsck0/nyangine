/**
 * @file replicate_snapshot.h
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_entity.h"
#include "nyangine/net/net_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_NetEntityState NYA_NetEntityState;
typedef struct NYA_NetSnapshot    NYA_NetSnapshot;
typedef struct NYA_NetReplica     NYA_NetReplica;
typedef struct NYA_NetReplicaSample NYA_NetReplicaSample;
typedef struct NYA_NetReplicaMap  NYA_NetReplicaMap;

/**
 * Bumped whenever the encoding changes in a way an older peer would misread.
 * */
#define NYA_NET_SNAPSHOT_VERSION 2

/** How many replicated entities one snapshot may carry. Sizes the per-snapshot arrays. */
#define NYA_NET_MAX_REPLICATED 2048

/**
 * Fractional bits positions and velocities are sent with when a snapshot does not say: steps of 1/64 of a world unit.
 * A game in metres wants more, one in pixels fewer. See NYA_NetSnapshot.position_bits.
 * */
#define NYA_NET_POSITION_BITS_DEFAULT 6
#define NYA_NET_POSITION_BITS_MAX     16

/**
 * Which fields of an entity differ from its baseline.
 * */
typedef enum {
    NYA_NET_FIELD_POSITION         = 1U << 0,
    NYA_NET_FIELD_ROTATION         = 1U << 1,
    NYA_NET_FIELD_SCALE            = 1U << 2,
    NYA_NET_FIELD_VELOCITY         = 1U << 3,
    NYA_NET_FIELD_ANGULAR_VELOCITY = 1U << 4,
    NYA_NET_FIELD_STATE            = 1U << 5,
    NYA_NET_FIELD_TYPE             = 1U << 6,
    NYA_NET_FIELD_FLAGS            = 1U << 7,

    /** Everything. What a full snapshot stamps on every entity. */
    NYA_NET_FIELD_ALL = 0x00FF,
} NYA_NetField;

/**
 * One replicated entity, as it crosses the wire.
 * */
struct NYA_NetEntityState {
    NYA_EntityHandle handle;

    u32 type;
    u64 flags;
    u32 state;

    f32x3          position;
    NYA_Quaternion rotation;
    f32x3          scale;
    f32x3          velocity;
    f32x3          angular_velocity;
};

/**
 * The replicated world at one tick.
 * */
struct NYA_NetSnapshot {
    /** The server tick this describes. */
    u64 tick;

    /** The tick a decoded snapshot was a delta against, or zero for a whole one. */
    u64 baseline_tick;

    /** The newest of the receiving client's commands the server had applied. What prediction replays from. */
    u64 command_tick;

    /** Fractional bits positions and velocities are sent with. Zero is NYA_NET_POSITION_BITS_DEFAULT. */
    u8 position_bits;

    NYA_NetEntityState* entities;
    u32                 entity_count;
};

/** Snapshots of transform each replica keeps. Enough for the delay a jittery link needs plus one to extrapolate from. */
#define NYA_NET_REPLICA_SAMPLES 4

/** Where a replica was at one server tick. */
struct NYA_NetReplicaSample {
    u64            tick;
    f32x3          position;
    f32x3          velocity;
    NYA_Quaternion rotation;
};

/**
 * One entity, as both sides name it.
 * */
struct NYA_NetReplica {
    /** What the server calls it. The only name that appears on the wire. */
    NYA_EntityHandle remote;

    /** What this process calls it, from its own entity table. */
    NYA_EntityHandle local;

    /** Set by each apply, cleared before it. What the despawn sweep reads. */
    b8 present;

    /** The newest snapshots' transforms, oldest first, so the entity can be drawn at any moment between them. */
    NYA_NetReplicaSample samples[NYA_NET_REPLICA_SAMPLES];
    u32                  sample_count;
};

/**
 * Which local entity stands for which server entity.
 * */
struct NYA_NetReplicaMap {
    NYA_NetReplica entries[NYA_NET_MAX_REPLICATED];
    u32            count;

    /** Which entry holds each server index, plus one, so a snapshot finds its replicas without a search. Zero is none. */
    u16 by_remote_index[NYA_ENTITY_MAX];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Captures every entity carrying `replicated_flag` out of the current world.
 * */
NYA_API NYA_Error nya_net_snapshot_capture(NYA_Arena* arena, u64 flag, u64 tick, OUT NYA_NetSnapshot* out_snapshot) __attr_no_discard;

/**
 * Writes `snapshot` as bytes, sending only what differs from `baseline`.
 * */
NYA_API NYA_Error nya_net_snapshot_encode(NYA_Arena* arena, const NYA_NetSnapshot* snapshot, const NYA_NetSnapshot* baseline, OUT NYA_String* out)
    __attr_no_discard;

/** The tick a payload describes and the baseline it needs, without decoding it. False when even that is malformed. */
NYA_API b8 nya_net_snapshot_peek(const u8* data, u64 size, OUT u64* out_tick, OUT u64* out_baseline_tick) __attr_no_discard;

/**
 * Reads a snapshot back against the baseline its header names, which must be `baseline`.
 * */
NYA_API NYA_Error nya_net_snapshot_decode(NYA_Arena* arena, const u8* data, u64 size, const NYA_NetSnapshot* baseline, OUT NYA_NetSnapshot* out_snapshot)
    __attr_no_discard;

/**
 * Writes `snapshot` into the local world: moves what moved, spawns what is new, despawns what left.
 * */
NYA_API void nya_net_snapshot_apply(const NYA_NetSnapshot* snapshot, u64 flag, NYA_NetReplicaMap* map, NYA_EntityHandle predicted_remote);

/**
 * Forgets every mapping without touching the entities.
 * */
NYA_API void nya_net_replica_map_clear(NYA_NetReplicaMap* map);

/**
 * Despawns every entity the map knows about, then forgets them.
 * */
NYA_API void nya_net_replica_map_despawn_all(NYA_NetReplicaMap* map);

/**
 * The local entity standing for a server entity, or NYA_ENTITY_HANDLE_NONE.
 * */
NYA_API NYA_EntityHandle nya_net_replica_local(const NYA_NetReplicaMap* map, NYA_EntityHandle remote) __attr_no_discard;

/** The reverse: which server entity a local one stands for. NYA_ENTITY_HANDLE_NONE for anything purely local. */
NYA_API NYA_EntityHandle nya_net_replica_remote(const NYA_NetReplicaMap* map, NYA_EntityHandle local) __attr_no_discard;

/**
 * Places every replica where it was at `render_tick`, a fractional server tick: between the two samples around it,
 * or, past the newest, carried on by its velocity for at most `extrapolation_limit_s`. The predicted entity and
 * anything the solver owns are left alone.
 * */
NYA_API void nya_net_replica_interpolate(NYA_NetReplicaMap* map, f64 render_tick, f32 tick_seconds, f32 extrapolation_limit_s, NYA_EntityHandle predicted_remote);

/**
 * Reads one entity's state out of a snapshot. Null when it is not in it.
 * */
NYA_API const NYA_NetEntityState* nya_net_snapshot_find(const NYA_NetSnapshot* snapshot, NYA_EntityHandle handle) __attr_no_discard;

/** Copies a snapshot into `arena`, so it can be kept as a baseline after the tick it came from. */
NYA_API NYA_NetSnapshot nya_net_snapshot_clone(NYA_Arena* arena, const NYA_NetSnapshot* snapshot) __attr_no_discard;

/**
 * Writes `state` onto an entity, field by field.
 * */
NYA_API void nya_net_entity_state_apply(NYA_Entity* entity, const NYA_NetEntityState* state);

/** Whether two states differ at all, and in which fields. Zero means identical. */
NYA_API u16 nya_net_entity_state_diff(const NYA_NetEntityState* from, const NYA_NetEntityState* to) __attr_no_discard;

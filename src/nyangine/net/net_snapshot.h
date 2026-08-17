/**
 * @file net_snapshot.h
 *
 * What the server tells clients about the world, and how little of it it has to send.
 *
 * A snapshot is a **complete statement of the replicated world at one tick**. Not a list of changes —
 * a statement. That is what makes it safe to lose one: the next arrives a sixteenth of a second later
 * and says everything the lost one would have. It is also why snapshots go out unreliably. Resending
 * one would deliver stale truth *after* fresher truth, which is worse than the gap it filled.
 *
 * ## What is replicated, and what is not
 *
 * Only entities the game marks. An entity is replicated when it carries NYA_ENTITY_FLAG_REPLICATED —
 * a flag in the game's own `flags` word, whose bit the game names — because the engine has no way to
 * know that a decorative particle emitter is not worth 40 bytes per tick per player while a crate is.
 *
 * Of a replicated entity, a fixed set of fields: the transform, the motion, the state and type bits,
 * and the game's flags. Not the callbacks (function pointers do not cross a process), not
 * `user_data` (a pointer into the server's memory), not the physics bodies (each side owns its own
 * solver), and not `name` (a literal in the server's binary).
 *
 * ## Delta compression, and what it is against
 *
 * A snapshot is encoded against a **baseline**: the newest snapshot that *this particular client*
 * has acknowledged. Per client, not global — a client on a bad connection is further behind and
 * needs more sent, and one on a good connection should not pay for it.
 *
 * Each entity carries a bitmask of which fields differ from the baseline, and only those follow. An
 * entity identical to its baseline costs its handle and a zero mask. A world where nothing moved
 * costs almost nothing, which is the common case in a building game.
 *
 * When a client has acknowledged nothing — it just joined, or it has been dropping packets long
 * enough that its last acknowledged snapshot has aged out of NYA_NET_SNAPSHOT_HISTORY — the snapshot
 * is encoded against nothing and every field is sent. That is a *full* snapshot, and it is the
 * recovery path as well as the join path.
 *
 * ## Why this is not serde_nya
 *
 * serde_nya is the engine's binary format and it is the right thing for a save file, a config, or the
 * handshake — see net_message.h, which does use it. It works on NYA_Object, which is a hash map, and
 * a snapshot is emitted for every replicated entity for every client for every tick. Building a hash
 * map per entity per tick would put an allocation and a hash where a memcpy belongs.
 *
 * So a snapshot is a flat record stream: fixed field order, explicit little-endian encoding,
 * versioned by NYA_NET_SNAPSHOT_VERSION. Structural messages that happen once keep the nya format,
 * where its self-describing shape earns the cost.
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
typedef struct NYA_NetReplicaMap  NYA_NetReplicaMap;

/**
 * Bumped whenever the encoding changes in a way an older peer would misread.
 *
 * Checked at the handshake, not per snapshot: a mismatch is refused before any state is exchanged,
 * because a client that misparses a snapshot does not fail cleanly — it plays a subtly wrong game.
 * */
#define NYA_NET_SNAPSHOT_VERSION 1

/** How many replicated entities one snapshot may carry. Sizes the per-snapshot arrays. */
#define NYA_NET_MAX_REPLICATED 2048

/**
 * Which fields of an entity differ from its baseline.
 *
 * Grouped rather than one bit per scalar: position is three floats that move together, and spending
 * three bits to say so would cost more than it saves. Sixteen bits, so the mask is two bytes.
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
 *
 * A flat copy rather than a pointer into the entity table, because a snapshot outlives the tick it
 * was taken at — the server keeps NYA_NET_SNAPSHOT_HISTORY of them per client to delta against, and
 * by then the entity may have moved, been despawned, or had its slot reused.
 *
 * The handle carries its generation, which is what makes despawn-and-respawn-into-the-same-slot
 * distinguishable from "the same entity moved". A client that ignored the generation would apply one
 * entity's state to another.
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
 *
 * Entities are held in **handle-index order**, not in the order the entity iterator produced them.
 * Delta encoding pairs an entity against its baseline by index, and a merge over two sorted lists is
 * linear where a lookup per entity would be quadratic — but more importantly, a stable order means
 * two snapshots of an unchanged world are byte-identical, which is what makes "nothing changed" cost
 * nothing.
 * */
struct NYA_NetSnapshot {
    /** The server tick this describes. What a client reconciles its prediction against. */
    u64 tick;

    NYA_NetEntityState* entities;
    u32                 entity_count;
};

/**
 * One entity, as both sides name it.
 *
 * The server's handle and the client's handle for the same thing, which are **not the same number**.
 * See NYA_NetReplicaMap.
 * */
struct NYA_NetReplica {
    /** What the server calls it. The only name that appears on the wire. */
    NYA_EntityHandle remote;

    /** What this process calls it, from its own entity table. */
    NYA_EntityHandle local;

    /** Set by each apply, cleared before it. What the despawn sweep reads. */
    b8 present;

    /*
     * ── interpolation ──
     *
     * Where this entity was at the last two snapshots, so it can be drawn moving between them instead
     * of stepping. See nya_net_replica_interpolate.
     */

    /** The transform the previous snapshot gave, and the one the newest gave. */
    f32x3          from_position;
    NYA_Quaternion from_rotation;
    f32x3          to_position;
    NYA_Quaternion to_rotation;

    /** Which server tick each of the two came from, so the gap between them is known. */
    u64 from_tick;
    u64 to_tick;

    /**
     * How far between `from` and `to` the entity is currently drawn, 0..1.
     *
     * Advanced by nya_net_replica_interpolate every frame and reset by each snapshot. Held here rather
     * than recomputed from a clock so that a frame which runs long does not overshoot — the value is
     * clamped, and clamping a stored number is what stops an entity flying past its destination.
     * */
    f32 alpha;

    /** False until two snapshots have arrived, because one point does not describe motion. */
    b8 can_interpolate;
};

/**
 * Which local entity stands for which server entity.
 *
 * ## Why this has to exist
 *
 * A handle is an index into an entity table plus a generation, and **the two processes have different
 * tables**. The server's entity 7 is not the client's entity 7 — the client's table already holds its
 * own particles, its own UI props, whatever it spawned before connecting, and its free list handed out
 * slots in its own order.
 *
 * So a snapshot names entities in the *server's* handle space, and applying one means translating.
 * Without this map a client cannot even tell "an entity I have already spawned moved" from "a new
 * entity appeared", and the second snapshot spawns a duplicate of everything in the first.
 *
 * A flat array with a linear scan, because it is bounded by NYA_NET_MAX_REPLICATED and walked once per
 * entity per snapshot. A hash map would cost a hash per entity per tick to search what is, in a
 * typical scene, a few dozen entries.
 *
 * ## Not used on a listen server
 *
 * There, the client and the server share one table and one world — the client is reading exactly what
 * the server wrote. It does not apply snapshots at all, so there is nothing to translate. See
 * nya_net_transport_is_local.
 * */
struct NYA_NetReplicaMap {
    NYA_NetReplica entries[NYA_NET_MAX_REPLICATED];
    u32            count;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Captures every entity carrying `replicated_flag` out of the current world.
 *
 * `replicated_flag` is a bit of the game's own NYA_Entity.flags — the engine does not name it,
 * because which entities are worth replicating is a game's decision and not a property the engine
 * could infer. Passing zero replicates nothing, which is what a game with no multiplayer wants and
 * costs one iteration.
 *
 * Allocated in `arena`, sorted by handle index. Reads the world; changes nothing.
 * */
NYA_API NYA_Error nya_net_snapshot_capture(NYA_Arena* arena, u64 flag, u64 tick, OUT NYA_NetSnapshot* out_snapshot) __attr_no_discard;

/**
 * Writes `snapshot` as bytes, sending only what differs from `baseline`.
 *
 * `baseline` may be null, which produces a full snapshot — every field of every entity. That is what
 * a joining client gets, and what a client that has fallen too far behind to have a usable baseline
 * gets as recovery.
 *
 * The result is appended to `out`, so a caller may prepend a message header without a second buffer.
 * */
NYA_API NYA_Error nya_net_snapshot_encode(NYA_Arena* arena, const NYA_NetSnapshot* snapshot, const NYA_NetSnapshot* baseline, OUT NYA_String* out)
    __attr_no_discard;

/**
 * Reads a snapshot back, filling in unchanged fields from `baseline`.
 *
 * `baseline` must be the snapshot the encoder used — the client keeps its own history for exactly
 * this, keyed by the tick the payload names. Handing over the wrong one does not fail loudly; it
 * produces a world subtly out of step, which is why the tick is on the wire rather than implied.
 *
 * NYA_ERROR_INVALID_ARGUMENT on anything malformed, and it is checked rather than trusted: this is
 * the one function in the engine parsing bytes an untrusted peer chose.
 * */
NYA_API NYA_Error nya_net_snapshot_decode(NYA_Arena* arena, const u8* data, u64 size, const NYA_NetSnapshot* baseline, OUT NYA_NetSnapshot* out_snapshot)
    __attr_no_discard;

/**
 * Writes `snapshot` into the local world: moves what moved, spawns what is new, despawns what left.
 *
 * What a client does with every snapshot it receives.
 *
 * `map` translates between the server's handle space and this process's — see NYA_NetReplicaMap, and
 * note that it is *required*: a snapshot names entities the server's way, and without the map the
 * second snapshot spawns a duplicate of everything in the first. It is updated in place, so the same
 * map must be passed every time.
 *
 * A server entity the map has never seen is spawned locally and recorded. A mapped entity the snapshot
 * does not mention is despawned, because the server's statement is complete and its absence is the
 * server saying so.
 *
 * `predicted_remote` is spared, and is a handle in the **server's** space — the one WELCOME carried.
 * An entity the client is predicting is not overwritten here, because prediction is precisely the
 * claim that the client's own answer for it is better than a snapshot a round trip old.
 * Reconciliation handles that one; see net_client.h. Pass NYA_ENTITY_HANDLE_NONE to apply everything.
 *
 * `flag` is stamped on anything spawned, so a later snapshot's despawn sweep and a game's own queries
 * both recognise it.
 * */
NYA_API void nya_net_snapshot_apply(const NYA_NetSnapshot* snapshot, u64 flag, NYA_NetReplicaMap* map, NYA_EntityHandle predicted_remote);

/**
 * Forgets every mapping without touching the entities.
 *
 * For a caller that is about to destroy the world anyway. Anything still mapped becomes an orphan, so
 * a client that intends to keep playing wants nya_net_replica_map_despawn_all instead.
 * */
NYA_API void nya_net_replica_map_clear(NYA_NetReplicaMap* map);

/**
 * Despawns every entity the map knows about, then forgets them.
 *
 * What disconnecting must do. The sweep in nya_net_snapshot_apply only removes what the *server* has
 * stopped mentioning, which is precise — it will not touch an entity the client spawned itself — but it
 * also means nothing removes a replica once the snapshots stop. Without this, reconnecting would leave
 * the previous session's entities standing in the world forever and then spawn a second copy of each.
 *
 * Deferred, so it is safe to call from inside a drain or an iteration.
 * */
NYA_API void nya_net_replica_map_despawn_all(NYA_NetReplicaMap* map);

/**
 * The local entity standing for a server entity, or NYA_ENTITY_HANDLE_NONE.
 *
 * What a client uses to turn the handle WELCOME gave it into something it can actually read — and what
 * a game uses to find the entity a server-side event refers to.
 * */
NYA_API NYA_EntityHandle nya_net_replica_local(const NYA_NetReplicaMap* map, NYA_EntityHandle remote) __attr_no_discard;

/** The reverse: which server entity a local one stands for. NYA_ENTITY_HANDLE_NONE for anything purely local. */
NYA_API NYA_EntityHandle nya_net_replica_remote(const NYA_NetReplicaMap* map, NYA_EntityHandle local) __attr_no_discard;

/**
 * Moves every replica a fraction of the way from the previous snapshot's transform to the newest.
 *
 * ## Why anything is interpolated at all
 *
 * Snapshots arrive at the snapshot rate, which is at best the tick rate and usually a third of it, and
 * a frame renders far more often than that. Writing each snapshot straight onto the entity makes every
 * remote player step: still for three frames, jump, still for three frames, jump. It is the single most
 * visible artefact in a game that does not do this, and it is visible at any latency — including zero.
 *
 * So a replica keeps *two* transforms and is drawn between them. The entity's actual position is
 * therefore always slightly in the past — by about one snapshot interval — which is the trade every
 * game of this kind makes, and which is what every player has always been looking at.
 *
 * ## What is not interpolated
 *
 * The predicted entity. Pass its **server** handle as `predicted_remote` and it is skipped: prediction
 * already puts it exactly where the client believes it is *now*, and interpolating would drag it back
 * toward where the server last said it was. Reconciliation is what corrects that one.
 *
 * Anything with a physics body is also left alone — the solver owns its transform, and writing to it
 * would be undone within the tick.
 *
 * ## Where to call it
 *
 * Once per **frame**, not per tick, because it exists to fill the gaps between ticks. A game calls it
 * from its render path or from a variable-rate update; the engine does not call it, because only the
 * game knows whether it wants smoothing at all.
 * */
NYA_API void nya_net_replica_interpolate(NYA_NetReplicaMap* map, f32 delta_time_s, f32 snapshot_interval_s, NYA_EntityHandle predicted_remote);

/**
 * Reads one entity's state out of a snapshot. Null when it is not in it.
 *
 * What reconciliation asks: "what did the server say I was at tick T". Binary search, since entities
 * are in handle order.
 * */
NYA_API const NYA_NetEntityState* nya_net_snapshot_find(const NYA_NetSnapshot* snapshot, NYA_EntityHandle handle) __attr_no_discard;

/** Copies a snapshot into `arena`, so it can be kept as a baseline after the tick it came from. */
NYA_API NYA_NetSnapshot nya_net_snapshot_clone(NYA_Arena* arena, const NYA_NetSnapshot* snapshot) __attr_no_discard;

/**
 * Writes `state` onto an entity, field by field.
 *
 * Exposed because reconciliation needs it on one entity without applying a whole snapshot. Does not
 * touch the physics body: the solver owns an attached entity's transform, so a snapshot that wrote
 * `position` directly would be overwritten by the next step. See nya_net_snapshot_apply.
 * */
NYA_API void nya_net_entity_state_apply(NYA_Entity* entity, const NYA_NetEntityState* state);

/** Whether two states differ at all, and in which fields. Zero means identical. */
NYA_API u16 nya_net_entity_state_diff(const NYA_NetEntityState* from, const NYA_NetEntityState* to) __attr_no_discard;

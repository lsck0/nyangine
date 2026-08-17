#include "nyangine/nyangine.h"

#include "nyangine/net/net_bytes.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE ENCODING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * A snapshot payload is:
 *
 *     u64 tick
 *     u32 entity_count
 *     then, per entity, in handle-index order:
 *         u32 handle_index
 *         u32 handle_generation
 *         u16 field_mask          which of the fields below follow. Zero means "same as baseline".
 *         then only the fields the mask names, in the fixed order of NYA_NetField:
 *             position          3 × f32
 *             rotation          4 × f32
 *             scale             3 × f32
 *             velocity          3 × f32
 *             angular_velocity  3 × f32
 *             state             u32
 *             type              u32
 *             flags             u64
 *
 * Little endian throughout, written byte by byte, so a big endian host produces identical bytes
 * rather than a stream only its own kind can read. Floats go over as their bit pattern: IEEE 754 is
 * what every platform this targets uses, and reinterpreting is exact where a decimal round trip is
 * not.
 *
 * The handle's *generation* is on the wire, not just its index. An entity despawned and another
 * spawned into the same slot is a different entity, and a receiver that compared only indices would
 * apply the newcomer's state to a stale local copy — which looks like an entity teleporting into
 * somebody else's position rather than like a bug.
 */

#define _NYA_NET_SNAPSHOT_HEADER_SIZE 12
#define _NYA_NET_SNAPSHOT_ENTITY_HEADER_SIZE 10

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Compares two entity states by handle index, for the capture sort. */
NYA_INTERNAL s32 _nya_net_state_compare(const NYA_NetEntityState* a, const NYA_NetEntityState* b);

/**
 * Whether two entity handles name the same entity.
 *
 * Local because core_entity.h offers no such thing — only nya_entity_is_valid, which asks a different
 * question. Both halves are compared: the generation is what distinguishes an entity from whoever
 * next occupies its slot, and comparing indices alone is the bug this exists to avoid.
 * */
NYA_INTERNAL b8 _nya_net_handle_equals(NYA_EntityHandle a, NYA_EntityHandle b) __attr_no_discard;

/**
 * Whether a handle names anything at all.
 *
 * The generation, not the index: slot zero is a perfectly ordinary entity, and only generation zero
 * means "never assigned". Same rule as nya_net_peer_is_set, on the other handle type — which is why
 * this exists rather than reaching for that one with a cast.
 * */
NYA_INTERNAL b8 _nya_net_handle_is_set(NYA_EntityHandle handle) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_snapshot_capture(NYA_Arena* arena, u64 flag, u64 tick, OUT NYA_NetSnapshot* out_snapshot) {
    nya_assert(arena != nullptr);
    nya_assert(out_snapshot != nullptr);

    *out_snapshot = (NYA_NetSnapshot){ .tick = tick };

    // Nothing is replicated when the game names no flag. Not an error: it is what single player and
    // every test that does not care about networking pass, and it costs one walk of the table.
    if (flag == 0) return NYA_OK;

    NYA_NetEntityState* entities = nya_arena_alloc(arena, NYA_NET_MAX_REPLICATED * sizeof(NYA_NetEntityState));
    u32                 count    = 0;

    nya_entity_foreach (entity) {
        if ((entity->flags & flag) == 0) continue;

        /*
         * A despawning entity is captured as gone rather than as present-but-dying.
         *
         * NYA_ENTITY_STATE_DESPAWNING means the barrier will remove it at the end of the tick. Sending
         * it would have every client spawn a copy and then despawn it a tick later — a flicker for
         * something that was never really there.
         */
        if ((entity->state & NYA_ENTITY_STATE_DESPAWNING) != 0) continue;

        if (count >= NYA_NET_MAX_REPLICATED) {
            // Reported once rather than per entity: at this point the world is over budget and the
            // useful information is that it happened, not how many times.
            nya_warn("More than %d replicated entities; the rest are not being sent.", NYA_NET_MAX_REPLICATED);
            break;
        }

        entities[count++] = (NYA_NetEntityState){
            .handle = entity->handle,
            .type   = entity->type,
            .flags  = entity->flags,
            .state  = (u32)entity->state,

            .position         = entity->position,
            .rotation         = entity->rotation,
            .scale            = entity->scale,
            .velocity         = entity->velocity,
            .angular_velocity = entity->angular_velocity,
        };
    }

    /*
     * Sorted by handle index.
     *
     * The iterator is already deterministic — it walks an occupancy bitset low bit first — so this is
     * very nearly sorted already. It is done explicitly anyway because the delta encoder pairs
     * entities against a baseline by walking two lists in step, and that is only correct if both are
     * ordered by the same key. Relying on the iterator's order would make the encoder quietly wrong
     * the day the iterator changed.
     */
    for (u32 i = 1; i < count; i++) {
        NYA_NetEntityState current = entities[i];
        u32                j       = i;

        while (j > 0 && _nya_net_state_compare(&entities[j - 1], &current) > 0) {
            entities[j] = entities[j - 1];
            j--;
        }

        entities[j] = current;
    }

    out_snapshot->entities     = entities;
    out_snapshot->entity_count = count;

    return NYA_OK;
}

NYA_Error nya_net_snapshot_encode(NYA_Arena* arena, const NYA_NetSnapshot* snapshot, const NYA_NetSnapshot* baseline, OUT NYA_String* out) {
    nya_unused(arena);

    nya_assert(snapshot != nullptr);
    nya_assert(out != nullptr);

    _nya_net_write_u64(out, snapshot->tick);
    _nya_net_write_u32(out, snapshot->entity_count);

    /*
     * Both lists are in handle order, so one pass over each finds every pairing.
     *
     * `baseline_at` only ever moves forward: an entity in the baseline that the snapshot no longer
     * has is skipped past, and one the snapshot has that the baseline does not gets a full mask.
     */
    u32 baseline_at = 0;

    for (u32 i = 0; i < snapshot->entity_count; i++) {
        const NYA_NetEntityState* state = &snapshot->entities[i];

        const NYA_NetEntityState* previous = nullptr;

        if (baseline != nullptr) {
            while (baseline_at < baseline->entity_count && baseline->entities[baseline_at].handle.index < state->handle.index) baseline_at++;

            if (baseline_at < baseline->entity_count) {
                const NYA_NetEntityState* candidate = &baseline->entities[baseline_at];

                // The generation has to match too. A slot reused by a different entity is not a
                // baseline for this one, and delta-ing against it would send a mask saying "nothing
                // changed" about an entity the client has never seen.
                if (candidate->handle.index == state->handle.index && candidate->handle.generation == state->handle.generation) previous = candidate;
            }
        }

        u16 mask = previous == nullptr ? (u16)NYA_NET_FIELD_ALL : nya_net_entity_state_diff(previous, state);

        _nya_net_write_u32(out, state->handle.index);
        _nya_net_write_u32(out, state->handle.generation);
        _nya_net_write_u16(out, mask);

        if (mask & NYA_NET_FIELD_POSITION) _nya_net_write_f32x3(out, state->position);

        if (mask & NYA_NET_FIELD_ROTATION) {
            _nya_net_write_f32(out, state->rotation.x);
            _nya_net_write_f32(out, state->rotation.y);
            _nya_net_write_f32(out, state->rotation.z);
            _nya_net_write_f32(out, state->rotation.w);
        }

        if (mask & NYA_NET_FIELD_SCALE) _nya_net_write_f32x3(out, state->scale);
        if (mask & NYA_NET_FIELD_VELOCITY) _nya_net_write_f32x3(out, state->velocity);
        if (mask & NYA_NET_FIELD_ANGULAR_VELOCITY) _nya_net_write_f32x3(out, state->angular_velocity);
        if (mask & NYA_NET_FIELD_STATE) _nya_net_write_u32(out, state->state);
        if (mask & NYA_NET_FIELD_TYPE) _nya_net_write_u32(out, state->type);
        if (mask & NYA_NET_FIELD_FLAGS) _nya_net_write_u64(out, state->flags);
    }

    return NYA_OK;
}

NYA_Error nya_net_snapshot_decode(NYA_Arena* arena, const u8* data, u64 size, const NYA_NetSnapshot* baseline, OUT NYA_NetSnapshot* out_snapshot) {
    nya_assert(arena != nullptr);
    nya_assert(out_snapshot != nullptr);

    *out_snapshot = (NYA_NetSnapshot){ 0 };

    if (data == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no snapshot bytes");

    _NYA_NetReader reader = { .data = data, .size = size };

    u64 tick  = _nya_net_read_u64(&reader);
    u32 count = _nya_net_read_u32(&reader);

    if (reader.failed) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot too short to carry its own header");

    /*
     * The count is checked before it is used to allocate.
     *
     * It came off the wire, so a peer can claim four billion entities. Allocating from it first and
     * validating second is how a malformed packet becomes an out-of-memory abort — and this is the
     * one decoder in the engine reading bytes an untrusted peer chose.
     */
    if (count > NYA_NET_MAX_REPLICATED) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot claiming %u entities, past the %d limit", count, NYA_NET_MAX_REPLICATED);
    }

    // A cheap second bound: even an all-deltas snapshot spends the per-entity header on each one, so
    // a payload shorter than that cannot hold what it claims however small the masks are.
    if (size < _NYA_NET_SNAPSHOT_HEADER_SIZE + ((u64)count * _NYA_NET_SNAPSHOT_ENTITY_HEADER_SIZE)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot claiming %u entities in %llu bytes", count, (unsigned long long)size);
    }

    NYA_NetEntityState* entities = count == 0 ? nullptr : nya_arena_alloc(arena, count * sizeof(NYA_NetEntityState));

    u32 baseline_at = 0;

    /*
     * The order the entities must arrive in, and why it is checked rather than assumed.
     *
     * Both the encoder and this decoder pair an entity against its baseline by walking two sorted lists
     * in step, and `baseline_at` only ever moves *forward*. That is linear rather than quadratic, and it
     * is correct only if the wire order really is ascending by handle index.
     *
     * The encoder guarantees it. A peer is not the encoder. An out-of-order entity would walk
     * `baseline_at` past its own baseline, so it would silently be decoded against the wrong one — or
     * against none — and its unnamed fields would be filled in from a different entity's state. That is
     * not a crash; it is a client quietly shown a wrong world, which is worse.
     *
     * So the order is validated, and a snapshot that breaks it is rejected. Strictly increasing, since
     * one index cannot hold two entities at one instant.
     */
    u32 previous_index = 0;
    b8  have_previous  = false;

    for (u32 i = 0; i < count; i++) {
        u32 index      = _nya_net_read_u32(&reader);
        u32 generation = _nya_net_read_u32(&reader);
        u16 mask       = _nya_net_read_u16(&reader);

        if (reader.failed) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot truncated mid entity");

        if (have_previous && index <= previous_index) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot whose entities are not in ascending handle order (%u after %u)", index,
                             previous_index);
        }

        previous_index = index;
        have_previous  = true;

        /*
         * A generation of zero names nothing. Refused, because it would be recorded in a replica map as
         * a live pairing that nya_net_replica_local could never distinguish from an empty slot.
         */
        if (generation == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot entity with a zero generation");

        /*
         * Unnamed fields come from the baseline, which is the whole point of a delta.
         *
         * A zeroed start rather than the baseline's values would make "unchanged" mean "reset to
         * origin" — every entity that did not move would snap to (0,0,0) with an identity rotation
         * and a scale of nothing.
         */
        NYA_NetEntityState state = { .handle = { .index = index, .generation = generation } };

        if (baseline != nullptr) {
            while (baseline_at < baseline->entity_count && baseline->entities[baseline_at].handle.index < index) baseline_at++;

            if (baseline_at < baseline->entity_count) {
                const NYA_NetEntityState* candidate = &baseline->entities[baseline_at];

                if (candidate->handle.index == index && candidate->handle.generation == generation) {
                    state = *candidate;
                    state.handle = (NYA_EntityHandle){ .index = index, .generation = generation };
                }
            }
        }

        if (mask & NYA_NET_FIELD_POSITION) state.position = _nya_net_read_f32x3(&reader);

        if (mask & NYA_NET_FIELD_ROTATION) {
            state.rotation.x = _nya_net_read_f32(&reader);
            state.rotation.y = _nya_net_read_f32(&reader);
            state.rotation.z = _nya_net_read_f32(&reader);
            state.rotation.w = _nya_net_read_f32(&reader);
        }

        if (mask & NYA_NET_FIELD_SCALE) state.scale = _nya_net_read_f32x3(&reader);
        if (mask & NYA_NET_FIELD_VELOCITY) state.velocity = _nya_net_read_f32x3(&reader);
        if (mask & NYA_NET_FIELD_ANGULAR_VELOCITY) state.angular_velocity = _nya_net_read_f32x3(&reader);
        if (mask & NYA_NET_FIELD_STATE) state.state = _nya_net_read_u32(&reader);
        if (mask & NYA_NET_FIELD_TYPE) state.type = _nya_net_read_u32(&reader);
        if (mask & NYA_NET_FIELD_FLAGS) state.flags = _nya_net_read_u64(&reader);

        if (reader.failed) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot truncated inside entity %u", i);

        entities[i] = state;
    }

    out_snapshot->tick         = tick;
    out_snapshot->entities     = entities;
    out_snapshot->entity_count = count;

    return NYA_OK;
}

void nya_net_snapshot_apply(const NYA_NetSnapshot* snapshot, u64 flag, NYA_NetReplicaMap* map, NYA_EntityHandle predicted_remote) {
    nya_assert(snapshot != nullptr);
    nya_assert(map != nullptr, "applying a snapshot needs a replica map; see NYA_NetReplicaMap");

    // Nothing is present until this snapshot says so. What is left unmarked at the end is what the
    // server has removed.
    for (u32 i = 0; i < map->count; i++) map->entries[i].present = false;

    for (u32 i = 0; i < snapshot->entity_count; i++) {
        const NYA_NetEntityState* state = &snapshot->entities[i];

        NYA_NetReplica* replica = nullptr;

        for (u32 at = 0; at < map->count; at++) {
            if (!_nya_net_handle_equals(map->entries[at].remote, state->handle)) continue;

            replica = &map->entries[at];
            break;
        }

        /*
         * Prediction is spared, and is matched in the *server's* handle space.
         *
         * `predicted_remote` is what WELCOME carried, which is the only name the client and server
         * agree on for this entity. Comparing against the local handle instead would never match,
         * because the two tables number things differently — and the client's own player would be
         * overwritten by a snapshot a round trip old on every single tick.
         */
        b8 is_predicted = _nya_net_handle_equals(state->handle, predicted_remote);

        if (replica != nullptr) {
            replica->present = true;

            if (is_predicted) continue;

            NYA_Entity* entity = nya_entity_get(replica->local);

            /*
             * Mapped, but the local entity is gone — despawned locally, or its slot reused.
             *
             * The mapping is dropped and the entity re-spawned below, rather than trusting a stale
             * handle. Reachable whenever a client despawns something the server still has.
             */
            if (entity == nullptr) {
                replica->remote  = NYA_ENTITY_HANDLE_NONE;
                replica->present = false;
                replica          = nullptr;
            } else {
                /*
                 * The previous target becomes the new origin, and the snapshot becomes the new target.
                 *
                 * Recorded before the state is applied, because `to_*` still holds where the last
                 * snapshot put this entity — which is exactly where the interpolation should start from.
                 * Reading the *entity* instead would start from wherever interpolation had got to, which
                 * compounds the lag a little more with every snapshot.
                 */
                replica->from_position = replica->to_position;
                replica->from_rotation = replica->to_rotation;
                replica->from_tick     = replica->to_tick;

                replica->to_position = state->position;
                replica->to_rotation = state->rotation;
                replica->to_tick     = snapshot->tick;

                replica->alpha           = 0.0F;
                replica->can_interpolate = true;

                nya_net_entity_state_apply(entity, state);
                continue;
            }
        }

        // Never seen. Spawned locally, and the pairing recorded so the next snapshot moves it rather
        // than spawning another.
        if (map->count >= NYA_NET_MAX_REPLICATED) {
            nya_warn("More than %d replicated entities; the rest cannot be tracked.", NYA_NET_MAX_REPLICATED);
            break;
        }

        NYA_EntityHandle spawned = nya_entity_spawn(
            .type     = state->type,
            // The engine's replication flag is forced on rather than taken from the wire alone, so an
            // entity the server replicates is one this client's own sweep will recognise next tick.
            .flags    = state->flags | flag,
            .state    = (NYA_EntityState)state->state,
            .position = state->position,
            .rotation = state->rotation,
            .scale    = state->scale
        );

        if (!nya_entity_is_valid(spawned)) continue;

        NYA_Entity* fresh = nya_entity_get(spawned);
        if (fresh != nullptr) nya_net_entity_state_apply(fresh, state);

        /*
         * Into a slot the sweep above vacated where there is one, so a session that spawns and
         * despawns steadily does not walk the map off its end.
         */
        NYA_NetReplica* slot = nullptr;

        for (u32 at = 0; at < map->count; at++) {
            if (_nya_net_handle_is_set(map->entries[at].remote)) continue;

            slot = &map->entries[at];
            break;
        }

        if (slot == nullptr) slot = &map->entries[map->count++];

        /*
         * A newly spawned replica has one transform, so it cannot be interpolated yet.
         *
         * Both ends are set to where it is, and `can_interpolate` stays false until a second snapshot
         * gives it somewhere to move to. Interpolating from a zeroed origin would have every entity fly
         * in from the world origin on the frame it appeared.
         */
        *slot = (NYA_NetReplica){
            .remote = state->handle,
            .local  = spawned,
            .present = true,

            .from_position = state->position,
            .from_rotation = state->rotation,
            .to_position   = state->position,
            .to_rotation   = state->rotation,
            .from_tick     = snapshot->tick,
            .to_tick       = snapshot->tick,

            .alpha           = 1.0F,
            .can_interpolate = false,
        };
    }

    /*
     * Whatever the snapshot did not mention is gone.
     *
     * A snapshot is a complete statement, so absence is the server saying so. Deferred, because this
     * runs from inside the client's message drain and the barrier is what makes a despawn during
     * iteration safe.
     */
    for (u32 i = 0; i < map->count; i++) {
        NYA_NetReplica* replica = &map->entries[i];

        if (replica->present) continue;
        if (!nya_entity_is_valid(replica->local)) {
            replica->remote = NYA_ENTITY_HANDLE_NONE;
            continue;
        }

        // The predicted entity is never swept: its absence from a snapshot a round trip old is not
        // evidence it is gone.
        if (_nya_net_handle_equals(replica->remote, predicted_remote)) continue;

        nya_entity_despawn_deferred(replica->local);

        *replica = (NYA_NetReplica){ .remote = NYA_ENTITY_HANDLE_NONE, .local = NYA_ENTITY_HANDLE_NONE };
    }
}

void nya_net_replica_map_clear(NYA_NetReplicaMap* map) {
    nya_assert(map != nullptr);

    // Deliberately does not despawn. A caller reconnecting wants a fresh world, and one shutting down
    // is about to destroy the world anyway — despawning here would make the first case impossible to
    // express without spawning everything twice.
    *map = (NYA_NetReplicaMap){ 0 };
}

void nya_net_replica_interpolate(NYA_NetReplicaMap* map, f32 delta_time_s, f32 snapshot_interval_s, NYA_EntityHandle predicted_remote) {
    nya_assert(map != nullptr);

    // A zero or negative interval would divide by nothing. Treated as "no smoothing" rather than
    // asserted, because it is a plausible thing for a game to compute from a snapshot rate of zero.
    if (snapshot_interval_s <= 0.0F || delta_time_s <= 0.0F) return;

    for (u32 i = 0; i < map->count; i++) {
        NYA_NetReplica* replica = &map->entries[i];

        if (!replica->can_interpolate) continue;
        if (!_nya_net_handle_is_set(replica->remote)) continue;

        // Prediction already places this one where the client believes it is now. Interpolating would
        // drag it back toward where the server last said it was — which is the correction reconciliation
        // exists to make deliberately, not something to do every frame.
        if (_nya_net_handle_equals(replica->remote, predicted_remote)) continue;

        NYA_Entity* entity = nya_entity_get(replica->local);
        if (entity == nullptr) continue;

        // The solver owns an attached entity's transform and rewrites it every step, so writing here
        // would be undone within the tick. See the same note in nya_net_entity_state_apply.
        if (nya_physics2d_body_attached(entity)) continue;

        /*
         * Advanced by however much of a snapshot interval this frame was.
         *
         * Clamped at one, so a frame that ran long stops at the target rather than overshooting it — an
         * entity that flies past where the server said it was and then snaps back is worse than one that
         * arrives slightly early and waits.
         */
        replica->alpha += delta_time_s / snapshot_interval_s;
        if (replica->alpha > 1.0F) replica->alpha = 1.0F;

        f32 alpha = replica->alpha;

        entity->position = replica->from_position + ((replica->to_position - replica->from_position) * alpha);

        // Spherical, not component-wise: interpolating a quaternion's four numbers linearly and
        // normalising afterwards takes the short way round but at a varying rate, so a spinning object
        // visibly speeds up and slows down between snapshots.
        entity->rotation = nya_quaternion_slerp(replica->from_rotation, replica->to_rotation, alpha);
    }
}

void nya_net_replica_map_despawn_all(NYA_NetReplicaMap* map) {
    nya_assert(map != nullptr);

    for (u32 i = 0; i < map->count; i++) {
        if (!nya_entity_is_valid(map->entries[i].local)) continue;

        nya_entity_despawn_deferred(map->entries[i].local);
    }

    *map = (NYA_NetReplicaMap){ 0 };
}

NYA_EntityHandle nya_net_replica_local(const NYA_NetReplicaMap* map, NYA_EntityHandle remote) {
    nya_assert(map != nullptr);

    for (u32 i = 0; i < map->count; i++) {
        if (!_nya_net_handle_equals(map->entries[i].remote, remote)) continue;

        return map->entries[i].local;
    }

    return NYA_ENTITY_HANDLE_NONE;
}

NYA_EntityHandle nya_net_replica_remote(const NYA_NetReplicaMap* map, NYA_EntityHandle local) {
    nya_assert(map != nullptr);

    for (u32 i = 0; i < map->count; i++) {
        if (!_nya_net_handle_equals(map->entries[i].local, local)) continue;

        return map->entries[i].remote;
    }

    return NYA_ENTITY_HANDLE_NONE;
}

const NYA_NetEntityState* nya_net_snapshot_find(const NYA_NetSnapshot* snapshot, NYA_EntityHandle handle) {
    nya_assert(snapshot != nullptr);

    // Binary search, because the entities are in handle-index order and this is asked once per
    // replicated entity per snapshot from nya_net_snapshot_apply's despawn pass.
    u32 low  = 0;
    u32 high = snapshot->entity_count;

    while (low < high) {
        u32 middle = low + ((high - low) / 2);

        u32 candidate = snapshot->entities[middle].handle.index;

        if (candidate < handle.index) {
            low = middle + 1;
            continue;
        }

        if (candidate > handle.index) {
            high = middle;
            continue;
        }

        // Index found. The generation still has to match, or this is a different entity that happens
        // to occupy the same slot.
        if (snapshot->entities[middle].handle.generation != handle.generation) return nullptr;

        return &snapshot->entities[middle];
    }

    return nullptr;
}

NYA_NetSnapshot nya_net_snapshot_clone(NYA_Arena* arena, const NYA_NetSnapshot* snapshot) {
    nya_assert(arena != nullptr);
    nya_assert(snapshot != nullptr);

    NYA_NetSnapshot clone = { .tick = snapshot->tick, .entity_count = snapshot->entity_count };

    if (snapshot->entity_count == 0) return clone;

    clone.entities = nya_arena_alloc(arena, snapshot->entity_count * sizeof(NYA_NetEntityState));
    nya_memcpy(clone.entities, snapshot->entities, snapshot->entity_count * sizeof(NYA_NetEntityState));

    return clone;
}

void nya_net_entity_state_apply(NYA_Entity* entity, const NYA_NetEntityState* state) {
    nya_assert(entity != nullptr);
    nya_assert(state != nullptr);

    entity->type  = state->type;
    entity->flags = state->flags;

    // The despawning bit is the local table's business, not the server's: a client mid-despawn must
    // not have that cleared by a snapshot taken before the despawn was requested.
    entity->state = (NYA_EntityState)((state->state & ~(u32)NYA_ENTITY_STATE_DESPAWNING) | (entity->state & NYA_ENTITY_STATE_DESPAWNING));

    entity->scale            = state->scale;
    entity->velocity         = state->velocity;
    entity->angular_velocity = state->angular_velocity;

    /*
     * A bodied entity is moved through the solver, not by assignment.
     *
     * While a body is attached it owns the entity's transform and rewrites it every step, so writing
     * `position` directly is overwritten within the tick — the same footgun the physics headers
     * document at length. nya_physics2d_teleport moves the body, which then moves the entity.
     *
     * Only 2D has a teleport today. A 3D bodied entity therefore replicates its transform but cannot
     * have it forced, which is honest rather than silently broken: on a client the 3D solver would
     * fight the snapshot. The matching nya_physics3d_teleport is the fix and belongs in physics3d.
     */
    if (nya_physics2d_body_attached(entity)) {
        // Yaw only: a 2D body has one rotational degree of freedom, and the quaternion's z/w carry it.
        f32 pitch = 0.0F;
        f32 yaw   = 0.0F;
        f32 roll  = 0.0F;
        nya_quaternion_to_euler(state->rotation, &pitch, &yaw, &roll);

        nya_unused(pitch, yaw);

        /*
         * `roll`, not `yaw`, for a 2D body.
         *
         * The 2D world is the z = 0 plane and a body rotates *about* z — which nya_quaternion_to_euler
         * calls roll, since it names its angles for a 3D convention where yaw is about y. Reading the
         * yaw here would feed the solver an angle that is zero for every rotation a 2D body can have.
         */
        nya_physics2d_teleport(entity, (f32x2){ state->position.x, state->position.y }, roll);
        nya_physics2d_velocity_set(entity, (f32x2){ state->velocity.x, state->velocity.y });

        return;
    }

    entity->position = state->position;
    entity->rotation = state->rotation;
}

u16 nya_net_entity_state_diff(const NYA_NetEntityState* from, const NYA_NetEntityState* to) {
    nya_assert(from != nullptr);
    nya_assert(to != nullptr);

    u16 mask = 0;

    /*
     * Compared exactly, not within a tolerance.
     *
     * A tolerance here would be a lossy channel with no way to recover: an entity drifting by less
     * than epsilon per tick would never be sent, and the client's copy would diverge without bound
     * because each snapshot is delta'd against the *last sent* state rather than against the truth.
     * Quantising the values themselves is the right way to spend fewer bytes, and it belongs in the
     * encoder where the loss is visible.
     */
    /*
     * Compared component by component, never with memcmp over the vector type.
     *
     * `sizeof(f32x3)` is **sixteen**, not twelve: it is an ext_vector_type(3), which x86-64 pads to
     * the next power of two. So a memcmp over it compares four bytes of padding that no code sets and
     * nothing means. Capture copies the whole 16 bytes out of the entity, padding included, while the
     * decoder builds its vectors from three floats with the padding zeroed — so two states holding
     * identical coordinates compared as different, and every entity was sent in full every tick with
     * the delta compression silently doing nothing.
     *
     * It failed in the direction that merely wastes bandwidth. The same comparison in a de-duplicating
     * cache would have failed the other way.
     */
    if (from->position.x != to->position.x || from->position.y != to->position.y || from->position.z != to->position.z) {
        mask |= NYA_NET_FIELD_POSITION;
    }

    if (from->rotation.x != to->rotation.x || from->rotation.y != to->rotation.y || from->rotation.z != to->rotation.z
        || from->rotation.w != to->rotation.w) {
        mask |= NYA_NET_FIELD_ROTATION;
    }

    if (from->scale.x != to->scale.x || from->scale.y != to->scale.y || from->scale.z != to->scale.z) mask |= NYA_NET_FIELD_SCALE;

    if (from->velocity.x != to->velocity.x || from->velocity.y != to->velocity.y || from->velocity.z != to->velocity.z) {
        mask |= NYA_NET_FIELD_VELOCITY;
    }

    if (from->angular_velocity.x != to->angular_velocity.x || from->angular_velocity.y != to->angular_velocity.y
        || from->angular_velocity.z != to->angular_velocity.z) {
        mask |= NYA_NET_FIELD_ANGULAR_VELOCITY;
    }

    if (from->state != to->state) mask |= NYA_NET_FIELD_STATE;
    if (from->type != to->type) mask |= NYA_NET_FIELD_TYPE;
    if (from->flags != to->flags) mask |= NYA_NET_FIELD_FLAGS;

    return mask;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_net_handle_equals(NYA_EntityHandle a, NYA_EntityHandle b) {
    return a.index == b.index && a.generation == b.generation;
}

b8 _nya_net_handle_is_set(NYA_EntityHandle handle) {
    return handle.generation != 0;
}

s32 _nya_net_state_compare(const NYA_NetEntityState* a, const NYA_NetEntityState* b) {
    if (a->handle.index != b->handle.index) return a->handle.index < b->handle.index ? -1 : 1;

    // Two entities cannot occupy one slot at one instant, so this only breaks ties between a snapshot
    // and itself. Ordered by generation anyway, so the sort is total rather than merely consistent.
    if (a->handle.generation != b->handle.generation) return a->handle.generation < b->handle.generation ? -1 : 1;

    return 0;
}

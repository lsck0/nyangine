#include "nyangine/nyangine.h"

#include "nyangine/net/net_bytes.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE ENCODING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * A snapshot payload describes the world at a tick against a baseline the client acknowledged, and says only what
 * changed. Numbers are varints; positions, velocities, scales and angular velocities are fixed point and sent as the
 * difference from the baseline's fixed point value, so a slow object costs a byte an axis:
 *
 * ```
 * tick, baseline gap (0: no baseline), command tick, position bits u8
 * removed count, then each removed index as the gap from the one before
 * changed count, then each changed entity:
 *     index gap, field mask (bit 8: new, with its generation after and every field present)
 *     position    3 signed    rotation u32, smallest three    scale 3 signed    velocity 3 signed
 *     angular velocity 3 signed    state, type, flags
 * ```
 *
 * Entities the baseline has and the payload does not mention are unchanged. The fixed point steps are powers of
 * two, so a value converted back to f32 converts forward to the same integer: client and server derive the
 * baseline's integers independently and always agree.
 */

/** Marks an entity the baseline does not have, in the field mask. */
#define _NYA_NET_SNAPSHOT_NEW (1U << 8)

/** Fixed point bits for the fields whose range does not depend on the game's units. */
#define _NYA_NET_SNAPSHOT_SCALE_BITS   8
#define _NYA_NET_SNAPSHOT_ANGULAR_BITS 10

/** The largest fixed point magnitude on the wire. Anything larger is clamped on the way out and refused on the way in. */
#define _NYA_NET_SNAPSHOT_QUANTIZED_MAX (1LL << 40)

/** Smallest three: the three smaller components lie within ±1/√2, in 10 bits each. */
#define _NYA_NET_ROTATION_STEPS 511.0F
#define _NYA_NET_ROTATION_RANGE 0.70710678F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Compares two entity states by handle index, for the capture sort. */
NYA_INTERNAL s32 _nya_net_state_compare(const NYA_NetEntityState* a, const NYA_NetEntityState* b);

/**
 * Whether two entity handles name the same entity.
 * */
NYA_INTERNAL b8 _nya_net_handle_equals(NYA_EntityHandle a, NYA_EntityHandle b) __attr_no_discard;

/**
 * Whether a handle names anything at all.
 * */
NYA_INTERNAL b8 _nya_net_handle_is_set(NYA_EntityHandle handle) __attr_no_discard;

/** `value` in fixed point with `bits` fractional bits. Not finite is zero. */
NYA_INTERNAL s64 _nya_net_quantize(f32 value, u32 bits) __attr_no_discard;
NYA_INTERNAL f32 _nya_net_dequantize(s64 value, u32 bits) __attr_no_discard;

/** A unit quaternion in 32 bits: which component is largest, and the other three. */
NYA_INTERNAL u32            _nya_net_rotation_pack(NYA_Quaternion rotation) __attr_no_discard;
NYA_INTERNAL NYA_Quaternion _nya_net_rotation_unpack(u32 packed) __attr_no_discard;

/** Which fields differ once both states are on the wire's fixed point grid. */
NYA_INTERNAL u16 _nya_net_snapshot_diff_quantized(const NYA_NetEntityState* from, const NYA_NetEntityState* to, u32 bits) __attr_no_discard;

/** Writes the fields `mask` names, the fixed point ones as differences from `base`. */
NYA_INTERNAL void _nya_net_snapshot_write_fields(NYA_String* out, const NYA_NetEntityState* state, const NYA_NetEntityState* base, u16 mask, u32 bits);

/** Reads the fields `mask` names onto `state`, which holds the base on entry. False when the payload is malformed. */
NYA_INTERNAL b8 _nya_net_snapshot_read_fields(_NYA_NetReader* reader, NYA_NetEntityState* state, u16 mask, u32 bits) __attr_no_discard;

NYA_INTERNAL void _nya_net_snapshot_write_vector(NYA_String* out, f32x3 value, f32x3 base, u32 bits);
NYA_INTERNAL b8   _nya_net_snapshot_read_vector(_NYA_NetReader* reader, f32x3* value, u32 bits) __attr_no_discard;

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
         */
        if ((entity->state & NYA_ENTITY_STATE_DESPAWNING) != 0) continue;

        if (count >= NYA_NET_MAX_REPLICATED) {
            // Reported once rather than per entity: at this point the world is over budget and the
            // useful information is that it happened, not how many times.
            nya_log_warn("More than %d replicated entities; the rest are not being sent.", NYA_NET_MAX_REPLICATED);
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
    nya_trace_scope(NYA_TRACE_NET_ENCODE);

    nya_assert(arena != nullptr);
    nya_assert(snapshot != nullptr);
    nya_assert(out != nullptr);
    nya_assert(snapshot->entity_count <= NYA_NET_MAX_REPLICATED);

    u32 bits = snapshot->position_bits == 0 ? NYA_NET_POSITION_BITS_DEFAULT : snapshot->position_bits;
    if (bits > NYA_NET_POSITION_BITS_MAX) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%u position bits, past the %d limit", bits, NYA_NET_POSITION_BITS_MAX);

    // a baseline has to be older than what is described against it, or the gap on the wire means nothing.
    if (baseline != nullptr && baseline->tick >= snapshot->tick) baseline = nullptr;

    _nya_net_write_varint(out, snapshot->tick);
    _nya_net_write_varint(out, baseline == nullptr ? 0 : snapshot->tick - baseline->tick);
    _nya_net_write_varint(out, snapshot->command_tick);
    nya_string_push_back(out, (u8)bits);

    u32 baseline_count = baseline == nullptr ? 0 : baseline->entity_count;

    /*
     * Both lists are in handle order, so one walk pairs every entity with its baseline. Whatever the baseline had at
     * an index the snapshot no longer has is removed; a different generation at the same index is a new entity.
     */
    u32  removed_count = 0;
    u32* removed       = baseline_count == 0 ? nullptr : nya_arena_alloc(arena, (u64)baseline_count * sizeof(u32));

    for (u32 at = 0, from = 0; from < baseline_count; from++) {
        u32 index = baseline->entities[from].handle.index;

        while (at < snapshot->entity_count && snapshot->entities[at].handle.index < index) at++;

        if (at < snapshot->entity_count && snapshot->entities[at].handle.index == index) continue;

        removed[removed_count++] = index;
    }

    u16* masks         = snapshot->entity_count == 0 ? nullptr : nya_arena_alloc(arena, (u64)snapshot->entity_count * sizeof(u16));
    u32  changed_count = 0;

    for (u32 i = 0, from = 0; i < snapshot->entity_count; i++) {
        const NYA_NetEntityState* state = &snapshot->entities[i];

        while (from < baseline_count && baseline->entities[from].handle.index < state->handle.index) from++;

        const NYA_NetEntityState* previous = nullptr;

        if (from < baseline_count && baseline->entities[from].handle.index == state->handle.index
            && baseline->entities[from].handle.generation == state->handle.generation) {
            previous = &baseline->entities[from];
        }

        masks[i] = previous == nullptr ? (u16)(_NYA_NET_SNAPSHOT_NEW | NYA_NET_FIELD_ALL) : _nya_net_snapshot_diff_quantized(previous, state, bits);

        if (masks[i] != 0) changed_count++;
    }

    _nya_net_write_varint(out, removed_count);

    for (u32 i = 0, next = 0; i < removed_count; i++) {
        _nya_net_write_varint(out, removed[i] - next);
        next = removed[i] + 1;
    }

    _nya_net_write_varint(out, changed_count);

    for (u32 i = 0, next = 0, from = 0; i < snapshot->entity_count; i++) {
        if (masks[i] == 0) continue;

        const NYA_NetEntityState* state = &snapshot->entities[i];

        _nya_net_write_varint(out, state->handle.index - next);
        next = state->handle.index + 1;

        _nya_net_write_varint(out, masks[i]);

        NYA_NetEntityState base = { .scale = { 0.0F, 0.0F, 0.0F } };

        if ((masks[i] & _NYA_NET_SNAPSHOT_NEW) != 0) {
            _nya_net_write_varint(out, state->handle.generation);
        } else {
            while (from < baseline_count && baseline->entities[from].handle.index < state->handle.index) from++;
            base = baseline->entities[from];
        }

        _nya_net_snapshot_write_fields(out, state, &base, masks[i], bits);
    }

    return NYA_OK;
}

b8 nya_net_snapshot_peek(const u8* data, u64 size, OUT u64* out_tick, OUT u64* out_baseline_tick) {
    nya_assert(out_tick != nullptr);
    nya_assert(out_baseline_tick != nullptr);

    *out_tick          = 0;
    *out_baseline_tick = 0;

    if (data == nullptr) return false;

    _NYA_NetReader reader = { .data = data, .size = size };

    u64 tick = _nya_net_read_varint(&reader);
    u64 gap  = _nya_net_read_varint(&reader);

    if (reader.failed || gap > tick) return false;

    *out_tick          = tick;
    *out_baseline_tick = gap == 0 ? 0 : tick - gap;

    return true;
}

NYA_Error nya_net_snapshot_decode(NYA_Arena* arena, const u8* data, u64 size, const NYA_NetSnapshot* baseline, OUT NYA_NetSnapshot* out_snapshot) {
    nya_trace_scope(NYA_TRACE_NET_DECODE);

    nya_assert(arena != nullptr);
    nya_assert(out_snapshot != nullptr);

    *out_snapshot = (NYA_NetSnapshot){ 0 };

    if (data == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no snapshot bytes");

    _NYA_NetReader reader = { .data = data, .size = size };

    u64 tick         = _nya_net_read_varint(&reader);
    u64 gap          = _nya_net_read_varint(&reader);
    u64 command_tick = _nya_net_read_varint(&reader);
    u8  bits         = _nya_net_read_u8(&reader);
    u64 removed      = _nya_net_read_varint(&reader);

    if (reader.failed) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot too short to carry its own header");
    if (bits > NYA_NET_POSITION_BITS_MAX) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot with %u position bits", bits);
    if (gap > tick) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot whose baseline is before tick zero");

    // a delta is only meaningful against exactly the snapshot it was made from.
    if (gap == 0) baseline = nullptr;
    else if (baseline == nullptr || baseline->tick != tick - gap) return nya_error(NYA_ERROR_NOT_FOUND, "a snapshot delta against tick %llu, which is not the baseline given", (unsigned long long)(tick - gap));

    u32 baseline_count = baseline == nullptr ? 0 : baseline->entity_count;

    // counts are checked before they size anything: a removal names a baseline entity, and every entry costs a byte.
    if (removed > baseline_count || removed > size - reader.at) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot removing %llu of %u entities", (unsigned long long)removed, baseline_count);

    // baseline order is what the walks below depend on, and a baseline is only ever something this decoder produced.
    u32* removed_indices = removed == 0 ? nullptr : nya_arena_alloc(arena, removed * sizeof(u32));

    for (u64 i = 0, next = 0; i < removed; i++) {
        u64 index = next + _nya_net_read_varint(&reader);

        if (reader.failed || index >= NYA_ENTITY_MAX) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot removal past the entity table");

        removed_indices[i] = (u32)index;
        next               = index + 1;
    }

    u64 changed = _nya_net_read_varint(&reader);

    if (reader.failed || changed > NYA_NET_MAX_REPLICATED || changed * 2 > size - reader.at) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot claiming %llu changed entities in %llu bytes", (unsigned long long)changed, (unsigned long long)size);
    }

    u64                 capacity = (u64)baseline_count + changed;
    NYA_NetEntityState* entities = capacity == 0 ? nullptr : nya_arena_alloc(arena, capacity * sizeof(NYA_NetEntityState));
    u32                 count    = 0;

    u32 from        = 0;
    u32 removed_at  = 0;
    u64 next        = 0;

    /*
     * One merge: baseline entities are copied through up to each changed index, skipping removals, then the changed
     * entity replaces or joins them. Indices only ever ascend, which rules out duplicates.
     */
    for (u64 i = 0; i <= changed; i++) {
        u64 index = NYA_ENTITY_MAX;
        u16 mask  = 0;

        if (i < changed) {
            index = next + _nya_net_read_varint(&reader);
            mask  = (u16)nya_min(_nya_net_read_varint(&reader), (u64)U16_MAX);

            if (reader.failed || index >= NYA_ENTITY_MAX) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot entity past the entity table");
            if ((mask & ~(u16)(_NYA_NET_SNAPSHOT_NEW | NYA_NET_FIELD_ALL)) != 0 || mask == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot entity with mask %u", mask);

            next = index + 1;
        }

        for (; from < baseline_count && baseline->entities[from].handle.index <= index; from++) {
            const NYA_NetEntityState* kept = &baseline->entities[from];

            while (removed_at < removed && removed_indices[removed_at] < kept->handle.index) removed_at++;

            b8 is_removed  = removed_at < removed && removed_indices[removed_at] == kept->handle.index;
            b8 is_replaced = kept->handle.index == index;

            if (is_removed && is_replaced) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot both removing and describing entity %llu", (unsigned long long)index);
            if (is_removed || (is_replaced && (mask & _NYA_NET_SNAPSHOT_NEW) != 0)) continue;
            if (is_replaced) break;

            entities[count++] = *kept;
        }

        if (i == changed) break;

        NYA_NetEntityState base = { 0 };

        if ((mask & _NYA_NET_SNAPSHOT_NEW) != 0) {
            u64 generation = _nya_net_read_varint(&reader);

            // a new entity arrives whole, and a generation of zero names nothing.
            if (mask != (_NYA_NET_SNAPSHOT_NEW | NYA_NET_FIELD_ALL)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a new snapshot entity without every field");
            if (generation == 0 || generation > U32_MAX) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot entity with generation %llu", (unsigned long long)generation);

            base.handle = (NYA_EntityHandle){ .index = (u32)index, .generation = (u32)generation };
        } else {
            if (from >= baseline_count || baseline->entities[from].handle.index != index) {
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot changing entity %llu, which the baseline does not have", (unsigned long long)index);
            }

            base = baseline->entities[from++];
        }

        if (!_nya_net_snapshot_read_fields(&reader, &base, mask, bits)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot truncated inside entity %llu", (unsigned long long)index);

        entities[count++] = base;
    }

    if (reader.failed || reader.at != size) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot with %llu bytes past its end", (unsigned long long)(size - reader.at));
    if (count > NYA_NET_MAX_REPLICATED) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a snapshot of %u entities, past the %d limit", count, NYA_NET_MAX_REPLICATED);

    *out_snapshot = (NYA_NetSnapshot){
        .tick          = tick,
        .baseline_tick = gap == 0 ? 0 : tick - gap,
        .command_tick  = command_tick,
        .position_bits = bits,
        .entities      = entities,
        .entity_count  = count,
    };

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

        // the decoder bounds every index, but a snapshot built by hand has not been through it.
        if (state->handle.index >= NYA_ENTITY_MAX) continue;

        NYA_NetReplica* replica = nullptr;

        u16 entry = map->by_remote_index[state->handle.index];

        if (entry != 0 && _nya_net_handle_equals(map->entries[entry - 1].remote, state->handle)) replica = &map->entries[entry - 1];

        /*
         * Prediction is spared, and is matched in the *server's* handle space.
         */
        b8 is_predicted = _nya_net_handle_equals(state->handle, predicted_remote);

        if (replica != nullptr) {
            replica->present = true;

            if (is_predicted) continue;

            NYA_Entity* entity = nya_entity_get(replica->local);

            /* Mapped, but the local entity is gone: despawned locally or its slot reused. */
            if (entity == nullptr) {
                map->by_remote_index[replica->remote.index] = 0;
                replica->remote  = NYA_ENTITY_HANDLE_NONE;
                replica->present = false;
                replica          = nullptr;
            } else {
                // a snapshot older than the newest sample is a late arrival; the samples stay in tick order without it.
                if (snapshot->tick > replica->samples[replica->sample_count - 1].tick) {
                    if (replica->sample_count == NYA_NET_REPLICA_SAMPLES) {
                        nya_memmove(replica->samples, replica->samples + 1, (NYA_NET_REPLICA_SAMPLES - 1) * sizeof(NYA_NetReplicaSample));
                        replica->sample_count--;
                    }

                    replica->samples[replica->sample_count++] = (NYA_NetReplicaSample){
                        .tick = snapshot->tick, .position = state->position, .velocity = state->velocity, .rotation = state->rotation,
                    };
                }

                nya_net_entity_state_apply(entity, state);
                continue;
            }
        }

        // Never seen. Spawned locally, and the pairing recorded so the next snapshot moves it rather
        // than spawning another.
        if (map->count >= NYA_NET_MAX_REPLICATED) {
            nya_log_warn("More than %d replicated entities; the rest cannot be tracked.", NYA_NET_MAX_REPLICATED);
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

        // a previous occupant of this server index, with an older generation, is replaced here.
        map->by_remote_index[state->handle.index] = (u16)(slot - map->entries + 1);

        *slot = (NYA_NetReplica){
            .remote       = state->handle,
            .local        = spawned,
            .present      = true,
            .samples      = { { .tick = snapshot->tick, .position = state->position, .velocity = state->velocity, .rotation = state->rotation } },
            .sample_count = 1,
        };
    }

    /*
     * Whatever the snapshot did not mention is gone.
     */
    for (u32 i = 0; i < map->count; i++) {
        NYA_NetReplica* replica = &map->entries[i];

        if (replica->present || !_nya_net_handle_is_set(replica->remote)) continue;

        if (!nya_entity_is_valid(replica->local)) {
            if (map->by_remote_index[replica->remote.index] == i + 1) map->by_remote_index[replica->remote.index] = 0;
            replica->remote = NYA_ENTITY_HANDLE_NONE;
            continue;
        }

        // The predicted entity is never swept: its absence from a snapshot a round trip old is not
        // evidence it is gone.
        if (_nya_net_handle_equals(replica->remote, predicted_remote)) continue;

        nya_entity_despawn_deferred(replica->local);

        if (map->by_remote_index[replica->remote.index] == i + 1) map->by_remote_index[replica->remote.index] = 0;

        *replica = (NYA_NetReplica){ .remote = NYA_ENTITY_HANDLE_NONE, .local = NYA_ENTITY_HANDLE_NONE };
    }
}

void nya_net_replica_map_clear(NYA_NetReplicaMap* map) {
    nya_assert(map != nullptr);

    // does not despawn. A reconnecting caller wants a fresh world, and one shutting down destroys the
    // world anyway.
    *map = (NYA_NetReplicaMap){ 0 };
}

void nya_net_replica_interpolate(NYA_NetReplicaMap* map, f64 render_tick, f32 tick_seconds, f32 extrapolation_limit_s, NYA_EntityHandle predicted_remote) {
    nya_assert(map != nullptr);

    if (tick_seconds <= 0.0F || !isfinite(render_tick)) return;

    f64 limit_ticks = nya_max((f64)extrapolation_limit_s, 0.0) / (f64)tick_seconds;

    for (u32 i = 0; i < map->count; i++) {
        NYA_NetReplica* replica = &map->entries[i];

        if (replica->sample_count == 0 || !_nya_net_handle_is_set(replica->remote)) continue;

        // prediction already places this one. Interpolating would drag it back toward the last server position.
        if (_nya_net_handle_equals(replica->remote, predicted_remote)) continue;

        NYA_Entity* entity = nya_entity_get(replica->local);
        if (entity == nullptr) continue;

        // the solver owns an attached entity's transform and rewrites it every step. See nya_net_entity_state_apply.
        if (nya_physics2d_body_attached(entity)) continue;

        const NYA_NetReplicaSample* newest = &replica->samples[replica->sample_count - 1];
        const NYA_NetReplicaSample* oldest = &replica->samples[0];

        if (render_tick >= (f64)newest->tick) {
            // past what has arrived: carried on for a little, then held, since a guess that runs on is worse than a pause.
            f64 ahead = nya_min(render_tick - (f64)newest->tick, limit_ticks);

            entity->position = newest->position + (newest->velocity * (f32)(ahead * (f64)tick_seconds));
            entity->rotation = newest->rotation;
        } else if (render_tick <= (f64)oldest->tick) {
            entity->position = oldest->position;
            entity->rotation = oldest->rotation;
        } else {
            u32 later = 1;
            while (later < replica->sample_count - 1 && (f64)replica->samples[later].tick <= render_tick) later++;

            const NYA_NetReplicaSample* from = &replica->samples[later - 1];
            const NYA_NetReplicaSample* to   = &replica->samples[later];

            f32 alpha = (f32)((render_tick - (f64)from->tick) / (f64)(to->tick - from->tick));

            entity->position = from->position + ((to->position - from->position) * alpha);

            // spherical, so a spinning object turns at an even rate between snapshots.
            entity->rotation = nya_quaternion_slerp(from->rotation, to->rotation, alpha);
        }

        // already placed for this frame, so drawn as written rather than blended with the previous tick.
        nya_entity_transform_snap(entity);
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

    if (remote.index >= NYA_ENTITY_MAX) return NYA_ENTITY_HANDLE_NONE;

    u16 entry = map->by_remote_index[remote.index];
    if (entry == 0 || !_nya_net_handle_equals(map->entries[entry - 1].remote, remote)) return NYA_ENTITY_HANDLE_NONE;

    return map->entries[entry - 1].local;
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
     */
    /*
     * Compared component by component, never with memcmp over the vector type.
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

s64 _nya_net_quantize(f32 value, u32 bits) {
    if (!isfinite(value)) return 0;

    f64 scaled = nya_clamp((f64)value * (f64)(1ULL << bits), -(f64)_NYA_NET_SNAPSHOT_QUANTIZED_MAX, (f64)_NYA_NET_SNAPSHOT_QUANTIZED_MAX);

    return (s64)llround(scaled);
}

f32 _nya_net_dequantize(s64 value, u32 bits) {
    return (f32)((f64)value / (f64)(1ULL << bits));
}

u32 _nya_net_rotation_pack(NYA_Quaternion rotation) {
    f32 components[4] = { rotation.x, rotation.y, rotation.z, rotation.w };

    f32 length = sqrtf((rotation.x * rotation.x) + (rotation.y * rotation.y) + (rotation.z * rotation.z) + (rotation.w * rotation.w));

    // not a rotation at all: sent as the identity rather than as whatever the arithmetic below would make of it.
    if (!isfinite(length) || length < 1e-6F) return _nya_net_rotation_pack(nya_quaternion_identity);

    u32 largest = 3;
    for (u32 i = 0; i < 4; i++) {
        if (fabsf(components[i]) > fabsf(components[largest])) largest = i;
    }

    // q and -q are the same rotation, so the largest is made positive and need not be sent at all.
    f32 sign   = components[largest] < 0.0F ? -1.0F : 1.0F;
    u32 packed = largest << 30;
    u32 slot   = 0;

    for (u32 i = 0; i < 4; i++) {
        if (i == largest) continue;

        f32 value = components[i] * sign / length;
        s32 steps = (s32)lroundf(nya_clamp(value / _NYA_NET_ROTATION_RANGE, -1.0F, 1.0F) * _NYA_NET_ROTATION_STEPS);

        packed |= (u32)(steps + 511) << (20 - (slot * 10));
        slot++;
    }

    return packed;
}

NYA_Quaternion _nya_net_rotation_unpack(u32 packed) {
    f32 components[4] = { 0 };

    u32 largest = packed >> 30;
    u32 slot    = 0;
    f32 sum     = 0.0F;

    for (u32 i = 0; i < 4; i++) {
        if (i == largest) continue;

        s32 steps = (s32)((packed >> (20 - (slot * 10))) & 0x3FF) - 511;

        components[i] = ((f32)steps / _NYA_NET_ROTATION_STEPS) * _NYA_NET_ROTATION_RANGE;
        sum          += components[i] * components[i];
        slot++;
    }

    components[largest] = sqrtf(nya_max(1.0F - sum, 0.0F));

    // a hostile 1023 in every slot sums past one; normalising keeps even that a rotation.
    return nya_quaternion_normalize((NYA_Quaternion){ components[0], components[1], components[2], components[3] });
}

u16 _nya_net_snapshot_diff_quantized(const NYA_NetEntityState* from, const NYA_NetEntityState* to, u32 bits) {
    u16 mask = 0;

    for (u32 axis = 0; axis < 3; axis++) {
        if (_nya_net_quantize(from->position[axis], bits) != _nya_net_quantize(to->position[axis], bits)) mask |= NYA_NET_FIELD_POSITION;
        if (_nya_net_quantize(from->scale[axis], _NYA_NET_SNAPSHOT_SCALE_BITS) != _nya_net_quantize(to->scale[axis], _NYA_NET_SNAPSHOT_SCALE_BITS)) mask |= NYA_NET_FIELD_SCALE;
        if (_nya_net_quantize(from->velocity[axis], bits) != _nya_net_quantize(to->velocity[axis], bits)) mask |= NYA_NET_FIELD_VELOCITY;

        if (_nya_net_quantize(from->angular_velocity[axis], _NYA_NET_SNAPSHOT_ANGULAR_BITS) != _nya_net_quantize(to->angular_velocity[axis], _NYA_NET_SNAPSHOT_ANGULAR_BITS)) {
            mask |= NYA_NET_FIELD_ANGULAR_VELOCITY;
        }
    }

    if (_nya_net_rotation_pack(from->rotation) != _nya_net_rotation_pack(to->rotation)) mask |= NYA_NET_FIELD_ROTATION;

    if (from->state != to->state) mask |= NYA_NET_FIELD_STATE;
    if (from->type != to->type) mask |= NYA_NET_FIELD_TYPE;
    if (from->flags != to->flags) mask |= NYA_NET_FIELD_FLAGS;

    return mask;
}

void _nya_net_snapshot_write_vector(NYA_String* out, f32x3 value, f32x3 base, u32 bits) {
    for (u32 axis = 0; axis < 3; axis++) _nya_net_write_signed(out, _nya_net_quantize(value[axis], bits) - _nya_net_quantize(base[axis], bits));
}

b8 _nya_net_snapshot_read_vector(_NYA_NetReader* reader, f32x3* value, u32 bits) {
    for (u32 axis = 0; axis < 3; axis++) {
        s64 delta = _nya_net_read_signed(reader);

        // bounded before the addition, so neither it nor the result can overflow.
        if (reader->failed || delta > 2 * _NYA_NET_SNAPSHOT_QUANTIZED_MAX || delta < -2 * _NYA_NET_SNAPSHOT_QUANTIZED_MAX) return false;

        s64 quantized = _nya_net_quantize((*value)[axis], bits) + delta;

        if (quantized > _NYA_NET_SNAPSHOT_QUANTIZED_MAX || quantized < -_NYA_NET_SNAPSHOT_QUANTIZED_MAX) return false;

        (*value)[axis] = _nya_net_dequantize(quantized, bits);
    }

    return true;
}

void _nya_net_snapshot_write_fields(NYA_String* out, const NYA_NetEntityState* state, const NYA_NetEntityState* base, u16 mask, u32 bits) {
    if (mask & NYA_NET_FIELD_POSITION) _nya_net_snapshot_write_vector(out, state->position, base->position, bits);
    if (mask & NYA_NET_FIELD_ROTATION) _nya_net_write_u32(out, _nya_net_rotation_pack(state->rotation));
    if (mask & NYA_NET_FIELD_SCALE) _nya_net_snapshot_write_vector(out, state->scale, base->scale, _NYA_NET_SNAPSHOT_SCALE_BITS);
    if (mask & NYA_NET_FIELD_VELOCITY) _nya_net_snapshot_write_vector(out, state->velocity, base->velocity, bits);
    if (mask & NYA_NET_FIELD_ANGULAR_VELOCITY) _nya_net_snapshot_write_vector(out, state->angular_velocity, base->angular_velocity, _NYA_NET_SNAPSHOT_ANGULAR_BITS);
    if (mask & NYA_NET_FIELD_STATE) _nya_net_write_varint(out, state->state);
    if (mask & NYA_NET_FIELD_TYPE) _nya_net_write_varint(out, state->type);
    if (mask & NYA_NET_FIELD_FLAGS) _nya_net_write_varint(out, state->flags);
}

b8 _nya_net_snapshot_read_fields(_NYA_NetReader* reader, NYA_NetEntityState* state, u16 mask, u32 bits) {
    if ((mask & NYA_NET_FIELD_POSITION) && !_nya_net_snapshot_read_vector(reader, &state->position, bits)) return false;
    if (mask & NYA_NET_FIELD_ROTATION) state->rotation = _nya_net_rotation_unpack(_nya_net_read_u32(reader));
    if ((mask & NYA_NET_FIELD_SCALE) && !_nya_net_snapshot_read_vector(reader, &state->scale, _NYA_NET_SNAPSHOT_SCALE_BITS)) return false;
    if ((mask & NYA_NET_FIELD_VELOCITY) && !_nya_net_snapshot_read_vector(reader, &state->velocity, bits)) return false;
    if ((mask & NYA_NET_FIELD_ANGULAR_VELOCITY) && !_nya_net_snapshot_read_vector(reader, &state->angular_velocity, _NYA_NET_SNAPSHOT_ANGULAR_BITS)) return false;

    if (mask & NYA_NET_FIELD_STATE) {
        u64 value = _nya_net_read_varint(reader);
        if (value > U32_MAX) return false;
        state->state = (u32)value;
    }

    if (mask & NYA_NET_FIELD_TYPE) {
        u64 value = _nya_net_read_varint(reader);
        if (value > U32_MAX) return false;
        state->type = (u32)value;
    }

    if (mask & NYA_NET_FIELD_FLAGS) state->flags = _nya_net_read_varint(reader);

    return !reader->failed;
}

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

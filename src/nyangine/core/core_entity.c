#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_entity_apply_deferred_despawn(void* data);

/** Advances a running nya_entity_move_to by one tick. No-op for an entity with no move. */
NYA_INTERNAL void _nya_entity_target_step(NYA_Entity* entity, f32 delta_time_s);

/** Advances an animated entity's animator and delivers its signals to on_animation. */
NYA_INTERNAL void _nya_entity_animation_step(NYA_Entity* entity, f32 delta_time_s);

/** Draws whatever NYA_EntityVisual says, before the entity's own on_render runs. */
NYA_INTERNAL void _nya_entity_visual_draw(NYA_Entity* entity, NYA_Window* window);

/** Sort comparator: depth, then texture to keep the batching, then slot to make the order total. */
NYA_INTERNAL int _nya_entity_draw_entry_compare(const void* left, const void* right);

/** One entity in a draw list, with the keys it is sorted on. See nya_system_entity_render_in. */
typedef struct {
    NYA_EntityHandle handle;
    f32              z_order;

    /**
     * The texture handle the entity's visual draws with, or null.
     *
     * Compared by address, not content: asset handles are interned literals, so two sprites out of one
     * sheet share the identical pointer and comparing bytes would be strcmp inside a sort comparator.
     * */
    NYA_ConstCString texture;
} NYA_EntityDrawEntry;

/** Adds a slot to every bitset its kind and flags put it in. */
NYA_INTERNAL void _nya_entity_index_add(u32 slot, u32 type, u64 flags);

/** The exact inverse, given the kind and flags the entity had at the time. */
NYA_INTERNAL void _nya_entity_index_remove(u32 slot, u32 type, u64 flags);

/** The bitset for a kind, or null when that kind is past NYA_ENTITY_KIND_MAX. */
NYA_INTERNAL u64* _nya_entity_index_kind_bits(u32 type);

/** Words of the bitsets that could hold anything, from the table's high water mark. */
NYA_INTERNAL u32 _nya_entity_index_word_count(void);

NYA_INTERNAL void _nya_entity_bitset_set(u64* bits, u32 slot);
NYA_INTERNAL void _nya_entity_bitset_clear(u64* bits, u32 slot);

/** Which cell a world position falls in. Floored, so it is continuous across zero. */
NYA_INTERNAL void _nya_entity_grid_cell(f32x2 position, OUT s32* out_x, OUT s32* out_y);

/** Cell coordinates to a bucket. Masked rather than divided, which is why the count is a power of two. */
NYA_INTERNAL u32 _nya_entity_grid_bucket(s32 cell_x, s32 cell_y);

/**
 * The shared body of every query: walks the cells covering a rectangle and emits what is really in it.
 *
 * `type_filter` is applied only when `filter_by_type` is set, so "any kind" and "kind zero" stay
 * distinguishable.
 * */
NYA_INTERNAL u32 _nya_entity_query(f32x2 min, f32x2 max, b8 filter_by_type, u32 type_filter, b8 filter_by_flags, u64 flag_filter,
                                   OUT NYA_EntityHandle* out, u32 capacity);

/**
 * Runs `hit`'s on_click, or reports that there was nothing to run.
 *
 * Shared by both nya_entity_click overloads, so the "no entity or no callback is not a click" rule is written once.
 * */
NYA_INTERNAL NYA_EntityHandle _nya_entity_click_deliver(NYA_EntityHandle hit, f32x3 world_point, u8 button);

/**
 * Moves the hover to `hit`, running the two callbacks the move implies.
 *
 * Shared by both nya_entity_hover overloads and by nya_entity_hover_clear, which is the same operation
 * with nothing under the cursor — so the "left" edge is produced in one place rather than three.
 * */
NYA_INTERNAL NYA_EntityHandle _nya_entity_hover_move(NYA_EntityHandle hit);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_system_entity_init(void) {
    NYA_Arena* allocator = nya_arena_create(.name = "entity_system_allocator");

    NYA_EntitySystem* system = &nya_world()->entity_system;

    *system = (NYA_EntitySystem){
        .allocator   = allocator,
        .entities    = nya_arena_alloc(allocator, NYA_ENTITY_MAX * sizeof(NYA_Entity)),
        .occupied    = nya_arena_alloc(allocator, NYA_ENTITY_MAX * sizeof(b8)),
        .generations = nya_arena_alloc(allocator, NYA_ENTITY_MAX * sizeof(u32)),
        .free_slots  = nya_arena_alloc(allocator, NYA_ENTITY_MAX * sizeof(u32)),

        .grid = {
            .buckets   = nya_arena_alloc(allocator, NYA_ENTITY_GRID_BUCKETS * sizeof(u32)),
            .next      = nya_arena_alloc(allocator, NYA_ENTITY_MAX * sizeof(u32)),
            .cell_size = NYA_ENTITY_GRID_CELL_SIZE,
        },

        .index = {
            .live  = nya_arena_alloc(allocator, NYA_ENTITY_BITSET_WORDS * sizeof(u64)),
            .kinds = nya_arena_alloc(allocator, (u64)NYA_ENTITY_KIND_MAX * NYA_ENTITY_BITSET_WORDS * sizeof(u64)),
            .flags = nya_arena_alloc(allocator, (u64)NYA_ENTITY_FLAG_COUNT * NYA_ENTITY_BITSET_WORDS * sizeof(u64)),
        },
    };

    nya_memset(system->entities, 0, NYA_ENTITY_MAX * sizeof(NYA_Entity));
    nya_memset(system->occupied, 0, NYA_ENTITY_MAX * sizeof(b8));

    // Generations start at 1 so a zeroed handle (an uninitialized struct field) never resolves to slot 0.
    for (u32 i = 0; i < NYA_ENTITY_MAX; i++) system->generations[i] = 1;

    // Seeded in reverse so the first spawns come from slot 0 upward, keeping the high water mark tight.
    for (u32 i = 0; i < NYA_ENTITY_MAX; i++) system->free_slots[i] = NYA_ENTITY_MAX - 1 - i;
    system->free_count = NYA_ENTITY_MAX;

    // Empty rather than zeroed: slot 0 is a legal entity, so zero cannot mean "nothing here".
    for (u32 i = 0; i < NYA_ENTITY_GRID_BUCKETS; i++) system->grid.buckets[i] = NYA_ENTITY_GRID_EMPTY;

    // Zero *is* the right empty for the index — a clear bit means "not in this set".
    nya_memset(system->index.live, 0, NYA_ENTITY_BITSET_WORDS * sizeof(u64));
    nya_memset(system->index.kinds, 0, (u64)NYA_ENTITY_KIND_MAX * NYA_ENTITY_BITSET_WORDS * sizeof(u64));
    nya_memset(system->index.flags, 0, (u64)NYA_ENTITY_FLAG_COUNT * NYA_ENTITY_BITSET_WORDS * sizeof(u64));

    nya_log_info("Entity system initialized (%d slots, %d grid buckets at %.0f units).", NYA_ENTITY_MAX, NYA_ENTITY_GRID_BUCKETS,
             (f64)NYA_ENTITY_GRID_CELL_SIZE);
}

void nya_system_entity_deinit(void) {
    nya_entity_clear();

    NYA_EntitySystem* system = &nya_world()->entity_system;

    nya_arena_destroy(system->allocator);
    *system = (NYA_EntitySystem){ 0 };

    nya_log_info("Entity system deinitialized.");
}

void nya_system_entity_update(f32 delta_time_s) {
    // A linear walk over every slot, so this scales with the world rather than with what is awake.
    nya_perf_time_this_function();

    // Before the callbacks: the physics step has already written this tick's positions by the time this
    // runs, so a query during update sees them.
    nya_system_entity_grid_rebuild();

    nya_entity_foreach (entity) {
        if (!nya_flag_check(entity->state, NYA_ENTITY_STATE_ACTIVE)) continue;

        // Integrate before the callback, so update sees this tick's position rather than last tick's.
        //
        // Skipped for a simulated entity: nya_system_physics2d_update has already written this tick's
        // transform out of the solver, and integrating on top would add velocity a second time — visible
        // as a body that falls at roughly twice gravity and drifts out of its own collision shape.
        if (!nya_flag_check(entity->state, NYA_ENTITY_STATE_STATIC) && !entity->physics2d.attached) {
            entity->position += entity->velocity * delta_time_s;

            // Radians/sec per axis; small enough per tick that quaternion compose + renormalize is cheap and stable.
            f32x3 delta   = entity->angular_velocity * delta_time_s;
            f32x3 squared = delta * delta;
            if (squared.x + squared.y + squared.z > 0.0F) {
                NYA_Quaternion spin = nya_quaternion_from_euler(delta.x, delta.y, delta.z);
                entity->rotation    = nya_quaternion_normalize(nya_quaternion_multiply(spin, entity->rotation));
            }
        }

        // After velocity integration, not before: a move is an absolute statement of position, and
        // integrating on top of it would bend the curve by whatever velocity was left on the entity.
        _nya_entity_target_step(entity, delta_time_s);

        _nya_entity_animation_step(entity, delta_time_s);

        NYA_EntityOnUpdateFn on_update = nya_callback_get(entity->on_update);
        if (on_update != nullptr) on_update(entity, delta_time_s);
    }

    // Last, so it accounts for everything that moved this tick — integration, an interpolated move,
    // and anything an on_update did. Rendering and every query run after this, so what they see is
    // coherent; a callback that has to see a child move within its own tick calls
    // nya_entity_transform_sync. See the hierarchy note in core_entity.h.
    nya_system_entity_transforms_update();
}

/*
 * ─────────────────────────────────────────────────────────
 * HIERARCHY
 * ─────────────────────────────────────────────────────────
 */

/** Whether two handles name the same live entity. Handles are a pair, and there is no operator. */
NYA_INTERNAL b8 _nya_entity_handle_equals(NYA_EntityHandle a, NYA_EntityHandle b) {
    return a.index == b.index && a.generation == b.generation;
}

/** Takes `child` out of its parent's sibling list. A no-op for a root. */
NYA_INTERNAL void _nya_entity_unlink(NYA_Entity* child) {
    NYA_Entity* parent = nya_entity_get(child->parent);

    child->parent = NYA_ENTITY_HANDLE_NONE;

    // The parent may already be gone — despawn clears the link on its way out, and a stale parent
    // handle stops resolving. Either way the child is a root now.
    if (parent == nullptr) {
        child->next_sibling = NYA_ENTITY_HANDLE_NONE;
        return;
    }

    if (_nya_entity_handle_equals(parent->first_child, child->handle)) {
        parent->first_child = child->next_sibling;
    } else {
        // A singly-linked list, so removal walks to the node before. Short: sibling lists are a
        // handful of entries, and the alternative is a back pointer on every entity in the world.
        for (NYA_Entity* sibling = nya_entity_get(parent->first_child); sibling != nullptr;
             sibling             = nya_entity_get(sibling->next_sibling)) {
            if (!_nya_entity_handle_equals(sibling->next_sibling, child->handle)) continue;

            sibling->next_sibling = child->next_sibling;
            break;
        }
    }

    child->next_sibling = NYA_ENTITY_HANDLE_NONE;

    if (parent->child_count > 0) parent->child_count--;

    nya_world()->entity_system.parented_count--;
}

/** Captures `child`'s offset from `parent` from the world transforms both currently have. */
NYA_INTERNAL void _nya_entity_capture_local(NYA_Entity* child, const NYA_Entity* parent) {
    /*
     * The inverse of the parent's transform applied to the child's, written out for the pieces this
     * engine actually composes rather than through a general matrix inverse.
     *
     * ⚠ Scale is per axis and rotation is a quaternion, so a parent with a non-uniform scale *and* a
     * rotation does not have an exact inverse of this form — the composition below would shear. The
     * offset is captured against the parent's scale as if it were uniform, which is exact for the
     * uniform case and for the unrotated case, and the two cover everything a game usually builds a
     * hierarchy out of. A rotated, non-uniformly scaled parent is documented as unsupported rather
     * than silently approximated.
     */
    f32x3 scale = parent->scale;

    // A zero scale on any axis has no inverse; treated as one, so a parent that has not been given a
    // scale yet — which is a zeroed struct — behaves like an identity rather than dividing by zero.
    if (scale.x == 0.0F) scale.x = 1.0F;
    if (scale.y == 0.0F) scale.y = 1.0F;
    if (scale.z == 0.0F) scale.z = 1.0F;

    NYA_Quaternion inverse_rotation = nya_quaternion_conjugate(nya_quaternion_normalize(parent->rotation));

    f32x3 offset = child->position - parent->position;

    child->local_position = nya_quaternion_rotate(inverse_rotation, offset) / scale;
    child->local_rotation = nya_quaternion_multiply(inverse_rotation, child->rotation);
    child->local_scale    = child->scale / scale;
}

/** Writes `child`'s world transform from `parent`'s and its own stored local. The inverse of the above. */
NYA_INTERNAL void _nya_entity_compose(NYA_Entity* child, const NYA_Entity* parent) {
    NYA_Quaternion rotation = nya_quaternion_normalize(parent->rotation);

    child->position = parent->position + nya_quaternion_rotate(rotation, child->local_position * parent->scale);
    child->rotation = nya_quaternion_multiply(rotation, child->local_rotation);
    child->scale    = parent->scale * child->local_scale;
}

/** Recomposes every descendant of `parent`, depth first. */
NYA_INTERNAL void _nya_entity_propagate(NYA_Entity* parent) {
    for (NYA_Entity* child = nya_entity_get(parent->first_child); child != nullptr; child = nya_entity_get(child->next_sibling)) {
        _nya_entity_compose(child, parent);
        _nya_entity_propagate(child);
    }
}

b8 nya_entity_is_ancestor(NYA_EntityHandle ancestor, NYA_EntityHandle descendant) {
    if (!nya_entity_is_valid(ancestor) || !nya_entity_is_valid(descendant)) return false;

    for (NYA_Entity* walk = nya_entity_get(descendant); walk != nullptr; walk = nya_entity_get(walk->parent)) {
        if (_nya_entity_handle_equals(walk->parent, ancestor)) return true;
    }

    return false;
}

b8 nya_entity_parent_set(NYA_EntityHandle child_handle, NYA_EntityHandle parent_handle) {
    NYA_Entity* child = nya_entity_get(child_handle);
    if (child == nullptr) return false;

    // Unparenting. Keeps the world transform, which is what _nya_entity_unlink leaves alone.
    if (!nya_entity_is_valid(parent_handle)) {
        _nya_entity_unlink(child);
        return true;
    }

    if (_nya_entity_handle_equals(child_handle, parent_handle)) {
        nya_log_error("Cannot parent entity '%s' to itself.", child->name ? child->name : "(unnamed)");
        return false;
    }

    /*
     * A cycle would make the propagation walk recurse forever, and it is the one mistake a scene
     * graph invites — dragging a node onto its own descendant in an editor is the usual way in.
     * Checked before anything is unlinked, so a refused reparent leaves the tree untouched.
     */
    if (nya_entity_is_ancestor(child_handle, parent_handle)) {
        nya_log_error("Cannot parent entity '%s' to its own descendant; that would make a cycle.", child->name ? child->name : "(unnamed)");
        return false;
    }

    // Already there. Returned early rather than unlinked and relinked, which would silently reorder
    // the parent's sibling list for no reason.
    if (_nya_entity_handle_equals(child->parent, parent_handle)) return true;

    _nya_entity_unlink(child);

    NYA_Entity* parent = nya_entity_get(parent_handle);

    // Pushed at the front: appending would walk the list on every parenting, and nothing depends on
    // the order siblings come out in.
    child->parent       = parent_handle;
    child->next_sibling = parent->first_child;
    parent->first_child = child_handle;
    parent->child_count++;

    nya_world()->entity_system.parented_count++;

    _nya_entity_capture_local(child, parent);

    return true;
}

void nya_entity_parent_clear(NYA_EntityHandle child) {
    (void)nya_entity_parent_set(child, NYA_ENTITY_HANDLE_NONE);
}

NYA_EntityHandle nya_entity_parent(const NYA_Entity* entity) {
    return entity != nullptr ? entity->parent : NYA_ENTITY_HANDLE_NONE;
}

u32 nya_entity_children(const NYA_Entity* entity, OUT NYA_EntityHandle* out, u32 capacity) {
    if (entity == nullptr) return 0;

    u32 count = 0;

    for (NYA_Entity* child = nya_entity_get(entity->first_child); child != nullptr; child = nya_entity_get(child->next_sibling)) {
        // Counted even past the capacity, so a caller can size a buffer from the answer rather than
        // guessing and never learning it guessed low.
        if (out != nullptr && count < capacity) out[count] = child->handle;

        count++;
    }

    return count;
}

void nya_entity_transform_sync(NYA_EntityHandle handle) {
    NYA_Entity* entity = nya_entity_get(handle);
    if (entity == nullptr) return;

    // The entity itself first, if it has a parent: syncing a child means "put me where my parent
    // says", and doing only its descendants would leave it behind by one link.
    NYA_Entity* parent = nya_entity_get(entity->parent);
    if (parent != nullptr) _nya_entity_compose(entity, parent);

    _nya_entity_propagate(entity);
}

void nya_system_entity_transforms_update(void) {
    NYA_EntitySystem* system = &nya_world()->entity_system;

    // The whole cost of the hierarchy in a world that does not use one.
    if (system->parented_count == 0) return;

    nya_perf_time_this_function();

    /*
     * From the roots down, so a chain resolves in one pass.
     *
     * A root here is any entity with children and no parent. Walking every slot to find them is the
     * same linear scan the update loop already does, and the alternative — a maintained root list —
     * is more state to keep in step for a saving that only shows up in a world that is almost
     * entirely hierarchy.
     */
    nya_entity_foreach (entity) {
        if (entity->first_child.generation == 0) continue;
        if (nya_entity_is_valid(entity->parent)) continue;

        _nya_entity_propagate(entity);
    }
}

f32_4x4 nya_entity_world_matrix(const NYA_Entity* entity) {
    if (entity == nullptr) return f32_4x4_id;

    // Scale of zero on every axis is a zeroed entity rather than a deliberate collapse, and a model
    // matrix built from it is singular. Read as one, matching _nya_entity_capture_local.
    f32x3 scale = entity->scale;
    if (scale.x == 0.0F && scale.y == 0.0F && scale.z == 0.0F) scale = (f32x3){ 1.0F, 1.0F, 1.0F };

    return nya_matrix_transform(entity->position, nya_quaternion_to_matrix3(nya_quaternion_normalize(entity->rotation)), scale);
}

/*
 * ─────────────────────────────────────────────────────────
 * INTERPOLATED MOTION
 * ─────────────────────────────────────────────────────────
 */

void nya_entity_move_to(NYA_Entity* entity, f32x3 target, f32 duration_s, NYA_EaseType ease) {
    nya_entity_move_to_with_options(entity, target, duration_s, (NYA_TweenOptions){ .ease = ease });
}

void nya_entity_move_to_with_options(NYA_Entity* entity, f32x3 target, f32 duration_s, NYA_TweenOptions options) {
    if (entity == nullptr) return;

    if (entity->physics2d.attached && entity->physics2d.type != NYA_PHYSICS_BODY_KINEMATIC) {
        nya_log_warn(
            "Ignoring nya_entity_move_to on entity '%s': its body is %s, and the solver owns the transform",
            entity->name ? entity->name : "(unnamed)",
            entity->physics2d.type == NYA_PHYSICS_BODY_STATIC ? "static" : "dynamic"
        );
        return;
    }

    // Whatever was running is abandoned first, so calling this mid-move restarts rather than stacking
    // two tweens on the same staging value — which would interleave writes and produce neither curve.
    nya_entity_move_stop(entity);

    // A move of no duration is a teleport, not a division by zero, so a caller computing duration from
    // distance needs no special case for standing still. Handled here rather than left to the tween's
    // own zero-duration path, which would still cost a slot and a frame of latency.
    if (duration_s <= 0.0F) {
        if (entity->physics2d.attached) nya_physics2d_teleport(entity, target.xy, nya_physics2d_rotation(entity));
        else entity->position = target;

        entity->move_position = target;
        return;
    }

    /*
     * The tween reads its start value at the first sample, not here — and for a delayed move that is
     * the right moment, since the entity may have moved during the delay. Seeding the staging value
     * with the current position is still needed: _nya_entity_target_step applies it every tick the
     * handle resolves, delay included, and an unseeded one would snap the entity to wherever the last
     * move left it.
     */
    entity->move_position = entity->position;
    entity->move_tween    = nya_tween_f32x3_with_options(&entity->move_position, target, duration_s, options);
}

void nya_entity_move_to_at_speed(NYA_Entity* entity, f32x3 target, f32 world_units_per_second) {
    if (entity == nullptr) return;

    if (world_units_per_second <= 0.0F) {
        nya_entity_move_to(entity, target, 0.0F, NYA_EASE_LINEAR);
        return;
    }

    f32x3 delta    = target - entity->position;
    f32   distance = sqrtf((delta.x * delta.x) + (delta.y * delta.y) + (delta.z * delta.z));

    nya_entity_move_to(entity, target, distance / world_units_per_second, NYA_EASE_LINEAR);
}

void nya_entity_move_stop(NYA_Entity* entity) {
    if (entity == nullptr) return;

    b8 was_moving = entity->move_tween.generation != 0 || entity->move_settling;

    // By target rather than by handle: a caller may have started a tween on move_position itself, and
    // "stop moving" has to mean all of them or the leftover keeps writing.
    nya_tween_cancel_target(&entity->move_position);
    entity->move_tween    = NYA_TWEEN_NONE;
    entity->move_settling = false;

    // "Keeps the position reached" is only true of a body if the velocity driving it goes too — the
    // solver has no idea the move was abandoned and would carry the last commanded speed forever.
    // Guarded on kinematic because that is the only kind a move can be attached to, and a dynamic
    // body's own velocity is not this function's to clear.
    if (was_moving && entity->physics2d.attached && entity->physics2d.type == NYA_PHYSICS_BODY_KINEMATIC) {
        nya_physics2d_velocity_set(entity, f32x2_zero);
    }
}

b8 nya_entity_moving(const NYA_Entity* entity) {
    return entity != nullptr && nya_tween_active(entity->move_tween);
}

f32 nya_entity_move_progress(const NYA_Entity* entity) {
    if (entity == nullptr) return 1.0F;

    return nya_tween_progress(entity->move_tween);
}

void _nya_entity_target_step(NYA_Entity* entity, f32 delta_time_s) {
    /*
     * A move is over when its handle stops resolving, and the handle is cleared here rather than by
     * the tween — so a non-zero handle that no longer resolves is exactly "arrived this tick", which
     * is the tick the final value still has to be applied on.
     *
     * This reads the value the tween wrote earlier in the frame; nya_system_tween_update runs before
     * nya_system_entity_update in the fixed loop, and the ordering is what makes one indirection
     * enough. See core_app.c.
     */
    if (entity->move_tween.generation == 0) {
        /*
         * The tick after a body-backed move arrived. The velocity carrying its final step was set
         * during the previous update and the solver only consumed it at the start of this one, so this
         * is the earliest tick it can be cleared without throwing that step away — which is what
         * clearing it on the arrival tick, as this used to, did every time.
         */
        if (entity->move_settling) {
            entity->move_settling = false;
            if (entity->physics2d.attached) nya_physics2d_velocity_set(entity, f32x2_zero);
        }

        return;
    }

    b8 arrived = !nya_tween_active(entity->move_tween);

    if (entity->physics2d.attached) {
        // A kinematic body is moved by telling the solver how fast it is going, not by writing where it
        // is — that is what makes it sweep through the tick and push whatever stands on it, which is why
        // a moving platform is kinematic rather than static.
        nya_physics2d_velocity_set(entity, (entity->move_position.xy - entity->position.xy) / delta_time_s);
    } else {
        entity->position = entity->move_position;
    }

    if (arrived) {
        // Cleared before anything else can see this tick, so an on_update checking nya_entity_moving on
        // the arrival tick is told the move is over.
        entity->move_tween    = NYA_TWEEN_NONE;
        entity->move_settling = entity->physics2d.attached;
    }
}

f32 nya_entity_sort_key(const NYA_Entity* entity) {
    if (entity == nullptr) return 0.0F;

    // The anchor is added rather than replacing the position, so an entity whose sprite is drawn from
    // its centre says how far down its feet are and nothing else has to know.
    if (entity->visual.y_sorted) return entity->position.y + entity->visual.y_sort_anchor;

    return entity->visual.z_order;
}

void nya_system_entity_render(NYA_Window* window) {
    nya_assert(window != nullptr);

    u32 target_width, target_height;
    nya_render2d_target_size(window, &target_width, &target_height);

    /*
     * All four corners, not two: under camera rotation, two opposite screen corners don't bound the view
     * — the other two stick out. The extent of all four bounds the visible region regardless.
     *
     * With no camera set nya_render2d_screen_to_world is the identity, so this degenerates to the target
     * in screen pixels — right for a layer drawing screen space entities.
     */
    f32x2 corners[4] = {
        nya_render2d_screen_to_world(window, (f32x2){ 0.0F, 0.0F }),
        nya_render2d_screen_to_world(window, (f32x2){ (f32)target_width, 0.0F }),
        nya_render2d_screen_to_world(window, (f32x2){ 0.0F, (f32)target_height }),
        nya_render2d_screen_to_world(window, (f32x2){ (f32)target_width, (f32)target_height }),
    };

    f32x2 min = corners[0];
    f32x2 max = corners[0];

    for (u32 i = 1; i < 4; i++) {
        min.x = nya_min(min.x, corners[i].x);
        min.y = nya_min(min.y, corners[i].y);
        max.x = nya_max(max.x, corners[i].x);
        max.y = nya_max(max.y, corners[i].y);
    }

    min.x -= NYA_ENTITY_RENDER_CULL_MARGIN;
    min.y -= NYA_ENTITY_RENDER_CULL_MARGIN;
    max.x += NYA_ENTITY_RENDER_CULL_MARGIN;
    max.y += NYA_ENTITY_RENDER_CULL_MARGIN;

    nya_system_entity_render_in(window, min, max);
}

void nya_system_entity_render_in(NYA_Window* window, f32x2 min, f32x2 max) {
    nya_perf_time_this_function();

    NYA_EntityHandle visible[NYA_ENTITY_MAX];
    u32              count = nya_entity_query_rect(min, max, visible, NYA_ENTITY_MAX);

    /*
     * Collected and sorted before anything is drawn: the query answers in grid bucket order, which is
     * neither spatial nor stable across a rebuild, so two sprites could otherwise swap depth because one
     * moved into a different cell. Sorting is what makes NYA_EntityVisual.z_order mean anything.
     */
    static NYA_EntityDrawEntry entries[NYA_ENTITY_MAX];

    u32 entry_count = 0;

    for (u32 i = 0; i < count; i++) {
        NYA_Entity* entity = nya_entity_get(visible[i]);
        if (entity == nullptr) continue;

        // Clearing NYA_ENTITY_STATE_VISIBLE is what "invisible" is — one bit, not two that can disagree.
        if (!nya_flag_check(entity->state, NYA_ENTITY_STATE_VISIBLE)) continue;

        entries[entry_count++] = (NYA_EntityDrawEntry){
            .handle  = entity->handle,
            .z_order = nya_entity_sort_key(entity),
            .texture = entity->visual.sprite.texture,
        };
    }

    // Plain qsort rather than nya_array_sort (a macro over NYA_Array): this list is a fixed stack buffer
    // precisely so drawing allocates nothing.
    qsort(entries, entry_count, sizeof(NYA_EntityDrawEntry), _nya_entity_draw_entry_compare);

    for (u32 i = 0; i < entry_count; i++) {
        NYA_Entity* entity = nya_entity_get(entries[i].handle);

        // Re-resolved rather than kept as a pointer: on_render is game code and may despawn something,
        // including a later entry in this list.
        if (entity == nullptr) continue;

        _nya_entity_visual_draw(entity, window);

        // After the visual, so a health bar lands on top of its sprite rather than under it.
        NYA_EntityOnRenderFn on_render = nya_callback_get(entity->on_render);
        if (on_render != nullptr) on_render(entity, window);
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * SPATIAL QUERIES
 * ─────────────────────────────────────────────────────────
 */

void nya_system_entity_grid_rebuild(void) {
    nya_perf_time_this_function();

    NYA_EntitySystem* system = &nya_world()->entity_system;
    NYA_EntityGrid*   grid   = &system->grid;

    // Clearing is the bucket heads only; chain links are overwritten as entities are inserted, and a
    // no-longer-live entity is simply never linked in.
    for (u32 i = 0; i < NYA_ENTITY_GRID_BUCKETS; i++) grid->buckets[i] = NYA_ENTITY_GRID_EMPTY;

    grid->count = 0;

    for (u32 slot = 0; slot < system->high_water_mark; slot++) {
        if (!system->occupied[slot]) continue;

        const NYA_Entity* entity = &system->entities[slot];

        s32 cell_x, cell_y;
        _nya_entity_grid_cell((f32x2){ entity->position.x, entity->position.y }, &cell_x, &cell_y);

        u32 bucket = _nya_entity_grid_bucket(cell_x, cell_y);

        // Pushed at the head: insertion is two writes and never walks the chain.
        grid->next[slot]   = grid->buckets[bucket];
        grid->buckets[bucket] = slot;

        grid->count++;
    }
}

u32 nya_entity_query_rect(f32x2 min, f32x2 max, OUT NYA_EntityHandle* out, u32 capacity) {
    return _nya_entity_query(min, max, false, 0, false, 0, out, capacity);
}

u32 nya_entity_query_kind(f32x2 min, f32x2 max, u32 type, OUT NYA_EntityHandle* out, u32 capacity) {
    return _nya_entity_query(min, max, true, type, false, 0, out, capacity);
}

u32 nya_entity_query_flags(f32x2 min, f32x2 max, u64 flags, OUT NYA_EntityHandle* out, u32 capacity) {
    return _nya_entity_query(min, max, false, 0, true, flags, out, capacity);
}

u32 nya_entity_query_radius(f32x2 center, f32 radius, OUT NYA_EntityHandle* out, u32 capacity) {
    nya_assert(out != nullptr);

    if (radius <= 0.0F || capacity == 0) return 0;

    // The bounding square first, then an exact distance test. Squared throughout, so the circle costs a
    // multiply rather than a square root per candidate.
    f32x2 min = { center.x - radius, center.y - radius };
    f32x2 max = { center.x + radius, center.y + radius };

    u32 found = _nya_entity_query(min, max, false, 0, false, 0, out, capacity);

    f32 radius_squared = radius * radius;
    u32 kept           = 0;

    for (u32 i = 0; i < found; i++) {
        const NYA_Entity* entity = nya_entity_get(out[i]);
        if (entity == nullptr) continue;

        f32 dx = entity->position.x - center.x;
        f32 dy = entity->position.y - center.y;

        if ((dx * dx) + (dy * dy) > radius_squared) continue;

        out[kept++] = out[i];
    }

    return kept;
}

NYA_EntityHandle nya_entity_click(f32x2 world_point, u8 button) __attr_overloaded {
    // z zero: the 2D world is the z = 0 plane. See NYA_EntityOnClickFn.
    return _nya_entity_click_deliver(nya_physics2d_entity_at(world_point), (f32x3){ world_point.x, world_point.y, 0.0F }, button);
}

NYA_EntityHandle nya_entity_hover(f32x2 world_point) __attr_overloaded {
    return _nya_entity_hover_move(nya_physics2d_entity_at(world_point));
}

NYA_EntityHandle nya_entity_hover(f32x3 origin, f32x3 direction) __attr_overloaded {
    // The struck point is dropped: on_hover does not take one. See NYA_EntityOnHoverFn.
    return _nya_entity_hover_move(nya_physics3d_raycast(origin, direction, nullptr, nullptr));
}

void nya_entity_hover_clear(void) {
    (void)_nya_entity_hover_move(NYA_ENTITY_HANDLE_NONE);
}

NYA_EntityHandle nya_entity_hovered(void) {
    return nya_world()->entity_system.hovered;
}

NYA_EntityHandle nya_entity_click(f32x3 origin, f32x3 direction, u8 button) __attr_overloaded {
    f32x3 point = { 0 };

    NYA_EntityHandle hit = nya_physics3d_raycast(origin, direction, &point, nullptr);

    /*
     * The point on the struck surface, not the ray's origin — it decides which face was hit, where to
     * put a decal, which end of a lever was pulled. Handing the camera position instead would make
     * every click on an object report the same coordinates.
     *
     * Left zeroed when nothing was hit; _nya_entity_click_deliver never reads it in that case, since an
     * invalid handle returns before the callback.
     */
    return _nya_entity_click_deliver(hit, point, button);
}

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

NYA_EntityHandle nya_entity_spawn_with_options(NYA_EntitySpawnOptions options) {
    NYA_EntitySystem* system = &nya_world()->entity_system;

    if (system->free_count == 0) {
        nya_log_error("Cannot spawn entity '%s': all %d entity slots are in use.", options.name ? options.name : "(unnamed)", NYA_ENTITY_MAX);
        return NYA_ENTITY_HANDLE_NONE;
    }

    u32 slot = system->free_slots[--system->free_count];

    NYA_EntityHandle handle = { .index = slot, .generation = system->generations[slot] };

    NYA_Entity* entity = &system->entities[slot];
    *entity            = (NYA_Entity){
        .handle           = handle,
        .state            = options.state,
        .type             = options.type,
        .flags            = options.flags,
        .name             = options.name,
        .position         = options.position,
        .rotation         = options.rotation,
        .scale            = options.scale,
        .velocity         = options.velocity,
        .angular_velocity = options.angular_velocity,
        .user_data        = options.user_data,
        .on_spawn         = options.on_spawn,
        .on_despawn       = options.on_despawn,
        .on_update        = options.on_update,
        .on_render        = options.on_render,
        .on_collision     = options.on_collision,
        .on_click         = options.on_click,
        .on_hover         = options.on_hover,
        .on_animation     = options.on_animation,
        .visual           = options.visual,
        .light            = options.light,

        /*
         * A root, with an identity local transform.
         *
         * Spelled out rather than left to the zero: a zeroed handle *is* NONE and a zeroed quaternion
         * is not the identity, and a parenting operation composing against it would collapse the child
         * to nothing. Written here so the entity is coherent the instant it exists.
         */
        .parent       = NYA_ENTITY_HANDLE_NONE,
        .first_child  = NYA_ENTITY_HANDLE_NONE,
        .next_sibling = NYA_ENTITY_HANDLE_NONE,

        .local_rotation = nya_quaternion_identity,
        .local_scale    = { 1.0F, 1.0F, 1.0F },
    };

    system->occupied[slot] = true;
    system->count++;

    _nya_entity_index_add(slot, options.type, options.flags);
    if (slot + 1 > system->high_water_mark) system->high_water_mark = slot + 1;

    // Called with the entity already live, so on_spawn can spawn more, despawn itself, or hand its own
    // handle to something else.
    NYA_EntityOnSpawnFn on_spawn = nya_callback_get(entity->on_spawn);
    if (on_spawn != nullptr) on_spawn(entity);

    return handle;
}

void nya_entity_despawn(NYA_EntityHandle entity) {
    NYA_EntitySystem* system = &nya_world()->entity_system;

    NYA_Entity* target = nya_entity_get(entity);
    if (target == nullptr) return;

    NYA_EntityOnDespawnFn on_despawn = nya_callback_get(target->on_despawn);
    if (on_despawn != nullptr) on_despawn(target);

    /*
     * Both solvers, after on_despawn (which may still want to read a velocity or overlap off the body)
     * and before the slot is cleared. Skipping either leaves that body behind: still colliding, still
     * costing a step, and unreachable to destroy once the handle stops resolving.
     *
     * Both are called unconditionally rather than picking by which looks attached — each is a no-op for
     * an entity without that kind of body, and asking "which one" is how the second gets forgotten. It
     * did: only the 2D detach was here, so every 3D body ever spawned leaked into its world.
     */
    nya_physics2d_body_detach(entity);
    nya_physics3d_body_detach(entity);

    // A tween does not own its target and cannot know the entity went away, so a running move would go
    // on writing into a slot that is about to be zeroed and then reused. By address rather than by
    // handle, which also takes any tween a caller aimed at move_position itself.
    nya_tween_cancel_target(&target->move_position);

    /*
     * The subtree goes with it.
     *
     * ⚠ **Despawning a parent despawns its children**, recursively, and that is a decision rather
     * than a consequence — a turret whose tank is gone is not something a game wants left floating at
     * the last place the tank was. Call nya_entity_parent_clear on a child first to keep it.
     *
     * The children are collected before any of them is despawned: despawning unlinks, which rewrites
     * the sibling list this would otherwise be walking.
     */
    while (nya_entity_is_valid(target->first_child)) {
        NYA_EntityHandle child = target->first_child;

        // Unlinked first so the recursion cannot come back around to this entity through the parent
        // link, and so `first_child` really advances even if the child refuses to go.
        nya_entity_parent_clear(child);
        nya_entity_despawn(child);
    }

    // And this entity leaves its own parent's list, or that list would name a slot about to be reused.
    if (nya_entity_is_valid(target->parent)) nya_entity_parent_clear(entity);

    /*
     * The hover is dropped without running on_hover: on_despawn has already run and the entity is on its
     * way out, so telling it the cursor left is a callback into something half torn down. The generation
     * bump below would stop the stale handle resolving anyway; clearing it here keeps nya_entity_hovered
     * honest in the same frame rather than until the cursor next moves.
     */
    if (system->hovered.index == entity.index && system->hovered.generation == entity.generation) {
        system->hovered = NYA_ENTITY_HANDLE_NONE;
    }

    // Before the slot is cleared: removing needs the kind and flags it is being removed for.
    _nya_entity_index_remove(entity.index, target->type, target->flags);

    // Bumping the generation makes every outstanding handle to this entity stop resolving.
    system->generations[entity.index]++;
    system->occupied[entity.index] = false;
    system->count--;

    system->free_slots[system->free_count++] = entity.index;

    *target = (NYA_Entity){ 0 };
}

void nya_entity_despawn_deferred(NYA_EntityHandle entity) {
    NYA_Entity* target = nya_entity_get(entity);
    if (target == nullptr) return;

    // Idempotent: a projectile hitting two things in one tick shouldn't queue two despawns, with the
    // second applying to whatever reused the slot.
    if (nya_flag_check(target->state, NYA_ENTITY_STATE_DESPAWNING)) return;

    nya_flag_set(target->state, NYA_ENTITY_STATE_DESPAWNING);
    nya_sim_defer(_nya_entity_apply_deferred_despawn, &entity, sizeof(entity));
}

NYA_Entity* nya_entity_get(NYA_EntityHandle entity) {
    NYA_EntitySystem* system = &nya_world()->entity_system;

    if (entity.index >= NYA_ENTITY_MAX) return nullptr;
    if (!system->occupied[entity.index]) return nullptr;
    if (system->generations[entity.index] != entity.generation) return nullptr;

    return &system->entities[entity.index];
}

b8 nya_entity_is_valid(NYA_EntityHandle entity) {
    return nya_entity_get(entity) != nullptr;
}

u32 nya_entity_count(void) {
    return nya_world()->entity_system.count;
}

void nya_entity_clear(void) {
    NYA_EntitySystem* system = &nya_world()->entity_system;

    // By slot rather than nya_entity_foreach: despawning is exactly what that macro is not safe under.
    for (u32 slot = 0; slot < system->high_water_mark; slot++) {
        if (!system->occupied[slot]) continue;
        nya_entity_despawn(system->entities[slot].handle);
    }

    system->high_water_mark = 0;
}

/*
 * ─────────────────────────────────────────────────────────
 * ITERATION
 * ─────────────────────────────────────────────────────────
 */

NYA_Entity* nya_entity_at_slot(u32 index) {
    NYA_EntitySystem* system = &nya_world()->entity_system;

    if (index >= NYA_ENTITY_MAX) return nullptr;
    if (!system->occupied[index]) return nullptr;

    return &system->entities[index];
}

u32 nya_entity_slot_count(void) {
    return nya_world()->entity_system.high_water_mark;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_entity_grid_cell(f32x2 position, OUT s32* out_x, OUT s32* out_y) {
    f32 cell_size = nya_world()->entity_system.grid.cell_size;

    // floorf, not a cast: a cast truncates toward zero, putting everything between -1 and 1 in the same
    // cell and mirroring the grid about the origin.
    *out_x = (s32)floorf(position.x / cell_size);
    *out_y = (s32)floorf(position.y / cell_size);
}

u32 _nya_entity_index_word_count(void) {
    // Rounded up, so the word holding the highest slot is included. Safe to stop here because the high
    // water mark only grows: a slot past it has never been occupied, so its bit has never been set.
    return (nya_world()->entity_system.high_water_mark + 63) / 64;
}

void _nya_entity_bitset_set(u64* bits, u32 slot) {
    bits[slot >> 6] |= 1ULL << (slot & 63);
}

void _nya_entity_bitset_clear(u64* bits, u32 slot) {
    bits[slot >> 6] &= ~(1ULL << (slot & 63));
}

u64* _nya_entity_index_kind_bits(u32 type) {
    if (type >= NYA_ENTITY_KIND_MAX) return nullptr;

    return &nya_world()->entity_system.index.kinds[(u64)type * NYA_ENTITY_BITSET_WORDS];
}

void _nya_entity_index_add(u32 slot, u32 type, u64 flags) {
    NYA_EntityIndex* index = &nya_world()->entity_system.index;

    _nya_entity_bitset_set(index->live, slot);

    u64* kind_bits = _nya_entity_index_kind_bits(type);
    if (kind_bits != nullptr) _nya_entity_bitset_set(kind_bits, slot);

    while (flags != 0) {
        u32 bit = (u32)nya_bits_ctz_u64(flags);
        flags &= flags - 1;

        _nya_entity_bitset_set(&index->flags[(u64)bit * NYA_ENTITY_BITSET_WORDS], slot);
    }
}

void _nya_entity_index_remove(u32 slot, u32 type, u64 flags) {
    NYA_EntityIndex* index = &nya_world()->entity_system.index;

    // `live` is what every query is masked by, so clearing it alone already makes the slot unreachable.
    // The rest are cleared anyway: a stale bit would otherwise be set again on the next spawn into this
    // slot, disagreeing about a kind the new entity is not.
    _nya_entity_bitset_clear(index->live, slot);

    u64* kind_bits = _nya_entity_index_kind_bits(type);
    if (kind_bits != nullptr) _nya_entity_bitset_clear(kind_bits, slot);

    while (flags != 0) {
        u32 bit = (u32)nya_bits_ctz_u64(flags);
        flags &= flags - 1;

        _nya_entity_bitset_clear(&index->flags[(u64)bit * NYA_ENTITY_BITSET_WORDS], slot);
    }
}

u32 _nya_entity_grid_bucket(s32 cell_x, s32 cell_y) {
    /*
     * The usual pair of large primes, multiplied in u64 and truncated on purpose — done in 64 bits
     * because the 32 bit form wraps and the sanitized build treats that as the accident it usually is,
     * the same trap the mote hash fell into. Casting the signed coordinate through u32 first is what
     * makes negative coordinates hash sensibly rather than sign extending into the high half.
     */
    u64 hash = ((u64)(u32)cell_x * 73856093ULL) ^ ((u64)(u32)cell_y * 19349663ULL);

    return (u32)(hash & (u64)(NYA_ENTITY_GRID_BUCKETS - 1));
}

u32 _nya_entity_query(f32x2 min, f32x2 max, b8 filter_by_type, u32 type_filter, b8 filter_by_flags, u64 flag_filter, OUT NYA_EntityHandle* out,
                      u32 capacity) {
    nya_assert(out != nullptr);

    if (capacity == 0) return 0;
    if (min.x > max.x || min.y > max.y) return 0;

    NYA_EntitySystem*     system = &nya_world()->entity_system;
    const NYA_EntityGrid* grid   = &system->grid;

    s32 min_x, min_y, max_x, max_y;
    _nya_entity_grid_cell(min, &min_x, &min_y);
    _nya_entity_grid_cell(max, &max_x, &max_y);

    u32 found = 0;

    for (s32 cell_y = min_y; cell_y <= max_y; cell_y++) {
        for (s32 cell_x = min_x; cell_x <= max_x; cell_x++) {
            u32 bucket = _nya_entity_grid_bucket(cell_x, cell_y);

            for (u32 slot = grid->buckets[bucket]; slot != NYA_ENTITY_GRID_EMPTY; slot = grid->next[slot]) {
                if (!system->occupied[slot]) continue;

                const NYA_Entity* entity = &system->entities[slot];

                /*
                 * Two filters, both load bearing. The cell check is what makes a *hash* grid correct:
                 * distant cells share buckets, so a bucket holds entities this query never asked about —
                 * and since each entity's own cell is visited exactly once, it also stops double-emitting.
                 */
                s32 entity_cell_x, entity_cell_y;
                _nya_entity_grid_cell((f32x2){ entity->position.x, entity->position.y }, &entity_cell_x, &entity_cell_y);

                if (entity_cell_x != cell_x || entity_cell_y != cell_y) continue;

                // The exact test: a cell is coarser than the rectangle that asked.
                if (entity->position.x < min.x || entity->position.x > max.x) continue;
                if (entity->position.y < min.y || entity->position.y > max.y) continue;

                if (filter_by_type && entity->type != type_filter) continue;

                // Every bit, not any bit: "flammable and wet" means both, not either.
                if (filter_by_flags && (entity->flags & flag_filter) != flag_filter) continue;

                out[found++] = entity->handle;
                if (found == capacity) return found;
            }
        }
    }

    return found;
}

void nya_entity_flag_enable(NYA_Entity* entity, u64 flags) {
    if (entity == nullptr) return;

    nya_entity_flags_set(entity, entity->flags | flags);
}

void nya_entity_flag_disable(NYA_Entity* entity, u64 flags) {
    if (entity == nullptr) return;

    nya_entity_flags_set(entity, entity->flags & ~flags);
}

void nya_entity_flags_set(NYA_Entity* entity, u64 flags) {
    if (entity == nullptr) return;
    if (entity->flags == flags) return;

    NYA_EntityIndex* index = &nya_world()->entity_system.index;

    u32 slot = entity->handle.index;

    // Only the bits that actually changed are touched: clearing every old bit and setting every new one
    // is six read-modify-writes instead of one for the common case of flipping one flag out of three.
    u64 changed = entity->flags ^ flags;

    while (changed != 0) {
        u32 bit = (u32)nya_bits_ctz_u64(changed);
        changed &= changed - 1;

        u64* bits = &index->flags[(u64)bit * NYA_ENTITY_BITSET_WORDS];

        if (flags & (1ULL << bit)) _nya_entity_bitset_set(bits, slot);
        else _nya_entity_bitset_clear(bits, slot);
    }

    entity->flags = flags;
}

/*
 * ─────────────────────────────────────────────────────────
 * ITERATION
 * ─────────────────────────────────────────────────────────
 */

NYA_EntityIter _nya_entity_iter_kind(u32 type) {
    NYA_EntityIndex* index = &nya_world()->entity_system.index;

    u64* bits = _nya_entity_index_kind_bits(type);

    NYA_EntityIter iter = {
        // A kind past the indexed range has no bitset, so the walk falls back to every live slot and
        // checks `type` per entity — slower, correct, and unreachable by accident.
        .bits       = bits != nullptr ? bits : index->live,
        .check_type = bits == nullptr,
        .require_type = type,

        // Positioned before the first word, so the first advance lands on the first match, letting the
        // macro test `entity` immediately with no separate priming call.
        .word       = 0,
        .word_count = _nya_entity_index_word_count(),
        .remaining  = 0,
    };

    // Prime it: `word` zero and `remaining` empty loads word zero and finds its first set bit, or walks
    // to the end and reports nothing.
    iter.remaining = iter.word_count > 0 ? iter.bits[0] & index->live[0] : 0;
    iter.entity    = nullptr;

    _nya_entity_iter_advance(&iter);

    return iter;
}

NYA_EntityIter _nya_entity_iter_flags(u64 flags) {
    NYA_EntityIndex* index = &nya_world()->entity_system.index;

    NYA_EntityIter iter = {
        .require_flags = flags,
        .word          = 0,
        .word_count    = _nya_entity_index_word_count(),
    };

    /*
     * Walk the first requested bit's set, checking the rest per entity. Intersecting every requested
     * bitset would yield strictly fewer candidates but need a scratch buffer, for a query that is nearly
     * always one flag and where the extra check is one AND against a word already in a register.
     *
     * No flags at all means "everything", which is the live set.
     */
    iter.bits = flags != 0 ? &index->flags[(u64)nya_bits_ctz_u64(flags) * NYA_ENTITY_BITSET_WORDS] : index->live;

    iter.remaining = iter.word_count > 0 ? iter.bits[0] & index->live[0] : 0;

    _nya_entity_iter_advance(&iter);

    return iter;
}

void _nya_entity_iter_advance(NYA_EntityIter* iter) {
    NYA_EntitySystem* system = &nya_world()->entity_system;

    for (;;) {
        // A zero word skips sixty four slots at once, making a rare kind nearly free to look for.
        while (iter->remaining == 0) {
            iter->word++;

            if (iter->word >= iter->word_count) {
                iter->entity = nullptr;
                return;
            }

            iter->remaining = iter->bits[iter->word] & system->index.live[iter->word];
        }

        u32 bit = (u32)nya_bits_ctz_u64(iter->remaining);
        iter->remaining &= iter->remaining - 1;

        u32 slot = (iter->word * 64) + bit;
        if (slot >= NYA_ENTITY_MAX) continue;

        NYA_Entity* entity = &system->entities[slot];

        if (iter->check_type && entity->type != iter->require_type) continue;
        if ((entity->flags & iter->require_flags) != iter->require_flags) continue;

        iter->entity = entity;
        return;
    }
}

NYA_INTERNAL void _nya_entity_apply_deferred_despawn(void* data) {
    NYA_EntityHandle handle = *(NYA_EntityHandle*)data;
    nya_entity_despawn(handle);
}

/*
 * ─────────────────────────────────────────────────────────
 * APPEARANCE
 * ─────────────────────────────────────────────────────────
 */

int _nya_entity_draw_entry_compare(const void* left, const void* right) {
    const NYA_EntityDrawEntry* a = left;
    const NYA_EntityDrawEntry* b = right;

    if (a->z_order < b->z_order) return -1;
    if (a->z_order > b->z_order) return 1;

    /*
     * Ties broken by texture: a draw call holds one texture, so sorting purely by depth would interleave
     * a hundred sprites sharing a sheet with sprites from another, turning one draw call into a hundred.
     * Grouping equal-depth entities by texture puts the batching back.
     *
     * Compared by address, since asset handles are interned literals — comparing the strings would be
     * strcmp inside a sort.
     */
    if ((uintptr_t)a->texture < (uintptr_t)b->texture) return -1;
    if ((uintptr_t)a->texture > (uintptr_t)b->texture) return 1;

    // Finally by slot, so the order is total — otherwise a sort could permute equal elements and two
    // sprites at the same depth on the same sheet would flicker past each other.
    if (a->handle.index < b->handle.index) return -1;
    if (a->handle.index > b->handle.index) return 1;

    return 0;
}

void _nya_entity_animation_step(NYA_Entity* entity, f32 delta_time_s) {
    if (entity->visual.kind != NYA_ENTITY_VISUAL_ANIMATION) return;

    NYA_SpriteAnimationSignal signals[NYA_SPRITE_ANIMATION_MAX_SIGNALS];

    u32 count = nya_sprite_animator_advance(&entity->visual.animator, delta_time_s, signals, nya_carray_length(signals));

    // Applied whether or not anything was signalled: a paused animator still has to draw the frame it's
    // paused on, and one that crossed no boundary this tick still has to draw the one it's in.
    nya_sprite_animator_apply(&entity->visual.animator, &entity->visual.atlas, &entity->visual.sprite);

    NYA_EntityOnAnimationFn on_animation = nya_callback_get(entity->on_animation);
    if (on_animation == nullptr) return;

    for (u32 i = 0; i < count; i++) {
        // Re-checked every iteration: a callback handling FINISHED may despawn the entity, and the
        // remaining signals would then deliver to a freed slot.
        if (!nya_entity_is_valid(entity->handle)) return;

        on_animation(entity, signals[i]);
    }
}

void _nya_entity_visual_draw(NYA_Entity* entity, NYA_Window* window) {
    switch (entity->visual.kind) {
        case NYA_ENTITY_VISUAL_SPRITE:
        case NYA_ENTITY_VISUAL_ANIMATION: {
            // Null until the texture asset resolves, normal for the first frames of a run.
            if (entity->visual.sprite.texture == nullptr) return;

            nya_render2d_sprite(window, &entity->visual.sprite, entity->position.xy);
        } break;

        case NYA_ENTITY_VISUAL_CUBE: {
            // Only while a 3D camera is set: there is no projection otherwise, and drawing through the
            // 2D one would put a cube somewhere arbitrary rather than nowhere.
            if (!nya_render3d_active(window)) return;

            nya_render3d_cube(window, entity->position, entity->visual.size, entity->rotation, entity->visual.color);
        } break;

        case NYA_ENTITY_VISUAL_NONE:
        case NYA_ENTITY_VISUAL_KIND_COUNT:
        default: break;
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * LIGHTS
 * ─────────────────────────────────────────────────────────
 */

/** One candidate light, with the score it is ranked by. See nya_system_entity_lights. */
typedef struct {
    NYA_Light2D light;
    f32x2           position;

    /** Radius times intensity: how much light this contributes at all. Larger wins. */
    f32 weight;
} NYA_EntityLightEntry;

NYA_INTERNAL int _nya_entity_light_compare(const void* left, const void* right) {
    const NYA_EntityLightEntry* a = left;
    const NYA_EntityLightEntry* b = right;

    // Descending, so the brightest survive the cap. Ties are left alone: two identical lights are
    // interchangeable, and forcing an order on them buys nothing.
    if (a->weight > b->weight) return -1;
    if (a->weight < b->weight) return 1;

    return 0;
}

u32 nya_system_entity_lights(f32x2 min, f32x2 max, OUT NYA_Light2D* out, OUT f32x2* out_positions, u32 capacity) {
    nya_perf_time_this_function();

    nya_assert(out != nullptr);
    nya_assert(out_positions != nullptr);

    if (capacity == 0) return 0;

    /*
     * A linear walk of the whole table rather than a spatial query: the grid indexes an entity's
     * *position*, but a light's reach is its radius, so a query would have to be widened by the largest
     * radius in the world (which nothing tracks) or by a guess. Lights are rare enough that walking is
     * honest and cheap.
     */
    static NYA_EntityLightEntry candidates[NYA_ENTITY_MAX];

    u32 found = 0;

    nya_entity_foreach (entity) {
        if (entity->light.radius <= 0.0F) continue;
        if (entity->light.intensity <= 0.0F) continue;

        f32x2 position = entity->position.xy + entity->light.offset;

        // Widened by the light's own radius: an off-screen light can still spill onto it, and its radius
        // is exactly the right margin, unlike a sprite's, which the renderer must guess at with
        // NYA_ENTITY_RENDER_CULL_MARGIN.
        f32 reach = entity->light.radius;

        if (position.x + reach < min.x || position.x - reach > max.x) continue;
        if (position.y + reach < min.y || position.y - reach > max.y) continue;

        candidates[found++] = (NYA_EntityLightEntry){
            .light    = entity->light,
            .position = position,
            .weight   = entity->light.radius * entity->light.intensity,
        };
    }

    // Sorted before the cap, so a scene with more lights than the pass can carry drops the ones nobody
    // would notice, not whichever was spawned last.
    if (found > capacity) qsort(candidates, found, sizeof(NYA_EntityLightEntry), _nya_entity_light_compare);

    u32 kept = nya_min(found, capacity);

    for (u32 i = 0; i < kept; i++) {
        out[i]           = candidates[i].light;
        out_positions[i] = candidates[i].position;

        // Zeroed is read as white here rather than at every consumer, so a light that only sets a size still works.
        if (out[i].color.r == 0.0F && out[i].color.g == 0.0F && out[i].color.b == 0.0F && out[i].color.a == 0.0F) out[i].color = NYA_COLOR_WHITE;
    }

    return kept;
}

NYA_EntityHandle _nya_entity_hover_move(NYA_EntityHandle hit) {
    NYA_EntitySystem* system = &nya_world()->entity_system;

    NYA_EntityHandle previous = system->hovered;

    // Nothing moved. The overwhelmingly common case, since this is called every frame and a cursor
    // rests on one thing for many of them, and the reason on_hover can be edge triggered at all.
    if (hit.index == previous.index && hit.generation == previous.generation) return hit;

    /*
     * Committed before either callback runs.
     *
     * Both callbacks are game code and either may call back into the entity system — a tooltip that
     * spawns, a highlight that despawns something. If the stored handle were still the old one at that
     * point, a callback asking nya_entity_hovered would be told the cursor is somewhere it has already
     * left, and one that called nya_entity_hover_clear would produce a second "left" for the same
     * entity.
     */
    system->hovered = hit;

    /*
     * Left first, then entered.
     *
     * A game whose leave handler clears a highlight and whose enter handler sets one would otherwise
     * clear the highlight it had just set, every time the cursor moved between two entities. Doing it
     * in this order makes the pair of callbacks compose without either knowing about the other.
     */
    NYA_Entity* left = nya_entity_get(previous);

    if (left != nullptr) {
        NYA_EntityOnHoverFn on_hover = nya_callback_get(left->on_hover);
        if (on_hover != nullptr) on_hover(left, false);
    }

    // Re-resolved rather than fetched above, because the leave callback has run in between and is
    // entitled to have despawned it.
    NYA_Entity* entered = nya_entity_get(hit);

    if (entered != nullptr) {
        NYA_EntityOnHoverFn on_hover = nya_callback_get(entered->on_hover);
        if (on_hover != nullptr) on_hover(entered, true);
    }

    return hit;
}

NYA_EntityHandle _nya_entity_click_deliver(NYA_EntityHandle hit, f32x3 world_point, u8 button) {
    NYA_Entity* entity = nya_entity_get(hit);
    if (entity == nullptr) return NYA_ENTITY_HANDLE_NONE;

    // No callback is how an entity declines to be clickable, so this is NONE rather than the handle:
    // a caller asking "was anything clicked" wants "no" when nothing reacted. See core_entity.h.
    NYA_EntityOnClickFn on_click = nya_callback_get(entity->on_click);
    if (on_click == nullptr) return NYA_ENTITY_HANDLE_NONE;

    on_click(entity, world_point, button);

    return hit;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * 3D QUERIES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The three-dimensional counterpart to _nya_entity_query.
 *
 * A separate walk rather than a z test bolted onto that one: the 2D queries are what a top-down game
 * uses every tick and are on a hot path, and widening their signature to carry a z range they would
 * always pass would cost every one of those callers for a case they do not have.
 *
 * Brute force over the slot table, which is what the editor case wants — a marquee select happens on
 * a click, not per tick, and a spatial index that had to be kept current every frame would cost more
 * than it saves.
 * */
NYA_INTERNAL u32 _nya_entity_query_box(f32x3 min, f32x3 max, b8 filter_type, u32 type, b8 filter_flags, u64 flags,
                                       OUT NYA_EntityHandle* out, u32 capacity) {
    nya_assert(out != nullptr);

    if (capacity == 0) return 0;

    u32 found = 0;

    for (u32 slot = 0; slot < nya_entity_slot_count(); slot++) {
        NYA_Entity* entity = nya_entity_at_slot(slot);

        if (entity == nullptr) continue;

        if (entity->position.x < min.x || entity->position.x > max.x) continue;
        if (entity->position.y < min.y || entity->position.y > max.y) continue;
        if (entity->position.z < min.z || entity->position.z > max.z) continue;

        if (filter_type && entity->type != type) continue;

        // Every bit, not any: a filter of two flags means both, which is what a caller asking for
        // "selectable and visible" means.
        if (filter_flags && (entity->flags & flags) != flags) continue;

        out[found] = entity->handle;
        found++;

        if (found >= capacity) break;
    }

    return found;
}

u32 nya_entity_query_box(f32x3 min, f32x3 max, OUT NYA_EntityHandle* out, u32 capacity) {
    return _nya_entity_query_box(min, max, false, 0, false, 0, out, capacity);
}

u32 nya_entity_query_box_kind(f32x3 min, f32x3 max, u32 type, OUT NYA_EntityHandle* out, u32 capacity) {
    return _nya_entity_query_box(min, max, true, type, false, 0, out, capacity);
}

u32 nya_entity_query_box_flags(f32x3 min, f32x3 max, u64 flags, OUT NYA_EntityHandle* out, u32 capacity) {
    return _nya_entity_query_box(min, max, false, 0, true, flags, out, capacity);
}

u32 nya_entity_query_sphere(f32x3 center, f32 radius, OUT NYA_EntityHandle* out, u32 capacity) {
    nya_assert(out != nullptr);

    if (radius <= 0.0F || capacity == 0) return 0;

    // The bounding box first, then an exact distance test on what it returns — the same two step the
    // 2D radius query uses, and squared throughout so the sphere costs no square root per candidate.
    f32x3 min = { center.x - radius, center.y - radius, center.z - radius };
    f32x3 max = { center.x + radius, center.y + radius, center.z + radius };

    u32 candidates = _nya_entity_query_box(min, max, false, 0, false, 0, out, capacity);

    f32 radius_squared = radius * radius;

    u32 kept = 0;

    for (u32 i = 0; i < candidates; i++) {
        NYA_Entity* entity = nya_entity_get(out[i]);

        if (entity == nullptr) continue;

        f32x3 delta = entity->position - center;

        if ((delta.x * delta.x) + (delta.y * delta.y) + (delta.z * delta.z) > radius_squared) continue;

        // Compacted in place: the survivors move down over the rejects, so the caller's array holds
        // exactly the hits with no second pass and no scratch.
        out[kept] = out[i];
        kept++;
    }

    return kept;
}

NYA_EntityHandle nya_entity_query_ray(f32x3 origin, f32x3 direction, f32 radius, OUT f32* out_distance) {
    if (out_distance != nullptr) *out_distance = 0.0F;

    if (radius <= 0.0F) return NYA_ENTITY_HANDLE_NONE;

    f32 length = sqrtf((direction.x * direction.x) + (direction.y * direction.y) + (direction.z * direction.z));

    // A zero length direction is not a ray. Returning nothing beats dividing by it.
    if (length <= 0.0F) return NYA_ENTITY_HANDLE_NONE;

    f32x3 unit = { direction.x / length, direction.y / length, direction.z / length };

    NYA_EntityHandle nearest          = NYA_ENTITY_HANDLE_NONE;
    f32              nearest_distance = 0.0F;

    f32 radius_squared = radius * radius;

    for (u32 slot = 0; slot < nya_entity_slot_count(); slot++) {
        NYA_Entity* entity = nya_entity_at_slot(slot);

        if (entity == nullptr) continue;

        f32x3 to_entity = entity->position - origin;

        /*
         * The projection of the entity onto the ray, then its perpendicular distance from it.
         *
         * `along` behind the origin means the entity is behind the camera, which is never a hit — a
         * click must not select something out of view.
         */
        f32 along = (to_entity.x * unit.x) + (to_entity.y * unit.y) + (to_entity.z * unit.z);

        if (along < 0.0F) continue;

        f32x3 closest = { origin.x + (unit.x * along), origin.y + (unit.y * along), origin.z + (unit.z * along) };
        f32x3 offset  = entity->position - closest;

        if ((offset.x * offset.x) + (offset.y * offset.y) + (offset.z * offset.z) > radius_squared) continue;

        // Nearest along the ray wins, so clicking a stack of objects picks the front one.
        if (nearest.generation != 0 && along >= nearest_distance) continue;

        nearest          = entity->handle;
        nearest_distance = along;
    }

    if (out_distance != nullptr) *out_distance = nearest_distance;

    return nearest;
}

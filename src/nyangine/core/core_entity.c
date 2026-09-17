#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_entity_apply_deferred_despawn(void* data);

/** Commits the next NYA_ENTITY_COMMIT_SLOTS slots of the table. False when the system refuses. */
NYA_INTERNAL b8 _nya_entity_table_commit(NYA_EntitySystem* system) __attr_no_discard;

/** Advances a running nya_entity_move_to by one tick. No-op for an entity with no move. */
NYA_INTERNAL void _nya_entity_target_step(NYA_Entity* entity, f32 delta_time_s);

/** Advances an animated entity's animator and delivers its signals to on_animation. */
NYA_INTERNAL void _nya_entity_animation_step(NYA_Entity* entity, f32 delta_time_s);

/** Draws whatever NYA_EntityVisual says, before the entity's own on_render runs. */
NYA_INTERNAL void _nya_entity_visual_draw(NYA_Entity* entity, NYA_Window* window);

/** Sort comparator: depth, then texture to keep batching, then slot to make the order total. */
NYA_INTERNAL int _nya_entity_draw_entry_compare(const void* left, const void* right);

/** One entity in a draw list, with the keys it is sorted on. See nya_system_entity_render_in. */
typedef struct {
    NYA_EntityHandle handle;
    f32              z_order;

    /**
     * The texture handle the entity's visual draws with, or null.
     * */
    NYA_ConstCString texture;
} NYA_EntityDrawEntry;

/** Adds a slot to every bitset its kind and flags put it in. */
NYA_INTERNAL void _nya_entity_index_add(u32 slot, u32 type, u64 flags);

/** The inverse, given the kind and flags the entity had. */
NYA_INTERNAL void _nya_entity_index_remove(u32 slot, u32 type, u64 flags);

/** The bitset for a kind, or null when that kind is past NYA_ENTITY_KIND_MAX. */
NYA_INTERNAL u64* _nya_entity_index_kind_bits(u32 type);

/** Bitset words that could hold anything, up to the table's high water mark. */
NYA_INTERNAL u32 _nya_entity_index_word_count(void);

NYA_INTERNAL void _nya_entity_bitset_set(u64* bits, u32 slot);
NYA_INTERNAL void _nya_entity_bitset_clear(u64* bits, u32 slot);

/** Which cell a world position falls in. Floored, so it is continuous across zero. */
NYA_INTERNAL void _nya_entity_grid_cell(f32x2 position, OUT s32* out_x, OUT s32* out_y);

/** Cell coordinates to a bucket, masked, hence the power-of-two count. */
NYA_INTERNAL u32 _nya_entity_grid_bucket(s32 cell_x, s32 cell_y);

/** The body of every query: walks the cells covering a rectangle and emits what is really inside it. */
NYA_INTERNAL u32 _nya_entity_query(f32x2 min, f32x2 max, b8 filter_by_type, u32 type_filter, b8 filter_by_flags, u64 flag_filter,
                                   OUT NYA_EntityHandle* out, u32 capacity);

/**
 * Runs `hit`'s on_click, or reports that there was nothing to run. Shared by both nya_entity_click
 * overloads.
 * */
NYA_INTERNAL NYA_EntityHandle _nya_entity_click_deliver(NYA_EntityHandle hit, f32x3 world_point, u8 button);

/**
 * Moves the hover to `hit`, running the two callbacks the move implies.
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
        .entities    = nya_memory_reserve(NYA_ENTITY_MAX * sizeof(NYA_Entity)),
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

    nya_assert(system->entities != nullptr, "could not reserve address space for %d entities", NYA_ENTITY_MAX);

    // the table itself is not touched: slots are committed and their generations set as spawning reaches them.
    nya_memset(system->occupied, 0, NYA_ENTITY_MAX * sizeof(b8));

    // empty, not zero: slot 0 is a real entity.
    for (u32 i = 0; i < NYA_ENTITY_GRID_BUCKETS; i++) system->grid.buckets[i] = NYA_ENTITY_GRID_EMPTY;

    // zero is the right empty for the bitsets.
    nya_memset(system->index.live, 0, NYA_ENTITY_BITSET_WORDS * sizeof(u64));
    nya_memset(system->index.kinds, 0, (u64)NYA_ENTITY_KIND_MAX * NYA_ENTITY_BITSET_WORDS * sizeof(u64));
    nya_memset(system->index.flags, 0, (u64)NYA_ENTITY_FLAG_COUNT * NYA_ENTITY_BITSET_WORDS * sizeof(u64));

    nya_log_info("Entity system initialized (%d slots, %d grid buckets at %.0f units).", NYA_ENTITY_MAX, NYA_ENTITY_GRID_BUCKETS,
             (f64)NYA_ENTITY_GRID_CELL_SIZE);
}

void nya_system_entity_deinit(void) {
    nya_entity_clear();

    NYA_EntitySystem* system = &nya_world()->entity_system;

    nya_memory_release(system->entities, NYA_ENTITY_MAX * sizeof(NYA_Entity));
    nya_arena_destroy(system->allocator);
    *system = (NYA_EntitySystem){ 0 };

    nya_log_info("Entity system deinitialized.");
}

void nya_system_entity_update(f32 delta_time_s) {
    // walks every slot, so this scales with the world, not with what is awake.
    nya_perf_time_this_function();

    // physics has written this tick's positions, so queries during update see them.
    nya_system_entity_grid_rebuild();

    nya_entity_foreach (entity) {
        if (!nya_flag_check(entity->state, NYA_ENTITY_STATE_ACTIVE)) continue;

        // integrated before the callback, so update sees this tick's position. skipped for simulated entities: the
        // solver already wrote the transform, and integrating again would double the velocity.
        if (!nya_flag_check(entity->state, NYA_ENTITY_STATE_STATIC) && !entity->physics2d.attached) {
            entity->position += entity->velocity * delta_time_s;

            // radians per second per axis; per tick that is small enough for quaternion compose and renormalize.
            f32x3 delta   = entity->angular_velocity * delta_time_s;
            f32x3 squared = delta * delta;
            if (squared.x + squared.y + squared.z > 0.0F) {
                NYA_Quaternion spin = nya_quaternion_from_euler(delta.x, delta.y, delta.z);
                entity->rotation    = nya_quaternion_normalize(nya_quaternion_multiply(spin, entity->rotation));
            }
        }

        // after integration: a move states an absolute position, and velocity must not bend it.
        _nya_entity_target_step(entity, delta_time_s);

        _nya_entity_animation_step(entity, delta_time_s);

        NYA_EntityOnUpdateFn on_update = nya_callback_get(entity->on_update);
        if (on_update != nullptr) on_update(entity, delta_time_s);
    }

    // last, so it includes everything that moved this tick. a callback that needs a child moved within its own
    // tick calls nya_entity_transform_sync.
    nya_system_entity_transforms_update();
}

/*
 * ─────────────────────────────────────────────────────────
 * HIERARCHY
 * ─────────────────────────────────────────────────────────
 */

/** Whether two handles name the same live entity. */
NYA_INTERNAL b8 _nya_entity_handle_equals(NYA_EntityHandle a, NYA_EntityHandle b) {
    return a.index == b.index && a.generation == b.generation;
}

/** Takes `child` out of its parent's sibling list. A no-op for a root. */
NYA_INTERNAL void _nya_entity_unlink(NYA_Entity* child) {
    NYA_Entity* parent = nya_entity_get(child->parent);

    child->parent = NYA_ENTITY_HANDLE_NONE;

    // the parent may be gone already; either way the child is a root now.
    if (parent == nullptr) {
        child->next_sibling = NYA_ENTITY_HANDLE_NONE;
        return;
    }

    if (_nya_entity_handle_equals(parent->first_child, child->handle)) {
        parent->first_child = child->next_sibling;
    } else {
        // removal walks to the previous node. sibling lists are short, and a back pointer would cost every entity.
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

/** Captures `child`'s offset from `parent` from their current world transforms. */
NYA_INTERNAL void _nya_entity_capture_local(NYA_Entity* child, const NYA_Entity* parent) {
    /*
     * The inverse of the parent's transform applied to the child's, for the pieces this engine composes. A
     * parent with both rotation and non-uniform scale has no exact inverse of this form and is unsupported;
     * the uniform and the unrotated cases are exact.
     */
    f32x3 scale = parent->scale;

    // a zero scale axis is read as one, so a zeroed parent acts as identity instead of dividing by zero.
    if (scale.x == 0.0F) scale.x = 1.0F;
    if (scale.y == 0.0F) scale.y = 1.0F;
    if (scale.z == 0.0F) scale.z = 1.0F;

    NYA_Quaternion inverse_rotation = nya_quaternion_conjugate(nya_quaternion_normalize(parent->rotation));

    f32x3 offset = child->position - parent->position;

    child->local_position = nya_quaternion_rotate(inverse_rotation, offset) / scale;
    child->local_rotation = nya_quaternion_multiply(inverse_rotation, child->rotation);
    child->local_scale    = child->scale / scale;
}

/** Writes `child`'s world transform from `parent`'s and the stored local. */
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

    // unparenting keeps the world transform.
    if (!nya_entity_is_valid(parent_handle)) {
        _nya_entity_unlink(child);
        return true;
    }

    if (_nya_entity_handle_equals(child_handle, parent_handle)) {
        nya_log_error("Cannot parent entity '%s' to itself.", child->name ? child->name : "(unnamed)");
        return false;
    }

    /*
     * A cycle would make propagation recurse forever. Checked before unlinking, so a refused reparent changes
     * nothing.
     */
    if (nya_entity_is_ancestor(child_handle, parent_handle)) {
        nya_log_error("Cannot parent entity '%s' to its own descendant; that would make a cycle.", child->name ? child->name : "(unnamed)");
        return false;
    }

    // already parented here; relinking would reorder the siblings for nothing.
    if (_nya_entity_handle_equals(child->parent, parent_handle)) return true;

    _nya_entity_unlink(child);

    NYA_Entity* parent = nya_entity_get(parent_handle);

    // pushed at the front; nothing depends on sibling order.
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
        // counted past the capacity, so the caller can size a buffer from the answer.
        if (out != nullptr && count < capacity) out[count] = child->handle;

        count++;
    }

    return count;
}

void nya_entity_transform_sync(NYA_EntityHandle handle) {
    NYA_Entity* entity = nya_entity_get(handle);
    if (entity == nullptr) return;

    // the entity itself first, or it would lag one link behind its parent.
    NYA_Entity* parent = nya_entity_get(entity->parent);
    if (parent != nullptr) _nya_entity_compose(entity, parent);

    _nya_entity_propagate(entity);
}

void nya_system_entity_transforms_update(void) {
    NYA_EntitySystem* system = &nya_world()->entity_system;

    // the whole cost of the hierarchy when nothing is parented.
    if (system->parented_count == 0) return;

    nya_perf_time_this_function();

    /* From the roots down, so a chain resolves in one pass. */
    nya_entity_foreach (entity) {
        if (entity->first_child.generation == 0) continue;
        if (nya_entity_is_valid(entity->parent)) continue;

        _nya_entity_propagate(entity);
    }
}

f32_4x4 nya_entity_world_matrix(const NYA_Entity* entity) {
    if (entity == nullptr) return f32_4x4_id;

    // all-zero scale is a zeroed entity, and its matrix would be singular. read as one.
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

    // the running move is abandoned first, or two tweens would write the same value.
    nya_entity_move_stop(entity);

    // zero duration teleports, without spending a tween slot and a frame.
    if (duration_s <= 0.0F) {
        if (entity->physics2d.attached) nya_physics2d_teleport(entity, target.xy, nya_physics2d_rotation(entity));
        else entity->position = target;

        entity->move_position = target;
        return;
    }

    /*
     * The tween reads its start at the first sample, which for a delayed move is correct. The staging value is
     * still seeded, because _nya_entity_target_step applies it during the delay too.
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

    // by target, so tweens started on move_position directly stop too.
    nya_tween_cancel_target(&entity->move_position);
    entity->move_tween    = NYA_TWEEN_NONE;
    entity->move_settling = false;

    // a kinematic body keeps moving at the last commanded velocity unless it is cleared. dynamic bodies own
    // their velocity.
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
     * The handle is cleared here, not by the tween, so a set handle that no longer resolves means "arrived this
     * tick", when the final value still has to be applied.
     */
    if (entity->move_tween.generation == 0) {
        /*
         * The tick after a body-backed move arrived. The solver consumes the final velocity at the start of this
         * update, so this is the earliest tick it can be cleared without losing that step.
         */
        if (entity->move_settling) {
            entity->move_settling = false;
            if (entity->physics2d.attached) nya_physics2d_velocity_set(entity, f32x2_zero);
        }

        return;
    }

    b8 arrived = !nya_tween_active(entity->move_tween);

    if (entity->physics2d.attached) {
        // kinematic bodies move by velocity, so they sweep through the tick and push what stands on them.
        nya_physics2d_velocity_set(entity, (entity->move_position.xy - entity->position.xy) / delta_time_s);
    } else {
        entity->position = entity->move_position;
    }

    if (arrived) {
        // cleared first, so nya_entity_moving reports false on the arrival tick.
        entity->move_tween    = NYA_TWEEN_NONE;
        entity->move_settling = entity->physics2d.attached;
    }
}

f32 nya_entity_sort_key(const NYA_Entity* entity) {
    if (entity == nullptr) return 0.0F;

    // the anchor is added, so a centred sprite only says how far down its feet are.
    if (entity->visual.y_sorted) return entity->position.y + entity->visual.y_sort_anchor;

    return entity->visual.z_order;
}

void nya_system_entity_render(NYA_Window* window) {
    nya_assert(window != nullptr);

    u32 target_width, target_height;
    nya_render2d_target_size(window, &target_width, &target_height);

    /* All four corners: under camera rotation, two opposite corners do not bound the view. */
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
     * Collected and sorted first. Queries answer in bucket order, which changes as entities cross cells, so
     * without sorting sprites could swap depth.
     */
    /*
     * From the frame arena, which matches this buffer's lifetime, instead of a function static sized by
     * NYA_ENTITY_MAX.
     */
    NYA_EntityDrawEntry* entries = nya_arena_alloc(nya_app_get()->frame_allocator, count * sizeof(NYA_EntityDrawEntry));

    // nothing to draw, and nothing allocated.
    if (entries == nullptr) return;

    u32 entry_count = 0;

    for (u32 i = 0; i < count; i++) {
        NYA_Entity* entity = nya_entity_get(visible[i]);
        if (entity == nullptr) continue;

        // invisible is one bit.
        if (!nya_flag_check(entity->state, NYA_ENTITY_STATE_VISIBLE)) continue;

        entries[entry_count++] = (NYA_EntityDrawEntry){
            .handle  = entity->handle,
            .z_order = nya_entity_sort_key(entity),
            .texture = entity->visual.sprite.texture,
        };
    }

    // qsort: nya_array_sort wants an NYA_Array.
    qsort(entries, entry_count, sizeof(NYA_EntityDrawEntry), _nya_entity_draw_entry_compare);

    for (u32 i = 0; i < entry_count; i++) {
        NYA_Entity* entity = nya_entity_get(entries[i].handle);

        // re-resolved each time: on_render may despawn later entries.
        if (entity == nullptr) continue;

        _nya_entity_visual_draw(entity, window);

        // after the visual, so a health bar draws over its sprite.
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

    // only bucket heads are cleared; chain links are overwritten on insert.
    for (u32 i = 0; i < NYA_ENTITY_GRID_BUCKETS; i++) grid->buckets[i] = NYA_ENTITY_GRID_EMPTY;

    grid->count = 0;

    for (u32 slot = 0; slot < system->high_water_mark; slot++) {
        if (!system->occupied[slot]) continue;

        const NYA_Entity* entity = &system->entities[slot];

        s32 cell_x, cell_y;
        _nya_entity_grid_cell((f32x2){ entity->position.x, entity->position.y }, &cell_x, &cell_y);

        u32 bucket = _nya_entity_grid_bucket(cell_x, cell_y);

        // pushed at the head, two writes.
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

    // bounding square first, then an exact squared distance test.
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
    // z zero: the 2D world is the z = 0 plane.
    return _nya_entity_click_deliver(nya_physics2d_entity_at(world_point), (f32x3){ world_point.x, world_point.y, 0.0F }, button);
}

NYA_EntityHandle nya_entity_hover(f32x2 world_point) __attr_overloaded {
    return _nya_entity_hover_move(nya_physics2d_entity_at(world_point));
}

NYA_EntityHandle nya_entity_hover(f32x3 origin, f32x3 direction) __attr_overloaded {
    // the point is dropped; on_hover takes none.
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

    /* The point on the struck surface, not the ray origin, so a click knows which face was hit. */
    return _nya_entity_click_deliver(hit, point, button);
}

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

NYA_EntityHandle nya_entity_spawn_with_options(NYA_EntitySpawnOptions options) {
    NYA_EntitySystem* system = &nya_world()->entity_system;

    if (system->free_count == 0 && system->touched_slots == NYA_ENTITY_MAX) {
        nya_log_error("Cannot spawn entity '%s': all %d entity slots are in use.", options.name ? options.name : "(unnamed)", NYA_ENTITY_MAX);
        return NYA_ENTITY_HANDLE_NONE;
    }

    // a despawned slot first, so the table only grows once every earlier slot is live.
    u32 slot = system->free_count > 0 ? system->free_slots[--system->free_count] : system->touched_slots;

    if (slot == system->touched_slots) {
        if (slot == system->committed_slots && !_nya_entity_table_commit(system)) {
            nya_log_error("Cannot spawn entity '%s': committing memory for slot %u failed.", options.name ? options.name : "(unnamed)", slot);
            return NYA_ENTITY_HANDLE_NONE;
        }

        // generations start at 1, so a zeroed handle never resolves to slot 0.
        system->generations[slot] = 1;
        system->touched_slots++;
    }

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

        /* A root, with an identity local transform. */
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

    // the entity is live, so on_spawn can spawn, despawn itself, or pass its handle on.
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
     * Both solvers, after on_despawn (which may still read the body) and before the slot is cleared. A
     * forgotten body keeps colliding and can never be destroyed.
     */
    nya_physics2d_body_detach(entity);
    nya_physics3d_body_detach(entity);

    // a running tween would keep writing into this slot after reuse. by address, which also catches tweens a
    // caller aimed at move_position.
    nya_tween_cancel_target(&target->move_position);

    /*
     * Despawning a parent despawns its children recursively. Call nya_entity_parent_clear on a child first to
     * keep it.
     */
    while (nya_entity_is_valid(target->first_child)) {
        NYA_EntityHandle child = target->first_child;

        // unlinked first, so the recursion cannot reach this entity again and `first_child` advances.
        nya_entity_parent_clear(child);
        nya_entity_despawn(child);
    }

    // and it leaves its parent's list, which would otherwise name a reused slot.
    if (nya_entity_is_valid(target->parent)) nya_entity_parent_clear(entity);

    /*
     * The hover is dropped without on_hover, since on_despawn already ran. Clearing it keeps nya_entity_hovered
     * correct this frame.
     */
    if (system->hovered.index == entity.index && system->hovered.generation == entity.generation) {
        system->hovered = NYA_ENTITY_HANDLE_NONE;
    }

    // before clearing the slot: removal needs the kind and flags.
    _nya_entity_index_remove(entity.index, target->type, target->flags);

    // bumping the generation invalidates every outstanding handle.
    system->generations[entity.index]++;
    system->occupied[entity.index] = false;
    system->count--;

    system->free_slots[system->free_count++] = entity.index;

    *target = (NYA_Entity){ 0 };
}

void nya_entity_despawn_deferred(NYA_EntityHandle entity) {
    NYA_Entity* target = nya_entity_get(entity);
    if (target == nullptr) return;

    // idempotent: two hits in one tick must not queue two despawns.
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

    // by slot: nya_entity_foreach is not safe under despawning.
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

b8 _nya_entity_table_commit(NYA_EntitySystem* system) {
    nya_assert(system->committed_slots < NYA_ENTITY_MAX);

    u32 slots = nya_min((u32)NYA_ENTITY_COMMIT_SLOTS, NYA_ENTITY_MAX - system->committed_slots);

    if (!nya_memory_commit(&system->entities[system->committed_slots], (u64)slots * sizeof(NYA_Entity))) return false;

    system->committed_slots += slots;

    nya_assert(system->committed_slots <= NYA_ENTITY_MAX);
    return true;
}

void _nya_entity_grid_cell(f32x2 position, OUT s32* out_x, OUT s32* out_y) {
    f32 cell_size = nya_world()->entity_system.grid.cell_size;

    // floorf: a cast truncates toward zero and mirrors the grid about the origin.
    *out_x = (s32)floorf(position.x / cell_size);
    *out_y = (s32)floorf(position.y / cell_size);
}

u32 _nya_entity_index_word_count(void) {
    // rounded up to include the word holding the highest slot. the high water mark only grows, so bits beyond it
    // were never set.
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

    // clearing `live` already makes the slot unreachable, but a stale kind or flag bit would come back on the
    // next spawn into this slot.
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
     * The usual large primes, multiplied in u64: the u32 form wraps, which the sanitized build aborts on.
     * Casting the signed coordinate through u32 first keeps negatives from sign extending.
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
                 * Two filters. Distant cells share buckets, so the cell check drops entities this query did not ask about,
                 * and since each entity's cell is visited once it also prevents duplicates.
                 */
                s32 entity_cell_x, entity_cell_y;
                _nya_entity_grid_cell((f32x2){ entity->position.x, entity->position.y }, &entity_cell_x, &entity_cell_y);

                if (entity_cell_x != cell_x || entity_cell_y != cell_y) continue;

                // the exact test: a cell is coarser than the rectangle.
                if (entity->position.x < min.x || entity->position.x > max.x) continue;
                if (entity->position.y < min.y || entity->position.y > max.y) continue;

                if (filter_by_type && entity->type != type_filter) continue;

                // every bit, not any.
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

    // only changed bits are touched.
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
        // a kind past the indexed range has no bitset, so the walk checks `type` per live slot.
        .bits       = bits != nullptr ? bits : index->live,
        .check_type = bits == nullptr,
        .require_type = type,

        // positioned before the first word, so the first advance lands on the first match.
        .word       = 0,
        .word_count = _nya_entity_index_word_count(),
        .remaining  = 0,
    };

    // loads word zero and finds its first set bit, or reports nothing.
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
     * Walks the first requested bit's set and checks the rest per entity. Intersecting bitsets would need
     * scratch, and queries are nearly always one flag.
     */
    iter.bits = flags != 0 ? &index->flags[(u64)nya_bits_ctz_u64(flags) * NYA_ENTITY_BITSET_WORDS] : index->live;

    iter.remaining = iter.word_count > 0 ? iter.bits[0] & index->live[0] : 0;

    _nya_entity_iter_advance(&iter);

    return iter;
}

void _nya_entity_iter_advance(NYA_EntityIter* iter) {
    NYA_EntitySystem* system = &nya_world()->entity_system;

    for (;;) {
        // a zero word skips 64 slots, so rare kinds are nearly free.
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

    /* Ties broken by texture, so equal-depth sprites from one sheet stay one draw call. */
    if ((uintptr_t)a->texture < (uintptr_t)b->texture) return -1;
    if ((uintptr_t)a->texture > (uintptr_t)b->texture) return 1;

    // then by slot, so the order is total and equal sprites do not flicker.
    if (a->handle.index < b->handle.index) return -1;
    if (a->handle.index > b->handle.index) return 1;

    return 0;
}

void _nya_entity_animation_step(NYA_Entity* entity, f32 delta_time_s) {
    if (entity->visual.kind != NYA_ENTITY_VISUAL_ANIMATION) return;

    NYA_SpriteAnimationSignal signals[NYA_SPRITE_ANIMATION_MAX_SIGNALS];

    u32 count = nya_sprite_animator_advance(&entity->visual.animator, delta_time_s, signals, nya_carray_length(signals));

    // applied every tick: a paused animator still draws its frame.
    nya_sprite_animator_apply(&entity->visual.animator, &entity->visual.atlas, &entity->visual.sprite);

    NYA_EntityOnAnimationFn on_animation = nya_callback_get(entity->on_animation);
    if (on_animation == nullptr) return;

    for (u32 i = 0; i < count; i++) {
        // rechecked every iteration: handling FINISHED may despawn the entity.
        if (!nya_entity_is_valid(entity->handle)) return;

        on_animation(entity, signals[i]);
    }
}

void _nya_entity_visual_draw(NYA_Entity* entity, NYA_Window* window) {
    switch (entity->visual.kind) {
        case NYA_ENTITY_VISUAL_SPRITE:
        case NYA_ENTITY_VISUAL_ANIMATION: {
            // null until the texture loads.
            if (entity->visual.sprite.texture == nullptr) return;

            nya_render2d_sprite(window, &entity->visual.sprite, entity->position.xy);
        } break;

        case NYA_ENTITY_VISUAL_CUBE: {
            // only with a 3D camera set; the 2D projection would put the cube somewhere arbitrary.
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

    /** Radius times intensity. Larger wins. */
    f32 weight;
} NYA_EntityLightEntry;

NYA_INTERNAL int _nya_entity_light_compare(const void* left, const void* right) {
    const NYA_EntityLightEntry* a = left;
    const NYA_EntityLightEntry* b = right;

    // descending, so the brightest survive the cap.
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
     * A walk over the whole table: the grid indexes positions, but a light's reach is its radius, which no query
     * could bound. Lights are rare.
     */
    /* From the frame arena, for the draw list's reason. Sized by the ceiling, since any entity may carry a light. */
    NYA_EntityLightEntry* candidates = nya_arena_alloc(nya_app_get()->frame_allocator, NYA_ENTITY_MAX * sizeof(NYA_EntityLightEntry));
    if (candidates == nullptr) return 0;

    u32 found = 0;

    nya_entity_foreach (entity) {
        if (entity->light.radius <= 0.0F) continue;
        if (entity->light.intensity <= 0.0F) continue;

        f32x2 position = entity->position.xy + entity->light.offset;

        // widened by the light's radius, since an off-screen light can still spill onto the view.
        f32 reach = entity->light.radius;

        if (position.x + reach < min.x || position.x - reach > max.x) continue;
        if (position.y + reach < min.y || position.y - reach > max.y) continue;

        candidates[found++] = (NYA_EntityLightEntry){
            .light    = entity->light,
            .position = position,
            .weight   = entity->light.radius * entity->light.intensity,
        };
    }

    // sorted before the cap, so the least noticeable lights are dropped.
    if (found > capacity) qsort(candidates, found, sizeof(NYA_EntityLightEntry), _nya_entity_light_compare);

    u32 kept = nya_min(found, capacity);

    for (u32 i = 0; i < kept; i++) {
        out[i]           = candidates[i].light;
        out_positions[i] = candidates[i].position;

        // zeroed colour is read as white here, so a light that only sets a size works.
        if (out[i].color.r == 0.0F && out[i].color.g == 0.0F && out[i].color.b == 0.0F && out[i].color.a == 0.0F) out[i].color = NYA_COLOR_WHITE;
    }

    return kept;
}

NYA_EntityHandle _nya_entity_hover_move(NYA_EntityHandle hit) {
    NYA_EntitySystem* system = &nya_world()->entity_system;

    NYA_EntityHandle previous = system->hovered;

    // nothing moved: the common case, and why on_hover can be edge triggered.
    if (hit.index == previous.index && hit.generation == previous.generation) return hit;

    /* Committed before either callback runs. */
    system->hovered = hit;

    /* Left first, then entered. */
    NYA_Entity* left = nya_entity_get(previous);

    if (left != nullptr) {
        NYA_EntityOnHoverFn on_hover = nya_callback_get(left->on_hover);
        if (on_hover != nullptr) on_hover(left, false);
    }

    // re-resolved: the leave callback may have despawned it.
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

    // no callback means not clickable, so this reports NONE.
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

/** The 3D counterpart to _nya_entity_query. */
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

        // every bit, not any.
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

    // bounding box, then an exact squared distance test.
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

        // compacted in place, so the caller's array holds exactly the hits.
        out[kept] = out[i];
        kept++;
    }

    return kept;
}

NYA_EntityHandle nya_entity_query_ray(f32x3 origin, f32x3 direction, f32 radius, OUT f32* out_distance) {
    if (out_distance != nullptr) *out_distance = 0.0F;

    if (radius <= 0.0F) return NYA_ENTITY_HANDLE_NONE;

    f32 length = sqrtf((direction.x * direction.x) + (direction.y * direction.y) + (direction.z * direction.z));

    // a zero length direction is not a ray.
    if (length <= 0.0F) return NYA_ENTITY_HANDLE_NONE;

    f32x3 unit = { direction.x / length, direction.y / length, direction.z / length };

    NYA_EntityHandle nearest          = NYA_ENTITY_HANDLE_NONE;
    f32              nearest_distance = 0.0F;

    f32 radius_squared = radius * radius;

    for (u32 slot = 0; slot < nya_entity_slot_count(); slot++) {
        NYA_Entity* entity = nya_entity_at_slot(slot);

        if (entity == nullptr) continue;

        f32x3 to_entity = entity->position - origin;

        /* The entity projected onto the ray, then its perpendicular distance. */
        f32 along = (to_entity.x * unit.x) + (to_entity.y * unit.y) + (to_entity.z * unit.z);

        if (along < 0.0F) continue;

        f32x3 closest = { origin.x + (unit.x * along), origin.y + (unit.y * along), origin.z + (unit.z * along) };
        f32x3 offset  = entity->position - closest;

        if ((offset.x * offset.x) + (offset.y * offset.y) + (offset.z * offset.z) > radius_squared) continue;

        // nearest along the ray wins, so the front object of a stack is picked.
        if (nearest.generation != 0 && along >= nearest_distance) continue;

        nearest          = entity->handle;
        nearest_distance = along;
    }

    if (out_distance != nullptr) *out_distance = nearest_distance;

    return nearest;
}

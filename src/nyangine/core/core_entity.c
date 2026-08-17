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
     * A pointer, compared by address rather than by content: asset handles are interned string
     * literals, so two sprites out of one sheet share the identical pointer and comparing the bytes
     * would be strcmp in a sort comparator.
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
 * Shared by both nya_entity_click overloads so the "no entity, or no callback, is not a click"
 * rule is written once. The two differ only in how they find the entity and the point.
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

    // Generations start at 1 so a zeroed handle, which is what an uninitialized struct field holds,
    // never resolves to slot 0.
    for (u32 i = 0; i < NYA_ENTITY_MAX; i++) system->generations[i] = 1;

    // Seeded in reverse so the first spawns come out of slot 0 upward, which keeps the high water
    // mark tight and makes a fresh world read sensibly in a debugger.
    for (u32 i = 0; i < NYA_ENTITY_MAX; i++) system->free_slots[i] = NYA_ENTITY_MAX - 1 - i;
    system->free_count = NYA_ENTITY_MAX;

    // Empty rather than zeroed: slot 0 is a legal entity, so zero cannot mean "nothing here".
    for (u32 i = 0; i < NYA_ENTITY_GRID_BUCKETS; i++) system->grid.buckets[i] = NYA_ENTITY_GRID_EMPTY;

    // Zero *is* the right empty for the index — a clear bit means "not in this set".
    nya_memset(system->index.live, 0, NYA_ENTITY_BITSET_WORDS * sizeof(u64));
    nya_memset(system->index.kinds, 0, (u64)NYA_ENTITY_KIND_MAX * NYA_ENTITY_BITSET_WORDS * sizeof(u64));
    nya_memset(system->index.flags, 0, (u64)NYA_ENTITY_FLAG_COUNT * NYA_ENTITY_BITSET_WORDS * sizeof(u64));

    nya_info("Entity system initialized (%d slots, %d grid buckets at %.0f units).", NYA_ENTITY_MAX, NYA_ENTITY_GRID_BUCKETS,
             (f64)NYA_ENTITY_GRID_CELL_SIZE);
}

void nya_system_entity_deinit(void) {
    nya_entity_clear();

    NYA_EntitySystem* system = &nya_world()->entity_system;

    nya_arena_destroy(system->allocator);
    *system = (NYA_EntitySystem){ 0 };

    nya_info("Entity system deinitialized.");
}

void nya_system_entity_update(f32 delta_time_s) {
    // A linear walk over every slot, so this grows with the world rather than with what is awake.
    // Worth seeing separately from the rest of frame_updating for exactly that reason.
    nya_perf_time_this_function();

    // Before the callbacks, so a layer or an entity querying during its update sees this tick's
    // positions — the physics step has already written them by the time this runs.
    nya_system_entity_grid_rebuild();

    nya_entity_foreach (entity) {
        if (!nya_flag_check(entity->state, NYA_ENTITY_STATE_ACTIVE)) continue;

        // Integrate before the callback, so an update that reads position sees where the entity is
        // this tick rather than where it was last one.
        //
        // Skipped entirely for a simulated entity: nya_system_physics2d_update has already written
        // this tick's transform out of the solver, and integrating on top of it would add the
        // velocity a second time — visible as a body that falls at roughly twice gravity and drifts
        // out of its own collision shape.
        if (!nya_flag_check(entity->state, NYA_ENTITY_STATE_STATIC) && !entity->physics2d.attached) {
            entity->position += entity->velocity * delta_time_s;

            // Angular velocity is radians per second about each axis. Small angle enough per tick
            // that composing it as a quaternion and renormalizing is both cheap and stable.
            f32x3 delta   = entity->angular_velocity * delta_time_s;
            f32x3 squared = delta * delta;
            if (squared.x + squared.y + squared.z > 0.0F) {
                NYA_Quaternion spin = nya_quaternion_from_euler(delta.x, delta.y, delta.z);
                entity->rotation    = nya_quaternion_normalize(nya_quaternion_multiply(spin, entity->rotation));
            }
        }

        // After the velocity integration rather than before it, because a move is an absolute
        // statement about where the entity is this tick and integrating on top of one would bend the
        // curve by whatever velocity happened to be left on the entity.
        _nya_entity_target_step(entity, delta_time_s);

        _nya_entity_animation_step(entity, delta_time_s);

        NYA_EntityOnUpdateFn on_update = nya_callback_get(entity->on_update);
        if (on_update != nullptr) on_update(entity, delta_time_s);
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * INTERPOLATED MOTION
 * ─────────────────────────────────────────────────────────
 */

void nya_entity_move_to(NYA_Entity* entity, f32x3 target, f32 duration_s, NYA_EaseType ease) {
    if (entity == nullptr) return;

    if (entity->physics2d.attached && entity->physics2d.type != NYA_PHYSICS_BODY_KINEMATIC) {
        nya_warn(
            "Ignoring nya_entity_move_to on entity '%s': its body is %s, and the solver owns the transform",
            entity->name ? entity->name : "(unnamed)",
            entity->physics2d.type == NYA_PHYSICS_BODY_STATIC ? "static" : "dynamic"
        );
        return;
    }

    // A move of no duration is a teleport rather than a division by zero, so that a caller computing
    // the duration from a distance does not have to special case standing still.
    if (duration_s <= 0.0F) {
        if (entity->physics2d.attached) nya_physics2d_teleport(entity, target.xy, nya_physics2d_rotation(entity));
        else entity->position = target;

        nya_entity_move_stop(entity);
        return;
    }

    entity->target_origin     = entity->position;
    entity->target_position   = target;
    entity->target_duration_s = duration_s;
    entity->target_elapsed_s  = 0.0F;
    entity->target_ease       = ease;
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

    // Duration is the flag, so clearing it is what ends the move. The rest is zeroed anyway to keep a
    // stopped entity from reporting a stale target through nya_entity_move_progress.
    entity->target_duration_s = 0.0F;
    entity->target_elapsed_s  = 0.0F;
}

b8 nya_entity_moving(const NYA_Entity* entity) {
    return entity != nullptr && entity->target_duration_s > 0.0F;
}

f32 nya_entity_move_progress(const NYA_Entity* entity) {
    if (!nya_entity_moving(entity)) return 1.0F;

    return nya_ease(entity->target_ease, entity->target_elapsed_s / entity->target_duration_s);
}

void _nya_entity_target_step(NYA_Entity* entity, f32 delta_time_s) {
    if (entity->target_duration_s <= 0.0F) return;

    entity->target_elapsed_s += delta_time_s;

    // Clamped rather than allowed to overshoot, because several easing curves are only defined as
    // the intended shape on [0, 1] — BACK_OUT past one keeps travelling, and the entity would sail
    // through its target on a long frame.
    b8 arrived = entity->target_elapsed_s >= entity->target_duration_s;
    if (arrived) entity->target_elapsed_s = entity->target_duration_s;

    f32   eased    = nya_ease(entity->target_ease, entity->target_elapsed_s / entity->target_duration_s);
    f32x3 position = entity->target_origin + ((entity->target_position - entity->target_origin) * eased);

    if (entity->physics2d.attached) {
        // A kinematic body is moved by telling the solver how fast it is going, not by writing where
        // it is: that is what makes it sweep through the tick and push whatever is standing on it,
        // which is the entire reason a moving platform is kinematic rather than static.
        nya_physics2d_velocity_set(entity, (position.xy - entity->position.xy) / delta_time_s);
    } else {
        entity->position = position;
    }

    if (arrived) {
        // Cleared before anything else can see this tick, so an on_update that checks
        // nya_entity_moving on the arrival tick is told the move is over rather than that it has one
        // more tick to run.
        nya_entity_move_stop(entity);

        // The solver would otherwise carry the last tick's velocity on into the next one, and a
        // platform that arrives keeps sliding.
        if (entity->physics2d.attached) nya_physics2d_velocity_set(entity, f32x2_zero);
    }
}

void nya_system_entity_render(NYA_Window* window) {
    nya_assert(window != nullptr);

    u32 target_width, target_height;
    nya_render2d_target_size(window, &target_width, &target_height);

    /*
     * All four corners, not two.
     *
     * A camera can be rotated, and under rotation the world points behind two opposite screen corners
     * do not bound the view — the other two stick out. Taking the extent of all four gives a
     * rectangle that contains the visible region whatever the camera is doing.
     *
     * With no camera set nya_render2d_screen_to_world is the identity, so this degenerates to the target
     * in screen pixels, which is exactly right for a layer drawing screen space entities.
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
     * Collected and sorted before anything is drawn.
     *
     * The query answers in grid bucket order, which is neither spatial nor stable across a rebuild —
     * so before this, two sprites could swap depth because one of them moved into a different cell.
     * Sorting is what makes NYA_EntityVisual.z_order mean anything.
     *
     * On the stack, and sized for the whole table, because the query already is: this is one array
     * of sixteen bytes per slot against that one's eight, on a function that runs once per camera.
     */
    static NYA_EntityDrawEntry entries[NYA_ENTITY_MAX];

    u32 entry_count = 0;

    for (u32 i = 0; i < count; i++) {
        NYA_Entity* entity = nya_entity_get(visible[i]);
        if (entity == nullptr) continue;

        // The invisible check. NYA_ENTITY_STATE_VISIBLE is the flag, and clearing it is what
        // "invisible" is — one bit rather than two that can disagree about the same thing.
        if (!nya_flag_check(entity->state, NYA_ENTITY_STATE_VISIBLE)) continue;

        entries[entry_count++] = (NYA_EntityDrawEntry){
            .handle  = entity->handle,
            .z_order = entity->visual.z_order,
            .texture = entity->visual.sprite.texture,
        };
    }

    // Plain qsort rather than nya_array_sort, which is a macro over an NYA_Array — this list is a
    // fixed stack buffer precisely so that drawing allocates nothing.
    qsort(entries, entry_count, sizeof(NYA_EntityDrawEntry), _nya_entity_draw_entry_compare);

    for (u32 i = 0; i < entry_count; i++) {
        NYA_Entity* entity = nya_entity_get(entries[i].handle);

        // Re-resolved rather than kept as a pointer: an on_render is game code and may despawn
        // something, including a later entry in this list.
        if (entity == nullptr) continue;

        _nya_entity_visual_draw(entity, window);

        // After the visual, so a health bar lands on top of the sprite it belongs to rather than
        // under it. An entity with no visual is unaffected, which is every entity that predates one.
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

    // Clearing is the bucket heads only. The chain links are overwritten as entities are inserted,
    // and an entity that is no longer live is simply never linked in.
    for (u32 i = 0; i < NYA_ENTITY_GRID_BUCKETS; i++) grid->buckets[i] = NYA_ENTITY_GRID_EMPTY;

    grid->count = 0;

    for (u32 slot = 0; slot < system->high_water_mark; slot++) {
        if (!system->occupied[slot]) continue;

        const NYA_Entity* entity = &system->entities[slot];

        s32 cell_x, cell_y;
        _nya_entity_grid_cell((f32x2){ entity->position.x, entity->position.y }, &cell_x, &cell_y);

        u32 bucket = _nya_entity_grid_bucket(cell_x, cell_y);

        // Pushed at the head, so insertion is two writes and never walks the chain.
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

    // The bounding square first, then an exact distance test on what it returns. Squared throughout,
    // so the circle costs a multiply rather than a square root per candidate.
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
    // z zero, because the 2D world is the z = 0 plane. See NYA_EntityOnClickFn.
    return _nya_entity_click_deliver(nya_physics2d_entity_at(world_point), (f32x3){ world_point.x, world_point.y, 0.0F }, button);
}

NYA_EntityHandle nya_entity_hover(f32x2 world_point) __attr_overloaded {
    return _nya_entity_hover_move(nya_physics2d_entity_at(world_point));
}

NYA_EntityHandle nya_entity_hover(f32x3 origin, f32x3 direction) __attr_overloaded {
    // The struck point is deliberately dropped: on_hover does not take one. See NYA_EntityOnHoverFn.
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
     * The point on the struck surface, not the ray's origin.
     *
     * That is the whole content of a 3D click: where on the thing it landed decides which face was
     * hit, where to put a decal, and which end of a lever was pulled. Handing the callback the
     * camera position instead would make every click on an object report the same coordinates.
     *
     * Left zeroed when nothing was hit, which _nya_entity_click_deliver never reads: an invalid
     * handle returns before the callback.
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
    };

    system->occupied[slot] = true;
    system->count++;

    _nya_entity_index_add(slot, options.type, options.flags);
    if (slot + 1 > system->high_water_mark) system->high_water_mark = slot + 1;

    // Called with the entity already live, so on_spawn can spawn more, despawn itself, or hand its
    // own handle to something else.
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
     * Both solvers, after on_despawn — which may still want to read a velocity or an overlap off the
     * body — and before the slot is cleared.
     *
     * An entity that leaves the world without this leaves its body behind: still colliding, still
     * costing a step, and no longer reachable to destroy, because the handle that named it has just
     * stopped resolving.
     *
     * Both, not whichever looks attached, because each call is already a no-op for an entity that
     * does not carry that kind of body — and asking "which one is it" here is exactly how the second
     * one gets forgotten. It was: only the 2D detach was here, so every 3D body ever spawned leaked
     * into its world and kept simulating.
     */
    nya_physics2d_body_detach(entity);
    nya_physics3d_body_detach(entity);

    /*
     * The hover is dropped without running on_hover.
     *
     * on_despawn has already run and the entity is on its way out, so telling it the cursor left is
     * both useless and a callback into something half torn down. The generation bump below would make
     * the stale handle stop resolving anyway; clearing it is what keeps nya_entity_hovered honest in
     * the same frame rather than until the cursor next moves.
     */
    if (system->hovered.index == entity.index && system->hovered.generation == entity.generation) {
        system->hovered = NYA_ENTITY_HANDLE_NONE;
    }

    // Before the slot is cleared, because removing needs the kind and flags it is being removed for.
    _nya_entity_index_remove(entity.index, target->type, target->flags);

    // Bumping the generation is what makes every outstanding handle to this entity stop resolving.
    system->generations[entity.index]++;
    system->occupied[entity.index] = false;
    system->count--;

    system->free_slots[system->free_count++] = entity.index;

    *target = (NYA_Entity){ 0 };
}

void nya_entity_despawn_deferred(NYA_EntityHandle entity) {
    NYA_Entity* target = nya_entity_get(entity);
    if (target == nullptr) return;

    // Idempotent: a projectile hitting two things in one tick should not queue two despawns and
    // have the second one apply to whatever reused the slot.
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

    // By slot rather than through nya_entity_foreach, because despawning is exactly the thing that
    // macro is not safe under.
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

    // floorf, not a cast. A cast truncates toward zero, which puts everything between -1 and 1 in
    // the same cell and mirrors the grid about the origin.
    *out_x = (s32)floorf(position.x / cell_size);
    *out_y = (s32)floorf(position.y / cell_size);
}

u32 _nya_entity_index_word_count(void) {
    // Rounded up, so the word holding the highest slot is included. The high water mark only grows,
    // which is what makes it safe to stop here: a slot past it has never been occupied, so its bit
    // has never been set in any bitset.
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

    // `live` is what every query is masked by, so clearing it alone would already make the slot
    // unreachable. The rest are cleared anyway: a stale bit would be set again on the next spawn into
    // this slot and the two would then disagree about a kind the new entity is not.
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
     * The usual pair of large primes, multiplied in u64 and truncated on purpose.
     *
     * Done in 64 bits because the 32 bit form wraps, and the sanitized build treats unsigned
     * wraparound as the accident it usually is — the same trap the mote hash fell into. Casting the
     * signed cell coordinate through u32 first is what makes negative coordinates hash sensibly
     * rather than sign extending into the high half.
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
                 * Two filters, and both are load bearing.
                 *
                 * The cell check is what makes a *hash* grid correct: distant cells share buckets, so
                 * a bucket holds entities this query never asked about — and because each entity's
                 * own cell is visited exactly once, it is also what stops one being emitted twice.
                 */
                s32 entity_cell_x, entity_cell_y;
                _nya_entity_grid_cell((f32x2){ entity->position.x, entity->position.y }, &entity_cell_x, &entity_cell_y);

                if (entity_cell_x != cell_x || entity_cell_y != cell_y) continue;

                // And the exact test, because a cell is coarser than the rectangle that asked.
                if (entity->position.x < min.x || entity->position.x > max.x) continue;
                if (entity->position.y < min.y || entity->position.y > max.y) continue;

                if (filter_by_type && entity->type != type_filter) continue;

                // Every bit, not any bit — the same rule the game side uses, so "flammable and wet"
                // means both rather than either.
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

    /*
     * Only the bits that actually changed are touched.
     *
     * The obvious version clears every old bit and sets every new one, which for the common case of
     * flipping one flag out of a word that has three set is six writes instead of one — and every
     * one of them is a read-modify-write of a shared cache line.
     */
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
        // checks `type` per entity — slower, correct, and something no game reaches by accident.
        .bits       = bits != nullptr ? bits : index->live,
        .check_type = bits == nullptr,
        .require_type = type,

        // Positioned before the first word, so the first advance lands on the first match. That is
        // what lets the macro test `entity` immediately without a separate priming call.
        .word       = 0,
        .word_count = _nya_entity_index_word_count(),
        .remaining  = 0,
    };

    // Prime it. `word` is zero and `remaining` empty, so this loads word zero and finds its first
    // set bit — or walks to the end and reports nothing, which is an empty loop.
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
     * Walk the first requested bit's set, and check the rest per entity.
     *
     * Intersecting every requested bitset would be strictly fewer candidates, and would need a
     * scratch buffer to hold the intersection — for a query that is nearly always one flag, and where
     * the extra check is one AND against a word already in a register.
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
        // A zero word skips sixty four slots at once, which is what makes a rare kind nearly free to
        // look for in a world full of something else.
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
     * Ties broken by texture, and that is the whole reason this is not a one-line comparator.
     *
     * A draw call holds one texture, so sorting purely by depth takes a hundred sprites that shared
     * a sheet and interleaves them with sprites from another — turning one draw call into a hundred.
     * Grouping equal-depth entities by texture costs nothing and puts the batching back.
     *
     * By address, because asset handles are interned literals. Two frames of one atlas are the
     * identical pointer; comparing the strings would be strcmp inside a sort.
     */
    if ((uintptr_t)a->texture < (uintptr_t)b->texture) return -1;
    if ((uintptr_t)a->texture > (uintptr_t)b->texture) return 1;

    // Finally by slot, so the order is total. Without this a sort is free to permute equal elements
    // and two sprites at the same depth on the same sheet would flicker past each other.
    if (a->handle.index < b->handle.index) return -1;
    if (a->handle.index > b->handle.index) return 1;

    return 0;
}

void _nya_entity_animation_step(NYA_Entity* entity, f32 delta_time_s) {
    if (entity->visual.kind != NYA_ENTITY_VISUAL_ANIMATION) return;

    NYA_SpriteAnimationSignal signals[NYA_SPRITE_ANIMATION_MAX_SIGNALS];

    u32 count = nya_sprite_animator_advance(&entity->visual.animator, delta_time_s, signals, nya_carray_length(signals));

    // The frame is applied whether or not anything was signalled: a paused animator still has to
    // draw the frame it is paused on, and an animator that crossed no frame boundary this tick still
    // has to draw the one it is in.
    nya_sprite_animator_apply(&entity->visual.animator, &entity->visual.atlas, &entity->visual.sprite);

    NYA_EntityOnAnimationFn on_animation = nya_callback_get(entity->on_animation);
    if (on_animation == nullptr) return;

    for (u32 i = 0; i < count; i++) {
        // Re-checked every iteration: a callback handling FINISHED is entitled to despawn the entity,
        // and the remaining signals would then be delivered to a freed slot.
        if (!nya_entity_is_valid(entity->handle)) return;

        on_animation(entity, signals[i]);
    }
}

void _nya_entity_visual_draw(NYA_Entity* entity, NYA_Window* window) {
    switch (entity->visual.kind) {
        case NYA_ENTITY_VISUAL_SPRITE:
        case NYA_ENTITY_VISUAL_ANIMATION: {
            // Null until the texture asset resolves, which is normal for the first frames of a run.
            if (entity->visual.sprite.texture == nullptr) return;

            nya_render2d_sprite(window, &entity->visual.sprite, entity->position.xy);
        } break;

        case NYA_ENTITY_VISUAL_CUBE: {
            // Only while a 3D camera is set. There is no projection otherwise, and drawing it
            // through the 2D one would put a cube somewhere arbitrary rather than nowhere.
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
    // interchangeable by definition, and forcing an order on them would buy nothing.
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
     * A linear walk of the whole table rather than a spatial query.
     *
     * The grid indexes an entity's *position*, and a light's reach is its radius — so a query would
     * have to be widened by the largest radius in the world, which nothing tracks, or by a guess.
     * Lights are rare enough that walking is honest and cheap: this is one comparison per entity,
     * against a query that would have to be widened until it returned most of them anyway.
     */
    static NYA_EntityLightEntry candidates[NYA_ENTITY_MAX];

    u32 found = 0;

    nya_entity_foreach (entity) {
        if (entity->light.radius <= 0.0F) continue;
        if (entity->light.intensity <= 0.0F) continue;

        f32x2 position = entity->position.xy + entity->light.offset;

        // Widened by the light's own radius, because a light whose origin is off screen still spills
        // onto it. Its radius is exactly the right margin, unlike a sprite's, where the renderer has
        // to guess with NYA_ENTITY_RENDER_CULL_MARGIN.
        f32 reach = entity->light.radius;

        if (position.x + reach < min.x || position.x - reach > max.x) continue;
        if (position.y + reach < min.y || position.y - reach > max.y) continue;

        candidates[found++] = (NYA_EntityLightEntry){
            .light    = entity->light,
            .position = position,
            .weight   = entity->light.radius * entity->light.intensity,
        };
    }

    // Sorted before the cap, so what gets dropped when a scene has more lights than the pass can
    // carry is the ones nobody would have noticed — not whichever happened to be spawned last.
    if (found > capacity) qsort(candidates, found, sizeof(NYA_EntityLightEntry), _nya_entity_light_compare);

    u32 kept = nya_min(found, capacity);

    for (u32 i = 0; i < kept; i++) {
        out[i]           = candidates[i].light;
        out_positions[i] = candidates[i].position;

        // Zeroed is read as white here rather than at every consumer, so a light that only says how
        // big it is still works.
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

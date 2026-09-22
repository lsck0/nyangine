#include "nyangine/nyangine.h"

#ifdef NYA_TESTING

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct _NYA_SimulationActions _NYA_SimulationActions;

/**
 * Everything the action set owns. One instance, because the engine has one current world; see the
 * header for why this is not a context parameter.
 * */
struct _NYA_SimulationActions {
    b8 initialized;

    NYA_World* world;

    /**
     * The described types the reflection action round trips, handed in by the caller. The engine
     * cannot name src/genyarated/reflection.h: that file is generated from a scan of the whole tree and
     * declares the game's types alongside the engine's.
     * */
    const NYA_TypeReflection* const* types;
    u32                              type_count;

    /** Scratch for the save round trip, reset whenever it is used so a long run does not grow. */
    NYA_Arena* scratch;

    /** Registered once and reused, so a body attach does not scan the name table every time. */
    NYA_PhysicsLayerMask layers[NYA_SIMULATION_KIND_COUNT];

    /** Whether the save root resolved. Without one the storage actions and faults do nothing. */
    b8 storage_available;

    /** The clock the last invariant sweep saw, so a clock going backwards is caught where it happens. */
    u64 clock_seen_ns;

    /** Worlds torn down and rebuilt by the restart fault. Reported at the end of a run. */
    u64 restarts;
};

NYA_INTERNAL _NYA_SimulationActions _NYA_SIMULATION_ACTIONS = { 0 };

/** A live entity picked from the table, or null when the world is empty. */
NYA_INTERNAL NYA_Entity* _nya_simulation_entity_pick(NYA_SimulationRun* run);

/** The save file's absolute path, or null when there is no save root. */
NYA_INTERNAL NYA_CString _nya_simulation_save_path(void);

/** Writes an object worth reading back: a filled, reflected engine config. */
NYA_INTERNAL void _nya_simulation_save_write(NYA_SimulationRun* run);

/* ── actions ── */

NYA_INTERNAL void _nya_simulation_do_entity_spawn(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_entity_despawn(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_entity_despawn_deferred(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_entity_flags(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_entity_parent(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_entity_unparent(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_entity_move(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_body2d_attach(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_body2d_detach(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_body2d_impulse(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_body2d_teleport(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_body_layers(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_body3d_attach(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_body3d_detach(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_gravity(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_freeze(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_tick(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_query(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_raycast(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_save(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_load(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_do_reflect_round_trip(NYA_SimulationRun* run);

/* ── faults ── */

NYA_INTERNAL void _nya_simulation_fault_save_corrupt(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_fault_save_torn(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_fault_save_vanish(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_fault_clock_jump(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_fault_world_restart(NYA_SimulationRun* run);

/* ── invariants ── */

NYA_INTERNAL void _nya_simulation_check_entity_table(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_check_body_counts(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_check_hierarchy(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_check_query_agrees(NYA_SimulationRun* run);
NYA_INTERNAL void _nya_simulation_check_clock(NYA_SimulationRun* run);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_simulation_actions_add(NYA_SimulationRun* run) {
    nya_assert(run != nullptr);
    nya_assert(!_NYA_SIMULATION_ACTIONS.initialized, "the engine action set is already registered; see the header");

    _NYA_SIMULATION_ACTIONS = (_NYA_SimulationActions){
        .initialized = true,
        .scratch     = nya_arena_create(.name = "simulation_actions"),
    };

    _NYA_SIMULATION_ACTIONS.world = nya_world_create();
    (void)nya_world_set(_NYA_SIMULATION_ACTIONS.world);

    NYA_ConstCString names[NYA_SIMULATION_KIND_COUNT] = {
        NYA_SIMULATION_LAYER_GROUND,
        NYA_SIMULATION_LAYER_ACTOR,
        NYA_SIMULATION_LAYER_DEBRIS,
        NYA_SIMULATION_LAYER_TRIGGER,
    };

    for (u32 i = 0; i < NYA_SIMULATION_KIND_COUNT; i++) _NYA_SIMULATION_ACTIONS.layers[i] = nya_physics_layer(names[i]);

    // an unwritable save root is an operating error, not a reason to stop: the storage actions report
    // themselves as never taken and the rest of the run still happens.
    NYA_Error storage                          = nya_system_save_init();
    _NYA_SIMULATION_ACTIONS.storage_available = storage.ok && nya_save_root() != nullptr;

    if (!_NYA_SIMULATION_ACTIONS.storage_available) {
        nya_log_warn("The simulation has no save root; its storage actions and disk faults are off.");
    }

    /*
     * The mix. Ticking is the most common thing a game does, so it is the most common thing here;
     * spawning outnumbers despawning so a world grows before it is torn at; faults are rare enough
     * that the run spends its time in the code being tested rather than in recovery.
     */
    nya_simulation_action_add(run, "entity_spawn", 90, _nya_simulation_do_entity_spawn);
    nya_simulation_action_add(run, "entity_despawn", 35, _nya_simulation_do_entity_despawn);
    nya_simulation_action_add(run, "entity_despawn_deferred", 35, _nya_simulation_do_entity_despawn_deferred);
    nya_simulation_action_add(run, "entity_flags", 30, _nya_simulation_do_entity_flags);
    nya_simulation_action_add(run, "entity_parent", 25, _nya_simulation_do_entity_parent);
    nya_simulation_action_add(run, "entity_unparent", 15, _nya_simulation_do_entity_unparent);
    nya_simulation_action_add(run, "entity_move", 25, _nya_simulation_do_entity_move);

    nya_simulation_action_add(run, "body2d_attach", 60, _nya_simulation_do_body2d_attach);
    nya_simulation_action_add(run, "body2d_detach", 25, _nya_simulation_do_body2d_detach);
    nya_simulation_action_add(run, "body2d_impulse", 40, _nya_simulation_do_body2d_impulse);
    nya_simulation_action_add(run, "body2d_teleport", 20, _nya_simulation_do_body2d_teleport);
    nya_simulation_action_add(run, "body_layers", 25, _nya_simulation_do_body_layers);
    nya_simulation_action_add(run, "body3d_attach", 40, _nya_simulation_do_body3d_attach);
    nya_simulation_action_add(run, "body3d_detach", 20, _nya_simulation_do_body3d_detach);
    nya_simulation_action_add(run, "gravity", 8, _nya_simulation_do_gravity);
    nya_simulation_action_add(run, "freeze", 8, _nya_simulation_do_freeze);

    nya_simulation_action_add(run, "tick", 200, _nya_simulation_do_tick);
    nya_simulation_action_add(run, "query", 40, _nya_simulation_do_query);
    nya_simulation_action_add(run, "raycast", 30, _nya_simulation_do_raycast);

    nya_simulation_action_add(run, "save", 20, _nya_simulation_do_save);
    nya_simulation_action_add(run, "load", 20, _nya_simulation_do_load);
    nya_simulation_fault_add(run, "fault_save_corrupt", 6, _nya_simulation_fault_save_corrupt);
    nya_simulation_fault_add(run, "fault_save_torn", 6, _nya_simulation_fault_save_torn);
    nya_simulation_fault_add(run, "fault_save_vanish", 4, _nya_simulation_fault_save_vanish);
    nya_simulation_fault_add(run, "fault_clock_jump", 4, _nya_simulation_fault_clock_jump);
    nya_simulation_fault_add(run, "fault_world_restart", 3, _nya_simulation_fault_world_restart);

    nya_simulation_check_add(run, "entity_table", _nya_simulation_check_entity_table);
    nya_simulation_check_add(run, "body_counts", _nya_simulation_check_body_counts);
    nya_simulation_check_add(run, "hierarchy", _nya_simulation_check_hierarchy);
    nya_simulation_check_add(run, "query_agrees", _nya_simulation_check_query_agrees);
    nya_simulation_check_add(run, "clock", _nya_simulation_check_clock);
}

void nya_simulation_actions_reflect_add(NYA_SimulationRun* run, const NYA_TypeReflection* const* types, u32 count) {
    nya_assert(run != nullptr);
    nya_assert(types != nullptr);
    nya_assert(_NYA_SIMULATION_ACTIONS.initialized, "nya_simulation_actions_add comes first; the action set owns the arena this uses");

    if (count == 0) return;

    _NYA_SIMULATION_ACTIONS.types      = types;
    _NYA_SIMULATION_ACTIONS.type_count = count;

    nya_simulation_action_add(run, "reflect_round_trip", 25, _nya_simulation_do_reflect_round_trip);
}

void nya_simulation_actions_remove(void) {
    if (!_NYA_SIMULATION_ACTIONS.initialized) return;

    if (_NYA_SIMULATION_ACTIONS.storage_available) (void)nya_save_delete(NYA_SIMULATION_SAVE_FILE);

    if (_NYA_SIMULATION_ACTIONS.world != nullptr) nya_world_destroy(_NYA_SIMULATION_ACTIONS.world);

    nya_arena_destroy(_NYA_SIMULATION_ACTIONS.scratch);
    nya_system_save_deinit();

    _NYA_SIMULATION_ACTIONS = (_NYA_SimulationActions){ 0 };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * HELPERS
 * ─────────────────────────────────────────────────────────
 */

NYA_Entity* _nya_simulation_entity_pick(NYA_SimulationRun* run) {
    u32 slots = nya_entity_slot_count();
    if (slots == 0) return nullptr;

    u32 start = (u32)nya_simulation_below(run, slots);

    // from a random slot forward, so the pick is uniform over slots rather than always finding the
    // first live one, which would make every action operate on the same entity.
    for (u32 offset = 0; offset < slots; offset++) {
        NYA_Entity* entity = nya_entity_at_slot((start + offset) % slots);
        if (entity != nullptr) return entity;
    }

    return nullptr;
}

NYA_CString _nya_simulation_save_path(void) {
    if (!_NYA_SIMULATION_ACTIONS.storage_available) return nullptr;

    NYA_String* path = nya_save_path(_NYA_SIMULATION_ACTIONS.scratch, NYA_SIMULATION_SAVE_FILE);
    if (path == nullptr) return nullptr;

    return nya_string_to_cstring(_NYA_SIMULATION_ACTIONS.scratch, path);
}

void _nya_simulation_save_write(NYA_SimulationRun* run) {
    /*
     * Built by hand rather than reflected: the engine cannot name a generated reflection table, and a
     * save file only has to be something with a shape for the reader to disagree with.
     */
    NYA_Object* object = nya_object_create(_NYA_SIMULATION_ACTIONS.scratch);

    nya_object_set(object, "step", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = run->step });
    nya_object_set(object, "entities", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = nya_entity_count() });
    nya_object_set(object, "clock_ns", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = nya_simulation_now_ns(run) });
    nya_object_set(object, "gravity_y", (NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = nya_physics2d_gravity().y });
    nya_object_set(object, "frozen", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = !nya_physics2d_enabled() });
    nya_object_set(object, "label", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"simulation" });

    (void)nya_save_write(NYA_SIMULATION_SAVE_FILE, object, nya_simulation_chance(run, 50) ? NYA_SERDE_PRETTY : NYA_SERDE_NONE);
}

/*
 * ─────────────────────────────────────────────────────────
 * ENTITY ACTIONS
 * ─────────────────────────────────────────────────────────
 */

void _nya_simulation_do_entity_spawn(NYA_SimulationRun* run) {
    // bounded well below NYA_ENTITY_MAX; see NYA_SIMULATION_ENTITY_MAX.
    if (nya_entity_count() >= NYA_SIMULATION_ENTITY_MAX) return;

    f32x3 position = {
        nya_simulation_shaped_f32(run, -512.0F, 512.0F),
        nya_simulation_shaped_f32(run, -512.0F, 512.0F),
        nya_simulation_shaped_f32(run, -512.0F, 512.0F),
    };

    f32x3 velocity = {
        nya_simulation_shaped_f32(run, -64.0F, 64.0F),
        nya_simulation_shaped_f32(run, -64.0F, 64.0F),
        nya_simulation_shaped_f32(run, -64.0F, 64.0F),
    };

    NYA_EntityHandle spawned = nya_entity_spawn(
        .name     = "simulated",
        .type     = (u32)nya_simulation_below(run, NYA_SIMULATION_KIND_COUNT),
        .flags    = nya_simulation_roll(run) & 0xFFULL,
        .position = position,
        .velocity = velocity,
        .state    = NYA_ENTITY_STATE_ACTIVE | (nya_simulation_chance(run, 80) ? NYA_ENTITY_STATE_VISIBLE : 0)
    );

    // a refusal is a legal answer, so this is not asserted. What is asserted is that a handle the
    // spawn called valid resolves.
    if (nya_entity_is_valid(spawned)) nya_assert(nya_entity_get(spawned) != nullptr, "a valid handle must resolve");
}

void _nya_simulation_do_entity_despawn(NYA_SimulationRun* run) {
    NYA_Entity* entity = _nya_simulation_entity_pick(run);
    if (entity == nullptr) return;

    NYA_EntityHandle handle = entity->handle;
    nya_entity_despawn(handle);

    // negative space: the slot must be gone right away, and the stale handle must be refused rather
    // than resolving to whatever takes the slot next.
    nya_assert(nya_entity_get(handle) == nullptr, "a despawned handle still resolves");
    nya_assert(!nya_entity_is_valid(handle), "a despawned handle still reports valid");
}

void _nya_simulation_do_entity_despawn_deferred(NYA_SimulationRun* run) {
    NYA_Entity* entity = _nya_simulation_entity_pick(run);
    if (entity == nullptr) return;

    NYA_EntityHandle handle = entity->handle;
    nya_entity_despawn_deferred(handle);

    // still alive until the barrier: that is the whole difference between the two calls.
    nya_assert(nya_entity_get(handle) != nullptr, "a deferred despawn took effect immediately");
}

void _nya_simulation_do_entity_flags(NYA_SimulationRun* run) {
    NYA_Entity* entity = _nya_simulation_entity_pick(run);
    if (entity == nullptr) return;

    u64 flags = nya_simulation_roll(run) & 0xFFFFULL;

    switch (nya_simulation_below(run, 3)) {
        case 0:  nya_entity_flag_enable(entity, flags); break;
        case 1:  nya_entity_flag_disable(entity, flags); break;
        default: nya_entity_flags_set(entity, flags); break;
    }
}

void _nya_simulation_do_entity_parent(NYA_SimulationRun* run) {
    NYA_Entity* child  = _nya_simulation_entity_pick(run);
    NYA_Entity* parent = _nya_simulation_entity_pick(run);

    if (child == nullptr || parent == nullptr) return;

    // deliberately unfiltered: parenting something to itself, or to its own descendant, is exactly the
    // cycle the engine has to refuse, and the hierarchy invariant is what proves it did.
    (void)nya_entity_parent_set(child->handle, parent->handle);
}

void _nya_simulation_do_entity_unparent(NYA_SimulationRun* run) {
    NYA_Entity* child = _nya_simulation_entity_pick(run);
    if (child == nullptr) return;

    nya_entity_parent_clear(child->handle);

    nya_assert(!nya_entity_is_valid(nya_entity_parent(child)), "an unparented entity still has a parent");
}

void _nya_simulation_do_entity_move(NYA_SimulationRun* run) {
    NYA_Entity* entity = _nya_simulation_entity_pick(run);
    if (entity == nullptr) return;

    f32x3 target = {
        nya_simulation_shaped_f32(run, -512.0F, 512.0F),
        nya_simulation_shaped_f32(run, -512.0F, 512.0F),
        nya_simulation_shaped_f32(run, -512.0F, 512.0F),
    };

    if (nya_simulation_chance(run, 20)) {
        nya_entity_move_stop(entity);
        return;
    }

    // a zero and a negative duration are both drawn on purpose: a tween that finishes before it starts
    // is the kind of input a designer produces by typing a minus sign.
    nya_entity_move_to(entity, target, nya_simulation_shaped_f32(run, 0.0F, 2.0F), NYA_EASE_LINEAR);
}

/*
 * ─────────────────────────────────────────────────────────
 * PHYSICS ACTIONS
 * ─────────────────────────────────────────────────────────
 */

void _nya_simulation_do_body2d_attach(NYA_SimulationRun* run) {
    NYA_Entity* entity = _nya_simulation_entity_pick(run);
    if (entity == nullptr) return;

    NYA_PhysicsLayerMask layers = _NYA_SIMULATION_ACTIONS.layers[nya_simulation_below(run, NYA_SIMULATION_KIND_COUNT)];

    (void)nya_physics2d_body_attach(
        entity->handle,
        .type          = (NYA_PhysicsBodyType)nya_simulation_below(run, 3),
        .shape         = (NYA_Physics2DShape)nya_simulation_below(run, NYA_PHYSICS2D_SHAPE_COUNT),
        .size          = { nya_simulation_shaped_f32(run, 0.0F, 64.0F), nya_simulation_shaped_f32(run, 0.0F, 64.0F) },
        .radius        = nya_simulation_shaped_f32(run, 0.0F, 32.0F),
        .length        = nya_simulation_shaped_f32(run, 0.0F, 32.0F),
        .is_sensor     = nya_simulation_chance(run, 20),
        .is_bullet     = nya_simulation_chance(run, 10),
        .one_way       = (NYA_Physics2DOneWay)nya_simulation_below(run, NYA_PHYSICS2D_ONE_WAY_COUNT),
        .layers        = layers,
        .collides_with = nya_simulation_roll(run) | NYA_PHYSICS_LAYER_DEFAULT
    );
}

void _nya_simulation_do_body2d_detach(NYA_SimulationRun* run) {
    NYA_Entity* entity = _nya_simulation_entity_pick(run);
    if (entity == nullptr) return;

    nya_physics2d_body_detach(entity->handle);

    nya_assert(!nya_physics2d_body_attached(entity), "a detached body still reports attached");
}

void _nya_simulation_do_body2d_impulse(NYA_SimulationRun* run) {
    NYA_Entity* entity = _nya_simulation_entity_pick(run);
    if (entity == nullptr || !nya_physics2d_body_attached(entity)) return;

    f32x2 impulse = { nya_simulation_shaped_f32(run, -128.0F, 128.0F), nya_simulation_shaped_f32(run, -128.0F, 128.0F) };

    if (nya_simulation_chance(run, 50)) {
        nya_physics2d_apply_impulse(entity, impulse);
    } else {
        nya_physics2d_apply_force(entity, impulse);
    }
}

void _nya_simulation_do_body2d_teleport(NYA_SimulationRun* run) {
    NYA_Entity* entity = _nya_simulation_entity_pick(run);
    if (entity == nullptr || !nya_physics2d_body_attached(entity)) return;

    f32x2 to = { nya_simulation_shaped_f32(run, -512.0F, 512.0F), nya_simulation_shaped_f32(run, -512.0F, 512.0F) };

    nya_physics2d_teleport(entity, to, nya_simulation_range_f32(run, -6.3F, 6.3F));
}

void _nya_simulation_do_body_layers(NYA_SimulationRun* run) {
    NYA_Entity* entity = _nya_simulation_entity_pick(run);
    if (entity == nullptr) return;

    NYA_PhysicsLayerMask layers        = _NYA_SIMULATION_ACTIONS.layers[nya_simulation_below(run, NYA_SIMULATION_KIND_COUNT)];
    NYA_PhysicsLayerMask collides_with = nya_simulation_roll(run);

    if (nya_physics2d_body_attached(entity)) {
        nya_physics2d_layers_set(entity, layers, collides_with);

        nya_assert(nya_physics2d_layers(entity) == layers, "the 2D body did not keep the layers it was given");
        nya_assert(nya_physics2d_collides_with(entity) == collides_with, "the 2D body did not keep the mask it was given");
    }

    if (nya_physics3d_body_attached(entity)) {
        nya_physics3d_layers_set(entity, layers, collides_with);

        nya_assert(nya_physics3d_layers(entity) == layers, "the 3D body did not keep the layers it was given");
        nya_assert(nya_physics3d_collides_with(entity) == collides_with, "the 3D body did not keep the mask it was given");
    }
}

void _nya_simulation_do_body3d_attach(NYA_SimulationRun* run) {
    NYA_Entity* entity = _nya_simulation_entity_pick(run);
    if (entity == nullptr) return;

    NYA_PhysicsLayerMask layers = _NYA_SIMULATION_ACTIONS.layers[nya_simulation_below(run, NYA_SIMULATION_KIND_COUNT)];

    // BOX, SPHERE and CAPSULE only: a MESH and a HEIGHTFIELD need arrays a caller owns, and handing
    // them shaped noise would test Box3D's allocator rather than the engine.
    (void)nya_physics3d_body_attach(
        entity->handle,
        .type          = (NYA_PhysicsBodyType)nya_simulation_below(run, 3),
        .shape         = (NYA_Physics3DShape)nya_simulation_below(run, 3),
        .size          = { nya_simulation_shaped_f32(run, 0.0F, 8.0F), nya_simulation_shaped_f32(run, 0.0F, 8.0F),
                           nya_simulation_shaped_f32(run, 0.0F, 8.0F) },
        .radius        = nya_simulation_shaped_f32(run, 0.0F, 4.0F),
        .length        = nya_simulation_shaped_f32(run, 0.0F, 4.0F),
        .is_sensor     = nya_simulation_chance(run, 20),
        .layers        = layers,
        .collides_with = nya_simulation_roll(run) | NYA_PHYSICS_LAYER_DEFAULT
    );
}

void _nya_simulation_do_body3d_detach(NYA_SimulationRun* run) {
    NYA_Entity* entity = _nya_simulation_entity_pick(run);
    if (entity == nullptr) return;

    nya_physics3d_body_detach(entity->handle);

    nya_assert(!nya_physics3d_body_attached(entity), "a detached 3D body still reports attached");
}

void _nya_simulation_do_gravity(NYA_SimulationRun* run) {
    f32x2 gravity2d = { nya_simulation_shaped_f32(run, -400.0F, 400.0F), nya_simulation_shaped_f32(run, -400.0F, 400.0F) };

    nya_physics2d_gravity_set(gravity2d);
    nya_physics3d_gravity_set((f32x3){ 0.0F, nya_simulation_shaped_f32(run, -20.0F, 20.0F), 0.0F });
}

void _nya_simulation_do_freeze(NYA_SimulationRun* run) {
    b8 enabled = nya_simulation_chance(run, 50);

    nya_physics2d_enabled_set(enabled);
    nya_physics3d_enabled_set(enabled);

    nya_assert(nya_physics2d_enabled() == enabled, "the 2D world did not take the enabled flag");
    nya_assert(nya_physics3d_enabled() == enabled, "the 3D world did not take the enabled flag");
}

/*
 * ─────────────────────────────────────────────────────────
 * FRAME ACTIONS
 * ─────────────────────────────────────────────────────────
 */

void _nya_simulation_do_tick(NYA_SimulationRun* run) {
    // the simulated clock, and the only place it moves during a normal step. Nothing sleeps, and
    // nothing reads a real clock to decide what to do.
    nya_simulation_advance(run, run->time_step_ns);

    f32 delta_time_s = nya_simulation_delta_s(run);

    // the real update order, so the simulation exercises the sequence a frame actually runs in rather
    // than one invented here.
    nya_system_entity_update(delta_time_s);
    nya_system_physics2d_update(delta_time_s);
    nya_system_physics3d_update(delta_time_s);
    nya_system_entity_transforms_update();
    nya_system_sim_apply_commands();
    nya_system_sim_end_frame();
}

void _nya_simulation_do_query(NYA_SimulationRun* run) {
    nya_system_entity_grid_rebuild();

    NYA_EntityHandle found[NYA_SIMULATION_ENTITY_MAX];

    f32x2 center = { nya_simulation_shaped_f32(run, -512.0F, 512.0F), nya_simulation_shaped_f32(run, -512.0F, 512.0F) };
    f32   radius = nya_simulation_shaped_f32(run, 0.0F, 256.0F);

    u32 count = nya_entity_query_radius(center, radius, found, nya_carray_length(found));
    nya_assert(count <= nya_carray_length(found), "a query wrote past the buffer it was given");

    f32x2 min = { center.x - radius, center.y - radius };
    f32x2 max = { center.x + radius, center.y + radius };

    count = nya_entity_query_rect(min, max, found, nya_carray_length(found));
    nya_assert(count <= nya_carray_length(found), "a rect query wrote past the buffer it was given");

    count = nya_entity_query_kind(min, max, (u32)nya_simulation_below(run, NYA_SIMULATION_KIND_COUNT), found, nya_carray_length(found));
    nya_assert(count <= nya_carray_length(found), "a kind query wrote past the buffer it was given");
}

void _nya_simulation_do_raycast(NYA_SimulationRun* run) {
    f32x2 origin    = { nya_simulation_shaped_f32(run, -512.0F, 512.0F), nya_simulation_shaped_f32(run, -512.0F, 512.0F) };
    f32x2 direction = { nya_simulation_shaped_f32(run, -512.0F, 512.0F), nya_simulation_shaped_f32(run, -512.0F, 512.0F) };

    NYA_PhysicsLayerMask layers = nya_simulation_chance(run, 50)
                                      ? NYA_PHYSICS_LAYER_ALL
                                      : _NYA_SIMULATION_ACTIONS.layers[nya_simulation_below(run, NYA_SIMULATION_KIND_COUNT)];

    f32x2 point  = { 0 };
    f32x2 normal = { 0 };

    NYA_EntityHandle hit = nya_physics2d_raycast(origin, direction, layers, &point, &normal);

    // the filter is the contract: a ray that named one layer must never come back holding a body that
    // is not in it.
    if (nya_entity_is_valid(hit)) {
        NYA_Entity* entity = nya_entity_get(hit);
        nya_assert(entity != nullptr, "a raycast returned a handle that does not resolve");
        nya_assert((nya_physics2d_layers(entity) & layers) != 0, "a raycast returned a body outside the layers it asked for");
    }

    (void)nya_physics2d_entity_at(origin, layers);
}

/*
 * ─────────────────────────────────────────────────────────
 * STORAGE ACTIONS
 * ─────────────────────────────────────────────────────────
 */

void _nya_simulation_do_save(NYA_SimulationRun* run) {
    if (!_NYA_SIMULATION_ACTIONS.storage_available) return;

    nya_arena_free_all(_NYA_SIMULATION_ACTIONS.scratch);
    _nya_simulation_save_write(run);
}

void _nya_simulation_do_load(NYA_SimulationRun* run) {
    if (!_NYA_SIMULATION_ACTIONS.storage_available) return;

    nya_arena_free_all(_NYA_SIMULATION_ACTIONS.scratch);

    NYA_Object* object = nullptr;
    NYA_Error   read   = nya_save_read(_NYA_SIMULATION_ACTIONS.scratch, NYA_SIMULATION_SAVE_FILE, NYA_SERDE_NONE, &object);

    /*
     * A failed read is the expected answer after a fault, so it is not a failure here. What must hold
     * is that success and a value agree: a reader that returns ok without an object is the bug this
     * looks for.
     */
    if (read.ok) {
        nya_assert(object != nullptr, "a save read reported success with nothing read");

        // every field is read back the way game code reads one, so a value of the wrong type is met by
        // the same code path a player's edited save file would meet.
        NYA_Value* step = nya_object_get(object, "step");
        if (step != nullptr && step->type == NYA_TYPE_U64) nya_assert(step->as_u64 <= run->step_count, "a save holds a step past the run");
    }
}

void _nya_simulation_do_reflect_round_trip(NYA_SimulationRun* run) {
    nya_arena_free_all(_NYA_SIMULATION_ACTIONS.scratch);

    nya_assert(_NYA_SIMULATION_ACTIONS.type_count > 0, "the reflection action ran without a type table");

    const NYA_TypeReflection* type = _NYA_SIMULATION_ACTIONS.types[nya_simulation_below(run, _NYA_SIMULATION_ACTIONS.type_count)];
    nya_assert(type != nullptr, "the reflection table has a hole in it");

    // struct only: an enum or a primitive at the top level has no instance of its own to round trip.
    if (type->kind != NYA_REFLECT_STRUCT || type->size == 0) return;

    u8* instance = nya_arena_alloc(_NYA_SIMULATION_ACTIONS.scratch, type->size);
    nya_memset(instance, 0, type->size);

    nya_simulation_fill(run, type, instance);

    NYA_Object* object = nya_reflect_to_object(_NYA_SIMULATION_ACTIONS.scratch, type, instance);
    if (object == nullptr) return;

    u8* restored = nya_arena_alloc(_NYA_SIMULATION_ACTIONS.scratch, type->size);
    nya_memset(restored, 0, type->size);

    (void)nya_reflect_from_object(type, restored, object);
}

/*
 * ─────────────────────────────────────────────────────────
 * FAULTS
 * ─────────────────────────────────────────────────────────
 */

void _nya_simulation_fault_save_corrupt(NYA_SimulationRun* run) {
    if (!_NYA_SIMULATION_ACTIONS.storage_available) return;

    nya_arena_free_all(_NYA_SIMULATION_ACTIONS.scratch);

    NYA_CString path = _nya_simulation_save_path();
    if (path == nullptr) return;

    NYA_String* content = nya_string_create(_NYA_SIMULATION_ACTIONS.scratch);
    if (!nya_file_read(path, content).ok || content->length == 0) return;

    /* A flipped bit, which is what a bad sector and a truncated checksum both look like from here. */
    u64 at = nya_simulation_below(run, content->length);
    content->items[at] ^= (u8)(1U << nya_simulation_below(run, 8));

    (void)nya_file_write(path, content);
}

void _nya_simulation_fault_save_torn(NYA_SimulationRun* run) {
    if (!_NYA_SIMULATION_ACTIONS.storage_available) return;

    nya_arena_free_all(_NYA_SIMULATION_ACTIONS.scratch);

    NYA_CString path = _nya_simulation_save_path();
    if (path == nullptr) return;

    NYA_String* content = nya_string_create(_NYA_SIMULATION_ACTIONS.scratch);
    if (!nya_file_read(path, content).ok || content->length == 0) return;

    // a torn write: the machine lost power partway through, so the tail of the file is simply missing.
    u64         keep = nya_simulation_below(run, content->length);
    NYA_String* torn = nya_string_substring_excld(_NYA_SIMULATION_ACTIONS.scratch, content, 0, keep);

    (void)nya_file_write(path, torn);
}

void _nya_simulation_fault_save_vanish(NYA_SimulationRun* run) {
    nya_unused(run);

    if (!_NYA_SIMULATION_ACTIONS.storage_available) return;

    (void)nya_save_delete(NYA_SIMULATION_SAVE_FILE);
}

void _nya_simulation_fault_clock_jump(NYA_SimulationRun* run) {
    // a long stall, a suspended laptop, a frame that took a second. Everything downstream sees one
    // enormous delta, which is where accumulators overflow and interpolations run past their end.
    nya_simulation_advance(run, run->time_step_ns * (1 + nya_simulation_below(run, 600)));
}

void _nya_simulation_fault_world_restart(NYA_SimulationRun* run) {
    nya_unused(run);

    /*
     * Crash-only, taken at a moment nobody chose: the world is destroyed with whatever it holds, which
     * runs every despawn and every body teardown in whatever order the table is in, and a fresh one
     * comes up in its place. If recovery only works from a tidy state, this is what finds out.
     */
    nya_world_destroy(_NYA_SIMULATION_ACTIONS.world);

    _NYA_SIMULATION_ACTIONS.world = nya_world_create();
    (void)nya_world_set(_NYA_SIMULATION_ACTIONS.world);

    // the layer registry died with the old world, so the names are registered again. The order is the
    // same, so the bits are the same, which is what keeps a replay from diverging here.
    NYA_ConstCString names[NYA_SIMULATION_KIND_COUNT] = {
        NYA_SIMULATION_LAYER_GROUND,
        NYA_SIMULATION_LAYER_ACTOR,
        NYA_SIMULATION_LAYER_DEBRIS,
        NYA_SIMULATION_LAYER_TRIGGER,
    };

    for (u32 i = 0; i < NYA_SIMULATION_KIND_COUNT; i++) {
        NYA_PhysicsLayerMask layer = nya_physics_layer(names[i]);

        nya_assert(layer == _NYA_SIMULATION_ACTIONS.layers[i], "a rebuilt world gave '%s' a different bit", names[i]);
    }

    _NYA_SIMULATION_ACTIONS.restarts++;

    nya_assert(nya_entity_count() == 0, "a fresh world came up holding entities");
    nya_assert(nya_physics2d_body_count() == 0, "a fresh world came up holding 2D bodies");
    nya_assert(nya_physics3d_body_count() == 0, "a fresh world came up holding 3D bodies");
}

/*
 * ─────────────────────────────────────────────────────────
 * INVARIANTS
 * ─────────────────────────────────────────────────────────
 */

void _nya_simulation_check_entity_table(NYA_SimulationRun* run) {
    u32 slots = nya_entity_slot_count();
    u32 live  = 0;

    for (u32 slot = 0; slot < slots; slot++) {
        NYA_Entity* entity = nya_entity_at_slot(slot);
        if (entity == nullptr) continue;

        live++;

        // a handle has to address the entity holding it; a slot or generation that drifted is how a
        // stale handle starts resolving to somebody else.
        if (nya_entity_get(entity->handle) != entity) {
            nya_simulation_fail(run, "slot %u holds an entity whose own handle resolves elsewhere", slot);
            return;
        }
    }

    if (live != nya_entity_count()) nya_simulation_fail(run, "the table holds %u entities and the count says %u", live, nya_entity_count());
}

void _nya_simulation_check_body_counts(NYA_SimulationRun* run) {
    u32 slots      = nya_entity_slot_count();
    u32 bodies_2d  = 0;
    u32 bodies_3d  = 0;

    for (u32 slot = 0; slot < slots; slot++) {
        NYA_Entity* entity = nya_entity_at_slot(slot);
        if (entity == nullptr) continue;

        bodies_2d += nya_physics2d_body_attached(entity) ? 1 : 0;
        bodies_3d += nya_physics3d_body_attached(entity) ? 1 : 0;
    }

    if (bodies_2d != nya_physics2d_body_count()) {
        nya_simulation_fail(run, "%u entities carry a 2D body and the solver counts %u", bodies_2d, nya_physics2d_body_count());
    }

    if (bodies_3d != nya_physics3d_body_count()) {
        nya_simulation_fail(run, "%u entities carry a 3D body and the solver counts %u", bodies_3d, nya_physics3d_body_count());
    }
}

void _nya_simulation_check_hierarchy(NYA_SimulationRun* run) {
    u32 slots = nya_entity_slot_count();

    for (u32 slot = 0; slot < slots; slot++) {
        NYA_Entity* entity = nya_entity_at_slot(slot);
        if (entity == nullptr) continue;

        // a cycle would make the transform pass recurse forever, so the engine refuses to build one.
        if (nya_entity_is_ancestor(entity->handle, entity->handle)) {
            nya_simulation_fail(run, "entity %u is its own ancestor", entity->handle.index);
            return;
        }

        NYA_EntityHandle parent = nya_entity_parent(entity);
        if (!nya_entity_is_valid(parent)) continue;

        if (nya_entity_get(parent) == nullptr) {
            nya_simulation_fail(run, "entity %u has a parent that does not resolve", entity->handle.index);
            return;
        }
    }
}

void _nya_simulation_check_query_agrees(NYA_SimulationRun* run) {
    /*
     * The spatial index against a linear scan. This is the one invariant with a real oracle rather than
     * an internal consistency check: the grid is an optimisation, and an optimisation that returns a
     * different answer from the obvious version is broken however fast it is.
     */
    nya_system_entity_grid_rebuild();

    f32x2 center = { nya_simulation_shaped_f32(run, -512.0F, 512.0F), nya_simulation_shaped_f32(run, -512.0F, 512.0F) };
    f32   radius = nya_simulation_range_f32(run, 1.0F, 256.0F);

    NYA_EntityHandle found[NYA_SIMULATION_ENTITY_MAX];
    u32              count = nya_entity_query_radius(center, radius, found, nya_carray_length(found));

    u32 expected = 0;
    u32 slots    = nya_entity_slot_count();

    for (u32 slot = 0; slot < slots; slot++) {
        NYA_Entity* entity = nya_entity_at_slot(slot);
        if (entity == nullptr) continue;

        f32 dx = entity->position.x - center.x;
        f32 dy = entity->position.y - center.y;

        // a NaN position compares false against everything, including itself, so it is in neither
        // count and the two still agree. That is the honest answer, not a special case.
        if ((dx * dx) + (dy * dy) <= radius * radius) expected++;
    }

    // capped: the query stops at the buffer, so a world larger than the buffer is not a disagreement.
    if (expected <= nya_carray_length(found) && count != expected) {
        nya_simulation_fail(run, "a radius query found %u entities where a scan finds %u", count, expected);
    }
}

void _nya_simulation_check_clock(NYA_SimulationRun* run) {
    if (nya_simulation_now_ns(run) < _NYA_SIMULATION_ACTIONS.clock_seen_ns) {
        nya_simulation_fail(run, "the simulated clock went backwards");
    }

    _NYA_SIMULATION_ACTIONS.clock_seen_ns = nya_simulation_now_ns(run);
}

#endif // NYA_TESTING

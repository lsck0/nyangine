#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_entity_apply_deferred_despawn(void* data);

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
    NYA_App* app = nya_app_get();

    NYA_Arena* allocator = nya_arena_create(.name = "entity_system_allocator");

    app->entity_system = (NYA_EntitySystem){
        .allocator   = allocator,
        .entities    = nya_arena_alloc(allocator, NYA_ENTITY_MAX * sizeof(NYA_Entity)),
        .occupied    = nya_arena_alloc(allocator, NYA_ENTITY_MAX * sizeof(b8)),
        .generations = nya_arena_alloc(allocator, NYA_ENTITY_MAX * sizeof(u32)),
        .free_slots  = nya_arena_alloc(allocator, NYA_ENTITY_MAX * sizeof(u32)),
    };

    nya_memset(app->entity_system.entities, 0, NYA_ENTITY_MAX * sizeof(NYA_Entity));
    nya_memset(app->entity_system.occupied, 0, NYA_ENTITY_MAX * sizeof(b8));

    // Generations start at 1 so a zeroed handle, which is what an uninitialized struct field holds,
    // never resolves to slot 0.
    for (u32 i = 0; i < NYA_ENTITY_MAX; i++) app->entity_system.generations[i] = 1;

    // Seeded in reverse so the first spawns come out of slot 0 upward, which keeps the high water
    // mark tight and makes a fresh world read sensibly in a debugger.
    for (u32 i = 0; i < NYA_ENTITY_MAX; i++) app->entity_system.free_slots[i] = NYA_ENTITY_MAX - 1 - i;
    app->entity_system.free_count = NYA_ENTITY_MAX;

    nya_info("Entity system initialized (%d slots).", NYA_ENTITY_MAX);
}

void nya_system_entity_deinit(void) {
    NYA_App* app = nya_app_get();

    nya_entity_clear();
    nya_arena_destroy(app->entity_system.allocator);
    app->entity_system = (NYA_EntitySystem){ 0 };

    nya_info("Entity system deinitialized.");
}

void nya_system_entity_update(f32 delta_time_s) {
    nya_entity_foreach(entity) {
        if (!nya_flag_check(entity->flags, NYA_ENTITY_FLAG_ACTIVE)) continue;

        // Integrate before the callback, so an update that reads position sees where the entity is
        // this tick rather than where it was last one.
        if (!nya_flag_check(entity->flags, NYA_ENTITY_FLAG_STATIC)) {
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

        NYA_EntityOnUpdateFn on_update = nya_callback_get(entity->on_update);
        if (on_update != nullptr) on_update(entity, delta_time_s);
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

NYA_EntityHandle nya_entity_spawn_with_options(NYA_EntitySpawnOptions options) {
    NYA_App*          app    = nya_app_get();
    NYA_EntitySystem* system = &app->entity_system;

    if (system->free_count == 0) {
        nya_warn("Cannot spawn entity '%s': all %d entity slots are in use.", options.name ? options.name : "(unnamed)", NYA_ENTITY_MAX);
        return NYA_ENTITY_HANDLE_NONE;
    }

    u32 slot = system->free_slots[--system->free_count];

    NYA_EntityHandle handle = { .index = slot, .generation = system->generations[slot] };

    NYA_Entity* entity = &system->entities[slot];
    *entity            = (NYA_Entity){
        .handle           = handle,
        .flags            = options.flags,
        .type             = options.type,
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
    };

    system->occupied[slot] = true;
    system->count++;
    if (slot + 1 > system->high_water_mark) system->high_water_mark = slot + 1;

    // Called with the entity already live, so on_spawn can spawn more, despawn itself, or hand its
    // own handle to something else.
    NYA_EntityOnSpawnFn on_spawn = nya_callback_get(entity->on_spawn);
    if (on_spawn != nullptr) on_spawn(entity);

    return handle;
}

void nya_entity_despawn(NYA_EntityHandle entity) {
    NYA_App*          app    = nya_app_get();
    NYA_EntitySystem* system = &app->entity_system;

    NYA_Entity* target = nya_entity_get(entity);
    if (target == nullptr) return;

    NYA_EntityOnDespawnFn on_despawn = nya_callback_get(target->on_despawn);
    if (on_despawn != nullptr) on_despawn(target);

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
    if (nya_flag_check(target->flags, NYA_ENTITY_FLAG_DESPAWNING)) return;

    nya_flag_set(target->flags, NYA_ENTITY_FLAG_DESPAWNING);
    nya_sim_defer(_nya_entity_apply_deferred_despawn, &entity, sizeof(entity));
}

NYA_Entity* nya_entity_get(NYA_EntityHandle entity) {
    NYA_App*          app    = nya_app_get();
    NYA_EntitySystem* system = &app->entity_system;

    if (entity.index >= NYA_ENTITY_MAX) return nullptr;
    if (!system->occupied[entity.index]) return nullptr;
    if (system->generations[entity.index] != entity.generation) return nullptr;

    return &system->entities[entity.index];
}

b8 nya_entity_is_valid(NYA_EntityHandle entity) {
    return nya_entity_get(entity) != nullptr;
}

u32 nya_entity_count(void) {
    NYA_App* app = nya_app_get();
    return app->entity_system.count;
}

void nya_entity_clear(void) {
    NYA_App*          app    = nya_app_get();
    NYA_EntitySystem* system = &app->entity_system;

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
    NYA_App*          app    = nya_app_get();
    NYA_EntitySystem* system = &app->entity_system;

    if (index >= NYA_ENTITY_MAX) return nullptr;
    if (!system->occupied[index]) return nullptr;

    return &system->entities[index];
}

u32 nya_entity_slot_count(void) {
    NYA_App* app = nya_app_get();
    return app->entity_system.high_water_mark;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_entity_apply_deferred_despawn(void* data) {
    NYA_EntityHandle handle = *(NYA_EntityHandle*)data;
    nya_entity_despawn(handle);
}

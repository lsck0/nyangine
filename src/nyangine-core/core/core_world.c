#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The current world, held here as well as on NYA_App: nya_app_get asserts `initialized`, which tests
 * that build a world without a full app would fail on every entity, physics and sim call.
 */
NYA_INTERNAL NYA_World* _nya_world_current = nullptr;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

NYA_World* nya_world_create(void) {
    NYA_Arena* allocator = nya_arena_create(.name = "world_allocator");

    NYA_World* world = nya_arena_alloc(allocator, sizeof(NYA_World));
    *world           = (NYA_World){ .allocator = allocator };

    /*
     * Current for the duration of the bring-up, then handed back, since the three systems reach their
     * state through nya_world instead of a parameter.
     */
    NYA_World* previous = nya_world_set(world);

    // before both solvers: a body attached during bring-up resolves its layers through this registry.
    nya_system_physics_layer_init();
    nya_system_physics2d_init();
    nya_system_physics3d_init();
    nya_system_entity_init();
    nya_system_sim_init();

    (void)nya_world_set(previous);

    return world;
}

void nya_world_destroy(NYA_World* world) {
    if (world == nullptr) return;

    // Current for the teardown too: on_despawn is game code that calls nya_entity_*, which must read
    // the world being torn down rather than whatever else happens to be current.
    NYA_World* previous = nya_world_set(world);

    nya_system_sim_deinit();
    nya_system_entity_deinit();
    nya_system_physics3d_deinit();
    nya_system_physics2d_deinit();
    nya_system_physics_layer_deinit();

    // destroying the current world leaves none current, rather than a pointer to freed memory.
    (void)nya_world_set(previous == world ? nullptr : previous);

    // Last: this frees the NYA_World struct itself, and everything the game hung off user_data.
    nya_arena_destroy(world->allocator);
}

/*
 * ─────────────────────────────────────────────────────────
 * THE CURRENT WORLD
 * ─────────────────────────────────────────────────────────
 */

NYA_World* nya_world(void) {
    nya_assert(_nya_world_current != nullptr, "There is no current world. nya_app_init creates one; see core_world.h.");
    return _nya_world_current;
}

b8 nya_world_exists(void) {
    return _nya_world_current != nullptr;
}

NYA_World* nya_world_set(NYA_World* world) {
    NYA_World* previous = _nya_world_current;
    _nya_world_current  = world;
    return previous;
}

/*
 * ─────────────────────────────────────────────────────────
 * GAME STATE
 * ─────────────────────────────────────────────────────────
 */

void* nya_world_user_data(void) {
    /*
     * Deliberately not asserting that a world exists, unlike nya_world: a game reads this to decide
     * fresh-start vs. reload, and teardown paths read it after the app has already released the
     * world. Both want an answer rather than an assertion, and null is a truthful one.
     */
    if (_nya_world_current == nullptr) return nullptr;
    return _nya_world_current->user_data;
}

void nya_world_user_data_set(void* user_data) {
    nya_world()->user_data = user_data;
}

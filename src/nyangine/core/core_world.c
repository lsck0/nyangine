#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The current world, held here rather than on NYA_App: going through nya_app_get would put an
 * assertion on `initialized` in front of every entity/physics/sim call, which a test that brings up
 * a world without a full application would fail. NYA_App holds the pointer too, as the owner; this
 * is the fast path.
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
     * Made current for the duration of the bring-up, then handed back — the three systems below reach
     * their state through nya_world rather than taking it as a parameter, so building a world that is
     * not yet current means being current for as long as it takes to build.
     *
     * Physics before entities, reversed on the way out: despawning an entity destroys the rigid body
     * it carries, so the physics world has to still exist while the entity table is emptied. That
     * ordering used to live in nya_app_init, one comment away from being lost; here it can't be
     * gotten wrong by a caller at all.
     */
    NYA_World* previous = nya_world_set(world);

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

    // A world cannot be left current after it's freed — destroying the current one leaves none,
    // rather than restoring a pointer to the thing about to be released.
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

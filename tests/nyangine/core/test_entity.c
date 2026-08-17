/**
 * The entity system: slots, generations, and the two ways to despawn.
 *
 * The generation counter is what most of this is about. A handle is a slot plus a generation, and
 * the generation is what makes a handle to a despawned entity stay invalid after the slot is reused
 * — without it, holding a handle across a despawn silently addresses whoever moved in.
 *
 * Headless throughout: entities are plain data and the system needs nothing but an arena.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Float comparison for the interpolation tests. Positions are built by lerp, so exact equality is
 * the wrong test even where the arithmetic happens to be exact. */
static b8 close_enough(f32 a, f32 b) {
  return fabsf(a - b) < 0.001F;
}

/** Counts calls, so a test can prove a lifecycle callback actually fired. */
static u32 spawn_calls   = 0;
static u32 despawn_calls = 0;
static u32 update_calls  = 0;

static void on_spawn(NYA_Entity* entity) {
  nya_unused(entity);
  spawn_calls++;
}

static void on_despawn(NYA_Entity* entity) {
  nya_unused(entity);
  despawn_calls++;
}

static void on_update(NYA_Entity* entity, f32 delta_time_s) {
  nya_unused(entity);
  nya_unused(delta_time_s);
  update_calls++;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  // The callback system, because spawn options carry NYA_CallbackHandles, and the sim system,
  // because nya_entity_despawn_deferred queues through nya_sim_defer. Without the latter the
  // deferred path allocates from a null arena and asserts inside the allocator, which points at
  // base_arena rather than at the missing dependency.
  nya_system_callback_init();
  // The world: entities, physics and the simulation barrier, brought up in the order they depend on
  // each other. See core_world.h.
  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);

  defer nya_system_callback_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a fresh system is empty, and nothing invalid resolves
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(nya_entity_count() == 0, "nothing spawned yet");

    NYA_EntityHandle nothing = { 0 };
    nya_assert(!nya_entity_is_valid(nothing), "a zeroed handle is not a valid entity");
    nya_assert(nya_entity_get(nothing) == nullptr);

    // Despawning something that was never spawned has to be a no-op rather than a fault: an unwind
    // path frequently holds a handle it is not sure about.
    nya_entity_despawn(nothing);
    nya_assert(nya_entity_count() == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a spawned entity is valid, counted, and carries what it was given
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EntityHandle handle = nya_entity_spawn(
      .name     = "player",
      .position = { 1.0F, 2.0F, 3.0F },
      .state    = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE
    );

    nya_assert(nya_entity_is_valid(handle), "the handle just returned is valid");
    nya_assert(nya_entity_count() == 1, "one spawn, one entity");

    NYA_Entity* entity = nya_entity_get(handle);
    nya_assert(entity != nullptr);
    nya_assert(nya_string_equals((NYA_CString)entity->name, "player"));
    nya_assert(entity->position[0] == 1.0F && entity->position[1] == 2.0F && entity->position[2] == 3.0F);
    nya_assert(nya_flag_check(entity->state, NYA_ENTITY_STATE_ACTIVE));
    nya_assert(nya_flag_check(entity->state, NYA_ENTITY_STATE_VISIBLE));
    nya_assert(!nya_flag_check(entity->state, NYA_ENTITY_STATE_STATIC));

    nya_entity_despawn(handle);
    nya_assert(nya_entity_count() == 0, "despawn is immediate");
    nya_assert(!nya_entity_is_valid(handle), "and the handle stops resolving");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a stale handle does not address whoever took the slot
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The reason handles carry a generation at all. Spawn, despawn, spawn again: the second entity
    // almost certainly lands in the freed slot, and the first handle must not reach it.
    NYA_EntityHandle first = nya_entity_spawn(.name = "first");
    nya_assert(nya_entity_is_valid(first));

    nya_entity_despawn(first);
    nya_assert(!nya_entity_is_valid(first));

    NYA_EntityHandle second = nya_entity_spawn(.name = "second");
    nya_assert(nya_entity_is_valid(second), "the new entity is fine");

    nya_assert(!nya_entity_is_valid(first), "the old handle is still dead even though its slot is occupied again");
    nya_assert(nya_entity_get(first) == nullptr, "and resolves to nothing rather than to 'second'");

    NYA_Entity* alive = nya_entity_get(second);
    nya_assert(alive != nullptr && nya_string_equals((NYA_CString)alive->name, "second"));

    nya_entity_despawn(second);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: many entities, each independently addressable
  // ─────────────────────────────────────────────────────────────────────────────
  {
    enum { COUNT = 256 };
    NYA_EntityHandle handles[COUNT];

    for (u32 i = 0; i < COUNT; i++) {
      handles[i] = nya_entity_spawn(.name = "many", .position = { (f32)i, 0.0F, 0.0F });
      nya_assert(nya_entity_is_valid(handles[i]), "spawn " FMTu32 " failed", i);
    }

    nya_assert(nya_entity_count() == COUNT, "got " FMTu32, nya_entity_count());

    // Each handle resolves to its own entity, which is what proves slots are not being shared.
    for (u32 i = 0; i < COUNT; i++) {
      NYA_Entity* entity = nya_entity_get(handles[i]);
      nya_assert(entity != nullptr, "entity " FMTu32 " vanished", i);
      nya_assert(entity->position[0] == (f32)i, "entity " FMTu32 " has position %f", i, (f64)entity->position[0]);
    }

    // Despawning from the middle must not disturb its neighbours.
    nya_entity_despawn(handles[COUNT / 2]);
    nya_assert(nya_entity_count() == COUNT - 1);
    nya_assert(!nya_entity_is_valid(handles[COUNT / 2]));
    nya_assert(nya_entity_is_valid(handles[COUNT / 2 - 1]), "the one before is untouched");
    nya_assert(nya_entity_is_valid(handles[COUNT / 2 + 1]), "and so is the one after");

    nya_entity_clear();
    nya_assert(nya_entity_count() == 0, "clear removes everything");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: deferred despawn waits, immediate despawn does not
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EntityHandle handle = nya_entity_spawn(.name = "deferred", .state = NYA_ENTITY_STATE_ACTIVE);

    nya_entity_despawn_deferred(handle);

    // Still there, and marked. This is what lets an entity despawn itself from inside its own
    // update without the system deleting the thing it is currently iterating.
    nya_assert(nya_entity_is_valid(handle), "deferred despawn does not take effect immediately");
    nya_assert(nya_flag_check(nya_entity_get(handle)->state, NYA_ENTITY_STATE_DESPAWNING), "but it is flagged");

    // The deferred despawn is a sim command, so it lands at the barrier rather than in the update.
    nya_system_entity_update(1.0F / 60.0F);
    nya_system_sim_apply_commands();

    nya_assert(!nya_entity_is_valid(handle), "applying the queued commands is what actually removes it");
    nya_assert(nya_entity_count() == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: lifecycle callbacks fire, and only for active entities
  // ─────────────────────────────────────────────────────────────────────────────
  {
    spawn_calls = despawn_calls = update_calls = 0;

    NYA_EntityHandle active = nya_entity_spawn(
      .name       = "active",
      .state      = NYA_ENTITY_STATE_ACTIVE,
      .on_spawn   = nya_callback(on_spawn),
      .on_despawn = nya_callback(on_despawn),
      .on_update  = nya_callback(on_update)
    );
    nya_assert(spawn_calls == 1, "on_spawn fired during the spawn, got " FMTu32, spawn_calls);

    // Explicitly not ACTIVE. Spawn defaults to ACTIVE | VISIBLE, so this has to be overridden
    // rather than merely omitted — the update loop skips it, which is what the flag is for.
    NYA_EntityHandle inactive = nya_entity_spawn(
      .name      = "inactive",
      .state     = NYA_ENTITY_STATE_VISIBLE,
      .on_update = nya_callback(on_update)
    );
    nya_assert(!nya_flag_check(nya_entity_get(inactive)->state, NYA_ENTITY_STATE_ACTIVE), "the override took");

    update_calls = 0;
    nya_system_entity_update(1.0F / 60.0F);
    nya_assert(update_calls == 1, "only the active entity updated, got " FMTu32, update_calls);

    nya_entity_despawn(active);
    nya_assert(despawn_calls == 1, "on_despawn fired, got " FMTu32, despawn_calls);

    nya_entity_despawn(inactive);
    nya_assert(despawn_calls == 1, "the entity with no on_despawn did not invent one");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: integration moves an entity by its velocity
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EntityHandle handle = nya_entity_spawn(
      .name     = "moving",
      .state    = NYA_ENTITY_STATE_ACTIVE,
      .position = { 0.0F, 0.0F, 0.0F },
      .velocity = { 1.0F, 0.0F, 0.0F }
    );

    // One second of updates at a sixtieth each, so the entity should have travelled one unit.
    for (u32 i = 0; i < 60; i++) nya_system_entity_update(1.0F / 60.0F);

    NYA_Entity* entity = nya_entity_get(handle);
    nya_assert(entity != nullptr);

    // Loose tolerance: this is sixty f32 additions, not one multiplication.
    f32 x = entity->position[0];
    nya_assert(x > 0.99F && x < 1.01F, "expected roughly 1.0, got %f", (f64)x);

    nya_entity_despawn(handle);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a static entity does not integrate
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EntityHandle handle = nya_entity_spawn(
      .name     = "static",
      .state    = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_STATIC,
      .velocity = { 1.0F, 0.0F, 0.0F }
    );

    for (u32 i = 0; i < 60; i++) nya_system_entity_update(1.0F / 60.0F);

    nya_assert(nya_entity_get(handle)->position[0] == 0.0F, "a static entity ignores its velocity");
    nya_entity_despawn(handle);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: slot iteration sees exactly the live entities
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_entity_clear();

    NYA_EntityHandle a = nya_entity_spawn(.name = "a");
    NYA_EntityHandle b = nya_entity_spawn(.name = "b");
    NYA_EntityHandle c = nya_entity_spawn(.name = "c");
    nya_entity_despawn(b);

    u32 seen = 0;
    for (u32 slot = 0; slot < nya_entity_slot_count(); slot++) {
      NYA_Entity* entity = nya_entity_at_slot(slot);
      if (entity == nullptr) continue;

      seen++;
      nya_assert(!nya_string_equals((NYA_CString)entity->name, "b"), "the despawned entity is not iterated");
    }

    nya_assert(seen == 2, "two live entities, saw " FMTu32, seen);
    nya_assert(nya_entity_at_slot(nya_entity_slot_count() + 100) == nullptr, "past the end is null, not a fault");

    nya_entity_despawn(a);
    nya_entity_despawn(c);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the spatial grid answers rectangle, radius and kind queries
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_entity_clear();

    // Three decades apart, so each lands in a different cell at any sane cell size, and one sits at
    // a negative coordinate — which is where a truncating cell computation goes wrong.
    NYA_EntityHandle near_plane   = nya_entity_spawn(.name = "near_plane", .type = 1, .position = { 10.0F, 10.0F, 0.0F });
    NYA_EntityHandle far_plane    = nya_entity_spawn(.name = "far_plane", .type = 1, .position = { 4000.0F, 4000.0F, 0.0F });
    NYA_EntityHandle behind = nya_entity_spawn(.name = "behind", .type = 2, .position = { -500.0F, -500.0F, 0.0F });

    nya_assert(nya_entity_is_valid(near_plane) && nya_entity_is_valid(far_plane) && nya_entity_is_valid(behind));

    nya_system_entity_grid_rebuild();

    NYA_EntityHandle found[16];

    // A box around the origin finds the near one and neither of the others.
    u32 count = nya_entity_query_rect((f32x2){ -100.0F, -100.0F }, (f32x2){ 100.0F, 100.0F }, found, 16);
    nya_assert(count == 1, "expected exactly the near_plane entity, got %u", count);
    nya_assert(found[0].index == near_plane.index);

    // Negative coordinates are a real cell, not a mirror of the positive one.
    count = nya_entity_query_rect((f32x2){ -600.0F, -600.0F }, (f32x2){ -400.0F, -400.0F }, found, 16);
    nya_assert(count == 1, "expected the entity at negative coordinates, got %u", count);
    nya_assert(found[0].index == behind.index);

    // Wide enough for everything.
    count = nya_entity_query_rect((f32x2){ -10000.0F, -10000.0F }, (f32x2){ 10000.0F, 10000.0F }, found, 16);
    nya_assert(count == 3, "a query covering the world must find all three, got %u", count);

    // A radius is the inscribed circle, not the bounding box: the far entity is inside the square
    // that encloses this circle and outside the circle itself.
    count = nya_entity_query_radius((f32x2){ 0.0F, 0.0F }, 100.0F, found, 16);
    nya_assert(count == 1, "expected only the near_plane entity inside the radius, got %u", count);

    // Kind filtering is the thing the physics broadphase cannot do.
    count = nya_entity_query_kind((f32x2){ -10000.0F, -10000.0F }, (f32x2){ 10000.0F, 10000.0F }, 2, found, 16);
    nya_assert(count == 1, "only one entity has type 2, got %u", count);
    nya_assert(found[0].index == behind.index);

    // Capacity is a hard cap and the caller is told how many it actually got.
    count = nya_entity_query_rect((f32x2){ -10000.0F, -10000.0F }, (f32x2){ 10000.0F, 10000.0F }, found, 2);
    nya_assert(count == 2, "a truncated query must report what it wrote, got %u", count);

    // An empty region is not an error.
    count = nya_entity_query_rect((f32x2){ 50000.0F, 50000.0F }, (f32x2){ 50100.0F, 50100.0F }, found, 16);
    nya_assert(count == 0, "an empty region must find nothing, got %u", count);

    // A despawn is invisible to the index until it is rebuilt, which is the contract: the grid is a
    // snapshot of the last rebuild, not a live view.
    nya_entity_despawn(far_plane);
    nya_system_entity_grid_rebuild();

    count = nya_entity_query_rect((f32x2){ -10000.0F, -10000.0F }, (f32x2){ 10000.0F, 10000.0F }, found, 16);
    nya_assert(count == 2, "the despawned entity must be gone after a rebuild, got %u", count);

    nya_entity_clear();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the kind and flag index is correct the moment it is changed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_entity_clear();

    enum { KIND_A = 1, KIND_B = 2 };
    enum { FLAG_X = 1ULL << 0, FLAG_Y = 1ULL << 1 };

    NYA_EntityHandle a1 = nya_entity_spawn(.name = "a1", .type = KIND_A);
    NYA_EntityHandle a2 = nya_entity_spawn(.name = "a2", .type = KIND_A, .flags = FLAG_X);
    NYA_EntityHandle b1 = nya_entity_spawn(.name = "b1", .type = KIND_B, .flags = FLAG_X | FLAG_Y);

    nya_assert(nya_entity_is_valid(a1) && nya_entity_is_valid(a2) && nya_entity_is_valid(b1));

    /*
     * Immediately, with no rebuild in between.
     *
     * This is the property that made the index incremental rather than rebuilt once a tick like the
     * spatial grid: a stale index does not give a slightly wrong answer, it silently omits an entity,
     * and a system that misses something for one frame is miserable to track down.
     */
    u32 count = 0;
    nya_entity_foreach_kind (KIND_A, entity) {
      nya_assert(entity->type == KIND_A, "the kind index returned the wrong kind");
      count++;
    }
    nya_assert(count == 2, "expected both KIND_A entities, got %u", count);

    count = 0;
    nya_entity_foreach_kind (KIND_B, entity) count++;
    nya_assert(count == 1, "expected one KIND_B entity, got %u", count);

    // Every bit, not any bit: b1 has both, a2 has only one.
    count = 0;
    nya_entity_foreach_flags (FLAG_X, entity) count++;
    nya_assert(count == 2, "expected both entities carrying FLAG_X, got %u", count);

    count = 0;
    nya_entity_foreach_flags (FLAG_X | FLAG_Y, entity) count++;
    nya_assert(count == 1, "a multi-bit query must require every bit, got %u", count);

    // A flag added after the fact is visible without anything being rebuilt.
    nya_entity_flag_enable(nya_entity_get(a1), FLAG_Y);

    count = 0;
    nya_entity_foreach_flags (FLAG_Y, entity) count++;
    nya_assert(count == 2, "a newly enabled flag must be queryable at once, got %u", count);

    // And removing one takes it back out.
    nya_entity_flag_disable(nya_entity_get(b1), FLAG_Y);

    count = 0;
    nya_entity_foreach_flags (FLAG_Y, entity) count++;
    nya_assert(count == 1, "a disabled flag must leave the index at once, got %u", count);

    // Despawning removes it from every set it was in.
    nya_entity_despawn(a2);

    count = 0;
    nya_entity_foreach_kind (KIND_A, entity) count++;
    nya_assert(count == 1, "a despawned entity must leave its kind, got %u", count);

    count = 0;
    nya_entity_foreach_flags (FLAG_X, entity) count++;
    nya_assert(count == 1, "a despawned entity must leave its flags, got %u", count);

    /*
     * Slot reuse is where a stale bit would show.
     *
     * The free list hands back the most recently freed slot, so this new entity lands in exactly the
     * one a2 occupied. If despawn had cleared only `live` and left the kind and flag bits set, this
     * would appear in KIND_A and under FLAG_X despite being neither.
     */
    NYA_EntityHandle reused = nya_entity_spawn(.name = "reused", .type = KIND_B);
    nya_assert(reused.index == a2.index, "expected the freed slot back, so the test means something");

    count = 0;
    nya_entity_foreach_kind (KIND_A, entity) count++;
    nya_assert(count == 1, "a reused slot must not inherit the old entity's kind, got %u", count);

    count = 0;
    nya_entity_foreach_flags (FLAG_X, entity) count++;
    nya_assert(count == 1, "a reused slot must not inherit the old entity's flags, got %u", count);

    // A kind past the indexed range has no bitset and falls back to a scan, which must still work.
    NYA_EntityHandle far_plane = nya_entity_spawn(.name = "far_plane", .type = NYA_ENTITY_KIND_MAX + 5);

    count = 0;
    nya_entity_foreach_kind (NYA_ENTITY_KIND_MAX + 5, entity) count++;
    nya_assert(count == 1, "an unindexed kind must still be findable, got %u", count);

    nya_unused(far_plane);

    nya_entity_clear();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: interpolated motion
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EntityHandle handle = nya_entity_spawn(.name = "mover", .position = { 0.0F, 0.0F, 0.0F });
    NYA_Entity*      entity = nya_entity_get(handle);

    nya_assert(!nya_entity_moving(entity), "a fresh entity is not moving");
    nya_assert(nya_entity_move_progress(entity) == 1.0F, "a still entity reads as arrived");

    nya_entity_move_to(entity, (f32x3){ 100.0F, 0.0F, 0.0F }, 1.0F, NYA_EASE_LINEAR);
    nya_assert(nya_entity_moving(entity), "the move is running");

    // Half the duration, linear, so exactly half the distance. Anything else here means the origin
    // was not captured at the call and the lerp is stepping from wherever the entity happens to be.
    nya_system_entity_update(0.5F);
    nya_assert(close_enough(entity->position.x, 50.0F), "half a linear move is half the distance, got %f", (f64)entity->position.x);

    // Deliberately overshooting the remaining time. The step clamps rather than extrapolating, so
    // the entity lands on the target instead of sailing past it on a long frame.
    nya_system_entity_update(10.0F);
    nya_assert(close_enough(entity->position.x, 100.0F), "an overlong tick still lands on the target, got %f", (f64)entity->position.x);
    nya_assert(!nya_entity_moving(entity), "the move ends on the tick it arrives, not the one after");

    // Restart, then abandon it partway. The entity keeps what it had reached rather than snapping
    // back to the origin or on to the target.
    nya_entity_move_to(entity, (f32x3){ 0.0F, 0.0F, 0.0F }, 1.0F, NYA_EASE_LINEAR);
    nya_system_entity_update(0.25F);
    nya_entity_move_stop(entity);

    nya_assert(!nya_entity_moving(entity), "a stopped move is not running");
    nya_assert(close_enough(entity->position.x, 75.0F), "a stopped move leaves the entity where it got to, got %f", (f64)entity->position.x);

    // Zero duration is the teleport spelling, and must not divide by it.
    nya_entity_move_to(entity, (f32x3){ -10.0F, 5.0F, 0.0F }, 0.0F, NYA_EASE_LINEAR);
    nya_assert(close_enough(entity->position.x, -10.0F) && close_enough(entity->position.y, 5.0F), "a zero duration move arrives immediately");
    nya_assert(!nya_entity_moving(entity), "a teleport leaves no move running");

    // Speed derived duration: 100 units at 50 per second is two seconds, so one second is halfway.
    nya_entity_move_to(entity, (f32x3){ 90.0F, 5.0F, 0.0F }, 0.0F, NYA_EASE_LINEAR);
    nya_entity_move_to_at_speed(entity, (f32x3){ -10.0F, 5.0F, 0.0F }, 50.0F);
    nya_system_entity_update(1.0F);
    nya_assert(close_enough(entity->position.x, 40.0F), "speed and distance decide the duration, got %f", (f64)entity->position.x);

    nya_entity_clear();
  }

  printf("PASSED: test_entity\n");
  return 0;
}

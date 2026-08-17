/**
 * Two separate worlds, one wire: the case in-process tests cannot reach.
 *
 * Every other net test runs the server and the client over one entity table, so a server handle and a
 * client handle happen to be the same number and a bug in translating between them is invisible. That
 * is not how a real client works, and it hid the largest gap in the netcode: a client spawned snapshot
 * entities into its own table, so its handles never matched the server's, and the second snapshot
 * could not tell "I already have this" from "this is new".
 *
 * This test builds two worlds — nya_world_create twice, swapped with nya_world_set — and moves
 * snapshots between them by hand. The client world's table is deliberately *pre-polluted* so its free
 * list hands out different indices than the server's, which is what makes the handle spaces genuinely
 * disagree rather than coincidentally agree.
 *
 * What it defends:
 *
 * - Applying the same snapshot twice does not duplicate anything.
 * - The two handle spaces really are different, and the map bridges them.
 * - An entity the server removes is removed locally; one the client owns itself is not.
 * - Reconnecting does not leave the previous session's entities standing.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FLAG_REPLICATED (1ULL << 5)

/** Counts the replicated entities in whichever world is current. */
static u32 replicated_count(void) {
  u32 count = 0;

  nya_entity_foreach (entity) {
    if ((entity->flags & FLAG_REPLICATED) == 0) continue;
    if ((entity->state & NYA_ENTITY_STATE_DESPAWNING) != 0) continue;

    count++;
  }

  return count;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  defer nya_system_callback_deinit();

  NYA_Arena* arena = nya_arena_create(.name = "test_replica");
  defer      nya_arena_destroy(arena);

  /*
   * Two worlds. Each owns its own entity table, which is the whole point.
   *
   * nya_world_set swaps which one the entity API talks to, so "the server does X" and "the client does
   * Y" below are literally two different tables being written.
   */
  NYA_World* server_world = nya_world_create();
  NYA_World* client_world = nya_world_create();

  defer nya_world_destroy(client_world);
  defer nya_world_destroy(server_world);

  NYA_NetReplicaMap map = { 0 };

  /*
   * The client's table is pushed out of step with the server's before anything is replicated.
   *
   * Spawning and despawning leaves the free list in a different order, so the client's next slot is not
   * the server's next slot. Without this the two would hand out 0, 1, 2… in the same order and the test
   * would pass even with no translation at all — which is exactly how the bug survived.
   */
  (void)nya_world_set(client_world);
  {
    NYA_EntityHandle filler[8];
    for (u32 i = 0; i < 8; i++) filler[i] = nya_entity_spawn(.name = "clutter", .position = { (f32)i, 0.0F, 0.0F });

    // Every other one, so the free list is interleaved rather than a clean run.
    for (u32 i = 0; i < 8; i += 2) nya_entity_despawn(filler[i]);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the two handle spaces genuinely differ
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: the server and client tables disagree about handles\n");

  NYA_EntityHandle server_a = NYA_ENTITY_HANDLE_NONE;
  NYA_EntityHandle server_b = NYA_ENTITY_HANDLE_NONE;

  {
    (void)nya_world_set(server_world);

    server_a = nya_entity_spawn(.name = "crate", .type = 11, .flags = FLAG_REPLICATED, .position = { 100.0F, 0.0F, 0.0F });
    server_b = nya_entity_spawn(.name = "barrel", .type = 12, .flags = FLAG_REPLICATED, .position = { 200.0F, 0.0F, 0.0F });

    nya_assert(nya_entity_is_valid(server_a) && nya_entity_is_valid(server_b));

    NYA_NetSnapshot snapshot = { 0 };
    NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 1, &snapshot));
    nya_assert(snapshot.entity_count == 2);

    // ── over the wire and into the other world ──────────────────────────────
    NYA_String* payload = nya_string_create(arena);
    NYA_EXPECT(nya_net_snapshot_encode(arena, &snapshot, nullptr, payload));

    (void)nya_world_set(client_world);

    NYA_NetSnapshot received = { 0 };
    NYA_EXPECT(nya_net_snapshot_decode(arena, payload->items, payload->length, nullptr, &received));

    u32 before = replicated_count();

    nya_net_snapshot_apply(&received, FLAG_REPLICATED, &map, NYA_ENTITY_HANDLE_NONE);
    nya_system_sim_apply_commands();

    nya_assert(replicated_count() == before + 2, "the client did not spawn the two entities the server described");

    NYA_EntityHandle local_a = nya_net_replica_local(&map, server_a);
    NYA_EntityHandle local_b = nya_net_replica_local(&map, server_b);

    nya_assert(nya_entity_is_valid(local_a) && nya_entity_is_valid(local_b), "both were mapped");

    /*
     * The assertion the whole file exists for.
     *
     * If these matched, the two tables would be in step and nothing here would be testing translation.
     * The pre-pollution above is what guarantees they do not.
     */
    b8 differs = local_a.index != server_a.index || local_b.index != server_b.index;
    nya_assert(differs, "the two worlds handed out the same indices; this test is not testing anything");

    printf("  server a=%u b=%u  ->  client a=%u b=%u\n", server_a.index, server_b.index, local_a.index, local_b.index);

    // And the right entity landed under the right mapping, which is what the indices differing makes
    // possible to get wrong.
    NYA_Entity* got_a = nya_entity_get(local_a);
    NYA_Entity* got_b = nya_entity_get(local_b);

    nya_assert(got_a != nullptr && got_b != nullptr);
    nya_assert(got_a->position.x == 100.0F && got_a->type == 11, "the crate came through as the crate");
    nya_assert(got_b->position.x == 200.0F && got_b->type == 12, "and the barrel as the barrel");
    nya_assert((got_a->flags & FLAG_REPLICATED) != 0, "a spawned replica carries the replication flag");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: applying repeatedly does not duplicate
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: repeated snapshots do not duplicate entities\n");
  {
    (void)nya_world_set(server_world);

    // The server moves one of them, so each snapshot is genuinely new rather than byte-identical.
    for (u32 round = 0; round < 10; round++) {
      (void)nya_world_set(server_world);

      NYA_Entity* moving = nya_entity_get(server_a);
      nya_assert(moving != nullptr);
      moving->position.x += 1.0F;

      NYA_NetSnapshot snapshot = { 0 };
      NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 2 + round, &snapshot));

      NYA_String* payload = nya_string_create(arena);
      NYA_EXPECT(nya_net_snapshot_encode(arena, &snapshot, nullptr, payload));

      (void)nya_world_set(client_world);

      NYA_NetSnapshot received = { 0 };
      NYA_EXPECT(nya_net_snapshot_decode(arena, payload->items, payload->length, nullptr, &received));

      nya_net_snapshot_apply(&received, FLAG_REPLICATED, &map, NYA_ENTITY_HANDLE_NONE);
      nya_system_sim_apply_commands();
    }

    /*
     * Still two. Without the map this would be twenty-two: every apply would fail to recognise what it
     * had already spawned and add another copy, and a real session would grow without bound at the
     * snapshot rate.
     */
    nya_assert(replicated_count() == 2, "ten snapshots produced %u entities instead of 2", replicated_count());

    // And the movement was applied to the existing entity rather than to a fresh one.
    NYA_Entity* moved = nya_entity_get(nya_net_replica_local(&map, server_a));
    nya_assert(moved != nullptr);
    nya_assert(moved->position.x == 110.0F, "the mapped entity tracked the server's movement (x = %f)", (f64)moved->position.x);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: what the server removes is removed; what the client owns is not
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: the sweep removes replicas and spares local entities\n");
  {
    // An entity the client made for itself, marked replicated. The server has never heard of it, so the
    // server's silence says nothing about it — sweeping by flag rather than by map would destroy it.
    (void)nya_world_set(client_world);
    NYA_EntityHandle client_owned = nya_entity_spawn(.name = "client effect", .flags = FLAG_REPLICATED, .position = { -50.0F, 0.0F, 0.0F });

    (void)nya_world_set(server_world);
    nya_entity_despawn(server_b);

    NYA_NetSnapshot snapshot = { 0 };
    NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 50, &snapshot));
    nya_assert(snapshot.entity_count == 1, "the server has one replicated entity left");

    NYA_String* payload = nya_string_create(arena);
    NYA_EXPECT(nya_net_snapshot_encode(arena, &snapshot, nullptr, payload));

    (void)nya_world_set(client_world);

    NYA_EntityHandle local_b = nya_net_replica_local(&map, server_b);
    nya_assert(nya_entity_is_valid(local_b), "the barrel is still here before the snapshot arrives");

    NYA_NetSnapshot received = { 0 };
    NYA_EXPECT(nya_net_snapshot_decode(arena, payload->items, payload->length, nullptr, &received));

    nya_net_snapshot_apply(&received, FLAG_REPLICATED, &map, NYA_ENTITY_HANDLE_NONE);
    nya_system_sim_apply_commands();

    nya_assert(!nya_entity_is_valid(local_b), "the entity the server dropped was not despawned locally");
    nya_assert(!nya_entity_is_valid(nya_net_replica_local(&map, server_b)), "and its mapping was forgotten");

    nya_assert(nya_entity_is_valid(client_owned), "the client's own entity was swept even though the server never knew it");
    nya_assert(nya_entity_is_valid(nya_net_replica_local(&map, server_a)), "and the surviving replica survived");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: disconnecting clears the replicated world
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: tearing the map down removes the replicated world\n");
  {
    (void)nya_world_set(client_world);

    NYA_EntityHandle survivor = nya_net_replica_local(&map, server_a);
    nya_assert(nya_entity_is_valid(survivor));

    nya_net_replica_map_despawn_all(&map);
    nya_system_sim_apply_commands();

    /*
     * What reconnecting depends on.
     *
     * The sweep in apply only removes what the server stopped mentioning, so nothing removes a replica
     * once the snapshots stop coming. Without this, a reconnect would find the map empty, spawn a second
     * copy of everything, and leave the first standing forever.
     */
    nya_assert(!nya_entity_is_valid(survivor), "a torn down map left its entities behind");
    nya_assert(!nya_entity_is_valid(nya_net_replica_local(&map, server_a)), "and left its mappings behind");

    // A client's own entity is not the connection's to remove.
    nya_assert(replicated_count() == 1, "only the client's own replicated entity remains, found %u", replicated_count());

    // ── and reconnecting starts clean ──────────────────────────────────────────
    (void)nya_world_set(server_world);

    NYA_NetSnapshot snapshot = { 0 };
    NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 100, &snapshot));

    NYA_String* payload = nya_string_create(arena);
    NYA_EXPECT(nya_net_snapshot_encode(arena, &snapshot, nullptr, payload));

    (void)nya_world_set(client_world);

    NYA_NetSnapshot received = { 0 };
    NYA_EXPECT(nya_net_snapshot_decode(arena, payload->items, payload->length, nullptr, &received));

    nya_net_snapshot_apply(&received, FLAG_REPLICATED, &map, NYA_ENTITY_HANDLE_NONE);
    nya_system_sim_apply_commands();

    nya_assert(replicated_count() == 2, "after reconnecting the client has its own entity plus one replica, found %u", replicated_count());
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a predicted entity is spared by both the apply and the sweep
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: the predicted entity is not overwritten or swept\n");
  {
    (void)nya_world_set(server_world);

    NYA_EntityHandle server_player = nya_entity_spawn(.name = "player", .flags = FLAG_REPLICATED, .position = { 0.0F, 0.0F, 0.0F });

    NYA_NetSnapshot first = { 0 };
    NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 200, &first));

    NYA_String* payload = nya_string_create(arena);
    NYA_EXPECT(nya_net_snapshot_encode(arena, &first, nullptr, payload));

    (void)nya_world_set(client_world);

    NYA_NetSnapshot received = { 0 };
    NYA_EXPECT(nya_net_snapshot_decode(arena, payload->items, payload->length, nullptr, &received));

    // Spawned normally the first time — there is nothing to spare until it exists.
    nya_net_snapshot_apply(&received, FLAG_REPLICATED, &map, NYA_ENTITY_HANDLE_NONE);
    nya_system_sim_apply_commands();

    NYA_EntityHandle local_player = nya_net_replica_local(&map, server_player);
    nya_assert(nya_entity_is_valid(local_player));

    // The client predicts it forward, away from where the server last said it was.
    NYA_Entity* predicted = nya_entity_get(local_player);
    predicted->position.x = 999.0F;

    // The same snapshot again, now naming it as predicted. Its position must survive.
    nya_net_snapshot_apply(&received, FLAG_REPLICATED, &map, server_player);
    nya_system_sim_apply_commands();

    predicted = nya_entity_get(local_player);
    nya_assert(predicted != nullptr, "the predicted entity was swept");
    nya_assert(predicted->position.x == 999.0F, "the predicted entity was overwritten by a snapshot (x = %f)", (f64)predicted->position.x);

    // And it is spared by the sweep too: a snapshot that omits it entirely must not remove it, because
    // that snapshot is a round trip old.
    (void)nya_world_set(server_world);
    nya_entity_despawn(server_player);

    NYA_NetSnapshot without = { 0 };
    NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 201, &without));

    NYA_String* second = nya_string_create(arena);
    NYA_EXPECT(nya_net_snapshot_encode(arena, &without, nullptr, second));

    (void)nya_world_set(client_world);

    NYA_NetSnapshot received_without = { 0 };
    NYA_EXPECT(nya_net_snapshot_decode(arena, second->items, second->length, nullptr, &received_without));

    nya_net_snapshot_apply(&received_without, FLAG_REPLICATED, &map, server_player);
    nya_system_sim_apply_commands();

    nya_assert(nya_entity_is_valid(local_player), "the predicted entity was swept by a snapshot that omitted it");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: remote entities are smoothed between snapshots rather than stepping
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: replicas interpolate between snapshots\n");
  {
    nya_net_replica_map_despawn_all(&map);
    nya_system_sim_apply_commands();

    (void)nya_world_set(server_world);

    // A clean server world, so the counting below is unambiguous.
    nya_entity_foreach (existing) nya_entity_despawn_deferred(existing->handle);
    nya_system_sim_apply_commands();

    NYA_EntityHandle mover = nya_entity_spawn(.name = "mover", .flags = FLAG_REPLICATED, .position = { 0.0F, 0.0F, 0.0F });

    /** Sends the server's current state to the client. */
    #define REPLICATE(at_tick)                                                                                                                             do {                                                                                                                                                   (void)nya_world_set(server_world);                                                                                                                    NYA_NetSnapshot _snapshot = { 0 };                                                                                                                    NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, (at_tick), &_snapshot));                                                                  NYA_String* _payload = nya_string_create(arena);                                                                                                      NYA_EXPECT(nya_net_snapshot_encode(arena, &_snapshot, nullptr, _payload));                                                                            (void)nya_world_set(client_world);                                                                                                                    NYA_NetSnapshot _received = { 0 };                                                                                                                    NYA_EXPECT(nya_net_snapshot_decode(arena, _payload->items, _payload->length, nullptr, &_received));                                                   nya_net_snapshot_apply(&_received, FLAG_REPLICATED, &map, NYA_ENTITY_HANDLE_NONE);                                                                    nya_system_sim_apply_commands();                                                                                                                    } while (0)

    REPLICATE(300);

    NYA_EntityHandle local_mover = nya_net_replica_local(&map, mover);
    nya_assert(nya_entity_is_valid(local_mover));

    /*
     * One snapshot is not motion, so nothing is interpolated yet.
     *
     * Interpolating from a zeroed origin here is what would make every entity fly in from the world
     * origin on the frame it appeared.
     */
    {
      (void)nya_world_set(client_world);

      NYA_Entity* entity = nya_entity_get(local_mover);
      f32         before = entity->position.x;

      nya_net_replica_interpolate(&map, 1.0F / 60.0F, 1.0F / 20.0F, NYA_ENTITY_HANDLE_NONE);

      entity = nya_entity_get(local_mover);
      nya_assert(entity->position.x == before, "a replica with only one snapshot must not move");
    }

    // The server moves it a long way, and tells the client.
    (void)nya_world_set(server_world);
    nya_entity_get(mover)->position = (f32x3){ 100.0F, 0.0F, 0.0F };

    REPLICATE(301);

    /*
     * Applying the snapshot puts the entity at the target, and interpolation then walks it there from
     * where it was. So immediately after a snapshot the entity is *at* 100 — and the first interpolated
     * frame pulls it back toward 0, which is the one-snapshot-of-lag this trades for smoothness.
     */
    (void)nya_world_set(client_world);

    nya_net_replica_interpolate(&map, 1.0F / 60.0F, 1.0F / 20.0F, NYA_ENTITY_HANDLE_NONE);

    NYA_Entity* entity = nya_entity_get(local_mover);
    nya_assert(entity != nullptr);

    f32 first = entity->position.x;

    nya_assert(first > 0.0F && first < 100.0F, "the first interpolated frame should be between the two snapshots, got %f", (f64)first);

    // Three frames per snapshot at 60/20, so a third of the way each frame.
    nya_assert(first > 25.0F && first < 40.0F, "one frame of three should be about a third of the way, got %f", (f64)first);

    // Monotonic toward the target, and it arrives rather than overshooting.
    f32 previous = first;

    for (u32 frame = 0; frame < 10; frame++) {
      nya_net_replica_interpolate(&map, 1.0F / 60.0F, 1.0F / 20.0F, NYA_ENTITY_HANDLE_NONE);

      entity = nya_entity_get(local_mover);
      nya_assert(entity != nullptr);

      nya_assert(entity->position.x >= previous, "interpolation went backwards on frame %u", frame);
      nya_assert(entity->position.x <= 100.0F, "interpolation overshot the target on frame %u (%f)", frame, (f64)entity->position.x);

      previous = entity->position.x;
    }

    nya_assert(previous == 100.0F, "interpolation never reached the target, stopped at %f", (f64)previous);

    /*
     * A frame far longer than the snapshot interval clamps rather than flying past.
     *
     * A stall — a loading hitch, a debugger breakpoint — produces exactly this, and an entity that
     * overshoots and then snaps back is more visible than one that simply arrives early.
     */
    (void)nya_world_set(server_world);
    nya_entity_get(mover)->position = (f32x3){ 200.0F, 0.0F, 0.0F };
    REPLICATE(302);

    (void)nya_world_set(client_world);

    nya_net_replica_interpolate(&map, 5.0F, 1.0F / 20.0F, NYA_ENTITY_HANDLE_NONE);

    entity = nya_entity_get(local_mover);
    nya_assert(entity->position.x == 200.0F, "a very long frame should clamp at the target, got %f", (f64)entity->position.x);

    // ── the predicted entity is left alone ────────────────────────────────────
    {
      (void)nya_world_set(server_world);
      nya_entity_get(mover)->position = (f32x3){ 300.0F, 0.0F, 0.0F };
      REPLICATE(303);

      (void)nya_world_set(client_world);

      // As if the client had predicted it somewhere else entirely.
      nya_entity_get(local_mover)->position = (f32x3){ 777.0F, 0.0F, 0.0F };

      nya_net_replica_interpolate(&map, 1.0F / 60.0F, 1.0F / 20.0F, mover);

      entity = nya_entity_get(local_mover);
      nya_assert(entity->position.x == 777.0F, "the predicted entity was dragged back by interpolation (%f)", (f64)entity->position.x);
    }

    #undef REPLICATE
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: clearing the map without touching the entities
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: clearing a map leaves its entities alone\n");
  {
    /*
     * The non-destructive counterpart to nya_net_replica_map_despawn_all, for a caller that is about to
     * destroy the world anyway.
     *
     * The distinction matters: a client that intends to keep playing wants the entities gone, and one that is
     * shutting down wants the cheapest possible teardown. Using the wrong one leaves either orphans in a live
     * world or a despawn pass over a world about to be freed.
     */
    (void)nya_world_set(client_world);

    NYA_EntityHandle survivor = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { 1.0F, 2.0F, 3.0F });

    NYA_NetReplicaMap scratch = { 0 };

    // A pairing by hand, since what is being tested is the clear rather than how the map was filled.
    scratch.entries[0] = (NYA_NetReplica){ .remote = { .index = 77, .generation = 1 }, .local = survivor, .present = true };
    scratch.count      = 1;

    nya_assert(nya_entity_is_valid(nya_net_replica_local(&scratch, (NYA_EntityHandle){ .index = 77, .generation = 1 })));

    nya_net_replica_map_clear(&scratch);

    nya_assert(scratch.count == 0, "the map was not cleared");
    nya_assert(!nya_entity_is_valid(nya_net_replica_local(&scratch, (NYA_EntityHandle){ .index = 77, .generation = 1 })),
               "a cleared map still resolves");

    // The entity is untouched, which is the whole difference from despawn_all.
    nya_system_sim_apply_commands();
    nya_assert(nya_entity_is_valid(survivor), "clearing the map despawned an entity");

    nya_entity_despawn(survivor);
    nya_system_sim_apply_commands();
  }

  // Both worlds are torn down by the defers above, in reverse order of creation.
  (void)nya_world_set(server_world);

  printf("PASSED: test_replica (0 failures)\n");

  return EXIT_SUCCESS;
}

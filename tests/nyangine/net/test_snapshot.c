/**
 * Snapshots: capture, delta encode, decode, apply.
 *
 * The dangerous property of this layer is that its bugs are quiet. A transport bug drops a
 * connection; a snapshot bug produces a world that is subtly wrong — an entity a few units off, a
 * rotation that never updates, a crate that should have been removed and was not. So most of what is
 * checked here is round-trip fidelity, field by field, and the specific cases where a delta can lie.
 *
 * The ones that matter most:
 *
 * - **An unchanged field must come from the baseline, not from zero.** Getting this wrong makes every
 *   stationary entity snap to the origin with an identity rotation, which is the single most likely
 *   mistake in a delta encoder.
 * - **A reused entity slot is a different entity.** Handles are generational, and a baseline whose
 *   generation differs must not be delta'd against — otherwise one entity's state is applied to
 *   another that merely inherited its index.
 * - **A decoder fed nonsense must fail rather than allocate or read out of bounds.** These bytes come
 *   from an untrusted peer; the count on the wire is a number somebody else chose.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** The game's own bit meaning "replicate me". The engine never names one; see nya_net_snapshot_capture. */
#define FLAG_REPLICATED (1ULL << 3)

/** A second game flag, to check that the whole `flags` word round trips rather than just the marker. */
#define FLAG_ENEMY (1ULL << 9)

static b8 states_equal(const NYA_NetEntityState* a, const NYA_NetEntityState* b) {
  return nya_net_entity_state_diff(a, b) == 0 && a->handle.index == b->handle.index && a->handle.generation == b->handle.generation;
}

/** Encodes then decodes, so a test asserts on what a client would actually end up with. */
static NYA_NetSnapshot round_trip(NYA_Arena* arena, const NYA_NetSnapshot* snapshot, const NYA_NetSnapshot* baseline, OUT u64* out_bytes) {
  NYA_String* buffer = nya_string_create(arena);

  NYA_EXPECT(nya_net_snapshot_encode(arena, snapshot, baseline, buffer));

  if (out_bytes != nullptr) *out_bytes = buffer->length;

  NYA_NetSnapshot decoded = { 0 };
  NYA_EXPECT(nya_net_snapshot_decode(arena, buffer->items, buffer->length, baseline, &decoded));

  return decoded;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);
  defer nya_system_callback_deinit();

  NYA_Arena* arena = nya_arena_create(.name = "test_snapshot");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: capture takes only what is marked
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: capture selects on the game's flag\n");
  {
    NYA_EntityHandle replicated = nya_entity_spawn(.name = "crate", .flags = FLAG_REPLICATED, .position = { 10.0F, 20.0F, 30.0F });
    NYA_EntityHandle local      = nya_entity_spawn(.name = "particle", .position = { 1.0F, 2.0F, 3.0F });

    NYA_NetSnapshot snapshot = { 0 };
    NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 7, &snapshot));

    nya_assert(snapshot.tick == 7, "the tick is carried, because reconciliation is keyed on it");
    nya_assert(snapshot.entity_count == 1, "only the marked entity is captured");
    nya_assert(snapshot.entities[0].handle.index == replicated.index);

    nya_assert(nya_net_snapshot_find(&snapshot, replicated) != nullptr);
    nya_assert(nya_net_snapshot_find(&snapshot, local) == nullptr, "an unmarked entity is not in the snapshot");

    // A flag of zero is what single player passes, and it must cost nothing rather than replicate all.
    NYA_NetSnapshot none = { 0 };
    NYA_EXPECT(nya_net_snapshot_capture(arena, 0, 7, &none));
    nya_assert(none.entity_count == 0, "no flag replicates nothing");

    nya_entity_despawn(replicated);
    nya_entity_despawn(local);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a full snapshot round trips every field exactly
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: full snapshot round trips every field\n");
  {
    NYA_EntityHandle handle = nya_entity_spawn(
      .name     = "subject",
      .type     = 42,
      .flags    = FLAG_REPLICATED | FLAG_ENEMY,
      .position = { 1.5F, -2.25F, 1024.125F },
      .scale    = { 2.0F, 3.0F, 4.0F }
    );

    NYA_Entity* entity = nya_entity_get(handle);
    nya_assert(entity != nullptr);

    entity->rotation         = nya_quaternion_from_euler(0.25F, 0.5F, 0.75F);
    entity->velocity         = (f32x3){ -9.5F, 0.0F, 3.25F };
    entity->angular_velocity = (f32x3){ 0.125F, -0.25F, 0.5F };

    NYA_NetSnapshot snapshot = { 0 };
    NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 99, &snapshot));
    nya_assert(snapshot.entity_count == 1);

    u64             bytes   = 0;
    NYA_NetSnapshot decoded = round_trip(arena, &snapshot, nullptr, &bytes);

    nya_assert(decoded.tick == 99);
    nya_assert(decoded.entity_count == 1);

    /*
     * Exact equality, not a tolerance.
     *
     * Floats cross the wire as their bit pattern rather than through a decimal form, so a round trip
     * is lossless and anything less than exact means a field was written or read wrongly. Values
     * above were chosen to be exactly representable so this assertion is about the codec.
     */
    nya_assert(states_equal(&snapshot.entities[0], &decoded.entities[0]), "a full snapshot round trip lost or changed a field");

    // Spot checks, so a failure says which field rather than only that one differed.
    nya_assert(decoded.entities[0].position.z == 1024.125F);
    nya_assert(decoded.entities[0].type == 42);
    nya_assert(decoded.entities[0].flags == (FLAG_REPLICATED | FLAG_ENEMY), "the whole flags word survives, not just the marker bit");
    nya_assert(decoded.entities[0].angular_velocity.y == -0.25F);
    nya_assert(decoded.entities[0].handle.generation == handle.generation, "the generation is on the wire");

    printf("  one entity, full: %llu bytes\n", (unsigned long long)bytes);

    nya_entity_despawn(handle);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a delta against an identical baseline costs almost nothing
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: an unchanged world deltas to nearly nothing\n");
  {
    NYA_EntityHandle handles[16];
    for (u32 i = 0; i < 16; i++) {
      handles[i] = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { (f32)i, (f32)i * 2.0F, 0.0F });
    }

    NYA_NetSnapshot first = { 0 };
    NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 1, &first));
    nya_assert(first.entity_count == 16);

    NYA_NetSnapshot baseline = nya_net_snapshot_clone(arena, &first);

    u64 full_bytes = 0;
    (void)round_trip(arena, &first, nullptr, &full_bytes);

    // Nothing moved between the two captures.
    NYA_NetSnapshot second = { 0 };
    NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 2, &second));

    u64             delta_bytes = 0;
    NYA_NetSnapshot decoded     = round_trip(arena, &second, &baseline, &delta_bytes);

    printf("  16 entities: %llu bytes full, %llu bytes delta\n", (unsigned long long)full_bytes, (unsigned long long)delta_bytes);

    nya_assert(delta_bytes < full_bytes / 4, "an unchanged world should delta to a small fraction of a full snapshot");

    /*
     * And the decode still produces the *full* state, not the empty delta.
     *
     * This is the assertion that catches "unchanged means zero": every field must have come back out
     * of the baseline. Getting it wrong puts all sixteen entities at the origin, which a byte-count
     * assertion alone would happily accept.
     */
    nya_assert(decoded.entity_count == 16);

    /*
     * Looked up by handle, not by position in the array.
     *
     * A snapshot is ordered by handle *index*, and the entity table hands freed slots back in LIFO
     * order — so after any test that despawned something, the sixteenth entity spawned may well sit
     * first. Indexing `decoded.entities[i]` and expecting the i-th spawn is a test that passes only on
     * a table that has never been reused.
     */
    for (u32 i = 0; i < 16; i++) {
      const NYA_NetEntityState* got = nya_net_snapshot_find(&decoded, handles[i]);
      nya_assert(got != nullptr, "entity %u is missing from the decoded snapshot", i);

      nya_assert(got->position.x == (f32)i, "entity %u lost its position to the delta", i);
      nya_assert(got->position.y == (f32)i * 2.0F);
      nya_assert(got->flags == FLAG_REPLICATED, "entity %u lost its flags", i);

      const NYA_NetEntityState* expected = nya_net_snapshot_find(&second, handles[i]);
      nya_assert(expected != nullptr);
      nya_assert(states_equal(expected, got));
    }

    // ── one entity moves; only it should be in the delta ──────────────────────
    {
      NYA_Entity* mover = nya_entity_get(handles[5]);
      nya_assert(mover != nullptr);
      mover->position = (f32x3){ 500.0F, 600.0F, 700.0F };

      NYA_NetSnapshot third = { 0 };
      NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 3, &third));

      u64             moved_bytes = 0;
      NYA_NetSnapshot moved       = round_trip(arena, &third, &baseline, &moved_bytes);

      nya_assert(moved_bytes > delta_bytes, "a moved entity costs more than an unchanged one");
      nya_assert(moved_bytes < full_bytes, "but still far less than a full snapshot");

      const NYA_NetEntityState* five = nya_net_snapshot_find(&moved, handles[5]);
      const NYA_NetEntityState* four = nya_net_snapshot_find(&moved, handles[4]);
      const NYA_NetEntityState* six  = nya_net_snapshot_find(&moved, handles[6]);

      nya_assert(five != nullptr && four != nullptr && six != nullptr);

      nya_assert(five->position.x == 500.0F, "the moved entity's new position arrived");
      nya_assert(four->position.x == 4.0F, "and the others kept theirs out of the baseline");
      nya_assert(six->position.x == 6.0F);

      // Put it back so the despawn loop below is not confused by it.
      mover->position = (f32x3){ 5.0F, 10.0F, 0.0F };
    }

    for (u32 i = 0; i < 16; i++) nya_entity_despawn(handles[i]);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a reused slot is not a baseline for its successor
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a reused entity slot is a different entity\n");
  {
    NYA_EntityHandle first = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { 100.0F, 100.0F, 100.0F });

    NYA_NetSnapshot before = { 0 };
    NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 1, &before));
    NYA_NetSnapshot baseline = nya_net_snapshot_clone(arena, &before);

    nya_entity_despawn(first);

    // The table hands slots back in LIFO order, so this very likely takes the index just freed —
    // which is exactly the collision being tested.
    NYA_EntityHandle second = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { -1.0F, -2.0F, -3.0F });

    nya_assert(second.index == first.index, "the test needs the slot to be reused to mean anything");
    nya_assert(second.generation != first.generation, "and the generation to have moved on");

    NYA_NetSnapshot after = { 0 };
    NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 2, &after));

    NYA_NetSnapshot decoded = round_trip(arena, &after, &baseline, nullptr);

    nya_assert(decoded.entity_count == 1);

    /*
     * The newcomer must arrive whole.
     *
     * Delta-ing it against the old occupant would emit a mask saying "position unchanged" — the two
     * differ, so in practice position would be sent, but `type`, `flags` and `scale` match and would
     * be omitted. The client, which has never seen this entity, would then fill them from a baseline
     * describing something else. Comparing the generation is what prevents it.
     */
    nya_assert(decoded.entities[0].handle.generation == second.generation);
    nya_assert(decoded.entities[0].position.x == -1.0F, "the new occupant's own position arrived");
    nya_assert(decoded.entities[0].position.z == -3.0F);
    nya_assert(states_equal(&after.entities[0], &decoded.entities[0]));

    // And nya_net_snapshot_find must refuse a stale handle rather than answering with the successor.
    nya_assert(nya_net_snapshot_find(&decoded, first) == nullptr, "a stale handle finds nothing");
    nya_assert(nya_net_snapshot_find(&decoded, second) != nullptr);

    nya_entity_despawn(second);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: applying a snapshot spawns, moves and despawns
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: apply reconciles the world with the snapshot\n");
  {
    // Build a snapshot describing two entities, by hand rather than by capture, so the "spawn what is
    // new" path is exercised against a world that does not contain them.
    NYA_NetEntityState described[2] = {
      {
        .handle   = { .index = 900, .generation = 1 },
        .type     = 3,
        .flags    = FLAG_REPLICATED,
        .state    = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE,
        .position = { 11.0F, 12.0F, 13.0F },
        .rotation = { 0.0F, 0.0F, 0.0F, 1.0F },
        .scale    = { 1.0F, 1.0F, 1.0F },
      },
      {
        .handle   = { .index = 901, .generation = 1 },
        .type     = 4,
        .flags    = FLAG_REPLICATED,
        .state    = NYA_ENTITY_STATE_ACTIVE,
        .position = { 21.0F, 22.0F, 23.0F },
        .rotation = { 0.0F, 0.0F, 0.0F, 1.0F },
        .scale    = { 1.0F, 1.0F, 1.0F },
      },
    };

    NYA_NetSnapshot incoming = { .tick = 5, .entities = described, .entity_count = 2 };

    /*
     * An entity this process spawned itself, marked replicated, that the snapshot does not mention.
     *
     * It must **survive**. The sweep removes what the server has stopped mentioning — which means what
     * the map knows about — and this was never mapped, so the server has never said anything about it
     * either way. Sweeping by flag instead would have a client destroy its own entities the moment it
     * connected to anything, which is why the map is the authority here rather than the flag.
     */
    NYA_EntityHandle local_only = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { 0.0F, 0.0F, 0.0F });

    // And an unreplicated one, which must be left entirely alone.
    NYA_EntityHandle untouched = nya_entity_spawn(.position = { 7.0F, 7.0F, 7.0F });

    NYA_NetReplicaMap map = { 0 };

    nya_net_snapshot_apply(&incoming, FLAG_REPLICATED, &map, NYA_ENTITY_HANDLE_NONE);

    // The despawn is deferred, so it takes the barrier to actually happen — the same barrier the app
    // loop runs at the end of every tick.
    nya_system_sim_apply_commands();

    nya_assert(nya_entity_is_valid(local_only), "an entity the map never knew about is not swept by a snapshot");
    nya_assert(nya_entity_is_valid(untouched), "an unreplicated entity is not touched by a snapshot");

    // The two described entities were spawned. Their local handles are the local table's, not the
    // server's, so they are found by looking for what has the right position.
    u32 found = 0;
    nya_entity_foreach (entity) {
      if ((entity->flags & FLAG_REPLICATED) == 0) continue;

      if (entity->position.x == 11.0F) {
        nya_assert(entity->type == 3);
        nya_assert(entity->position.z == 13.0F);
        found++;
      }

      if (entity->position.x == 21.0F) {
        nya_assert(entity->type == 4);
        found++;
      }
    }

    nya_assert(found == 2, "both described entities were spawned, found %u", found);

    /*
     * The map is what makes applying twice idempotent.
     *
     * This is the case the replica map exists for: the local handles are not the server's, so without
     * a translation the second apply cannot tell "I already have this" from "this is new" and spawns a
     * duplicate of everything. Every tick. Forever.
     */
    u32 after_first = 0;
    nya_entity_foreach (entity) {
      nya_unused(entity);
      after_first++;
    }

    nya_net_snapshot_apply(&incoming, FLAG_REPLICATED, &map, NYA_ENTITY_HANDLE_NONE);
    nya_system_sim_apply_commands();

    u32 after_second = 0;
    nya_entity_foreach (entity) {
      nya_unused(entity);
      after_second++;
    }

    nya_assert(after_second == after_first, "applying the same snapshot twice spawned duplicates (%u -> %u)", after_first, after_second);

    // And the mapping resolves in both directions.
    NYA_EntityHandle described_remote = { .index = 900, .generation = 1 };
    NYA_EntityHandle described_local  = nya_net_replica_local(&map, described_remote);

    nya_assert(nya_entity_is_valid(described_local), "the map resolves a server handle to a local one");

    NYA_Entity* resolved = nya_entity_get(described_local);
    nya_assert(resolved != nullptr && resolved->position.x == 11.0F, "and it resolves to the right entity");

    NYA_EntityHandle round_trip = nya_net_replica_remote(&map, described_local);
    nya_assert(round_trip.index == 900 && round_trip.generation == 1, "and back again");

    nya_assert(!nya_entity_is_valid(nya_net_replica_local(&map, (NYA_EntityHandle){ .index = 4242, .generation = 1 })),
               "an unmapped server handle resolves to nothing");

    // ── an entity the server drops is despawned locally ──────────────────────
    {
      NYA_NetSnapshot shrunk = { .tick = 6, .entities = described, .entity_count = 1 };

      nya_net_snapshot_apply(&shrunk, FLAG_REPLICATED, &map, NYA_ENTITY_HANDLE_NONE);
      nya_system_sim_apply_commands();

      nya_assert(!nya_entity_is_valid(nya_net_replica_local(&map, (NYA_EntityHandle){ .index = 901, .generation = 1 })),
                 "an entity the snapshot stopped mentioning was despawned and unmapped");

      nya_assert(nya_entity_is_valid(nya_net_replica_local(&map, described_remote)), "and the one it still mentions survived");
    }

    nya_entity_despawn(untouched);
    nya_entity_despawn(local_only);
    nya_entity_foreach (entity) {
      if ((entity->flags & FLAG_REPLICATED) != 0) nya_entity_despawn_deferred(entity->handle);
    }
    nya_system_sim_apply_commands();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the decoder refuses what a hostile peer can send
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: the decoder rejects malformed payloads\n");
  {
    NYA_NetSnapshot decoded = { 0 };

    // Nothing at all.
    nya_assert(!nya_net_snapshot_decode(arena, nullptr, 0, nullptr, &decoded).ok);

    u8 truncated[4] = { 1, 2, 3, 4 };
    nya_assert(!nya_net_snapshot_decode(arena, truncated, sizeof(truncated), nullptr, &decoded).ok,
               "a payload too short for the header is refused");

    /*
     * A count the payload cannot possibly hold.
     *
     * This is the one that matters: the count is a number the peer chose, and a decoder that
     * allocated from it before validating would turn a twelve byte datagram into a request for
     * gigabytes. Both bounds are checked — against NYA_NET_MAX_REPLICATED and against the payload's
     * own length.
     */
    NYA_String* lying = nya_string_create(arena);
    for (u32 i = 0; i < 8; i++) nya_string_push_back(lying, 0); // tick
    nya_string_push_back(lying, 0xFF);
    nya_string_push_back(lying, 0xFF);
    nya_string_push_back(lying, 0xFF);
    nya_string_push_back(lying, 0xFF); // count = 4294967295

    nya_assert(!nya_net_snapshot_decode(arena, lying->items, lying->length, nullptr, &decoded).ok,
               "an absurd entity count is refused rather than allocated");

    // A plausible count with no entity data behind it.
    NYA_String* short_body = nya_string_create(arena);
    for (u32 i = 0; i < 8; i++) nya_string_push_back(short_body, 0);
    nya_string_push_back(short_body, 4);
    nya_string_push_back(short_body, 0);
    nya_string_push_back(short_body, 0);
    nya_string_push_back(short_body, 0); // count = 4, and nothing follows

    nya_assert(!nya_net_snapshot_decode(arena, short_body->items, short_body->length, nullptr, &decoded).ok,
               "a count with no data behind it is refused");

    // A mask claiming fields the payload runs out before providing.
    NYA_String* short_fields = nya_string_create(arena);
    for (u32 i = 0; i < 8; i++) nya_string_push_back(short_fields, 0);
    nya_string_push_back(short_fields, 1);
    nya_string_push_back(short_fields, 0);
    nya_string_push_back(short_fields, 0);
    nya_string_push_back(short_fields, 0); // count = 1
    for (u32 i = 0; i < 8; i++) nya_string_push_back(short_fields, 0); // handle
    nya_string_push_back(short_fields, 0xFF);
    nya_string_push_back(short_fields, 0x00); // mask = every field, and none of them follow

    nya_assert(!nya_net_snapshot_decode(arena, short_fields->items, short_fields->length, nullptr, &decoded).ok,
               "a mask promising fields that are not there is refused");

    /*
     * Entities out of ascending handle order.
     *
     * The encoder and the decoder both pair an entity against its baseline by walking two sorted lists in
     * step, with the baseline cursor only ever moving forward. A peer that sends them out of order walks
     * that cursor past the matching baseline, so entities get decoded against the *wrong* one and their
     * unnamed fields are filled in from a different entity's state. That is not a crash — it is a client
     * quietly shown a wrong world, which is why the order is checked rather than assumed.
     */
    {
      NYA_String* unsorted = nya_string_create(arena);

      for (u32 i = 0; i < 8; i++) nya_string_push_back(unsorted, 0); // tick
      nya_string_push_back(unsorted, 2);
      nya_string_push_back(unsorted, 0);
      nya_string_push_back(unsorted, 0);
      nya_string_push_back(unsorted, 0); // count = 2

      // Index 5, generation 1, empty mask.
      nya_string_push_back(unsorted, 5);
      for (u32 i = 0; i < 3; i++) nya_string_push_back(unsorted, 0);
      nya_string_push_back(unsorted, 1);
      for (u32 i = 0; i < 3; i++) nya_string_push_back(unsorted, 0);
      nya_string_push_back(unsorted, 0);
      nya_string_push_back(unsorted, 0);

      // Then index 2 — lower than the one before it.
      nya_string_push_back(unsorted, 2);
      for (u32 i = 0; i < 3; i++) nya_string_push_back(unsorted, 0);
      nya_string_push_back(unsorted, 1);
      for (u32 i = 0; i < 3; i++) nya_string_push_back(unsorted, 0);
      nya_string_push_back(unsorted, 0);
      nya_string_push_back(unsorted, 0);

      nya_assert(!nya_net_snapshot_decode(arena, unsorted->items, unsorted->length, nullptr, &decoded).ok,
                 "entities out of handle order are refused");
    }

    // A duplicate index is the same problem: two entities cannot occupy one slot at one instant.
    {
      NYA_String* duplicate = nya_string_create(arena);

      for (u32 i = 0; i < 8; i++) nya_string_push_back(duplicate, 0);
      nya_string_push_back(duplicate, 2);
      for (u32 i = 0; i < 3; i++) nya_string_push_back(duplicate, 0);

      for (u32 entity = 0; entity < 2; entity++) {
        nya_string_push_back(duplicate, 3);
        for (u32 i = 0; i < 3; i++) nya_string_push_back(duplicate, 0);
        nya_string_push_back(duplicate, 1);
        for (u32 i = 0; i < 3; i++) nya_string_push_back(duplicate, 0);
        nya_string_push_back(duplicate, 0);
        nya_string_push_back(duplicate, 0);
      }

      nya_assert(!nya_net_snapshot_decode(arena, duplicate->items, duplicate->length, nullptr, &decoded).ok,
                 "a repeated handle index is refused");
    }

    /*
     * A generation of zero names nothing.
     *
     * Refused because a replica map records a pairing keyed on the remote handle, and a zero generation is
     * indistinguishable from an empty slot — so such an entity could be spawned and then never found
     * again, spawning another copy on every snapshot.
     */
    {
      NYA_String* zero_generation = nya_string_create(arena);

      for (u32 i = 0; i < 8; i++) nya_string_push_back(zero_generation, 0);
      nya_string_push_back(zero_generation, 1);
      for (u32 i = 0; i < 3; i++) nya_string_push_back(zero_generation, 0);

      for (u32 i = 0; i < 8; i++) nya_string_push_back(zero_generation, 0); // index 0, generation 0
      nya_string_push_back(zero_generation, 0);
      nya_string_push_back(zero_generation, 0);

      nya_assert(!nya_net_snapshot_decode(arena, zero_generation->items, zero_generation->length, nullptr, &decoded).ok,
                 "an entity with a zero generation is refused");
    }

    // An empty but well formed snapshot is legal: a world with nothing replicated in it.
    NYA_String* empty = nya_string_create(arena);
    for (u32 i = 0; i < 8; i++) nya_string_push_back(empty, 0);
    for (u32 i = 0; i < 4; i++) nya_string_push_back(empty, 0);

    NYA_EXPECT(nya_net_snapshot_decode(arena, empty->items, empty->length, nullptr, &decoded));
    nya_assert(decoded.entity_count == 0, "an empty snapshot decodes to an empty world, not an error");
  }

  printf("PASSED: test_snapshot (0 failures)\n");

  return EXIT_SUCCESS;
}

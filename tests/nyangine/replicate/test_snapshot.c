/**
 * Snapshots: capture, delta encode, decode, apply.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** The game's own bit meaning "replicate me". The engine never names one; see nya_net_snapshot_capture. */
#define FLAG_REPLICATED (1ULL << 3)

/** A second game flag, to check that the whole `flags` word round trips rather than just the marker. */
#define FLAG_ENEMY (1ULL << 9)

/** Equal on the wire: every field exact except the rotation, which crosses as smallest three and so within a fraction of a degree. */
static b8 states_equal(const NYA_NetEntityState* a, const NYA_NetEntityState* b) {
  u16 differ = nya_net_entity_state_diff(a, b) & (u16)~NYA_NET_FIELD_ROTATION;

  f32 dot = fabsf((a->rotation.x * b->rotation.x) + (a->rotation.y * b->rotation.y) + (a->rotation.z * b->rotation.z) + (a->rotation.w * b->rotation.w));

  return differ == 0 && dot > 0.99999F && a->handle.index == b->handle.index && a->handle.generation == b->handle.generation;
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

// TEST: capture takes only what is marked
NYA_INTERNAL void test_capture_takes_only_what_is_marked(NYA_Arena* arena) {
  printf("TEST: capture selects on the game's flag\n");
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

// TEST: a full snapshot round trips every field exactly
NYA_INTERNAL void test_full_snapshot_round_trips_every_field(NYA_Arena* arena) {
  printf("TEST: full snapshot round trips every field\n");
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

  // exact for everything on a power of two grid, which every value here is.
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

// TEST: a delta against an identical baseline costs almost nothing
NYA_INTERNAL void test_delta_against_identical_baseline(NYA_Arena* arena) {
  printf("TEST: an unchanged world deltas to nearly nothing\n");
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

  /* And the decode still produces the *full* state, not the empty delta. */
  nya_assert(decoded.entity_count == 16);

  /* Looked up by handle, not by position in the array. */
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

// TEST: a reused slot is not a baseline for its successor
NYA_INTERNAL void test_reused_slot_is_not_a_baseline(NYA_Arena* arena) {
  printf("TEST: a reused entity slot is a different entity\n");
  NYA_EntityHandle first = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { 100.0F, 100.0F, 100.0F });

  NYA_NetSnapshot before = { 0 };
  NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 1, &before));
  NYA_NetSnapshot baseline = nya_net_snapshot_clone(arena, &before);

  nya_entity_despawn(first);

  // the table reuses slots LIFO, so this very likely takes the index just freed: the collision under
  // test.
  NYA_EntityHandle second = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { -1.0F, -2.0F, -3.0F });

  nya_assert(second.index == first.index, "the test needs the slot to be reused to mean anything");
  nya_assert(second.generation != first.generation, "and the generation to have moved on");

  NYA_NetSnapshot after = { 0 };
  NYA_EXPECT(nya_net_snapshot_capture(arena, FLAG_REPLICATED, 2, &after));

  NYA_NetSnapshot decoded = round_trip(arena, &after, &baseline, nullptr);

  nya_assert(decoded.entity_count == 1);

  /* The newcomer must arrive whole. */
  nya_assert(decoded.entities[0].handle.generation == second.generation);
  nya_assert(decoded.entities[0].position.x == -1.0F, "the new occupant's own position arrived");
  nya_assert(decoded.entities[0].position.z == -3.0F);
  nya_assert(states_equal(&after.entities[0], &decoded.entities[0]));

  // And nya_net_snapshot_find must refuse a stale handle rather than answering with the successor.
  nya_assert(nya_net_snapshot_find(&decoded, first) == nullptr, "a stale handle finds nothing");
  nya_assert(nya_net_snapshot_find(&decoded, second) != nullptr);

  nya_entity_despawn(second);
}

// TEST: applying a snapshot spawns, moves and despawns
NYA_INTERNAL void test_applying_snapshot_spawns_moves_and_despawns(NYA_Arena* arena) {
  printf("TEST: apply reconciles the world with the snapshot\n");
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

  /* An entity this process spawned itself, marked replicated, that the snapshot does not mention. */
  NYA_EntityHandle local_only = nya_entity_spawn(.flags = FLAG_REPLICATED, .position = { 0.0F, 0.0F, 0.0F });

  // And an unreplicated one, which must be left entirely alone.
  NYA_EntityHandle untouched = nya_entity_spawn(.position = { 7.0F, 7.0F, 7.0F });

  /*
   * On the arena, not the stack: NYA_NetReplicaMap is 624 KB, and Windows gives a thread 1 MB by
   * default against Linux's 8. See the same note in test_replica.c.
   */
  NYA_NetReplicaMap* map = nya_arena_alloc(arena, sizeof(NYA_NetReplicaMap));
  nya_assert(map != nullptr);

  nya_memset(map, 0, sizeof(*map));

  nya_net_snapshot_apply(&incoming, FLAG_REPLICATED, map, NYA_ENTITY_HANDLE_NONE);

  // the despawn is deferred until the barrier, as the app loop runs at the end of every tick.
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

  /* The map is what makes applying twice idempotent. */
  u32 after_first = 0;
  nya_entity_foreach (entity) {
    nya_unused(entity);
    after_first++;
  }

  nya_net_snapshot_apply(&incoming, FLAG_REPLICATED, map, NYA_ENTITY_HANDLE_NONE);
  nya_system_sim_apply_commands();

  u32 after_second = 0;
  nya_entity_foreach (entity) {
    nya_unused(entity);
    after_second++;
  }

  nya_assert(after_second == after_first, "applying the same snapshot twice spawned duplicates (%u -> %u)", after_first, after_second);

  // And the mapping resolves in both directions.
  NYA_EntityHandle described_remote = { .index = 900, .generation = 1 };
  NYA_EntityHandle described_local  = nya_net_replica_local(map, described_remote);

  nya_assert(nya_entity_is_valid(described_local), "the map resolves a server handle to a local one");

  NYA_Entity* resolved = nya_entity_get(described_local);
  nya_assert(resolved != nullptr && resolved->position.x == 11.0F, "and it resolves to the right entity");

  NYA_EntityHandle round_trip = nya_net_replica_remote(map, described_local);
  nya_assert(round_trip.index == 900 && round_trip.generation == 1, "and back again");

  nya_assert(!nya_entity_is_valid(nya_net_replica_local(map, (NYA_EntityHandle){ .index = 4242, .generation = 1 })),
             "an unmapped server handle resolves to nothing");

  // ── an entity the server drops is despawned locally ──────────────────────
  {
    NYA_NetSnapshot shrunk = { .tick = 6, .entities = described, .entity_count = 1 };

    nya_net_snapshot_apply(&shrunk, FLAG_REPLICATED, map, NYA_ENTITY_HANDLE_NONE);
    nya_system_sim_apply_commands();

    nya_assert(!nya_entity_is_valid(nya_net_replica_local(map, (NYA_EntityHandle){ .index = 901, .generation = 1 })),
               "an entity the snapshot stopped mentioning was despawned and unmapped");

    nya_assert(nya_entity_is_valid(nya_net_replica_local(map, described_remote)), "and the one it still mentions survived");
  }

  nya_entity_despawn(untouched);
  nya_entity_despawn(local_only);
  nya_entity_foreach (entity) {
    if ((entity->flags & FLAG_REPLICATED) != 0) nya_entity_despawn_deferred(entity->handle);
  }
  nya_system_sim_apply_commands();
}

// TEST: the decoder refuses what a hostile peer can send
// TEST: fixed point on the wire
NYA_INTERNAL void test_fixed_point_on_the_wire(NYA_Arena* arena) {
  printf("TEST: positions cross on a power of two grid, and rotations within a fraction of a degree\n");
  NYA_NetEntityState state = { .handle = { .index = 3, .generation = 1 }, .scale = { 1.0F, 1.0F, 1.0F }, .rotation = nya_quaternion_identity };
  NYA_NetSnapshot    one   = { .tick = 5, .position_bits = 4, .entities = &state, .entity_count = 1 };

  // a sixteenth of a unit is kept exactly, a hundredth rounds to the nearest sixteenth.
  state.position          = (f32x3){ 100.0625F, -3.0F, 0.01F };
  NYA_NetSnapshot decoded = round_trip(arena, &one, nullptr, nullptr);

  nya_assert(decoded.position_bits == 4, "the precision is carried in the header");
  nya_assert(decoded.entities[0].position.x == 100.0625F && decoded.entities[0].position.y == -3.0F, "a value on the grid moved");
  nya_assert(decoded.entities[0].position.z == 0.0F, "0.01 at four bits should round to zero, got %f", (f64)decoded.entities[0].position.z);

  // decoding what was decoded gives the same bits: what lets both sides derive a baseline's integers alike.
  NYA_NetSnapshot again = round_trip(arena, &decoded, nullptr, nullptr);
  nya_assert(nya_memcmp(&again.entities[0].position, &decoded.entities[0].position, sizeof(f32x3)) == 0, "a second round trip moved a quantised position");

  // large, not finite, and absurd values are clamped rather than wrapped or trusted.
  state.position = (f32x3){ 1.0e30F, NAN, -INFINITY };
  decoded        = round_trip(arena, &one, nullptr, nullptr);
  nya_assert(isfinite(decoded.entities[0].position.x) && decoded.entities[0].position.x > 1.0e9F, "a huge position did not clamp");
  nya_assert(decoded.entities[0].position.y == 0.0F, "a NaN position did not become zero");

  // rotations: random unit quaternions, including both signs of the same rotation.
  NYA_RNG             rng     = nya_rng_create(.seed = "40747E");
  NYA_RNGDistribution uniform = { .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = -1.0, .max = 1.0 } };

  f32 worst_degrees = 0.0F;
  state.position    = (f32x3){ 0.0F, 0.0F, 0.0F };

  for (u32 i = 0; i < 2000; i++) {
    NYA_Quaternion q = { nya_rng_sample_f32(&rng, uniform), nya_rng_sample_f32(&rng, uniform), nya_rng_sample_f32(&rng, uniform), nya_rng_sample_f32(&rng, uniform) };
    if (i % 7 == 0) q = (NYA_Quaternion){ 0.0F, 0.0F, 0.70710678F, 0.70710678F }; // two equal largest components, a crate on its side

    state.rotation = nya_quaternion_normalize(q);
    decoded        = round_trip(arena, &one, nullptr, nullptr);

    NYA_Quaternion back = decoded.entities[0].rotation;
    f32 dot = fabsf((state.rotation.x * back.x) + (state.rotation.y * back.y) + (state.rotation.z * back.z) + (state.rotation.w * back.w));

    worst_degrees = nya_max(worst_degrees, 2.0F * acosf(nya_min(dot, 1.0F)) * 57.29578F);
  }

  printf("  2000 rotations in 32 bits each: worst error %.3f degrees\n", (f64)worst_degrees);
  nya_assert(worst_degrees < 0.25F, "smallest three lost %.3f degrees", (f64)worst_degrees);
}

// TEST: only what changed crosses, and removals are explicit
NYA_INTERNAL void test_only_what_changed_crosses_and_removals(NYA_Arena* arena) {
  printf("TEST: a delta names removed entities and omits unchanged ones\n");
  NYA_NetEntityState before[4] = { 0 };
  for (u32 i = 0; i < 4; i++) {
    before[i] = (NYA_NetEntityState){ .handle = { .index = 10 + (i * 3), .generation = 1 }, .position = { (f32)i * 8.0F, 0.0F, 0.0F }, .scale = { 1.0F, 1.0F, 1.0F }, .rotation = nya_quaternion_identity };
  }

  NYA_NetSnapshot baseline = { .tick = 100, .entities = before, .entity_count = 4 };
  NYA_NetSnapshot received = round_trip(arena, &baseline, nullptr, nullptr);

  // the second entity is gone, the third moved a little, and a new one sits where the first was.
  NYA_NetEntityState after[4] = { before[0], before[2], before[3], before[3] };
  after[0].handle.generation   = 2;
  after[1].position.x         += 0.5F;
  after[3].handle.index        = 40;

  NYA_NetSnapshot current = { .tick = 104, .entities = after, .entity_count = 4 };

  NYA_String* delta = nya_string_create(arena);
  NYA_EXPECT(nya_net_snapshot_encode(arena, &current, &baseline, delta));

  u64 tick          = 0;
  u64 baseline_tick = 0;
  nya_assert(nya_net_snapshot_peek(delta->items, delta->length, &tick, &baseline_tick) && tick == 104 && baseline_tick == 100, "the header does not name its baseline");

  NYA_NetSnapshot decoded = { 0 };
  NYA_EXPECT(nya_net_snapshot_decode(arena, delta->items, delta->length, &received, &decoded));

  nya_assert(decoded.entity_count == 4, "the delta decoded to %u entities", decoded.entity_count);
  for (u32 i = 0; i < 4; i++) nya_assert(states_equal(&after[i], &decoded.entities[i]), "entity %u came back different", i);

  // the moving one costs a few bytes; the unchanged one costs nothing.
  printf("  a delta with a replacement, a removal, a move and an addition: %llu bytes\n", (unsigned long long)delta->length);

  // against any baseline but the one it names, it is refused rather than misread.
  NYA_NetSnapshot wrong = received;
  wrong.tick            = 99;
  nya_assert(!nya_net_snapshot_decode(arena, delta->items, delta->length, &wrong, &decoded).ok, "a delta decoded against the wrong baseline");
  nya_assert(!nya_net_snapshot_decode(arena, delta->items, delta->length, nullptr, &decoded).ok, "a delta decoded against no baseline");

  // every truncation is refused, and so is a byte past the end.
  for (u64 prefix = 0; prefix < delta->length; prefix++) {
    nya_assert(!nya_net_snapshot_decode(arena, delta->items, prefix, &received, &decoded).ok, "a delta cut to %llu bytes was accepted", (unsigned long long)prefix);
  }

  nya_string_push_back(delta, 0);
  nya_assert(!nya_net_snapshot_decode(arena, delta->items, delta->length, &received, &decoded).ok, "trailing bytes were accepted");

  // an unchanged world is the header and two empty lists.
  NYA_NetSnapshot same = { .tick = 101, .entities = before, .entity_count = 4 };
  NYA_String*     none = nya_string_create(arena);
  NYA_EXPECT(nya_net_snapshot_encode(arena, &same, &baseline, none));
  nya_assert(none->length <= 8, "an unchanged world took %llu bytes", (unsigned long long)none->length);
}

// TEST: the decoder refuses what a hostile peer can send
NYA_INTERNAL void test_decoder_refuses_hostile_input(NYA_Arena* arena) {
  printf("TEST: the decoder rejects malformed payloads\n");
  NYA_NetSnapshot decoded = { 0 };

  nya_assert(!nya_net_snapshot_decode(arena, nullptr, 0, nullptr, &decoded).ok);

  /** tick, gap, command tick, bits, removed, then whatever the case needs. */
  #define PAYLOAD(...)                                                                                                                       \
    ({                                                                                                                                       \
      static const u8 _bytes[] = { __VA_ARGS__ };                                                                                            \
      (NYA_String){ .items = (u8*)_bytes, .length = sizeof(_bytes) };                                                                        \
    })

  NYA_String cases[] = {
    PAYLOAD(1, 2),                                            // too short for the header
    PAYLOAD(5, 0, 0, 17, 0, 0),                               // more position bits than allowed
    PAYLOAD(5, 6, 0, 6, 0, 0),                                // a baseline before tick zero
    PAYLOAD(5, 0, 0, 6, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F),     // an absurd changed count
    PAYLOAD(5, 0, 0, 6, 0, 4),                                // a count with nothing behind it
    PAYLOAD(5, 0, 0, 6, 1, 0, 0),                             // a removal with no baseline to remove from
    PAYLOAD(5, 0, 0, 6, 0, 1, 0, 0x7F),                       // a mask without the new bit against no baseline
    PAYLOAD(5, 0, 0, 6, 0, 1, 0, 0xFF, 0x03, 0),              // a new entity with generation zero
    PAYLOAD(5, 0, 0, 6, 0, 1, 0, 0x80, 0x02, 1),              // a new entity missing its fields
    PAYLOAD(5, 0, 0, 6, 0, 1, 0x80, 0x80, 0x01, 0xFF, 0x03, 1), // an index past the entity table
    PAYLOAD(5, 0, 0, 6, 0, 1, 0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01), // an unterminated varint
  };

  for (u32 i = 0; i < nya_carray_length(cases); i++) {
    nya_assert(!nya_net_snapshot_decode(arena, cases[i].items, cases[i].length, nullptr, &decoded).ok, "malformed case %u was accepted", i);
  }

  #undef PAYLOAD

  // An empty but well formed snapshot is legal: a world with nothing replicated in it.
  u8 empty[] = { 5, 0, 0, 6, 0, 0 };
  NYA_EXPECT(nya_net_snapshot_decode(arena, empty, sizeof(empty), nullptr, &decoded));
  nya_assert(decoded.entity_count == 0 && decoded.tick == 5, "an empty snapshot decodes to an empty world, not an error");
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

  test_capture_takes_only_what_is_marked(arena);
  test_full_snapshot_round_trips_every_field(arena);
  test_delta_against_identical_baseline(arena);
  test_reused_slot_is_not_a_baseline(arena);
  test_applying_snapshot_spawns_moves_and_despawns(arena);
  test_fixed_point_on_the_wire(arena);
  test_only_what_changed_crosses_and_removals(arena);
  test_decoder_refuses_hostile_input(arena);

  printf("PASSED: test_snapshot (0 failures)\n");

  return EXIT_SUCCESS;
}

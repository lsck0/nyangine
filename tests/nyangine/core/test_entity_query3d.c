/**
 * The 3D spatial queries, against a linear scan.
 *
 * The 2D radius query already has this oracle, inside the simulation harness, and the reasoning there
 * applies just as well here: the grid is an optimisation, and an optimisation that returns a different
 * answer from the obvious version is broken however fast it is.
 *
 * The 3D half had no oracle, no test and no caller. nya_entity_query_box, _box_kind, _box_flags and
 * _sphere were defined and called by nothing in the tree; so was the 2D nya_entity_query_flags. This is
 * the oracle for all five, and for nya_entity_query_ray, which had no caller either.
 *
 * Positions are spread over several grid buckets on purpose — the index is 128 units a bucket — so a
 * query that spans a boundary is the normal case here rather than an edge one.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define SPREAD  400.0F
#define PLACED  200
#define FOUND_MAX (PLACED * 2)

enum { KIND_CRATE = 3, KIND_LAMP = 4 };

/** A uniform number in [min, max], from the seeded stream. */
static f32 between(NYA_RNG* rng, f32 min, f32 max) {
  return nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = (f64)min, .max = (f64)max } });
}

/** A coordinate somewhere in the placed volume. */
static f32 spread(NYA_RNG* rng) { return between(rng, -SPREAD, SPREAD); }

#define FLAG_SOLID (1ULL << 3)
#define FLAG_LOUD  (1ULL << 4)

/** A handle is a pair, so it is compared as one. */
static b8 same_handle(NYA_EntityHandle a, NYA_EntityHandle b) { return a.index == b.index && a.generation == b.generation; }

/** Whether `handle` is somewhere in the first `count` of `found`. */
static b8 holds(const NYA_EntityHandle* found, u32 count, NYA_EntityHandle handle) {
  for (u32 i = 0; i < count; i++) {
    if (same_handle(found[i], handle)) return true;
  }

  return false;
}

/**
 * The obvious version: every live entity inside the box, counted by walking the table.
 *
 * `kind` and `flags` are filtered the way the query says it filters them, so a disagreement is the
 * index being wrong rather than the two asking different questions.
 * */
static u32 scan_box(f32x3 min, f32x3 max, b8 by_kind, u32 kind, b8 by_flags, u64 flags, OUT NYA_EntityHandle* out) {
  u32 count = 0;

  for (u32 slot = 0; slot < nya_entity_slot_count(); slot++) {
    NYA_Entity* entity = nya_entity_at_slot(slot);
    if (entity == nullptr) continue;

    const f32x3 at = entity->position;

    if (at.x < min.x || at.x > max.x) continue;
    if (at.y < min.y || at.y > max.y) continue;
    if (at.z < min.z || at.z > max.z) continue;

    if (by_kind && entity->type != kind) continue;
    if (by_flags && (entity->flags & flags) != flags) continue;

    out[count++] = entity->handle;
  }

  return count;
}

/** The same for a sphere. */
static u32 scan_sphere(f32x3 center, f32 radius, OUT NYA_EntityHandle* out) {
  u32 count = 0;

  for (u32 slot = 0; slot < nya_entity_slot_count(); slot++) {
    NYA_Entity* entity = nya_entity_at_slot(slot);
    if (entity == nullptr) continue;

    const f32x3 delta = entity->position - center;

    if ((delta.x * delta.x) + (delta.y * delta.y) + (delta.z * delta.z) > radius * radius) continue;

    out[count++] = entity->handle;
  }

  return count;
}

/**
 * The nearest entity whose centre lies within `radius` of the ray, and how far along the ray that centre
 * projects. Measured as the distance from the centre to the ray's line, by the cross product, so it does
 * not share the query's arithmetic.
 * */
static NYA_EntityHandle scan_ray(f32x3 origin, f32x3 direction, f32 radius, OUT f32* out_along) {
  const f32   length = sqrtf((direction.x * direction.x) + (direction.y * direction.y) + (direction.z * direction.z));
  const f32x3 unit   = direction / length;

  NYA_EntityHandle nearest = NYA_ENTITY_HANDLE_NONE;
  *out_along               = 0.0F;

  for (u32 slot = 0; slot < nya_entity_slot_count(); slot++) {
    NYA_Entity* entity = nya_entity_at_slot(slot);
    if (entity == nullptr) continue;

    const f32x3 to    = entity->position - origin;
    const f32   along = (to.x * unit.x) + (to.y * unit.y) + (to.z * unit.z);
    if (along < 0.0F) continue;

    const f32x3 cross = { (to.y * unit.z) - (to.z * unit.y), (to.z * unit.x) - (to.x * unit.z), (to.x * unit.y) - (to.y * unit.x) };
    const f32   off   = sqrtf((cross.x * cross.x) + (cross.y * cross.y) + (cross.z * cross.z));
    if (off > radius) continue;

    if (nearest.generation != 0 && along >= *out_along) continue;

    nearest    = entity->handle;
    *out_along = along;
  }

  return nearest;
}

/** Both answers hold the same handles, whatever order each produced them in. */
static b8 same_set(const NYA_EntityHandle* a, u32 a_count, const NYA_EntityHandle* b, u32 b_count) {
  if (a_count != b_count) return false;

  for (u32 i = 0; i < a_count; i++) {
    if (!holds(b, b_count, a[i])) return false;
  }

  return true;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);
  defer nya_system_callback_deinit();

  /*
   * Seeded rather than random: a disagreement between the index and the scan has to be reproducible,
   * and a test that spreads its entities differently every run cannot be replayed.
   */
  NYA_RNG rng = nya_rng_create(.seed = "DEFACED0DEFACED0");

  for (u32 i = 0; i < PLACED; i++) {
    const f32x3 at = {
      spread(&rng),
      spread(&rng),
      spread(&rng),
    };

    NYA_EntityHandle handle = nya_entity_spawn(.name = "placed", .position = at, .type = (i % 2) == 0 ? KIND_CRATE : KIND_LAMP);

    NYA_Entity* entity = nya_entity_get(handle);
    nya_assert(entity != nullptr);

    if ((i % 3) == 0) nya_entity_flag_enable(entity, FLAG_SOLID);
    if ((i % 5) == 0) nya_entity_flag_enable(entity, FLAG_LOUD);
  }

  nya_system_entity_grid_rebuild();

  NYA_EntityHandle found[FOUND_MAX];
  NYA_EntityHandle expected[FOUND_MAX];

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a box query agrees with the scan, over many boxes
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u32 disagreements = 0;
    u32 non_empty     = 0;

    for (u32 round = 0; round < 64; round++) {
      const f32x3 a = { spread(&rng), spread(&rng),
                        spread(&rng) };
      const f32   size = between(&rng, 1.0F, 300.0F);
      const f32x3 b    = { a.x + size, a.y + size, a.z + size };

      const u32 got  = nya_entity_query_box(a, b, found, FOUND_MAX);
      const u32 want = scan_box(a, b, false, 0, false, 0, expected);

      if (!same_set(found, got, expected, want)) disagreements++;
      if (want > 0) non_empty++;
    }

    nya_check(disagreements == 0, "the box query agrees with a scan, " FMTu32 " rounds disagreed", disagreements);

    // Otherwise the agreement above is sixty four empty answers agreeing with each other.
    nya_check(non_empty > 16, "and the boxes actually caught something, " FMTu32 " of 64 were non-empty", non_empty);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a sphere query agrees too, including at its rim
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u32 disagreements = 0;
    u32 non_empty     = 0;

    for (u32 round = 0; round < 64; round++) {
      const f32x3 center = { spread(&rng), spread(&rng),
                             spread(&rng) };
      const f32   radius = between(&rng, 1.0F, 200.0F);

      const u32 got  = nya_entity_query_sphere(center, radius, found, FOUND_MAX);
      const u32 want = scan_sphere(center, radius, expected);

      if (!same_set(found, got, expected, want)) disagreements++;
      if (want > 0) non_empty++;
    }

    nya_check(disagreements == 0, "the sphere query agrees with a scan, " FMTu32 " rounds disagreed", disagreements);
    nya_check(non_empty > 16, "and caught something, " FMTu32 " of 64 were non-empty", non_empty);

    // An entity exactly on the rim is inside: the test is `> radius_squared`, not `>=`.
    NYA_EntityHandle rim = nya_entity_spawn(.name = "rim", .position = { 1000.0F, 0.0F, 0.0F });
    nya_system_entity_grid_rebuild();

    const u32 on_rim = nya_entity_query_sphere((f32x3){ 990.0F, 0.0F, 0.0F }, 10.0F, found, FOUND_MAX);
    nya_check(holds(found, on_rim, rim), "an entity exactly on the rim is inside it");

    nya_entity_despawn(rim);
    nya_system_entity_grid_rebuild();

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the filtered forms filter, and filter the same way a scan does
  // ─────────────────────────────────────────────────────────────────────────────
  {
    const f32x3 min = { -SPREAD, -SPREAD, -SPREAD };
    const f32x3 max = { SPREAD, SPREAD, SPREAD };

    const u32 all = nya_entity_query_box(min, max, found, FOUND_MAX);
    nya_check(all == PLACED, "an everything box finds every entity, got " FMTu32 " of " FMTu32, all, (u32)PLACED);

    const u32 crates = nya_entity_query_box_kind(min, max, KIND_CRATE, found, FOUND_MAX);
    const u32 want_crates = scan_box(min, max, true, KIND_CRATE, false, 0, expected);

    nya_check(same_set(found, crates, expected, want_crates), "the kind filter agrees with a scan");
    nya_check(crates == PLACED / 2, "and keeps half of them, got " FMTu32, crates);

    const u32 solid = nya_entity_query_box_flags(min, max, FLAG_SOLID, found, FOUND_MAX);
    const u32 want_solid = scan_box(min, max, false, 0, true, FLAG_SOLID, expected);

    nya_check(same_set(found, solid, expected, want_solid), "the flag filter agrees with a scan");
    nya_check(solid > 0 && solid < all, "and is a real subset, got " FMTu32 " of " FMTu32, solid, all);

    // Every bit, not any: an entity with only one of the two must not come back.
    const u32 both = nya_entity_query_box_flags(min, max, FLAG_SOLID | FLAG_LOUD, found, FOUND_MAX);
    const u32 want_both = scan_box(min, max, false, 0, true, FLAG_SOLID | FLAG_LOUD, expected);

    nya_check(same_set(found, both, expected, want_both), "two flags together agree with a scan");
    nya_check(both < solid, "and are rarer than one of them, got " FMTu32 " against " FMTu32, both, solid);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the 2D flag query, the other one nothing called
  // ─────────────────────────────────────────────────────────────────────────────
  {
    const u32 flat = nya_entity_query_flags((f32x2){ -SPREAD, -SPREAD }, (f32x2){ SPREAD, SPREAD }, FLAG_SOLID, found, FOUND_MAX);

    // The 2D queries ignore z, so this selects a column and must find every solid entity there is.
    const u32 want = scan_box((f32x3){ -SPREAD, -SPREAD, -SPREAD }, (f32x3){ SPREAD, SPREAD, SPREAD }, false, 0, true, FLAG_SOLID, expected);

    nya_check(flat == want, "the 2D flag query ignores z and finds them all, got " FMTu32 " against " FMTu32, flat, want);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a ray picks the nearest entity it passes, as a scan would
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u32 disagreements = 0;
    u32 hits          = 0;

    const u32 placed = nya_entity_query_box((f32x3){ -SPREAD, -SPREAD, -SPREAD }, (f32x3){ SPREAD, SPREAD, SPREAD }, found, FOUND_MAX);
    nya_assert(placed == PLACED);

    for (u32 round = 0; round < 64; round++) {
      // aimed through a placed entity from outside the volume, so most rays cross several on the way.
      NYA_Entity* target = nya_entity_get(found[(u32)between(&rng, 0.0F, (f32)(placed - 1))]);
      nya_assert(target != nullptr);

      const f32x3 origin    = { spread(&rng), spread(&rng), SPREAD * 2.0F };
      const f32x3 direction = target->position - origin;
      const f32   radius    = between(&rng, 5.0F, 60.0F);

      f32                    distance      = 0.0F;
      f32                    want_distance = 0.0F;
      const NYA_EntityHandle got           = nya_entity_query_ray(origin, direction, radius, &distance);
      const NYA_EntityHandle want          = scan_ray(origin, direction, radius, &want_distance);

      if (!same_handle(got, want) || fabsf(distance - want_distance) > 0.01F) disagreements++;
      if (nya_entity_is_valid(got)) hits++;

      // the length of the direction is not the reach of the ray, only its heading.
      f32 scaled_distance = 0.0F;
      if (!same_handle(nya_entity_query_ray(origin, direction * 0.001F, radius, &scaled_distance), got)) disagreements++;
    }

    nya_check(disagreements == 0, "the ray query agrees with a scan, " FMTu32 " rounds disagreed", disagreements);

    // every ray was aimed through an entity, so every one must hit something.
    nya_check(hits == 64, "and every aimed ray hit, " FMTu32 " of 64 did", hits);

    /*
     * Far from the placed volume: three in a row on the x axis, one behind the origin. The front one of
     * the stack is the one picked, and what is behind the ray is never seen.
     */
    NYA_EntityHandle behind = nya_entity_spawn(.name = "behind", .position = { 2990.0F, 0.0F, 0.0F });
    NYA_EntityHandle front  = nya_entity_spawn(.name = "front", .position = { 3010.0F, 0.0F, 0.0F });
    NYA_EntityHandle back   = nya_entity_spawn(.name = "back", .position = { 3030.0F, 0.0F, 0.0F });
    NYA_EntityHandle aside  = nya_entity_spawn(.name = "aside", .position = { 3020.0F, 5.0F, 0.0F });

    f32 distance = 0.0F;

    nya_check(same_handle(nya_entity_query_ray((f32x3){ 3000.0F, 0, 0 }, (f32x3){ 1.0F, 0, 0 }, 1.0F, &distance), front),
              "the nearest of a stack is picked");
    nya_check(fabsf(distance - 10.0F) < 0.001F, "at its distance along the ray, got %f", (f64)distance);

    // a wider ray reaches the one five units off the axis, but the front one is still nearer.
    nya_check(same_handle(nya_entity_query_ray((f32x3){ 3000.0F, 0, 0 }, (f32x3){ 1.0F, 0, 0 }, 6.0F, &distance), front),
              "a wider ray still picks the nearest");

    // turned around, the ray finds the entity that was behind it and nothing of the stack.
    nya_check(same_handle(nya_entity_query_ray((f32x3){ 3000.0F, 0, 0 }, (f32x3){ -1.0F, 0, 0 }, 1.0F, &distance), behind),
              "the other way is the one behind");

    // no ray, and no width, is no hit.
    nya_check(!nya_entity_is_valid(nya_entity_query_ray((f32x3){ 3000.0F, 0, 0 }, (f32x3){ 0, 0, 0 }, 1.0F, &distance)),
              "a zero direction hits nothing");
    nya_check(distance == 0.0F, "and reports no distance");
    nya_check(!nya_entity_is_valid(nya_entity_query_ray((f32x3){ 3000.0F, 0, 0 }, (f32x3){ 1.0F, 0, 0 }, 0.0F, &distance)),
              "nor does a zero radius");

    nya_entity_despawn(behind);
    nya_entity_despawn(front);
    nya_entity_despawn(back);
    nya_entity_despawn(aside);
    nya_system_entity_grid_rebuild();

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a capacity smaller than the answer truncates rather than overruns
  // ─────────────────────────────────────────────────────────────────────────────
  {
    const f32x3 min = { -SPREAD, -SPREAD, -SPREAD };
    const f32x3 max = { SPREAD, SPREAD, SPREAD };

    NYA_EntityHandle few[8];
    const u32        got = nya_entity_query_box(min, max, few, nya_carray_length(few));

    nya_check(got <= nya_carray_length(few), "a small buffer is not overrun, got " FMTu32, got);
    nya_check(nya_entity_query_box(min, max, few, 0) == 0, "and a capacity of zero finds nothing");

    printf("  PASSED\n");
  }

  nya_log_info("PASSED: test_entity_query3d");

  return nya_check_failures() == 0 ? 0 : 1;
}

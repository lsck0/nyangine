/**
 * Regression test for nya_hset_intersection removing while iterating (base_hset.h).
 *
 * The macro walks dest's slots from 0 upward and calls nya_hset_remove on any item the source does
 * not hold. nya_hset_remove is a backward shift deletion: after clearing a slot it walks the rest of
 * the probe chain and reinserts each entry at the first free slot from its own hash, which is often
 * a *lower* index than where it was. An entry moved below the cursor is never looked at again, so it
 * survives an intersection it should not be in.
 *
 * The set below holds nine u32 keys in a table of sixteen and is intersected with the empty set, so
 * the correct answer is unambiguous: nothing survives. One item does.
 *
 * The other three operations had the same hazard in a form that only shows when the two arguments
 * are the *same set*: they iterate the source, which is fine until the source is also the thing
 * being mutated. `a \ a` and `a △ a` walk a table nya_hset_remove is shifting; `a ∪ a` is a no-op by
 * definition but can still trip nya_hset_insert's load factor check, and the rehash that follows
 * frees the `items` and `occupied` the loop is reading.
 *
 * test_hset.c already calls all four aliased forms and passes, but with two items in a table of
 * sixty four — too sparse for anything to collide, shift or resize. The aliased section here uses
 * the same nine-in-sixteen density as the case above, which is what makes the difference visible.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

nya_derive_hset(u32);

/*
 * Nine keys in a table of sixteen, chosen so the fnv1a probe chains overlap. Any set that collides
 * will do; this is simply one that reproduces it.
 */
static const u32 LEFT[]   = { 1, 10, 31, 8, 5, 20, 17, 16, 3 };
static const u32 RIGHT[]  = { 10, 16, 99 };
static const u32 SHARED[] = { 10, 16 };

/** A capacity-16 set holding `values`, which is the shape every case below starts from. */
static NYA_HSetᐸu32ᐳ* set_of(NYA_Arena* arena, const u32* values, u64 count) {
  NYA_HSetᐸu32ᐳ* set = nya_hset_create_with_capacity(arena, u32, 16);
  for (u64 i = 0; i < count; i++) nya_hset_insert(set, values[i]);
  return set;
}

/** The set's contents as a comma separated list, for a failure message worth reading. */
static void collect(NYA_HSetᐸu32ᐳ* set, NYA_String* out) {
  nya_string_clear(out);
  nya_hset_foreach (set, item) nya_string_extend_sprintf(out, "%u,", item);
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena*  arena = nya_arena_create(.name = "test_bug_hset_intersection");
  NYA_String* seen  = nya_string_create(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: intersecting with the empty set empties the destination
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: intersection with the empty set\n");
  {
    NYA_HSetᐸu32ᐳ* set   = set_of(arena, LEFT, nya_carray_length(LEFT));
    NYA_HSetᐸu32ᐳ* empty = set_of(arena, nullptr, 0);

    nya_assert(set->length == nya_carray_length(LEFT));
    nya_hset_intersection(set, empty);

    collect(set, seen);
    nya_assert(set->length == 0, "intersection with the empty set left " FMTu64 " item(s): " NYA_FMT_STRING, set->length, NYA_FMT_STRING_ARG(seen));

    nya_hset_destroy(set);
    nya_hset_destroy(empty);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: intersection keeps exactly the shared items
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: intersection keeps exactly the shared items\n");
  {
    NYA_HSetᐸu32ᐳ* set   = set_of(arena, LEFT, nya_carray_length(LEFT));
    NYA_HSetᐸu32ᐳ* other = set_of(arena, RIGHT, nya_carray_length(RIGHT));

    nya_hset_intersection(set, other);

    collect(set, seen);
    nya_assert(
      set->length == nya_carray_length(SHARED),
      "intersection has " FMTu64 " item(s), expected %zu: " NYA_FMT_STRING,
      set->length,
      nya_carray_length(SHARED),
      NYA_FMT_STRING_ARG(seen)
    );
    for (u64 i = 0; i < nya_carray_length(SHARED); i++) nya_assert(nya_hset_contains(set, SHARED[i]), "intersection dropped %u", SHARED[i]);

    nya_hset_destroy(set);
    nya_hset_destroy(other);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the operations that iterate the source, on two distinct sets
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: union, difference and symmetric difference\n");
  {
    NYA_HSetᐸu32ᐳ* other = set_of(arena, RIGHT, nya_carray_length(RIGHT));

    NYA_HSetᐸu32ᐳ* united = set_of(arena, LEFT, nya_carray_length(LEFT));
    nya_hset_union(united, other);
    nya_assert(united->length == 10, "union has " FMTu64 " item(s), expected 10", united->length);

    NYA_HSetᐸu32ᐳ* differenced = set_of(arena, LEFT, nya_carray_length(LEFT));
    nya_hset_difference(differenced, other);
    nya_assert(differenced->length == 7, "difference has " FMTu64 " item(s), expected 7", differenced->length);

    NYA_HSetᐸu32ᐳ* symmetric = set_of(arena, LEFT, nya_carray_length(LEFT));
    nya_hset_symmetric_difference(symmetric, other);
    nya_assert(symmetric->length == 8, "symmetric difference has " FMTu64 " item(s), expected 8", symmetric->length);

    nya_hset_destroy(united);
    nya_hset_destroy(differenced);
    nya_hset_destroy(symmetric);
    nya_hset_destroy(other);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: every operation called with the same set on both sides
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: aliased set operations\n");
  {
    // A ∩ A = A
    NYA_HSetᐸu32ᐳ* self_intersect = set_of(arena, LEFT, nya_carray_length(LEFT));
    nya_hset_intersection(self_intersect, self_intersect);
    collect(self_intersect, seen);
    nya_assert(
      self_intersect->length == nya_carray_length(LEFT),
      "A ∩ A has " FMTu64 " item(s), expected %zu: " NYA_FMT_STRING,
      self_intersect->length,
      nya_carray_length(LEFT),
      NYA_FMT_STRING_ARG(seen)
    );
    for (u64 i = 0; i < nya_carray_length(LEFT); i++) nya_assert(nya_hset_contains(self_intersect, LEFT[i]), "A ∩ A dropped %u", LEFT[i]);

    // A ∪ A = A
    NYA_HSetᐸu32ᐳ* self_union = set_of(arena, LEFT, nya_carray_length(LEFT));
    nya_hset_union(self_union, self_union);
    collect(self_union, seen);
    nya_assert(
      self_union->length == nya_carray_length(LEFT),
      "A ∪ A has " FMTu64 " item(s), expected %zu: " NYA_FMT_STRING,
      self_union->length,
      nya_carray_length(LEFT),
      NYA_FMT_STRING_ARG(seen)
    );

    // A \ A = ∅
    NYA_HSetᐸu32ᐳ* self_difference = set_of(arena, LEFT, nya_carray_length(LEFT));
    nya_hset_difference(self_difference, self_difference);
    collect(self_difference, seen);
    nya_assert(self_difference->length == 0, "A \\ A left " FMTu64 " item(s): " NYA_FMT_STRING, self_difference->length, NYA_FMT_STRING_ARG(seen));

    // A △ A = ∅
    NYA_HSetᐸu32ᐳ* self_symmetric = set_of(arena, LEFT, nya_carray_length(LEFT));
    nya_hset_symmetric_difference(self_symmetric, self_symmetric);
    collect(self_symmetric, seen);
    nya_assert(self_symmetric->length == 0, "A △ A left " FMTu64 " item(s): " NYA_FMT_STRING, self_symmetric->length, NYA_FMT_STRING_ARG(seen));

    nya_hset_destroy(self_intersect);
    nya_hset_destroy(self_union);
    nya_hset_destroy(self_difference);
    nya_hset_destroy(self_symmetric);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an aliased union right at the load factor, which is what resizes mid walk
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: aliased union at the load factor\n");
  {
    // Filled to one below the threshold so the first insert nya_hset_union attempts triggers
    // nya_hset_resize_and_rehash — which frees the items and occupied arrays being iterated.
    NYA_HSetᐸu32ᐳ* set       = nya_hset_create_with_capacity(arena, u32, 16);
    u64            threshold = (u64)((f32)set->capacity * _NYA_HASHSET_LOAD_FACTOR);
    for (u32 i = 0; i < (u32)threshold; i++) nya_hset_insert(set, i * 7 + 1);

    u64 length_before = set->length;
    nya_hset_union(set, set);

    nya_assert(set->length == length_before, "A ∪ A changed the length from " FMTu64 " to " FMTu64, length_before, set->length);
    for (u32 i = 0; i < (u32)threshold; i++) nya_assert(nya_hset_contains(set, i * 7 + 1), "A ∪ A dropped %u", i * 7 + 1);

    nya_hset_destroy(set);
  }
  printf("  PASSED\n");

  nya_string_destroy(seen);
  nya_arena_destroy(arena);

  printf("PASSED: test_bug_hset_intersection\n");
  return 0;
}

/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

nya_derive_hmap(u32, u64);
nya_derive_hmap(s32, s32);

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_hmap");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: basic hashmap creation
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* map = nya_hmap_create(arena, u32, u64);
  nya_assert(map->length == 0);
  nya_assert(map->capacity == _NYA_HASHMAP_DEFAULT_CAPACITY);
  nya_assert(map->keys != nullptr);
  nya_assert(map->values != nullptr);
  nya_assert(map->occupied != nullptr);
  nya_assert(map->arena == arena);
  nya_hmap_destroy(map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: creation with custom capacity
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* map_cap = nya_hmap_create_with_capacity(arena, u32, u64, 128);
  nya_assert(map_cap->length == 0);
  nya_assert(map_cap->capacity == 128);
  nya_hmap_destroy(map_cap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hmap_set and nya_hmap_get
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* insert_map = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(insert_map, 1U, 100UL);
  nya_assert(insert_map->length == 1);
  u64* val = nya_hmap_get(insert_map, 1U);
  nya_assert(val != nullptr);
  nya_assert(*val == 100UL);

  nya_hmap_set(insert_map, 2U, 200UL);
  nya_hmap_set(insert_map, 3U, 300UL);
  nya_assert(insert_map->length == 3);
  nya_assert(*nya_hmap_get(insert_map, 2U) == 200UL);
  nya_assert(*nya_hmap_get(insert_map, 3U) == 300UL);
  nya_hmap_destroy(insert_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hmap_contains
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* contains_map = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(contains_map, 10U, 1000UL);
  nya_hmap_set(contains_map, 20U, 2000UL);
  nya_assert(nya_hmap_contains(contains_map, 10U) == true);
  nya_assert(nya_hmap_contains(contains_map, 20U) == true);
  nya_assert(nya_hmap_contains(contains_map, 30U) == false);
  nya_assert(nya_hmap_contains(contains_map, 0U) == false);
  nya_hmap_destroy(contains_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hmap_get returns nullptr for missing keys
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* get_map = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(get_map, 5U, 500UL);
  nya_assert(nya_hmap_get(get_map, 5U) != nullptr);
  nya_assert(nya_hmap_get(get_map, 99U) == nullptr);
  nya_hmap_destroy(get_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hmap_remove
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* remove_map = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(remove_map, 1U, 10UL);
  nya_hmap_set(remove_map, 2U, 20UL);
  nya_hmap_set(remove_map, 3U, 30UL);
  nya_assert(remove_map->length == 3);

  nya_hmap_remove(remove_map, 2U);
  nya_assert(remove_map->length == 2);
  nya_assert(nya_hmap_contains(remove_map, 1U) == true);
  nya_assert(nya_hmap_contains(remove_map, 2U) == false);
  nya_assert(nya_hmap_contains(remove_map, 3U) == true);
  nya_assert(*nya_hmap_get(remove_map, 1U) == 10UL);
  nya_assert(*nya_hmap_get(remove_map, 3U) == 30UL);
  nya_hmap_destroy(remove_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hmap_remove first and last
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* remove_first_last = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(remove_first_last, 100U, 1UL);
  nya_hmap_set(remove_first_last, 200U, 2UL);
  nya_hmap_set(remove_first_last, 300U, 3UL);

  nya_hmap_remove(remove_first_last, 100U);
  nya_assert(remove_first_last->length == 2);
  nya_assert(nya_hmap_contains(remove_first_last, 100U) == false);

  nya_hmap_remove(remove_first_last, 300U);
  nya_assert(remove_first_last->length == 1);
  nya_assert(nya_hmap_contains(remove_first_last, 300U) == false);
  nya_assert(nya_hmap_contains(remove_first_last, 200U) == true);
  nya_hmap_destroy(remove_first_last);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hmap_clear
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* clear_map = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(clear_map, 1U, 1UL);
  nya_hmap_set(clear_map, 2U, 2UL);
  nya_hmap_set(clear_map, 3U, 3UL);
  nya_assert(clear_map->length == 3);

  nya_hmap_clear(clear_map);
  nya_assert(clear_map->length == 0);
  nya_assert(nya_hmap_contains(clear_map, 1U) == false);
  nya_assert(nya_hmap_contains(clear_map, 2U) == false);
  nya_assert(nya_hmap_contains(clear_map, 3U) == false);
  nya_assert(clear_map->capacity == _NYA_HASHMAP_DEFAULT_CAPACITY);
  nya_hmap_destroy(clear_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: automatic resize on load factor exceeded
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* resize_map = nya_hmap_create_with_capacity(arena, u32, u64, 8);
  nya_assert(resize_map->capacity == 8);
  for (u32 i = 0; i < 10; ++i) { nya_hmap_set(resize_map, i, (u64)(i * 100)); }
  nya_assert(resize_map->length == 10);
  nya_assert(resize_map->capacity >= 10);
  for (u32 i = 0; i < 10; ++i) {
    nya_assert(nya_hmap_contains(resize_map, i) == true);
    nya_assert(*nya_hmap_get(resize_map, i) == (u64)(i * 100));
  }
  nya_hmap_destroy(resize_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: many insertions and removals
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* stress_map = nya_hmap_create(arena, u32, u64);
  for (u32 i = 0; i < 100; ++i) { nya_hmap_set(stress_map, i, (u64)(i * 10)); }
  nya_assert(stress_map->length == 100);

  for (u32 i = 0; i < 50; ++i) { nya_hmap_remove(stress_map, i * 2); }
  nya_assert(stress_map->length == 50);

  for (u32 i = 0; i < 50; ++i) {
    nya_assert(nya_hmap_contains(stress_map, i * 2) == false);
    nya_assert(nya_hmap_contains(stress_map, i * 2 + 1) == true);
    nya_assert(*nya_hmap_get(stress_map, i * 2 + 1) == (u64)((i * 2 + 1) * 10));
  }
  nya_hmap_destroy(stress_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hmap_copy
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* orig_map = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(orig_map, 1U, 100UL);
  nya_hmap_set(orig_map, 2U, 200UL);
  nya_hmap_set(orig_map, 3U, 300UL);

  NYA_HMapᐸu32ˏu64ᐳ  copy_val = nya_hmap_copy(orig_map);
  NYA_HMapᐸu32ˏu64ᐳ* copy_map = &copy_val;
  nya_assert(copy_map->length == orig_map->length);
  nya_assert(copy_map->capacity == orig_map->capacity);
  nya_assert(*nya_hmap_get(copy_map, 1U) == 100UL);
  nya_assert(*nya_hmap_get(copy_map, 2U) == 200UL);
  nya_assert(*nya_hmap_get(copy_map, 3U) == 300UL);

  nya_hmap_remove(orig_map, 1U);
  nya_assert(nya_hmap_contains(orig_map, 1U) == false);
  nya_assert(nya_hmap_contains(copy_map, 1U) == true);
  nya_hmap_destroy(orig_map);
  // Note: copy_map points to a stack-allocated copy, so no destroy needed

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hmap_move
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Arena*    arena2   = nya_arena_create(.name = "test_hmap_move");
  NYA_HMapᐸu32ˏu64ᐳ* move_map = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(move_map, 10U, 1000UL);
  nya_hmap_set(move_map, 20U, 2000UL);

  nya_hmap_move(move_map, arena2);
  nya_assert(move_map->arena == arena2);
  nya_assert(move_map->length == 2);
  nya_assert(*nya_hmap_get(move_map, 10U) == 1000UL);
  nya_assert(*nya_hmap_get(move_map, 20U) == 2000UL);
  nya_hmap_destroy(move_map);
  nya_arena_destroy(arena2);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: manual iteration over keys and values
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* iter_map = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(iter_map, 1U, 100UL);
  nya_hmap_set(iter_map, 2U, 200UL);
  nya_hmap_set(iter_map, 3U, 300UL);

  u64 key_sum = 0;
  u64 val_sum = 0;
  for (u64 i = 0; i < iter_map->capacity; i++) {
    if (iter_map->occupied[i]) {
      key_sum += iter_map->keys[i];
      val_sum += iter_map->values[i];
    }
  }
  nya_assert(key_sum == 6);
  nya_assert(val_sum == 600);
  nya_hmap_destroy(iter_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: signed integer keys
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸs32ˏs32ᐳ* signed_map = nya_hmap_create(arena, s32, s32);
  nya_hmap_set(signed_map, -5, 50);
  nya_hmap_set(signed_map, 0, 0);
  nya_hmap_set(signed_map, 5, -50);
  nya_assert(signed_map->length == 3);
  nya_assert(*nya_hmap_get(signed_map, -5) == 50);
  nya_assert(*nya_hmap_get(signed_map, 0) == 0);
  nya_assert(*nya_hmap_get(signed_map, 5) == -50);
  nya_hmap_destroy(signed_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: remove non-existent key (should not crash)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* remove_nonexist = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(remove_nonexist, 1U, 10UL);
  nya_hmap_remove(remove_nonexist, 999U);
  nya_assert(remove_nonexist->length == 1);
  nya_assert(nya_hmap_contains(remove_nonexist, 1U) == true);
  nya_hmap_destroy(remove_nonexist);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: empty hashmap operations
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* empty_map = nya_hmap_create(arena, u32, u64);
  nya_assert(empty_map->length == 0);
  nya_assert(nya_hmap_contains(empty_map, 1U) == false);
  nya_assert(nya_hmap_get(empty_map, 1U) == nullptr);
  nya_hmap_remove(empty_map, 1U);
  nya_assert(empty_map->length == 0);
  nya_hmap_destroy(empty_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: destroy resets hashmap state
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* destroy_map = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(destroy_map, 1U, 1UL);
  nya_hmap_set(destroy_map, 2U, 2UL);
  nya_hmap_destroy(destroy_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: insert updates existing key value
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* update_map = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(update_map, 1U, 100UL);
  nya_assert(*nya_hmap_get(update_map, 1U) == 100UL);
  nya_hmap_set(update_map, 1U, 999UL);
  nya_assert(update_map->length == 1); // should still be 1, not 2
  nya_assert(*nya_hmap_get(update_map, 1U) == 999UL);
  nya_hmap_destroy(update_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: zero key value
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* zero_map = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(zero_map, 0U, 0UL);
  nya_assert(zero_map->length == 1);
  nya_assert(nya_hmap_contains(zero_map, 0U) == true);
  nya_assert(*nya_hmap_get(zero_map, 0U) == 0UL);
  nya_hmap_remove(zero_map, 0U);
  nya_assert(zero_map->length == 0);
  nya_assert(nya_hmap_contains(zero_map, 0U) == false);
  nya_hmap_destroy(zero_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: many keys that could cause collisions (same hash bucket)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* collision_map = nya_hmap_create_with_capacity(arena, u32, u64, 8);
  // Insert keys that will wrap around in a small capacity
  for (u32 i = 0; i < 20; ++i) { nya_hmap_set(collision_map, i, (u64)(i * 100)); }
  nya_assert(collision_map->length == 20);
  for (u32 i = 0; i < 20; ++i) {
    nya_assert(nya_hmap_contains(collision_map, i) == true);
    nya_assert(*nya_hmap_get(collision_map, i) == (u64)(i * 100));
  }
  // Remove some and verify others still accessible
  for (u32 i = 0; i < 10; ++i) { nya_hmap_remove(collision_map, i); }
  nya_assert(collision_map->length == 10);
  for (u32 i = 10; i < 20; ++i) {
    nya_assert(nya_hmap_contains(collision_map, i) == true);
    nya_assert(*nya_hmap_get(collision_map, i) == (u64)(i * 100));
  }
  nya_hmap_destroy(collision_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: clear then reuse
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* reuse_map = nya_hmap_create(arena, u32, u64);
  nya_hmap_set(reuse_map, 1U, 1UL);
  nya_hmap_set(reuse_map, 2U, 2UL);
  nya_hmap_clear(reuse_map);
  nya_assert(reuse_map->length == 0);
  // Insert same keys again
  nya_hmap_set(reuse_map, 1U, 10UL);
  nya_hmap_set(reuse_map, 2U, 20UL);
  nya_assert(reuse_map->length == 2);
  nya_assert(*nya_hmap_get(reuse_map, 1U) == 10UL);
  nya_assert(*nya_hmap_get(reuse_map, 2U) == 20UL);
  nya_hmap_destroy(reuse_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: remove all items one by one
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HMapᐸu32ˏu64ᐳ* remove_all_map = nya_hmap_create(arena, u32, u64);
  for (u32 i = 0; i < 10; ++i) { nya_hmap_set(remove_all_map, i, (u64)i); }
  for (u32 i = 0; i < 10; ++i) {
    nya_hmap_remove(remove_all_map, i);
    nya_assert(remove_all_map->length == 9 - i);
  }
  nya_assert(remove_all_map->length == 0);
  // Should be reusable
  nya_hmap_set(remove_all_map, 42U, 42UL);
  nya_assert(remove_all_map->length == 1);
  nya_hmap_destroy(remove_all_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: stress test - random insert/remove/get
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_RNG       rng           = nya_rng_create();
    NYA_HMapᐸu32ˏu64ᐳ* stress        = nya_hmap_create(arena, u32, u64);
    u64           expected[256] = { 0 };
    b8            present[256]  = { 0 };

    NYA_RNGDistribution key_dist = {
      .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
      .uniform = { .min = 0, .max = 255 }
    };
    NYA_RNGDistribution val_dist = {
      .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
      .uniform = { .min = 0, .max = 10000 }
    };
    NYA_RNGDistribution op_dist = {
      .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
      .uniform = { .min = 0, .max = 99 }
    };

    for (u32 iter = 0; iter < 10000; ++iter) {
      u32 op  = nya_rng_sample_u32(&rng, op_dist);
      u32 key = nya_rng_sample_u32(&rng, key_dist);

      if (op < 60) {
        // insert (updates if key exists)
        u64 val = nya_rng_sample_u64(&rng, val_dist);
        nya_hmap_set(stress, key, val);
        expected[key] = val;
        present[key]  = true;
      } else if (op < 80) {
        // remove
        nya_hmap_remove(stress, key);
        present[key] = false;
      } else {
        // get
        if (present[key]) {
          u64* val = nya_hmap_get(stress, key);
          nya_assert(val != nullptr);
          nya_assert(*val == expected[key]);
        } else {
          nya_assert(nya_hmap_get(stress, key) == nullptr);
        }
      }
    }

    // Verify final state
    u64 count = 0;
    for (u32 i = 0; i < 256; ++i) {
      if (present[i]) {
        count++;
        nya_assert(nya_hmap_contains(stress, i) == true);
        nya_assert(*nya_hmap_get(stress, i) == expected[i]);
      } else {
        nya_assert(nya_hmap_contains(stress, i) == false);
      }
    }
    nya_assert(stress->length == count);

    nya_hmap_destroy(stress);
  }

  nya_arena_destroy(arena);
  return 0;
}

/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

nya_derive_dict(u64);
nya_derive_dict(s32);

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_dict");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: basic dict creation
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* map = nya_dict_create(arena, u64);
  nya_assert(map->length == 0);
  nya_assert(map->capacity == _NYA_HASHMAP_DEFAULT_CAPACITY);
  nya_assert(map->keys != nullptr);
  nya_assert(map->values != nullptr);
  nya_assert(map->occupied != nullptr);
  nya_assert(map->arena == arena);
  nya_dict_destroy(map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: creation with custom capacity
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* map_cap = nya_dict_create_with_capacity(arena, u64, 128);
  nya_assert(map_cap->length == 0);
  nya_assert(map_cap->capacity == 128);
  nya_dict_destroy(map_cap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_dict_set and nya_dict_get
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* insert_map = nya_dict_create(arena, u64);
  nya_dict_set(insert_map, "alpha", 100UL);
  nya_assert(insert_map->length == 1);
  u64* val = nya_dict_get(insert_map, "alpha");
  nya_assert(val != nullptr);
  nya_assert(*val == 100UL);

  nya_dict_set(insert_map, "beta", 200UL);
  nya_dict_set(insert_map, "gamma", 300UL);
  nya_assert(insert_map->length == 3);
  nya_assert(*nya_dict_get(insert_map, "beta") == 200UL);
  nya_assert(*nya_dict_get(insert_map, "gamma") == 300UL);
  nya_dict_destroy(insert_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_dict_contains
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* contains_map = nya_dict_create(arena, u64);
  nya_dict_set(contains_map, "foo", 1000UL);
  nya_dict_set(contains_map, "bar", 2000UL);
  nya_assert(nya_dict_contains(contains_map, "foo") == true);
  nya_assert(nya_dict_contains(contains_map, "bar") == true);
  nya_assert(nya_dict_contains(contains_map, "baz") == false);
  nya_assert(nya_dict_contains(contains_map, "qux") == false);
  nya_dict_destroy(contains_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_dict_get returns nullptr for missing keys
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* get_map = nya_dict_create(arena, u64);
  nya_dict_set(get_map, "exists", 500UL);
  nya_assert(nya_dict_get(get_map, "exists") != nullptr);
  nya_assert(nya_dict_get(get_map, "missing") == nullptr);
  nya_dict_destroy(get_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_dict_remove
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* remove_map = nya_dict_create(arena, u64);
  nya_dict_set(remove_map, "a", 10UL);
  nya_dict_set(remove_map, "b", 20UL);
  nya_dict_set(remove_map, "c", 30UL);
  nya_assert(remove_map->length == 3);

  nya_dict_remove(remove_map, "b");
  nya_assert(remove_map->length == 2);
  nya_assert(nya_dict_contains(remove_map, "a") == true);
  nya_assert(nya_dict_contains(remove_map, "b") == false);
  nya_assert(nya_dict_contains(remove_map, "c") == true);
  nya_assert(*nya_dict_get(remove_map, "a") == 10UL);
  nya_assert(*nya_dict_get(remove_map, "c") == 30UL);
  nya_dict_destroy(remove_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_dict_remove first and last
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* remove_first_last = nya_dict_create(arena, u64);
  nya_dict_set(remove_first_last, "first", 1UL);
  nya_dict_set(remove_first_last, "middle", 2UL);
  nya_dict_set(remove_first_last, "last", 3UL);

  nya_dict_remove(remove_first_last, "first");
  nya_assert(remove_first_last->length == 2);
  nya_assert(nya_dict_contains(remove_first_last, "first") == false);

  nya_dict_remove(remove_first_last, "last");
  nya_assert(remove_first_last->length == 1);
  nya_assert(nya_dict_contains(remove_first_last, "last") == false);
  nya_assert(nya_dict_contains(remove_first_last, "middle") == true);
  nya_dict_destroy(remove_first_last);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_dict_clear
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* clear_map = nya_dict_create(arena, u64);
  nya_dict_set(clear_map, "x", 1UL);
  nya_dict_set(clear_map, "y", 2UL);
  nya_dict_set(clear_map, "z", 3UL);
  nya_assert(clear_map->length == 3);

  nya_dict_clear(clear_map);
  nya_assert(clear_map->length == 0);
  nya_assert(nya_dict_contains(clear_map, "x") == false);
  nya_assert(nya_dict_contains(clear_map, "y") == false);
  nya_assert(nya_dict_contains(clear_map, "z") == false);
  nya_assert(clear_map->capacity == _NYA_HASHMAP_DEFAULT_CAPACITY);
  nya_dict_destroy(clear_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: automatic resize on load factor exceeded
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* resize_map = nya_dict_create_with_capacity(arena, u64, 8);
  nya_assert(resize_map->capacity == 8);
  char* resize_keys[] = { "k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7", "k8", "k9" };
  for (u32 i = 0; i < 10; ++i) { nya_dict_set(resize_map, resize_keys[i], (u64)(i * 100)); }
  nya_assert(resize_map->length == 10);
  nya_assert(resize_map->capacity >= 10);
  for (u32 i = 0; i < 10; ++i) {
    nya_assert(nya_dict_contains(resize_map, resize_keys[i]) == true);
    nya_assert(*nya_dict_get(resize_map, resize_keys[i]) == (u64)(i * 100));
  }
  nya_dict_destroy(resize_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: insert updates existing key value
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* update_map = nya_dict_create(arena, u64);
  nya_dict_set(update_map, "key", 100UL);
  nya_assert(*nya_dict_get(update_map, "key") == 100UL);
  nya_dict_set(update_map, "key", 999UL);
  nya_assert(update_map->length == 1);
  nya_assert(*nya_dict_get(update_map, "key") == 999UL);
  nya_dict_destroy(update_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: remove non-existent key (should not crash)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* remove_nonexist = nya_dict_create(arena, u64);
  nya_dict_set(remove_nonexist, "present", 10UL);
  nya_dict_remove(remove_nonexist, "absent");
  nya_assert(remove_nonexist->length == 1);
  nya_assert(nya_dict_contains(remove_nonexist, "present") == true);
  nya_dict_destroy(remove_nonexist);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: empty dict operations
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* empty_map = nya_dict_create(arena, u64);
  nya_assert(empty_map->length == 0);
  nya_assert(nya_dict_contains(empty_map, "any") == false);
  nya_assert(nya_dict_get(empty_map, "any") == nullptr);
  nya_dict_remove(empty_map, "any");
  nya_assert(empty_map->length == 0);
  nya_dict_destroy(empty_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: clear then reuse
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* reuse_map = nya_dict_create(arena, u64);
  nya_dict_set(reuse_map, "a", 1UL);
  nya_dict_set(reuse_map, "b", 2UL);
  nya_dict_clear(reuse_map);
  nya_assert(reuse_map->length == 0);
  nya_dict_set(reuse_map, "a", 10UL);
  nya_dict_set(reuse_map, "b", 20UL);
  nya_assert(reuse_map->length == 2);
  nya_assert(*nya_dict_get(reuse_map, "a") == 10UL);
  nya_assert(*nya_dict_get(reuse_map, "b") == 20UL);
  nya_dict_destroy(reuse_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: remove all items one by one
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* remove_all = nya_dict_create(arena, u64);
  char* ra_keys[] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9" };
  for (u32 i = 0; i < 10; ++i) { nya_dict_set(remove_all, ra_keys[i], (u64)i); }
  for (u32 i = 0; i < 10; ++i) {
    nya_dict_remove(remove_all, ra_keys[i]);
    nya_assert(remove_all->length == 9 - i);
  }
  nya_assert(remove_all->length == 0);
  nya_dict_set(remove_all, "reuse", 42UL);
  nya_assert(remove_all->length == 1);
  nya_dict_destroy(remove_all);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: keys with same prefix are distinct
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* prefix_map = nya_dict_create(arena, u64);
  nya_dict_set(prefix_map, "key", 1UL);
  nya_dict_set(prefix_map, "key1", 2UL);
  nya_dict_set(prefix_map, "key12", 3UL);
  nya_assert(prefix_map->length == 3);
  nya_assert(*nya_dict_get(prefix_map, "key") == 1UL);
  nya_assert(*nya_dict_get(prefix_map, "key1") == 2UL);
  nya_assert(*nya_dict_get(prefix_map, "key12") == 3UL);
  nya_dict_destroy(prefix_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: foreach iteration
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Dictᐸu64ᐳ* iter_map = nya_dict_create(arena, u64);
  nya_dict_set(iter_map, "a", 100UL);
  nya_dict_set(iter_map, "b", 200UL);
  nya_dict_set(iter_map, "c", 300UL);

  u64 val_sum = 0;
  u64 count   = 0;
  nya_dict_foreach_value(iter_map, v) {
    val_sum += *v;
    count++;
  }
  nya_assert(count == 3);
  nya_assert(val_sum == 600);
  nya_dict_destroy(iter_map);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: many insertions and removals
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena*            tmp_arena = nya_arena_create(.name = "test_dict_stress");
    NYA_Dictᐸu64ᐳ* stress    = nya_dict_create(tmp_arena, u64);

    char key_buf[100][16];
    for (u32 i = 0; i < 100; ++i) {
      snprintf(key_buf[i], sizeof(key_buf[i]), "s%u", i);
      nya_dict_set(stress, key_buf[i], (u64)(i * 10));
    }
    nya_assert(stress->length == 100);

    for (u32 i = 0; i < 50; ++i) { nya_dict_remove(stress, key_buf[i * 2]); }
    nya_assert(stress->length == 50);

    for (u32 i = 0; i < 50; ++i) {
      nya_assert(nya_dict_contains(stress, key_buf[i * 2]) == false);
      nya_assert(nya_dict_contains(stress, key_buf[i * 2 + 1]) == true);
      nya_assert(*nya_dict_get(stress, key_buf[i * 2 + 1]) == (u64)((i * 2 + 1) * 10));
    }
    nya_dict_destroy(stress);
    nya_arena_destroy(tmp_arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: on stack creation
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Dictᐸu64ᐳ stack_dict = nya_dict_create_on_stack(arena, u64);
    nya_dict_set(&stack_dict, "x", 42UL);
    nya_assert(stack_dict.length == 1);
    nya_assert(*nya_dict_get(&stack_dict, "x") == 42UL);
    nya_dict_destroy_on_stack(stack_dict);
  }

  nya_arena_destroy(arena);
  return 0;
}

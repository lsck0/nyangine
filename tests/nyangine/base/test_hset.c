/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

nya_derive_hset(u32);
nya_derive_hset(s32);

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_hset");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: basic hashset creation
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* set = nya_hset_create(arena, u32);
  nya_assert(set->length == 0);
  nya_assert(set->capacity == _NYA_HASHSET_DEFAULT_CAPACITY);
  nya_assert(set->items != nullptr);
  nya_assert(set->occupied != nullptr);
  nya_assert(set->arena == arena);
  nya_hset_destroy(set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: creation with custom capacity
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* set_cap = nya_hset_create_with_capacity(arena, u32, 128);
  nya_assert(set_cap->length == 0);
  nya_assert(set_cap->capacity == 128);
  nya_hset_destroy(set_cap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hset_insert and nya_hset_contains
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* insert_set = nya_hset_create(arena, u32);
  nya_hset_insert(insert_set, 10U);
  nya_assert(insert_set->length == 1);
  nya_assert(nya_hset_contains(insert_set, 10U) == true);

  nya_hset_insert(insert_set, 20U);
  nya_hset_insert(insert_set, 30U);
  nya_assert(insert_set->length == 3);
  nya_assert(nya_hset_contains(insert_set, 10U) == true);
  nya_assert(nya_hset_contains(insert_set, 20U) == true);
  nya_assert(nya_hset_contains(insert_set, 30U) == true);
  nya_hset_destroy(insert_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hset_contains returns false for missing items
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* contains_set = nya_hset_create(arena, u32);
  nya_hset_insert(contains_set, 5U);
  nya_assert(nya_hset_contains(contains_set, 5U) == true);
  nya_assert(nya_hset_contains(contains_set, 99U) == false);
  nya_assert(nya_hset_contains(contains_set, 0U) == false);
  nya_hset_destroy(contains_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hset_remove
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* remove_set = nya_hset_create(arena, u32);
  nya_hset_insert(remove_set, 1U);
  nya_hset_insert(remove_set, 2U);
  nya_hset_insert(remove_set, 3U);
  nya_assert(remove_set->length == 3);

  nya_hset_remove(remove_set, 2U);
  nya_assert(remove_set->length == 2);
  nya_assert(nya_hset_contains(remove_set, 1U) == true);
  nya_assert(nya_hset_contains(remove_set, 2U) == false);
  nya_assert(nya_hset_contains(remove_set, 3U) == true);
  nya_hset_destroy(remove_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hset_remove first and last
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* remove_first_last = nya_hset_create(arena, u32);
  nya_hset_insert(remove_first_last, 100U);
  nya_hset_insert(remove_first_last, 200U);
  nya_hset_insert(remove_first_last, 300U);

  nya_hset_remove(remove_first_last, 100U);
  nya_assert(remove_first_last->length == 2);
  nya_assert(nya_hset_contains(remove_first_last, 100U) == false);

  nya_hset_remove(remove_first_last, 300U);
  nya_assert(remove_first_last->length == 1);
  nya_assert(nya_hset_contains(remove_first_last, 300U) == false);
  nya_assert(nya_hset_contains(remove_first_last, 200U) == true);
  nya_hset_destroy(remove_first_last);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hset_clear
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* clear_set = nya_hset_create(arena, u32);
  nya_hset_insert(clear_set, 1U);
  nya_hset_insert(clear_set, 2U);
  nya_hset_insert(clear_set, 3U);
  nya_assert(clear_set->length == 3);

  nya_hset_clear(clear_set);
  nya_assert(clear_set->length == 0);
  nya_assert(nya_hset_contains(clear_set, 1U) == false);
  nya_assert(nya_hset_contains(clear_set, 2U) == false);
  nya_assert(nya_hset_contains(clear_set, 3U) == false);
  nya_assert(clear_set->capacity == _NYA_HASHSET_DEFAULT_CAPACITY);
  nya_hset_destroy(clear_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: automatic resize on load factor exceeded
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* resize_set = nya_hset_create_with_capacity(arena, u32, 8);
  nya_assert(resize_set->capacity == 8);
  for (u32 i = 0; i < 10; ++i) { nya_hset_insert(resize_set, i * 10); }
  nya_assert(resize_set->length == 10);
  nya_assert(resize_set->capacity >= 10);
  for (u32 i = 0; i < 10; ++i) { nya_assert(nya_hset_contains(resize_set, i * 10) == true); }
  nya_hset_destroy(resize_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: many insertions and removals
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* stress_set = nya_hset_create(arena, u32);
  for (u32 i = 0; i < 100; ++i) { nya_hset_insert(stress_set, i); }
  nya_assert(stress_set->length == 100);

  for (u32 i = 0; i < 50; ++i) { nya_hset_remove(stress_set, i * 2); }
  nya_assert(stress_set->length == 50);

  for (u32 i = 0; i < 50; ++i) {
    nya_assert(nya_hset_contains(stress_set, i * 2) == false);
    nya_assert(nya_hset_contains(stress_set, i * 2 + 1) == true);
  }
  nya_hset_destroy(stress_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hset_copy
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* orig_set = nya_hset_create(arena, u32);
  nya_hset_insert(orig_set, 1U);
  nya_hset_insert(orig_set, 2U);
  nya_hset_insert(orig_set, 3U);

  NYA_HSetᐸu32ᐳ* copy_set = nya_arena_alloc(arena, sizeof(NYA_HSetᐸu32ᐳ));
  *copy_set         = nya_hset_copy(orig_set);
  nya_assert(copy_set->length == orig_set->length);
  nya_assert(copy_set->capacity == orig_set->capacity);
  nya_assert(nya_hset_contains(copy_set, 1U) == true);
  nya_assert(nya_hset_contains(copy_set, 2U) == true);
  nya_assert(nya_hset_contains(copy_set, 3U) == true);

  nya_hset_remove(orig_set, 1U);
  nya_assert(nya_hset_contains(orig_set, 1U) == false);
  nya_assert(nya_hset_contains(copy_set, 1U) == true);
  nya_hset_destroy(orig_set);
  nya_hset_destroy(copy_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hset_move
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Arena* arena2   = nya_arena_create(.name = "test_hset_move");
  NYA_HSetᐸu32ᐳ*   move_set = nya_hset_create(arena, u32);
  nya_hset_insert(move_set, 10U);
  nya_hset_insert(move_set, 20U);

  nya_hset_move(move_set, arena2);
  nya_assert(move_set->arena == arena2);
  nya_assert(move_set->length == 2);
  nya_assert(nya_hset_contains(move_set, 10U) == true);
  nya_assert(nya_hset_contains(move_set, 20U) == true);
  nya_hset_destroy(move_set);
  nya_arena_destroy(arena2);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: manual iteration over items
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* iter_set = nya_hset_create(arena, u32);
  nya_hset_insert(iter_set, 1U);
  nya_hset_insert(iter_set, 2U);
  nya_hset_insert(iter_set, 3U);

  u64 item_sum = 0;
  for (u64 i = 0; i < iter_set->capacity; i++) {
    if (iter_set->occupied[i]) { item_sum += iter_set->items[i]; }
  }
  nya_assert(item_sum == 6);
  nya_hset_destroy(iter_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: signed integer items
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸs32ᐳ* signed_set = nya_hset_create(arena, s32);
  nya_hset_insert(signed_set, -5);
  nya_hset_insert(signed_set, 0);
  nya_hset_insert(signed_set, 5);
  nya_assert(signed_set->length == 3);
  nya_assert(nya_hset_contains(signed_set, -5) == true);
  nya_assert(nya_hset_contains(signed_set, 0) == true);
  nya_assert(nya_hset_contains(signed_set, 5) == true);
  nya_assert(nya_hset_contains(signed_set, -10) == false);
  nya_hset_destroy(signed_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: remove non-existent item (should not crash)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* remove_nonexist = nya_hset_create(arena, u32);
  nya_hset_insert(remove_nonexist, 1U);
  nya_hset_remove(remove_nonexist, 999U);
  nya_assert(remove_nonexist->length == 1);
  nya_assert(nya_hset_contains(remove_nonexist, 1U) == true);
  nya_hset_destroy(remove_nonexist);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: empty hashset operations
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* empty_set = nya_hset_create(arena, u32);
  nya_assert(empty_set->length == 0);
  nya_assert(nya_hset_contains(empty_set, 1U) == false);
  nya_hset_remove(empty_set, 1U);
  nya_assert(empty_set->length == 0);
  nya_hset_destroy(empty_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: duplicate insertion (set behavior - should deduplicate)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* dup_set = nya_hset_create(arena, u32);
  nya_hset_insert(dup_set, 42U);
  nya_hset_insert(dup_set, 42U);
  nya_hset_insert(dup_set, 42U);
  nya_assert(dup_set->length == 1);
  nya_assert(nya_hset_contains(dup_set, 42U) == true);
  nya_hset_destroy(dup_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: destroy resets hashset state
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* destroy_set = nya_hset_create(arena, u32);
  nya_hset_insert(destroy_set, 1U);
  nya_hset_insert(destroy_set, 2U);
  nya_hset_destroy(destroy_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hset_union
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* union_a = nya_hset_create(arena, u32);
  NYA_HSetᐸu32ᐳ* union_b = nya_hset_create(arena, u32);
  nya_hset_insert(union_a, 1U);
  nya_hset_insert(union_a, 2U);
  nya_hset_insert(union_a, 3U);
  nya_hset_insert(union_b, 3U);
  nya_hset_insert(union_b, 4U);
  nya_hset_insert(union_b, 5U);
  nya_hset_union(union_a, union_b);
  nya_assert(union_a->length == 5);
  nya_assert(nya_hset_contains(union_a, 1U) == true);
  nya_assert(nya_hset_contains(union_a, 2U) == true);
  nya_assert(nya_hset_contains(union_a, 3U) == true);
  nya_assert(nya_hset_contains(union_a, 4U) == true);
  nya_assert(nya_hset_contains(union_a, 5U) == true);
  nya_hset_destroy(union_a);
  nya_hset_destroy(union_b);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hset_intersection
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* inter_a = nya_hset_create(arena, u32);
  NYA_HSetᐸu32ᐳ* inter_b = nya_hset_create(arena, u32);
  nya_hset_insert(inter_a, 1U);
  nya_hset_insert(inter_a, 2U);
  nya_hset_insert(inter_a, 3U);
  nya_hset_insert(inter_a, 4U);
  nya_hset_insert(inter_b, 2U);
  nya_hset_insert(inter_b, 4U);
  nya_hset_insert(inter_b, 6U);
  nya_hset_intersection(inter_a, inter_b);
  nya_assert(inter_a->length == 2);
  nya_assert(nya_hset_contains(inter_a, 1U) == false);
  nya_assert(nya_hset_contains(inter_a, 2U) == true);
  nya_assert(nya_hset_contains(inter_a, 3U) == false);
  nya_assert(nya_hset_contains(inter_a, 4U) == true);
  nya_hset_destroy(inter_a);
  nya_hset_destroy(inter_b);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hset_difference
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* diff_a = nya_hset_create(arena, u32);
  NYA_HSetᐸu32ᐳ* diff_b = nya_hset_create(arena, u32);
  nya_hset_insert(diff_a, 1U);
  nya_hset_insert(diff_a, 2U);
  nya_hset_insert(diff_a, 3U);
  nya_hset_insert(diff_a, 4U);
  nya_hset_insert(diff_b, 2U);
  nya_hset_insert(diff_b, 4U);
  nya_hset_difference(diff_a, diff_b);
  nya_assert(diff_a->length == 2);
  nya_assert(nya_hset_contains(diff_a, 1U) == true);
  nya_assert(nya_hset_contains(diff_a, 2U) == false);
  nya_assert(nya_hset_contains(diff_a, 3U) == true);
  nya_assert(nya_hset_contains(diff_a, 4U) == false);
  nya_hset_destroy(diff_a);
  nya_hset_destroy(diff_b);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hset_symmetric_difference
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* sym_a = nya_hset_create(arena, u32);
  NYA_HSetᐸu32ᐳ* sym_b = nya_hset_create(arena, u32);
  nya_hset_insert(sym_a, 1U);
  nya_hset_insert(sym_a, 2U);
  nya_hset_insert(sym_a, 3U);
  nya_hset_insert(sym_b, 2U);
  nya_hset_insert(sym_b, 3U);
  nya_hset_insert(sym_b, 4U);
  nya_hset_symmetric_difference(sym_a, sym_b);
  nya_assert(sym_a->length == 2);
  nya_assert(nya_hset_contains(sym_a, 1U) == true);
  nya_assert(nya_hset_contains(sym_a, 2U) == false);
  nya_assert(nya_hset_contains(sym_a, 3U) == false);
  nya_assert(nya_hset_contains(sym_a, 4U) == true);
  nya_hset_destroy(sym_a);
  nya_hset_destroy(sym_b);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: set operations with empty sets
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* empty_a = nya_hset_create(arena, u32);
  NYA_HSetᐸu32ᐳ* empty_b = nya_hset_create(arena, u32);
  nya_hset_insert(empty_a, 1U);
  nya_hset_insert(empty_a, 2U);
  nya_hset_union(empty_a, empty_b);
  nya_assert(empty_a->length == 2);
  nya_hset_intersection(empty_a, empty_b);
  nya_assert(empty_a->length == 0);
  nya_hset_destroy(empty_a);
  nya_hset_destroy(empty_b);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: set operations with identical sets
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* ident_a = nya_hset_create(arena, u32);
  NYA_HSetᐸu32ᐳ* ident_b = nya_hset_create(arena, u32);
  nya_hset_insert(ident_a, 1U);
  nya_hset_insert(ident_a, 2U);
  nya_hset_insert(ident_b, 1U);
  nya_hset_insert(ident_b, 2U);
  nya_hset_symmetric_difference(ident_a, ident_b);
  nya_assert(ident_a->length == 0);
  nya_hset_destroy(ident_a);
  nya_hset_destroy(ident_b);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: zero value element
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* zero_set = nya_hset_create(arena, u32);
  nya_hset_insert(zero_set, 0U);
  nya_assert(zero_set->length == 1);
  nya_assert(nya_hset_contains(zero_set, 0U) == true);
  nya_hset_remove(zero_set, 0U);
  nya_assert(zero_set->length == 0);
  nya_assert(nya_hset_contains(zero_set, 0U) == false);
  nya_hset_destroy(zero_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: duplicate insert is idempotent
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* dup_set2 = nya_hset_create(arena, u32);
  nya_hset_insert(dup_set2, 42U);
  nya_assert(dup_set2->length == 1);
  nya_hset_insert(dup_set2, 42U);
  nya_assert(dup_set2->length == 1); // should still be 1
  nya_hset_insert(dup_set2, 42U);
  nya_assert(dup_set2->length == 1);
  nya_hset_destroy(dup_set2);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: set operations with self - union with self
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* self_union = nya_hset_create(arena, u32);
  nya_hset_insert(self_union, 1U);
  nya_hset_insert(self_union, 2U);
  nya_hset_union(self_union, self_union); // A ∪ A = A
  nya_assert(self_union->length == 2);
  nya_assert(nya_hset_contains(self_union, 1U) == true);
  nya_assert(nya_hset_contains(self_union, 2U) == true);
  nya_hset_destroy(self_union);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: set operations with self - intersection with self
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* self_inter = nya_hset_create(arena, u32);
  nya_hset_insert(self_inter, 1U);
  nya_hset_insert(self_inter, 2U);
  nya_hset_intersection(self_inter, self_inter); // A ∩ A = A
  nya_assert(self_inter->length == 2);
  nya_assert(nya_hset_contains(self_inter, 1U) == true);
  nya_assert(nya_hset_contains(self_inter, 2U) == true);
  nya_hset_destroy(self_inter);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: set operations with self - difference with self
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* self_diff = nya_hset_create(arena, u32);
  nya_hset_insert(self_diff, 1U);
  nya_hset_insert(self_diff, 2U);
  nya_hset_difference(self_diff, self_diff); // A \ A = ∅
  nya_assert(self_diff->length == 0);
  nya_hset_destroy(self_diff);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: set operations with self - symmetric difference with self
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* self_sym = nya_hset_create(arena, u32);
  nya_hset_insert(self_sym, 1U);
  nya_hset_insert(self_sym, 2U);
  nya_hset_symmetric_difference(self_sym, self_sym); // A △ A = ∅
  nya_assert(self_sym->length == 0);
  nya_hset_destroy(self_sym);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: collision handling in small capacity
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* small_set = nya_hset_create_with_capacity(arena, u32, 4);
  for (u32 i = 0; i < 20; ++i) { nya_hset_insert(small_set, i); }
  nya_assert(small_set->length == 20);
  for (u32 i = 0; i < 20; ++i) { nya_assert(nya_hset_contains(small_set, i) == true); }
  // Remove and check
  for (u32 i = 0; i < 10; ++i) { nya_hset_remove(small_set, i); }
  nya_assert(small_set->length == 10);
  for (u32 i = 10; i < 20; ++i) { nya_assert(nya_hset_contains(small_set, i) == true); }
  nya_hset_destroy(small_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: clear and reuse
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HSetᐸu32ᐳ* reuse_set = nya_hset_create(arena, u32);
  nya_hset_insert(reuse_set, 1U);
  nya_hset_insert(reuse_set, 2U);
  nya_hset_clear(reuse_set);
  nya_assert(reuse_set->length == 0);
  nya_hset_insert(reuse_set, 3U);
  nya_hset_insert(reuse_set, 4U);
  nya_assert(reuse_set->length == 2);
  nya_assert(nya_hset_contains(reuse_set, 1U) == false);
  nya_assert(nya_hset_contains(reuse_set, 3U) == true);
  nya_hset_destroy(reuse_set);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: stress test - random insert/remove/contains
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_RNG  rng          = nya_rng_create();
    NYA_HSetᐸu32ᐳ* stress       = nya_hset_create(arena, u32);
    b8       present[256] = { 0 };

    NYA_RNGDistribution key_dist = {
      .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
      .uniform = { .min = 0, .max = 255 }
    };
    NYA_RNGDistribution op_dist = {
      .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
      .uniform = { .min = 0, .max = 99 }
    };

    for (u32 iter = 0; iter < 10000; ++iter) {
      u32 op  = nya_rng_sample_u32(&rng, op_dist);
      u32 key = nya_rng_sample_u32(&rng, key_dist);

      if (op < 60) {
        nya_hset_insert(stress, key);
        present[key] = true;
      } else if (op < 80) {
        nya_hset_remove(stress, key);
        present[key] = false;
      } else {
        nya_assert(nya_hset_contains(stress, key) == present[key]);
      }
    }

    // Verify final state
    u64 count = 0;
    for (u32 i = 0; i < 256; ++i) {
      if (present[i]) {
        count++;
        nya_assert(nya_hset_contains(stress, i) == true);
      } else {
        nya_assert(nya_hset_contains(stress, i) == false);
      }
    }
    nya_assert(stress->length == count);

    nya_hset_destroy(stress);
  }

  nya_arena_destroy(arena);
  return 0;
}

/**
 * @file test_bug_destroy_on_stack.c
 *
 * The on-stack destructors have to leave a container that is genuinely empty, not one that merely
 * lost its pointers.
 *
 * base_heap.h had already been fixed for this, and its comment says why: clearing only the pointer
 * left the heap "claiming to own a block it no longer had, so a second destroy handed the arena a
 * null pointer with a non-zero size, and a push onto the destroyed heap saw length < capacity and
 * wrote through null rather than reallocating". The same defect was still in two of its siblings.
 *
 * - nya_hmap_destroy_on_stack nulled keys, values and occupied but left length and capacity, so a
 *   set afterwards skipped the resize and indexed `occupied` through null. UBSan reports it as
 *   "applying non-zero offset to null pointer".
 * - nya_array_destroy_on_stack zeroed length and capacity but left `items` dangling. nya_array_resize
 *   picks alloc-vs-realloc by testing `items == nullptr`, so the next push reallocated a block the
 *   arena had already reclaimed. That one usually appeared to work, because the free list hands the
 *   same block straight back — which is the worst way for it to behave.
 *
 * Reusing a destroyed container is not something callers should do, but "destroy then destroy again"
 * happens whenever a cleanup path runs twice, and that has to be safe.
 *
 * Note the argument conventions differ: heap and hmap take a value, array, ring and hset take a
 * pointer. That inconsistency is deliberate-by-now and documented in base_heap.h.
 * */

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

nya_derive_hmap(u32, u32);
nya_derive_hset(u32);

static s32 u32_compare(const u32* a, const u32* b) {
    return (*a < *b) ? -1 : (*a > *b);
}
nya_derive_heap(u32);

s32 main(void) {
    NYA_Arena* arena = nya_arena_create();
    defer      nya_arena_destroy(arena);

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: array
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: nya_array_destroy_on_stack\n");
    {
        NYA_Arrayᐸu32ᐳ array = nya_array_create_on_stack(arena, u32);
        nya_array_add(&array, 11U);
        nya_array_destroy_on_stack(&array);

        nya_check(array.items == nullptr, "items was left dangling rather than nulled");
        nya_check(array.length == 0, "length is " FMTu64 ", expected 0", array.length);
        nya_check(array.capacity == 0, "capacity is " FMTu64 ", expected 0", array.capacity);

        // Destroying twice must not hand the arena a pointer with a stale size.
        nya_array_destroy_on_stack(&array);

        // And the array must be reusable, taking the first-allocation path rather than reallocating
        // a block the arena has already reclaimed.
        nya_array_add(&array, 22U);
        nya_check(array.length == 1, "length is " FMTu64 " after reuse, expected 1", array.length);
        nya_check(*nya_array_get(&array, 0) == 22U, "reused array holds the wrong value");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: hmap
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: nya_hmap_destroy_on_stack\n");
    {
        NYA_HMapᐸu32ˏu32ᐳ map = nya_hmap_create_on_stack(arena, u32, u32);
        nya_hmap_set(&map, 1U, 2U);
        nya_hmap_destroy_on_stack(map);

        nya_check(map.keys == nullptr, "keys was not nulled");
        nya_check(map.length == 0, "length is " FMTu64 " after destroy, expected 0", map.length);
        nya_check(map.capacity == 0, "capacity is " FMTu64 " after destroy, expected 0", map.capacity);

        nya_hmap_destroy_on_stack(map);

        // The set below is what used to dereference null: capacity said 64, so no resize happened.
        nya_hmap_set(&map, 3U, 4U);
        nya_check(map.length == 1, "length is " FMTu64 " after reuse, expected 1", map.length);

        u32* found = nya_hmap_get(&map, 3U);
        nya_check(found != nullptr && *found == 4U, "reused map lost its entry");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: hset and heap, which were already correct
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: nya_hset_destroy_on_stack and nya_heap_destroy_on_stack\n");
    {
        NYA_HSetᐸu32ᐳ set = nya_hset_create_on_stack(arena, u32);
        nya_hset_insert(&set, 5U);
        nya_hset_destroy_on_stack(&set);

        nya_check(set.length == 0, "hset length is " FMTu64 " after destroy", set.length);
        nya_check(set.capacity == 0, "hset capacity is " FMTu64 " after destroy", set.capacity);

        NYA_Heapᐸu32ᐳ heap = nya_heap_create_on_stack(arena, u32, &u32_compare);
        nya_heap_push(&heap, 5U);
        nya_heap_destroy_on_stack(heap);

        nya_check(heap.length == 0, "heap length is " FMTu64 " after destroy", heap.length);
        nya_check(heap.capacity == 0, "heap capacity is " FMTu64 " after destroy", heap.capacity);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}

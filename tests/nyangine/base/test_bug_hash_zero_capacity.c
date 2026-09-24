/**
 * @file test_bug_hash_zero_capacity.c
 * */

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

nya_derive_hmap(u32, u32);
nya_derive_dict(u32);
nya_derive_hset(u32);

s32 main(void) {
    NYA_Arena* arena = nya_arena_create();
    defer      nya_arena_destroy(arena);

    // TEST: reading a zero capacity container reports empty rather than dividing
    //
    // Lookups take `hash(key) % capacity` before the probe loop, whose own bound would not run, so a get
    // on a never written container must not divide by zero.
    printf("TEST: reading a zero capacity container\n");
    {
        NYA_HMapᐸu32ˏu32ᐳ* map  = nya_hmap_create_with_capacity(arena, u32, u32, 0);
        NYA_Dictᐸu32ᐳ*     dict = nya_dict_create_with_capacity(arena, u32, 0);
        NYA_HSetᐸu32ᐳ*     set  = nya_hset_create_with_capacity(arena, u32, 0);

        nya_check(nya_hmap_get(map, 1U) == nullptr, "hmap get returned a value from an empty map");
        nya_check(!nya_hmap_contains(map, 1U), "hmap claimed to contain a key");
        nya_check(nya_dict_get(dict, "nobody") == nullptr, "dict get returned a value from an empty dict");
        nya_check(!nya_dict_contains(dict, "nobody"), "dict claimed to contain a key");
        nya_check(!nya_hset_contains(set, 1U), "hset claimed to contain an item");

        // Removing from an empty container is a no-op, not a fault.
        nya_hmap_remove(map, 1U);
        nya_dict_remove(dict, "nobody");
        nya_hset_remove(set, 1U);

        nya_check(map->length == 0, "hmap length is " FMTu64 " after removing from empty", map->length);
        nya_check(dict->length == 0, "dict length is " FMTu64 " after removing from empty", dict->length);
        nya_check(set->length == 0, "hset length is " FMTu64 " after removing from empty", set->length);
    }

    // TEST: a zero capacity hmap grows on first set
    printf("TEST: zero capacity hmap\n");
    {
        NYA_HMapᐸu32ˏu32ᐳ* map = nya_hmap_create_with_capacity(arena, u32, u32, 0);
        nya_check(map->capacity == 0, "expected a capacity of 0 to start, got " FMTu64, map->capacity);

        nya_hmap_add(map, 7U, 42U);

        nya_check(map->capacity > 0, "capacity is still " FMTu64 " after a set", map->capacity);
        nya_check(map->length == 1, "length is " FMTu64 ", expected 1", map->length);

        u32* found = nya_hmap_get(map, 7U);
        nya_check(found != nullptr, "key 7 was not found after being set");
        if (found != nullptr) nya_check(*found == 42U, "key 7 gave " FMTu32 ", expected 42", *found);

        // Still a working table afterwards, not merely one that survived the first write.
        for (u32 i = 0; i < 200U; i++) nya_hmap_add(map, i, i * 3U);
        nya_check(map->length == 200, "length is " FMTu64 " after 200 distinct keys, expected 200", map->length);

        u32* late = nya_hmap_get(map, 199U);
        nya_check(late != nullptr && *late == 597U, "key 199 did not survive the growth");
    }

    // TEST: a zero capacity dict grows on first set
    printf("TEST: zero capacity dict\n");
    {
        NYA_Dictᐸu32ᐳ* dict = nya_dict_create_with_capacity(arena, u32, 0);
        nya_check(dict->capacity == 0, "expected a capacity of 0 to start, got " FMTu64, dict->capacity);

        nya_dict_add(dict, "alice", 1U);

        nya_check(dict->capacity > 0, "capacity is still " FMTu64 " after a set", dict->capacity);
        nya_check(dict->length == 1, "length is " FMTu64 ", expected 1", dict->length);

        u32* found = nya_dict_get(dict, "alice");
        nya_check(found != nullptr, "key \"alice\" was not found after being set");
        if (found != nullptr) nya_check(*found == 1U, "key \"alice\" gave " FMTu32 ", expected 1", *found);
    }

    // TEST: a zero capacity hset grows on first insert
    printf("TEST: zero capacity hset\n");
    {
        NYA_HSetᐸu32ᐳ* set = nya_hset_create_with_capacity(arena, u32, 0);
        nya_check(set->capacity == 0, "expected a capacity of 0 to start, got " FMTu64, set->capacity);

        nya_hset_add(set, 9U);

        nya_check(set->capacity > 0, "capacity is still " FMTu64 " after an insert", set->capacity);
        nya_check(set->length == 1, "length is " FMTu64 ", expected 1", set->length);
        nya_check(nya_hset_contains(set, 9U), "9 was not present after being inserted");

        // A duplicate must not grow the set a second time.
        nya_hset_add(set, 9U);
        nya_check(set->length == 1, "length is " FMTu64 " after inserting 9 twice, expected 1", set->length);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}

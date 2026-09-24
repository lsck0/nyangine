/**
 * The hash map: the container the event table, the input state and the asset index are all built on.
 *
 * bench_core already measures the raw hash functions and the asset memo that sits in front of one
 * dictionary. This measures the generic map itself — the get, the insert and the string-keyed
 * lookup — because a byte-keyed hmap and a string-keyed dict are the two shapes every other table in
 * the engine is an instance of, and a probe or a rehash regression would show here first.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

nya_derive_hmap(u64, u64);
nya_derive_dict(u64);

/** Enough entries to be past the caches an eight-entry table would fit in, still a realistic table. */
#define KEYS 4096U

/** Scattered rather than sequential, so the probe sequences are not one tidy run. */
static u64 keys[KEYS];

/* The string keys the dict is measured on: the asset-handle shape, one directory deep. */
static char strings[KEYS][24];

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_containers");
    defer      nya_arena_destroy(arena);

    for (u32 i = 0; i < KEYS; i++) {
        keys[i] = (u64)(i + 1) * 0x9E3779B97F4A7C15ULL;
        (void)snprintf(strings[i], sizeof(strings[i]), "assets/tex/%u.png", i);
    }

    // Sized once to hold every key below the load factor, so add is a pure insert and get is a pure probe — neither is really measuring the rehash, which has its own line.
    NYA_HMapᐸu64ˏu64ᐳ* map = nya_hmap_create_with_capacity(arena, u64, u64, KEYS * 2);
    for (u32 i = 0; i < KEYS; i++) nya_hmap_add(map, keys[i], (u64)i);

    nya_bench_begin("hash map, u64 keys (4096 entries)");

    // The lookup the engine does far more of than anything else: every key present, every probe a hit.
    nya_bench("get, all present", KEYS, {
        u64 found = 0;
        for (u32 i = 0; i < KEYS; i++) {
            u64* value  = nya_hmap_get(map, keys[i]);
            found      += value != nullptr ? *value : 0;
        }
        nya_bench_keep(found);
    });

    // A miss walks the probe chain to an empty slot rather than stopping at a match, so it is the other half of the story a "does this exist" check pays.
    nya_bench("get, all absent", KEYS, {
        u32 misses = 0;
        for (u32 i = 0; i < KEYS; i++) misses += nya_hmap_get(map, ~keys[i]) == nullptr ? 1U : 0U;
        nya_bench_keep(misses);
    });

    // Filling a table from empty into space it already has: the insert cost with the rehash factored out, since the clear keeps the capacity.
    nya_bench("clear + insert all", KEYS, {
        nya_hmap_clear(map);
        for (u32 i = 0; i < KEYS; i++) nya_hmap_add(map, keys[i], (u64)i);
        nya_bench_keep(map->length);
    });

    if (nya_bench_end() != 0) return 1;

    // The string-keyed twin: hash the bytes and strcmp on a collision, which is the config lookup and the asset-handle lookup both.
    NYA_Dictᐸu64ᐳ* dict = nya_dict_create_with_capacity(arena, u64, KEYS * 2);
    for (u32 i = 0; i < KEYS; i++) nya_dict_add(dict, strings[i], (u64)i);

    nya_bench_begin("string dict (4096 entries)");

    nya_bench("get, all present", KEYS, {
        u64 found = 0;
        for (u32 i = 0; i < KEYS; i++) {
            u64* value  = nya_dict_get(dict, strings[i]);
            found      += value != nullptr ? *value : 0;
        }
        nya_bench_keep(found);
    });

    return nya_bench_end();
}

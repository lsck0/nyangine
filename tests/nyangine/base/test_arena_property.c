/**
 * The arena allocator as laws rather than examples: every pointer it hands out is aligned as the arena
 * was asked for, two live allocations never share a byte, the bytes written into one allocation are the
 * bytes read back out of it however the allocator has churned around it, and free_all returns the arena
 * to empty so the next allocation starts from nothing.
 *
 * The example cases — a grow-and-shrink realloc, a region larger than the region size, garbage
 * collection freeing an unused region — live in test_arena.c and test_arena_freelist.c. This is the
 * part a fixed script cannot reach: a random braid of alloc, free and free_all, the shape a real
 * subsystem puts an arena through, with an oracle that fails the instant the allocator overlaps two
 * live blocks or corrupts one while servicing another from its free list. The sanitizers are the oracle
 * for a read or a write off the end; the range check below is the oracle for two blocks meeting.
 **/

// A larger entropy budget than the default 1024 so a long op sequence — up to two hundred allocations,
// each with its own drawn size — spends real bytes on every draw rather than the zeroes a draw past the
// end reads. Set before the engine is included, which is also what makes this file its own unity build.
#define NYA_PROPERTY_ENTROPY_MAX 4096

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Cases per law. Thousands, so a rare interleaving of frees and reuses has room to turn up. */
#define CASES 3000

/** Fixed, so the suite is the same run every time. "arena" and "prop" folded into eight ASCII bytes. */
#define SEED 0x6172656E61707270ULL

/** The most allocations alive at once. A small region and this cap keep both the free-list and the multi-region paths busy. */
#define LIVE_MAX 64

/** The largest single allocation a law asks for. A few of these overflow the region and chain a new one. */
#define ALLOC_BYTES_MAX 1024

/** A region small enough that a handful of allocations spill past it, which is what exercises the region chain. */
#define REGION_BYTES 4096

/* A LIVE ALLOCATION AND ITS FILL */

/** One allocation the law is holding, with the id its fill was written from. */
typedef struct {
    u8* ptr;
    u64 size;
    u32 id;
} Live;

/** The byte an allocation with `id` carries at `offset`. Distinct ids fill distinctly, so a clobber shows. */
static u8 fill_byte(u32 id, u64 offset) {
    return (u8)(id * 131u + offset * 17u + 7u);
}

/** Writes the whole allocation with its fill, so a later read can prove nothing else wrote over it. */
static void paint(Live* live) {
    for (u64 offset = 0; offset < live->size; offset++) live->ptr[offset] = fill_byte(live->id, offset);
}

/** Whether every byte of the allocation is still the fill it was painted with. */
static b8 intact(const Live* live) {
    for (u64 offset = 0; offset < live->size; offset++) {
        if (live->ptr[offset] != fill_byte(live->id, offset)) return false;
    }
    return true;
}

/** Whether the ranges [a, a+a_size) and [b, b+b_size) share any byte. */
static b8 ranges_overlap(const u8* a, u64 a_size, const u8* b, u64 b_size) { return a < b + b_size && b < a + a_size; }

/**
 * A power-of-two alignment the arena takes: 8, 16, 32 or 64. Eight is the arena's floor, and the
 * sanitizer build pads each allocation by sixty-four bytes and asks that the alignment divide it, so
 * sixty-four is the ceiling here.
 * */
static u8 draw_alignment(NYA_Property* property) { return (u8)(8u << nya_property_draw_below(property, 4)); }

/* LAWS */

/** Every pointer the arena returns is a multiple of the alignment it was created with, and a zero-size ask is null. */
static b8 law_pointer_is_aligned(NYA_Property* property) {
    u8         alignment = draw_alignment(property);
    NYA_Arena* arena     = nya_arena_create(.name = "property_align", .alignment = alignment, .region_size = REGION_BYTES);
    defer      nya_arena_destroy(arena);

    // A zero-size allocation is the one that must come back null rather than aliasing the next byte.
    if (nya_arena_alloc(arena, 0) != nullptr) {
        nya_property_note(property, "a zero-size allocation was not null");
        return false;
    }

    u32 rounds = 4 + (u32)nya_property_draw_below(property, 32);
    for (u32 round = 0; round < rounds; round++) {
        u64 size = 1 + nya_property_draw_below(property, ALLOC_BYTES_MAX);
        u8* ptr  = nya_arena_alloc(arena, size);

        if (ptr == nullptr) {
            nya_property_note(property, "a %llu-byte allocation came back null", (unsigned long long)size);
            return false;
        }

        if ((uintptr_t)ptr % alignment != 0) {
            nya_property_note(property, "a %llu-byte allocation at %p is not aligned to %u", (unsigned long long)size, (void*)ptr, alignment);
            return false;
        }
    }

    return true;
}

/**
 * Through a random braid of alloc, free and free_all: no two live allocations ever share a byte, and the
 * bytes painted into one are the bytes still there after everything the allocator did around it. A free
 * that hands a block to the free list and a later alloc that reuses it must not tread on a neighbour.
 * */
static b8 law_live_allocations_never_overlap(NYA_Property* property) {
    NYA_Arena* arena = nya_arena_create(.name = "property_overlap", .alignment = draw_alignment(property), .region_size = REGION_BYTES);
    defer      nya_arena_destroy(arena);

    Live live[LIVE_MAX] = { 0 };
    u32  live_count     = 0;
    u32  next_id        = 1; // zero is reserved for "no allocation" so a fill is never all sevens by accident.

    u32 rounds = 8 + (u32)nya_property_draw_below(property, 48);
    for (u32 round = 0; round < rounds; round++) {
        u32 choice = (u32)nya_property_draw_below(property, 100);

        if (choice < 60 && live_count < LIVE_MAX) {
            // Allocate, and prove the new block overlaps nothing that is already live before painting it.
            u64 size = 1 + nya_property_draw_below(property, ALLOC_BYTES_MAX);
            u8* ptr  = nya_arena_alloc(arena, size);

            if (ptr == nullptr) {
                nya_property_note(property, "a %llu-byte allocation came back null at round %u", (unsigned long long)size, round);
                return false;
            }

            for (u32 index = 0; index < live_count; index++) {
                if (ranges_overlap(ptr, size, live[index].ptr, live[index].size)) {
                    nya_property_note(property, "a new %llu-byte block at %p overlaps a live %llu-byte block at %p", (unsigned long long)size,
                                      (void*)ptr, (unsigned long long)live[index].size, (void*)live[index].ptr);
                    return false;
                }
            }

            live[live_count] = (Live){ .ptr = ptr, .size = size, .id = next_id++ };
            paint(&live[live_count]);
            live_count++;
        } else if (choice < 85 && live_count > 0) {
            // Free one live block chosen at random; the free list may now hold it for the next alloc.
            u32  victim = (u32)nya_property_draw_below(property, live_count);
            Live freed  = live[victim];

            nya_arena_free(arena, freed.ptr, freed.size);

            live[victim] = live[live_count - 1];
            live_count--;
        } else {
            // free_all drops everything at once and must leave the arena reporting nothing in use.
            nya_arena_free_all(arena);
            live_count = 0;

            if (nya_arena_memory_usage_bytes(arena) != 0) {
                nya_property_note(property, "free_all left %llu bytes in use at round %u",
                                  (unsigned long long)nya_arena_memory_usage_bytes(arena), round);
                return false;
            }
        }

        // Whatever the round did, every block the law still holds reads back exactly as it was painted.
        for (u32 index = 0; index < live_count; index++) {
            if (!intact(&live[index])) {
                nya_property_note(property, "the %llu-byte block at %p was clobbered by round %u", (unsigned long long)live[index].size,
                                  (void*)live[index].ptr, round);
                return false;
            }
        }
    }

    return true;
}

/** free_all returns the arena to empty: it reports nothing in use, and it serves an allocation again straight after. */
static b8 law_free_all_returns_to_empty(NYA_Property* property) {
    NYA_Arena* arena = nya_arena_create(.name = "property_free_all", .alignment = draw_alignment(property), .region_size = REGION_BYTES);
    defer      nya_arena_destroy(arena);

    u32 count = 1 + (u32)nya_property_draw_below(property, 200);
    for (u32 index = 0; index < count; index++) {
        u64 size = 1 + nya_property_draw_below(property, ALLOC_BYTES_MAX);
        if (nya_arena_alloc(arena, size) == nullptr) {
            nya_property_note(property, "a %llu-byte allocation came back null before free_all", (unsigned long long)size);
            return false;
        }
    }

    nya_arena_free_all(arena);

    if (nya_arena_memory_usage_bytes(arena) != 0) {
        nya_property_note(property, "free_all of %u allocations left %llu bytes in use", count, (unsigned long long)nya_arena_memory_usage_bytes(arena));
        return false;
    }

    // Empty is a working arena, not a spent one: the next allocation succeeds and is aligned.
    u8* ptr = nya_arena_alloc(arena, 64);
    nya_property_note(property, "the arena would not allocate after free_all");
    return ptr != nullptr && (uintptr_t)ptr % arena->options.alignment == 0;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    u32 failures = 0;

    failures += nya_property_check("every allocation is aligned to the arena", CASES, SEED, law_pointer_is_aligned);
    failures += nya_property_check("live allocations never overlap and never clobber", CASES, SEED, law_live_allocations_never_overlap);
    failures += nya_property_check("free_all returns the arena to empty", CASES, SEED, law_free_all_returns_to_empty);

    return failures == 0 ? 0 : 1;
}

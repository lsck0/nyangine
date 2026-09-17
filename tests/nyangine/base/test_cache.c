/**
 * The general cache: content keys, tags as invalidation, both eviction policies, destructors, and the
 * ceiling row a named cache shows under.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

typedef struct {
    u32 id;
    u32 destroyed_marker;
} Resource;

/* Every destructor call, in order, so a test can say which values left and when. */
static u32 destroyed_ids[64];
static u32 destroyed_count = 0;

static void resource_destroy(void* value, void* user_data) {
    nya_assert(user_data == &destroyed_count, "the destructor gets the user data it was created with");

    Resource* resource = value;
    nya_assert(resource->destroyed_marker == 0, "a value is destroyed once");
    resource->destroyed_marker = 1;

    if (destroyed_count < nya_carray_length(destroyed_ids)) destroyed_ids[destroyed_count] = resource->id;
    destroyed_count++;
}

static void destroyed_reset(void) {
    destroyed_count = 0;
    nya_memset(destroyed_ids, 0, sizeof(destroyed_ids));
}

/* Inserts a resource and asserts it went in. */
static Resource* insert_resource(NYA_Cache* cache, NYA_ConstCString key, u64 tag, u32 id) {
    void*     slot  = nullptr;
    NYA_Error error = nya_cache_insert(cache, key, strlen(key), tag, &slot);
    nya_assert(error.ok, "inserting '%s' failed: %s", key, error.message);
    nya_assert(slot != nullptr);

    Resource* resource = slot;
    nya_assert(resource->id == 0 && resource->destroyed_marker == 0, "an inserted value starts zeroed");
    resource->id = id;
    return resource;
}

static Resource* get_resource(NYA_Cache* cache, NYA_ConstCString key, u64 tag) {
    return nya_cache_get(cache, key, strlen(key), tag);
}

/* Finds a ceiling row by name, or returns the registry's count when there is none. */
static u32 ceiling_index(NYA_ConstCString name) {
    for (u32 i = 0; i < nya_ceiling_count(); i++) {
        if (nya_string_equals(nya_ceiling_name_at(i), name)) return i;
    }
    return nya_ceiling_count();
}

/* The cache the destructor below reaches back into. */
static NYA_Cache* reentered = nullptr;

static void reentering_destroy(void* value, void* user_data) {
    nya_unused(value, user_data);
    (void)nya_cache_get(reentered, "x", 1, 0);
}

/* A value type with an alignment the default would not give it. */
typedef struct {
    alignas(64) f32 lanes[4];
} Wide;

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_cache");

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: hit, miss, and a key compared by content rather than by address.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        destroyed_reset();

        NYA_Cache* cache = nya_cache_create(arena, Resource, .capacity = 4, .key_size_max = 32, .destructor = resource_destroy,
                                            .user_data = &destroyed_count);

        nya_assert(nya_cache_count(cache) == 0);
        nya_assert(nya_cache_capacity(cache) == 4);
        nya_assert(get_resource(cache, "./font.ttf@17", 0) == nullptr, "an empty cache misses");

        Resource* inserted = insert_resource(cache, "./font.ttf@17", 0, 17);
        nya_assert(nya_cache_count(cache) == 1);

        // the same text in a different buffer, which is what a handle built on the stack looks like.
        char copy[32];
        (void)snprintf(copy, sizeof(copy), "%s", "./font.ttf@17");
        nya_assert(get_resource(cache, copy, 0) == inserted, "a key is found by its bytes, wherever they live");

        // and the same buffer rewritten with other text misses, the pointer keyed memo's bug.
        (void)snprintf(copy, sizeof(copy), "%s", "./font.ttf@28");
        nya_assert(get_resource(cache, copy, 0) == nullptr, "a reused buffer with other text is another key");

        // a prefix is not the key: sizes are part of the comparison.
        nya_assert(nya_cache_get(cache, "./font.ttf@1", strlen("./font.ttf@1"), 0) == nullptr);
        nya_assert(nya_cache_get(cache, "./font.ttf@17", strlen("./font.ttf@17") + 1, 0) == nullptr, "the terminator makes another key");

        // too long to have been stored is a miss, not a crash.
        char long_key[64];
        nya_memset(long_key, 'x', sizeof(long_key));
        nya_assert(nya_cache_get(cache, long_key, sizeof(long_key), 0) == nullptr);

        void*     slot  = nullptr;
        NYA_Error error = nya_cache_insert(cache, long_key, sizeof(long_key), 0, &slot);
        nya_assert(!error.ok && error.kind == NYA_ERROR_INVALID_ARGUMENT, "a key over key_size_max is refused");
        nya_assert(slot == nullptr);
        nya_assert(nya_cache_count(cache) == 1, "a refused insert leaves the cache as it was");

        nya_assert(destroyed_count == 0, "nothing has left the cache yet");

        nya_cache_destroy(cache);
        nya_assert(destroyed_count == 1 && destroyed_ids[0] == 17, "destroy runs the destructor on what was left");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: keys are bytes, not strings. Integers and structs work as keys too.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Cache* cache = nya_cache_create(arena, u64, .capacity = 8, .key_size_max = sizeof(u32) * 2);

        struct {
            u32 path_hash;
            u32 point_size;
        } key_a = { 7, 17 }, key_b = { 7, 28 };

        void* slot = nullptr;
        NYA_EXPECT(nya_cache_insert(cache, &key_a, sizeof(key_a), 0, &slot));
        *(u64*)slot = 1700;
        NYA_EXPECT(nya_cache_insert(cache, &key_b, sizeof(key_b), 0, &slot));
        *(u64*)slot = 2800;

        u64* a = nya_cache_get(cache, &key_a, sizeof(key_a), 0);
        u64* b = nya_cache_get(cache, &key_b, sizeof(key_b), 0);
        nya_assert(a != nullptr && *a == 1700);
        nya_assert(b != nullptr && *b == 2800);

        nya_cache_destroy(cache);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the tag is the invalidation input. Another tag reads as stale, and the next insert replaces it.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        destroyed_reset();

        NYA_Cache* cache = nya_cache_create(arena, Resource, .capacity = 4, .key_size_max = 32, .destructor = resource_destroy,
                                            .user_data = &destroyed_count);

        Resource* first = insert_resource(cache, "atlas", 1, 100);

        nya_assert(get_resource(cache, "atlas", 1) == first, "the tag it was stored with hits");
        nya_assert(get_resource(cache, "atlas", 2) == nullptr, "a newer generation misses");

        void*           value  = nullptr;
        NYA_CacheLookup lookup = nya_cache_lookup(cache, "atlas", strlen("atlas"), 2, &value);
        nya_assert(lookup == NYA_CACHE_LOOKUP_STALE, "lookup tells stale from missing");
        nya_assert(value == first && ((Resource*)value)->id == 100, "a stale value is still readable");

        lookup = nya_cache_lookup(cache, "atlas", strlen("atlas"), 1, &value);
        nya_assert(lookup == NYA_CACHE_LOOKUP_HIT && value == first);

        lookup = nya_cache_lookup(cache, "missing", strlen("missing"), 1, &value);
        nya_assert(lookup == NYA_CACHE_LOOKUP_MISS && value == nullptr);

        // rebuilt against the new generation: the old value is destroyed first, in the same slot.
        Resource* second = insert_resource(cache, "atlas", 2, 200);
        nya_assert(destroyed_count == 1 && destroyed_ids[0] == 100, "replacing runs the destructor on the old value");
        nya_assert(nya_cache_count(cache) == 1, "a replacement is not a second entry");
        nya_assert(get_resource(cache, "atlas", 2) == second && second->id == 200);
        nya_assert(get_resource(cache, "atlas", 1) == nullptr, "the old generation is now the stale one");

        nya_cache_destroy(cache);
        nya_assert(destroyed_count == 2 && destroyed_ids[1] == 200);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: least recent eviction drops the entry hit or inserted longest ago.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        destroyed_reset();

        NYA_Cache* cache = nya_cache_create(arena, Resource, .capacity = 3, .key_size_max = 8, .eviction = NYA_CACHE_EVICTION_LEAST_RECENT,
                                            .destructor = resource_destroy, .user_data = &destroyed_count);

        (void)insert_resource(cache, "a", 0, 1);
        (void)insert_resource(cache, "b", 0, 2);
        (void)insert_resource(cache, "c", 0, 3);
        nya_assert(nya_cache_count(cache) == 3);

        // "a" is used, so "b" becomes the oldest.
        nya_assert(get_resource(cache, "a", 0) != nullptr);

        (void)insert_resource(cache, "d", 0, 4);
        nya_assert(nya_cache_count(cache) == 3, "a full least recent cache stays full");
        nya_assert(destroyed_count == 1 && destroyed_ids[0] == 2, "the least recently used entry was evicted");
        nya_assert(get_resource(cache, "b", 0) == nullptr);
        nya_assert(get_resource(cache, "a", 0) != nullptr);
        nya_assert(get_resource(cache, "c", 0) != nullptr);
        nya_assert(get_resource(cache, "d", 0) != nullptr);

        // order is now d, a, c by insertion then hits a, c, d: "a" is the oldest.
        (void)insert_resource(cache, "e", 0, 5);
        nya_assert(destroyed_count == 2 && destroyed_ids[1] == 1, "hits count as uses, so the order follows them");

        // a stale read is not a use.
        nya_assert(get_resource(cache, "c", 9) == nullptr);
        nya_assert(get_resource(cache, "d", 0) != nullptr);
        nya_assert(get_resource(cache, "e", 0) != nullptr);
        (void)insert_resource(cache, "f", 0, 6);
        nya_assert(destroyed_count == 3 && destroyed_ids[2] == 3, "a stale lookup does not refresh an entry");

        nya_cache_destroy(cache);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: refuse when full, and room again after a removal.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        destroyed_reset();

        NYA_Cache* cache = nya_cache_create(arena, Resource, .capacity = 2, .key_size_max = 8, .eviction = NYA_CACHE_EVICTION_REFUSE,
                                            .destructor = resource_destroy, .user_data = &destroyed_count);

        Resource* a = insert_resource(cache, "a", 0, 1);
        (void)insert_resource(cache, "b", 0, 2);

        void*     slot  = nullptr;
        NYA_Error error = nya_cache_insert(cache, "c", 1, 0, &slot);
        nya_assert(!error.ok && error.kind == NYA_ERROR_OUT_OF_MEMORY, "a full refusing cache returns an error");
        nya_assert(slot == nullptr);
        nya_assert(destroyed_count == 0, "refusing destroys nothing");
        nya_assert(get_resource(cache, "a", 0) == a, "and keeps what it had, at the same address");

        // replacing an existing key needs no room.
        (void)insert_resource(cache, "a", 1, 10);
        nya_assert(destroyed_count == 1 && destroyed_ids[0] == 1);

        nya_assert(nya_cache_remove(cache, "b", 1), "removing a present key reports it");
        nya_assert(destroyed_count == 2 && destroyed_ids[1] == 2, "remove runs the destructor");
        nya_assert(!nya_cache_remove(cache, "b", 1), "removing it twice does not");
        nya_assert(destroyed_count == 2);
        nya_assert(nya_cache_count(cache) == 1);

        (void)insert_resource(cache, "c", 0, 3);
        nya_assert(nya_cache_count(cache) == 2);

        nya_cache_clear(cache);
        nya_assert(nya_cache_count(cache) == 0);
        nya_assert(destroyed_count == 4, "clear runs the destructor on each value");
        nya_assert(get_resource(cache, "a", 1) == nullptr && get_resource(cache, "c", 0) == nullptr);

        // the storage stays usable after a clear.
        (void)insert_resource(cache, "a", 0, 5);
        (void)insert_resource(cache, "b", 0, 6);
        nya_assert(nya_cache_count(cache) == 2);

        destroyed_reset();
        nya_cache_destroy(cache);
        nya_assert(destroyed_count == 2);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: removals keep every other key findable. Linear probing without tombstones has to close the gap
    // a removal leaves, so this churns a cache against a plain array of what should be there.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        enum { CAPACITY = 64, KEYS = 256, ROUNDS = 20000 };

        NYA_Cache* cache = nya_cache_create(arena, u32, .capacity = CAPACITY, .key_size_max = 16);

        static b8  present[KEYS];
        u32        present_count = 0;
        u64        state         = 0x9E3779B97F4A7C15ULL;

        for (u32 round = 0; round < ROUNDS; round++) {
            state   ^= state << 13;
            state   ^= state >> 7;
            state   ^= state << 17;
            u32 key  = (u32)(state % KEYS);

            char text[16];
            s32  length = snprintf(text, sizeof(text), "key%u", key);

            if (present[key]) {
                u32* value = nya_cache_get(cache, text, (u64)length, 0);
                nya_check(value != nullptr && *value == key, "round %u: key %u went missing", round, key);

                nya_assert(nya_cache_remove(cache, text, (u64)length));
                present[key] = false;
                present_count--;
            } else if (present_count < CAPACITY) {
                void* slot = nullptr;
                NYA_EXPECT(nya_cache_insert(cache, text, (u64)length, 0, &slot));
                *(u32*)slot  = key;
                present[key] = true;
                present_count++;
            }

            nya_assert(nya_cache_count(cache) == present_count);
        }

        for (u32 key = 0; key < KEYS; key++) {
            char text[16];
            s32  length = snprintf(text, sizeof(text), "key%u", key);
            u32* value  = nya_cache_get(cache, text, (u64)length, 0);

            nya_check(present[key] == (value != nullptr), "key %u: present %d, found %d", key, present[key], value != nullptr);
            if (value != nullptr) nya_check(*value == key, "key %u holds %u", key, *value);
        }

        nya_cache_destroy(cache);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: values are aligned for their type.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Cache* cache = nya_cache_create(arena, Wide, .capacity = 5, .key_size_max = 4);

        for (u32 i = 0; i < 5; i++) {
            void* slot = nullptr;
            NYA_EXPECT(nya_cache_insert(cache, &i, sizeof(i), 0, &slot));
            nya_assert(((uintptr_t)slot & (alignof(Wide) - 1)) == 0, "value %u is not aligned to %zu", i, alignof(Wide));
            ((Wide*)slot)->lanes[3] = (f32)i;
        }

        for (u32 i = 0; i < 5; i++) {
            Wide* wide = nya_cache_get(cache, &i, sizeof(i), 0);
            nya_assert(wide != nullptr && wide->lanes[3] == (f32)i, "values do not overlap");
        }

        nya_cache_destroy(cache);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a named cache shows as a ceiling, and its row follows the count.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Cache* cache = nya_cache_create(arena, u32, .name = "test_cache_ceiling", .capacity = 4, .key_size_max = 4);

        u32 row = ceiling_index("test_cache_ceiling");
        nya_assert(row < nya_ceiling_count(), "a named cache registers a ceiling");
        nya_assert(nya_ceiling_capacity_at(row) == 4);
        nya_assert(nya_ceiling_live_at(row) == 0);

        for (u32 i = 0; i < 4; i++) {
            void* slot = nullptr;
            NYA_EXPECT(nya_cache_insert(cache, &i, sizeof(i), 0, &slot));
        }

        // the registry sorts by fullness, so the row is found again rather than assumed to stay put.
        row = ceiling_index("test_cache_ceiling");
        nya_assert(nya_ceiling_live_at(row) == 4, "a full cache reads full");

        u32   extra = 4;
        void* slot  = nullptr;
        nya_assert(!nya_cache_insert(cache, &extra, sizeof(extra), 0, &slot).ok, "the capacity is a ceiling");
        nya_assert(nya_ceiling_live_at(ceiling_index("test_cache_ceiling")) == 4);

        u32 zero = 0;
        nya_assert(nya_cache_remove(cache, &zero, sizeof(zero)));
        nya_assert(nya_ceiling_live_at(ceiling_index("test_cache_ceiling")) == 3, "a lone cache's row is its count");

        u32 registered = nya_ceiling_count();
        nya_cache_destroy(cache);
        nya_assert(nya_ceiling_live_at(ceiling_index("test_cache_ceiling")) == 0, "a destroyed cache leaves an empty row, not a dangling one");

        // a second cache under the name reuses the row instead of registering again.
        NYA_Cache* again = nya_cache_create(arena, u32, .name = "test_cache_ceiling", .capacity = 4, .key_size_max = 4);
        nya_assert(nya_ceiling_count() == registered);

        NYA_EXPECT(nya_cache_insert(again, &zero, sizeof(zero), 0, &slot));
        nya_assert(nya_ceiling_live_at(ceiling_index("test_cache_ceiling")) == 1);

        nya_cache_destroy(again);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a destructor that calls back into its own cache asserts.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Cache* cache = nya_cache_create(arena, u32, .capacity = 2, .key_size_max = 4, .destructor = reentering_destroy);
        reentered        = cache;

        void* slot = nullptr;
        NYA_EXPECT(nya_cache_insert(cache, "x", 1, 0, &slot));

        nya_expect_crash(nya_cache_clear(cache));
        nya_assert(nya_crash_caught()->source == NYA_CRASH_SOURCE_ASSERT);
    }

    nya_arena_destroy(arena);

    return nya_check_failures() == 0 ? 0 : 1;
}

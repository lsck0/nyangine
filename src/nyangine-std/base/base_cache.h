/**
 * @file base_cache.h
 *
 * A fixed capacity map from content keys to values, for memos and for owners of derived resources
 * (glyph atlases, uploaded vertex buffers) that have to find them again by name.
 *
 * - `nya_cache_create` / `nya_cache_destroy`: storage comes from an arena, once, and never grows.
 * - `nya_cache_get`: the value for a key whose tag matches, or null.
 * - `nya_cache_lookup`: the same, telling a missing key from a stale one.
 * - `nya_cache_add`: claims or replaces the entry for a key and returns its zeroed value.
 * - `nya_cache_remove` / `nya_cache_clear`: drop entries, running the destructor on each value.
 * - `nya_cache_count` / `nya_cache_capacity`.
 *
 * ```c
 * typedef struct { SDL_GPUTexture* texture; } Atlas;
 *
 * NYA_INTERNAL void atlas_destroy(void* value, void* user_data) {
 *     nya_unused(user_data);
 *     SDL_ReleaseGPUTexture(device, ((Atlas*)value)->texture);
 * }
 *
 * NYA_Cache* atlases = nya_cache_create(arena, Atlas, .name = "atlases", .capacity = 8, .key_size_max = 256,
 *                                       .eviction = NYA_CACHE_EVICTION_REFUSE, .destructor = atlas_destroy);
 *
 * // the tag is whatever the value was built from; a reloaded font has a new generation.
 * Atlas* atlas = nya_cache_get(atlases, path, strlen(path), font->generation);
 * if (atlas == nullptr) {
 *     void*     slot  = nullptr;
 *     NYA_Error error = nya_cache_add(atlases, path, strlen(path), font->generation, &slot);
 *     if (!error.ok) return nullptr;
 *
 *     atlas          = slot;
 *     atlas->texture = build_texture(font);
 * }
 *
 * nya_cache_destroy(atlases);
 * ```
 *
 * Why this shape:
 * - Keys are bytes, compared by content. A pointer key breaks for handles built in a reused stack buffer and
 *   for DLL literals after a hot reload.
 * - Staleness is an input. The owner knows what a value was derived from (a dictionary generation, an asset
 *   load), so it passes that as the tag instead of each cache inventing its own check.
 * - Values are plain bytes owned by the cache, at an address that holds until the entry is removed,
 *   replaced or evicted. Anything they own (GPU objects) goes through the caller's destructor, which keeps
 *   this in base: SDL free and testable headless.
 * - Fixed capacity, with eviction a field: least recently used for memos that can recompute, refuse for
 *   registries whose values the caller expects to persist. Each named cache shows as a ceiling.
 *
 * Not thread safe. Calls from inside a destructor and overlapping calls from two threads trip an assert.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/**
 * Distinct cache names that can show as ceilings. The engine names three.
 * */
#ifndef NYA_CACHE_CEILING_MAX
#define NYA_CACHE_CEILING_MAX 16
#endif

// TYPES

typedef struct NYA_Cache        NYA_Cache;
typedef struct NYA_CacheOptions NYA_CacheOptions;
typedef enum NYA_CacheEviction  NYA_CacheEviction;
typedef enum NYA_CacheLookup    NYA_CacheLookup;

/**
 * Releases what a value owns. The value bytes themselves belong to the cache. Must not call back into the
 * cache that runs it.
 * */
typedef void (*NYA_CacheDestructor)(void* value, void* user_data);

enum NYA_CacheEviction {
    /** A full cache refuses new keys. For values the caller expects to find again. */
    NYA_CACHE_EVICTION_REFUSE,

    /** A full cache drops the entry that was hit or inserted longest ago. For memos. */
    NYA_CACHE_EVICTION_LEAST_RECENT,

    NYA_CACHE_EVICTION_COUNT,
};

enum NYA_CacheLookup {
    NYA_CACHE_LOOKUP_MISS,

    /** The key is present with another tag. The value is still readable, and the next insert replaces it. */
    NYA_CACHE_LOOKUP_STALE,

    NYA_CACHE_LOOKUP_HIT,

    NYA_CACHE_LOOKUP_COUNT,
};

struct NYA_CacheOptions {
    /**
     * The ceiling row this cache shows under, or null for none. Must outlive the process, so a literal. Caches
     * sharing a name share a row: the count of a lone cache, the fullest seen while several exist.
     * */
    NYA_ConstCString name;

    /** Entries held at once. */
    u32 capacity;

    /** Longest key in bytes. Every entry reserves this much, so keep it near the real longest key. */
    u32 key_size_max;

    /** Set by nya_cache_create from the value type. */
    u32 value_size;
    u32 value_alignment;

    NYA_CacheEviction eviction;

    /** Run on every value that leaves the cache: removed, replaced, evicted, cleared or destroyed. May be null. */
    NYA_CacheDestructor destructor;
    void*               user_data;
};

// FUNCTIONS AND MACROS

/** Creates a cache of `value_type` values. The remaining arguments are NYA_CacheOptions designated initializers. */
#define nya_cache_create(arena, value_type, ...)                                                                                                     \
    nya_cache_create_with_options(                                                                                                                   \
        arena,                                                                                                                                       \
        (NYA_CacheOptions){ .value_size = sizeof(value_type), .value_alignment = alignof(value_type), __VA_ARGS__ }                                  \
    )

NYA_API NYA_Cache* nya_cache_create_with_options(NYA_Arena* arena, NYA_CacheOptions options) __attr_no_discard;

/** Runs the destructor on every value, then frees the storage. */
NYA_API void nya_cache_destroy(NYA_Cache* cache);

/**
 * The value stored under `key` with `tag`, or null when the key is missing or carries another tag. A hit
 * counts as a use for least recent eviction.
 * */
NYA_API void* nya_cache_get(NYA_Cache* cache, const void* key, u64 key_size, u64 tag) __attr_no_discard;

/** nya_cache_get, but a stale entry's value is returned too, with NYA_CACHE_LOOKUP_STALE. Null on a miss. */
NYA_API NYA_CacheLookup nya_cache_lookup(NYA_Cache* cache, const void* key, u64 key_size, u64 tag, OUT void** out_value);

/**
 * Stores `key` with `tag` and returns its value, zeroed. An existing entry for the key is replaced, its old
 * value destroyed first. A full cache evicts or refuses by its policy; refusing, like a key longer than
 * `key_size_max`, is an error and leaves the cache as it was.
 * */
NYA_API NYA_Error nya_cache_add(NYA_Cache* cache, const void* key, u64 key_size, u64 tag, OUT void** out_value);

/** Destroys and forgets the entry for `key`. False if there was none. */
NYA_API b8 nya_cache_remove(NYA_Cache* cache, const void* key, u64 key_size);

/** Destroys and forgets every entry. The storage stays. */
NYA_API void nya_cache_clear(NYA_Cache* cache);

NYA_API u32 nya_cache_count(const NYA_Cache* cache) __attr_no_discard;
NYA_API u32 nya_cache_capacity(const NYA_Cache* cache) __attr_no_discard;

#include "nyangine-core/nyangine.h"

// TYPES

/** No entry: the end of the free list, or a failed find. */
#define _NYA_CACHE_NONE UINT32_MAX

typedef struct {
    u64 hash;
    u64 tag;

    /**
     * The cache's clock when this was last hit or inserted. A stamp rather than a recency list: a hit is one
     * write instead of four relinks, and only an evicting insert pays for the scan.
     * */
    u64 last_used;

    /** Zero while the entry is free, which is why an empty key is refused. */
    u32 key_size;

    /** The next free entry, while this one is free. */
    u32 next_free;
} _NYA_CacheEntry;

/** One ceiling row, shared by every cache with the same name. The registry points at `live`. */
typedef struct {
    NYA_ConstCString name;
    u32              capacity;
    u32              live;

    /** Caches currently attached. With one, `live` is its count; with more, the fullest seen. */
    u32 caches;
} _NYA_CacheCeiling;

struct NYA_Cache {
    NYA_CacheOptions options;

    NYA_Arena* arena;
    void*      allocation;
    u64        allocation_size;

    /**
     * Open addressing with linear probing, twice the capacity so a probe run stays short. Each holds an entry
     * index plus one, zero when empty. Removal shifts back instead of leaving tombstones.
     * */
    u32* buckets;
    u32  bucket_mask;

    _NYA_CacheEntry* entries;

    /** `capacity * key_size_max` bytes and `capacity * value_stride` bytes, indexed like `entries`. */
    u8* keys;
    u8* values;
    u64 value_stride;

    u32 count;
    u32 free_head;
    u64 clock;

    _NYA_CacheCeiling* ceiling;

    /** Set while a call changes the cache, so re-entry from a destructor or a second thread asserts. */
    b8 busy;
};

// PRIVATE API DECLARATION

NYA_INTERNAL _NYA_CacheCeiling _nya_cache_ceilings[NYA_CACHE_CEILING_MAX] = { 0 };
NYA_INTERNAL u32               _nya_cache_ceiling_count                    = 0;

/** The name for messages; a cache without one is still worth a readable error. */
NYA_INTERNAL NYA_ConstCString _nya_cache_label(const NYA_Cache* cache) __attr_no_discard;

/** The entry index holding `key`, whatever its tag, or _NYA_CACHE_NONE. */
NYA_INTERNAL inline u32 _nya_cache_find(const NYA_Cache* cache, const void* key, u64 key_size, u64 hash) __attr_no_discard;

/**
 * Byte equality in eight byte words, the last word overlapping the one before it, so a key costs its length
 * over eight compares and no call. Keys are short, and memcmp on a runtime size is a libc call.
 * */
__attribute__((always_inline)) NYA_INTERNAL inline b8 _nya_cache_key_equals(const u8* a, const u8* b, u64 size) __attr_no_discard;

/** Links every entry into the free list and empties the buckets. */
NYA_INTERNAL void _nya_cache_reset(NYA_Cache* cache);

/** Runs the destructor and returns the entry to the free list. */
NYA_INTERNAL void _nya_cache_release(NYA_Cache* cache, u32 index);

/** The live entry with the oldest stamp. */
NYA_INTERNAL u32 _nya_cache_least_recent(const NYA_Cache* cache) __attr_no_discard;

/** Empties the bucket that points at `index` and closes the gap in its probe run. */
NYA_INTERNAL void _nya_cache_bucket_erase(NYA_Cache* cache, u32 index);

NYA_INTERNAL void _nya_cache_ceiling_attach(NYA_Cache* cache);
NYA_INTERNAL void _nya_cache_ceiling_update(NYA_Cache* cache);
NYA_INTERNAL void _nya_cache_ceiling_detach(NYA_Cache* cache);

// PUBLIC API IMPLEMENTATION

NYA_Cache* nya_cache_create_with_options(NYA_Arena* arena, NYA_CacheOptions options) {
    nya_assert(arena != nullptr);
    nya_assert(options.capacity > 0, "a cache needs room for at least one entry");
    nya_assert(options.capacity < UINT32_MAX / 4, "bucket indices are u32");
    nya_assert(options.key_size_max > 0, "keys are never empty, so the longest key must be at least one byte");
    nya_assert(options.value_size > 0);
    nya_assert(options.value_alignment > 0 && (options.value_alignment & (options.value_alignment - 1)) == 0);
    nya_assert(options.eviction < NYA_CACHE_EVICTION_COUNT);

    u32 bucket_count = 2;
    while (bucket_count < options.capacity * 2) bucket_count *= 2;

    u64 value_stride = ((u64)options.value_size + options.value_alignment - 1) & ~((u64)options.value_alignment - 1);

    u64 header_size  = sizeof(NYA_Cache);
    u64 buckets_size = (u64)bucket_count * sizeof(u32);
    u64 entries_size = (u64)options.capacity * sizeof(_NYA_CacheEntry);
    u64 keys_size    = (u64)options.capacity * options.key_size_max;
    u64 values_size  = (u64)options.capacity * value_stride;

    // one allocation. the slack covers aligning each part, whatever address the arena hands back.
    u64 slack = 4 * nya_max((u64)options.value_alignment, (u64)alignof(max_align_t));
    u64 total = header_size + buckets_size + entries_size + keys_size + values_size + slack;

    u8* allocation = nya_arena_alloc(arena, total);
    nya_assert(allocation != nullptr);
    nya_memset(allocation, 0, total);

    uintptr_t cursor = (uintptr_t)allocation;

    cursor            = (cursor + alignof(max_align_t) - 1) & ~(alignof(max_align_t) - 1);
    NYA_Cache* cache  = (NYA_Cache*)cursor;
    cursor           += header_size;

    cursor        = (cursor + alignof(u32) - 1) & ~(alignof(u32) - 1);
    u32* buckets  = (u32*)cursor;
    cursor       += buckets_size;

    cursor                    = (cursor + alignof(_NYA_CacheEntry) - 1) & ~(alignof(_NYA_CacheEntry) - 1);
    _NYA_CacheEntry* entries  = (_NYA_CacheEntry*)cursor;
    cursor                   += entries_size;

    u8* keys  = (u8*)cursor;
    cursor   += keys_size;

    cursor      = (cursor + options.value_alignment - 1) & ~((uintptr_t)options.value_alignment - 1);
    u8* values  = (u8*)cursor;
    cursor     += values_size;

    nya_assert(cursor <= (uintptr_t)allocation + total, "the cache layout ran past its allocation");

    *cache = (NYA_Cache){
        .options         = options,
        .arena           = arena,
        .allocation      = allocation,
        .allocation_size = total,
        .buckets         = buckets,
        .bucket_mask     = bucket_count - 1,
        .entries         = entries,
        .keys            = keys,
        .values          = values,
        .value_stride    = value_stride,
    };

    _nya_cache_reset(cache);
    _nya_cache_ceiling_attach(cache);

    return cache;
}

void nya_cache_destroy(NYA_Cache* cache) {
    nya_assert(cache != nullptr);

    nya_cache_clear(cache);
    _nya_cache_ceiling_detach(cache);

    // the struct lives inside the allocation, so it is read out before the free.
    NYA_Arena* arena      = cache->arena;
    void*      allocation = cache->allocation;
    u64        size       = cache->allocation_size;

    nya_arena_free(arena, allocation, size);
}

// Force-inlined into engine callers: nya_asset_get runs every draw and the call plus hash cost a nanosecond on a five-nanosecond lookup; not built on nya_cache_lookup, whose out parameter brings a stack protector.
__attribute__((always_inline)) void* nya_cache_get(NYA_Cache* cache, const void* key, u64 key_size, u64 tag) {
    nya_assert(cache != nullptr);
    nya_assert(key != nullptr && key_size > 0, "cache keys are never empty");
    nya_assert(!cache->busy, "cache '%s' was re-entered: from its own destructor, or from another thread", _nya_cache_label(cache));

    if (key_size > cache->options.key_size_max) return nullptr;

    u32 index = _nya_cache_find(cache, key, key_size, _nya_hash_wyhash(key, key_size));
    if (index == _NYA_CACHE_NONE) return nullptr;

    _NYA_CacheEntry* entry = &cache->entries[index];
    if (entry->tag != tag) return nullptr;

    entry->last_used = ++cache->clock;

    return cache->values + (index * cache->value_stride);
}

NYA_CacheLookup nya_cache_lookup(NYA_Cache* cache, const void* key, u64 key_size, u64 tag, OUT void** out_value) {
    nya_assert(cache != nullptr);
    nya_assert(key != nullptr && key_size > 0, "cache keys are never empty");
    nya_assert(out_value != nullptr);
    nya_assert(!cache->busy, "cache '%s' was re-entered: from its own destructor, or from another thread", _nya_cache_label(cache));

    *out_value = nullptr;

    // too long to have been stored.
    if (key_size > cache->options.key_size_max) return NYA_CACHE_LOOKUP_MISS;

    u32 index = _nya_cache_find(cache, key, key_size, _nya_hash_wyhash(key, key_size));
    if (index == _NYA_CACHE_NONE) return NYA_CACHE_LOOKUP_MISS;

    *out_value = cache->values + (index * cache->value_stride);

    _NYA_CacheEntry* entry = &cache->entries[index];
    if (entry->tag != tag) return NYA_CACHE_LOOKUP_STALE;

    entry->last_used = ++cache->clock;

    return NYA_CACHE_LOOKUP_HIT;
}

NYA_Error nya_cache_add(NYA_Cache* cache, const void* key, u64 key_size, u64 tag, OUT void** out_value) {
    nya_assert(cache != nullptr);
    nya_assert(key != nullptr && key_size > 0, "cache keys are never empty");
    nya_assert(out_value != nullptr);
    nya_assert(!cache->busy, "cache '%s' was re-entered: from its own destructor, or from another thread", _nya_cache_label(cache));

    *out_value = nullptr;

    if (key_size > cache->options.key_size_max) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a " FMTu64 " byte key does not fit cache '%s' (key_size_max " FMTu32 ")", key_size,
                         _nya_cache_label(cache), cache->options.key_size_max);
    }

    cache->busy = true;
    defer cache->busy = false;

    u64 hash  = _nya_hash_wyhash(key, key_size);
    u32 index = _nya_cache_find(cache, key, key_size, hash);

    if (index != _NYA_CACHE_NONE) {
        u8* value = cache->values + (index * cache->value_stride);

        if (cache->options.destructor != nullptr) cache->options.destructor(value, cache->options.user_data);
        nya_memset(value, 0, cache->value_stride);

        cache->entries[index].tag       = tag;
        cache->entries[index].last_used = ++cache->clock;

        *out_value = value;
        return NYA_OK;
    }

    if (cache->count == cache->options.capacity) {
        switch (cache->options.eviction) {
            case NYA_CACHE_EVICTION_REFUSE: {
                return nya_error(NYA_ERROR_OUT_OF_MEMORY, "cache '%s' is full at " FMTu32 " entries", _nya_cache_label(cache), cache->options.capacity);
            }

            case NYA_CACHE_EVICTION_LEAST_RECENT: {
                _nya_cache_release(cache, _nya_cache_least_recent(cache));
            } break;

            case NYA_CACHE_EVICTION_COUNT:
            default: nya_unreachable();
        }
    }

    nya_assert(cache->free_head != _NYA_CACHE_NONE, "cache '%s' counts " FMTu32 " entries but has no free one", _nya_cache_label(cache), cache->count);

    index            = cache->free_head;
    cache->free_head = cache->entries[index].next_free;

    cache->entries[index] = (_NYA_CacheEntry){
        .hash      = hash,
        .tag       = tag,
        .last_used = ++cache->clock,
        .key_size  = (u32)key_size,
        .next_free = _NYA_CACHE_NONE,
    };
    nya_memcpy(cache->keys + ((u64)index * cache->options.key_size_max), key, key_size);

    u8* value = cache->values + (index * cache->value_stride);
    nya_memset(value, 0, cache->value_stride);

    u32 bucket = (u32)hash & cache->bucket_mask;
    while (cache->buckets[bucket] != 0) bucket = (bucket + 1) & cache->bucket_mask;
    cache->buckets[bucket] = index + 1;

    cache->count++;
    _nya_cache_ceiling_update(cache);

    nya_assert(cache->count <= cache->options.capacity);
    nya_assert(_nya_cache_find(cache, key, key_size, hash) == index, "an inserted key must be findable");

    *out_value = value;
    return NYA_OK;
}

b8 nya_cache_remove(NYA_Cache* cache, const void* key, u64 key_size) {
    nya_assert(cache != nullptr);
    nya_assert(key != nullptr && key_size > 0, "cache keys are never empty");
    nya_assert(!cache->busy, "cache '%s' was re-entered: from its own destructor, or from another thread", _nya_cache_label(cache));

    if (key_size > cache->options.key_size_max) return false;

    u32 index = _nya_cache_find(cache, key, key_size, _nya_hash_wyhash(key, key_size));
    if (index == _NYA_CACHE_NONE) return false;

    cache->busy = true;
    _nya_cache_release(cache, index);
    cache->busy = false;

    return true;
}

void nya_cache_clear(NYA_Cache* cache) {
    nya_assert(cache != nullptr);
    nya_assert(!cache->busy, "cache '%s' was re-entered: from its own destructor, or from another thread", _nya_cache_label(cache));

    cache->busy = true;

    if (cache->options.destructor != nullptr) {
        for (u32 index = 0; index < cache->options.capacity; index++) {
            if (cache->entries[index].key_size == 0) continue;
            cache->options.destructor(cache->values + (index * cache->value_stride), cache->options.user_data);
        }
    }

    _nya_cache_reset(cache);
    _nya_cache_ceiling_update(cache);

    cache->busy = false;
}

u32 nya_cache_count(const NYA_Cache* cache) {
    nya_assert(cache != nullptr);
    return cache->count;
}

u32 nya_cache_capacity(const NYA_Cache* cache) {
    nya_assert(cache != nullptr);
    return cache->options.capacity;
}

// PRIVATE API IMPLEMENTATION

NYA_ConstCString _nya_cache_label(const NYA_Cache* cache) {
    return cache->options.name != nullptr ? cache->options.name : "unnamed";
}

u32 _nya_cache_find(const NYA_Cache* cache, const void* key, u64 key_size, u64 hash) {
    // at most half full, so an empty bucket always ends the probe.
    for (u32 bucket = (u32)hash & cache->bucket_mask;; bucket = (bucket + 1) & cache->bucket_mask) {
        u32 stored = cache->buckets[bucket];
        if (stored == 0) return _NYA_CACHE_NONE;

        u32                    index = stored - 1;
        const _NYA_CacheEntry* entry = &cache->entries[index];

        if (entry->hash != hash || entry->key_size != key_size) continue;
        if (!_nya_cache_key_equals(cache->keys + ((u64)index * cache->options.key_size_max), key, key_size)) continue;

        return index;
    }
}

b8 _nya_cache_key_equals(const u8* a, const u8* b, u64 size) {
    u64 word_a = 0;
    u64 word_b = 0;

    if (size < 8) {
        for (u64 i = 0; i < size; i++) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    for (u64 offset = 0; offset + 8 < size; offset += 8) {
        nya_memcpy(&word_a, a + offset, 8);
        nya_memcpy(&word_b, b + offset, 8);
        if (word_a != word_b) return false;
    }

    nya_memcpy(&word_a, a + size - 8, 8);
    nya_memcpy(&word_b, b + size - 8, 8);
    return word_a == word_b;
}

void _nya_cache_reset(NYA_Cache* cache) {
    nya_memset(cache->buckets, 0, ((u64)cache->bucket_mask + 1) * sizeof(u32));

    for (u32 i = 0; i < cache->options.capacity; i++) {
        cache->entries[i] = (_NYA_CacheEntry){ .next_free = i + 1 < cache->options.capacity ? i + 1 : _NYA_CACHE_NONE };
    }

    cache->count     = 0;
    cache->free_head = 0;
}

void _nya_cache_release(NYA_Cache* cache, u32 index) {
    nya_assert(index < cache->options.capacity);
    nya_assert(cache->entries[index].key_size != 0, "released a free cache entry");

    if (cache->options.destructor != nullptr) cache->options.destructor(cache->values + (index * cache->value_stride), cache->options.user_data);

    _nya_cache_bucket_erase(cache, index);

    cache->entries[index] = (_NYA_CacheEntry){ .next_free = cache->free_head };
    cache->free_head      = index;

    cache->count--;
    _nya_cache_ceiling_update(cache);
}

u32 _nya_cache_least_recent(const NYA_Cache* cache) {
    u32 oldest      = _NYA_CACHE_NONE;
    u64 oldest_used = UINT64_MAX;

    for (u32 i = 0; i < cache->options.capacity; i++) {
        const _NYA_CacheEntry* entry = &cache->entries[i];
        if (entry->key_size == 0 || entry->last_used >= oldest_used) continue;

        oldest      = i;
        oldest_used = entry->last_used;
    }

    nya_assert(oldest != _NYA_CACHE_NONE, "cache '%s' has no entry to evict", _nya_cache_label(cache));
    return oldest;
}

void _nya_cache_bucket_erase(NYA_Cache* cache, u32 index) {
    u32 mask = cache->bucket_mask;
    u32 hole = (u32)cache->entries[index].hash & mask;

    while (cache->buckets[hole] != index + 1) {
        nya_assert(cache->buckets[hole] != 0, "cache entry " FMTu32 " is missing from its probe run", index);
        hole = (hole + 1) & mask;
    }

    /* Backward shift: pull each later member of the run into the hole unless its home lies cyclically inside (hole, next], where moving it would hide it from its own probe. */
    for (u32 next = (hole + 1) & mask; cache->buckets[next] != 0; next = (next + 1) & mask) {
        u32 home = (u32)cache->entries[cache->buckets[next] - 1].hash & mask;

        b8 stays = hole <= next ? (hole < home && home <= next) : (hole < home || home <= next);
        if (stays) continue;

        cache->buckets[hole] = cache->buckets[next];
        hole                 = next;
    }

    cache->buckets[hole] = 0;
}

void _nya_cache_ceiling_attach(NYA_Cache* cache) {
    if (cache->options.name == nullptr) return;

    _NYA_CacheCeiling* row = nullptr;

    for (u32 i = 0; i < _nya_cache_ceiling_count; i++) {
        if (nya_string_equals(_nya_cache_ceilings[i].name, cache->options.name)) {
            row = &_nya_cache_ceilings[i];
            break;
        }
    }

    if (row == nullptr) {
        if (_nya_cache_ceiling_count >= NYA_CACHE_CEILING_MAX) {
            nya_log_warn("Cache '%s' is not shown as a ceiling: NYA_CACHE_CEILING_MAX (%d) names are taken.", cache->options.name,
                         NYA_CACHE_CEILING_MAX);
            return;
        }

        row  = &_nya_cache_ceilings[_nya_cache_ceiling_count++];
        *row = (_NYA_CacheCeiling){ .name = cache->options.name, .capacity = cache->options.capacity };

        // once per name, since the registry cannot forget a row.
        nya_ceiling_register(row->name, row->capacity, &row->live);
    }

    nya_assert(row->capacity == cache->options.capacity, "caches sharing the ceiling '%s' must share a capacity (" FMTu32 " and " FMTu32 ")",
               row->name, row->capacity, cache->options.capacity);

    row->caches++;
    cache->ceiling = row;

    _nya_cache_ceiling_update(cache);
}

void _nya_cache_ceiling_update(NYA_Cache* cache) {
    _NYA_CacheCeiling* row = cache->ceiling;
    if (row == nullptr) return;

    row->live = row->caches == 1 ? cache->count : nya_max(row->live, cache->count);
}

void _nya_cache_ceiling_detach(NYA_Cache* cache) {
    _NYA_CacheCeiling* row = cache->ceiling;
    if (row == nullptr) return;

    nya_assert(row->caches > 0);

    row->caches--;
    if (row->caches == 0) row->live = 0;

    cache->ceiling = nullptr;
}

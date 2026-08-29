#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void  _nya_arena_align_and_pad_size(NYA_Arena* arena, u64* size);
NYA_INTERNAL void  _nya_arena_region_destroy(NYA_Arena* arena, NYA_ArenaRegion* region);
NYA_INTERNAL void* _nya_arena_free_list_find(NYA_ArenaFreeList* free_list, u64 size) __attr_no_discard;
NYA_INTERNAL void  _nya_arena_free_list_add(NYA_ArenaRegion* region, void* ptr, u64 size);
NYA_INTERNAL void  _nya_arena_free_list_defragment(NYA_ArenaFreeList* free_list);
NYA_INTERNAL void  _nya_arena_free_list_destroy(NYA_ArenaFreeList* free_list);

NYA_INTERNAL NYA_ArenaActionCallback _nya_arena_action_callback = nullptr;

/*
 * ─────────────────────────────────────────────────────────
 * REGISTRY
 * ─────────────────────────────────────────────────────────
 */

/*
 * Fixed table of atomic slots, not a linked list behind a lock — base has no mutex, and adding one
 * for a debugging feature is the wrong trade. Slots are claimed with compare-exchange; a linear scan
 * of 256 pointers costs nothing next to creating an arena.
 */
NYA_INTERNAL atomic(NYA_Arena*) _nya_arena_registry[NYA_ARENA_REGISTRY_MAX];

/** Warned once rather than per arena, so overflowing does not bury the log it is trying to help with. */
NYA_INTERNAL atomic b8 _nya_arena_registry_overflow_reported = false;

/**
 * Slots currently claimed, kept alongside the CAS table purely so the ceiling registry has a plain
 * counter to point at — nya_arena_registry_count() already answers this by scanning, but scanning
 * on every read is not the shape nya_ceiling_register takes.
 * */
NYA_INTERNAL atomic u32 _nya_arena_registry_live_count = 0;

NYA_INTERNAL void _nya_arena_registry_add(NYA_Arena* arena);
NYA_INTERNAL void _nya_arena_registry_remove(NYA_Arena* arena);

/*
 * ─────────────────────────────────────────────────────────
 * CALLSITES
 * ─────────────────────────────────────────────────────────
 */

/*
 * One row per source location that has touched an arena. Fixed size and never compacted so a row's
 * address is stable while being read and written. Keyed on the file pointer, line and arena name:
 * file/function come from __FILE__/__FUNCTION__, the same literal at a given site, so pointer
 * comparison is correct and cheaper than strcmp on the hot path.
 *
 * Every field is atomic: the table is global, and two threads allocating from different arenas land
 * on the same row, incrementing counts and totals with nothing ordering them. Same trade as the
 * registry — atomics, not a mutex, since base has none.
 *
 * NYA_ArenaCallsiteStats stays a plain struct because it is returned by value and an _Atomic member
 * cannot be copied that way; readers assemble a snapshot from a row instead.
 */
#define _NYA_ARENA_CALLSITE_MAX 1024

/**
 * The stored form of a row. `file_name` doubles as the publication flag: written last with release
 * ordering, so a reader that sees it non-null sees every other field too; null means reserved but
 * not yet filled, and the reader skips it.
 * */
typedef struct {
    atomic(const char*) file_name;
    atomic u32          line_number;
    atomic(const char*) function_name;
    atomic(const char*) arena_name;

    atomic u64 alloc_count;
    atomic u64 free_count;
    atomic u64 allocated_bytes;
    atomic u64 freed_bytes;
    atomic s64 live_bytes;
} _NYA_ArenaCallsiteRow;

NYA_INTERNAL _NYA_ArenaCallsiteRow _nya_arena_callsites[_NYA_ARENA_CALLSITE_MAX];
NYA_INTERNAL atomic u32            _nya_arena_callsite_count             = 0;
NYA_INTERNAL atomic b8             _nya_arena_callsite_overflow_reported = false;

/** Finds or creates the row for this site. Null once the table is full. */
NYA_INTERNAL _NYA_ArenaCallsiteRow* _nya_arena_callsite_for(const char* arena_name, const char* file, u32 line, const char* function);
NYA_INTERNAL void                    _nya_arena_callsite_record_alloc(const char* arena_name, const char* file, u32 line, const char* function, u64 size);
NYA_INTERNAL void                    _nya_arena_callsite_record_free(const char* arena_name, const char* file, u32 line, const char* function, u64 size);

/** A row as the public API reports it. Fields are read individually; the row may move on under it. */
NYA_INTERNAL NYA_ArenaCallsiteStats _nya_arena_callsite_snapshot(const _NYA_ArenaCallsiteRow* row);

NYA_Arena* nya_arena_global = nullptr;
NYA_Arena* nya_arena_temp   = nullptr;

__attr_constructor NYA_INTERNAL void _nya_arena_init(void) {
    nya_arena_global = nya_arena_create(.name = "global_arena");
    nya_arena_temp   = nya_arena_create(.name = "temp_arena");
}

__attr_destructor NYA_INTERNAL void _nya_arena_shutdown(void) {
    nya_arena_destroy(nya_arena_global);
    nya_arena_destroy(nya_arena_temp);
    nya_arena_global = nullptr;
    nya_arena_temp   = nullptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * NON-DEBUG API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Arena* _nya_arena_nodebug_create_with_options(NYA_ArenaOptions options) {
    NYA_Arena* arena = nya_malloc(sizeof(NYA_Arena));
    *arena           = _nya_arena_nodebug_create_with_options_on_stack(options);

    // Heap arenas only: the on_stack variant returns by value, so there is no stable address to
    // register at the point it's built, and stack arenas are per-call scratch a report has no use for.
    _nya_arena_registry_add(arena);

    return arena;
}

NYA_Arena _nya_arena_nodebug_create_with_options_on_stack(NYA_ArenaOptions options) {
    nya_assert(options.region_size >= nya_kibyte_to_byte(4), "Region size must be at least 4 KiB.");
    nya_assert(options.region_size % options.alignment == 0, "Region size must be divisible by alignment.");
    nya_assert(options.alignment >= 8, "Alignment must be at least 8 bytes.");
    // `% 2 == 0` only said "even", which 24 and 40 also satisfy — and the alignment is used as a
    // mask, `(x + a - 1) & ~(a - 1)`, which is only an alignment operation when `a` is a power of
    // two. A non-power-of-two got through here and then silently misaligned every allocation.
    nya_assert((options.alignment & (options.alignment - 1)) == 0, "Alignment must be a power of two, got " FMTu8 ".", options.alignment);
    nya_assert(ASAN_PADDING % options.alignment == 0, "ASAN padding must be divisible by alignment.");

    NYA_Arena arena = {
        .options = options,
        .head    = nullptr,
        .tail    = nullptr,
    };

    return arena;
}

void* _nya_arena_nodebug_alloc(NYA_Arena* arena, u64 size) __attr_malloc {
    nya_assert(arena != nullptr);

    if (size == 0) return nullptr;

    _nya_arena_align_and_pad_size(arena, &size);

    if (size > arena->options.region_size) goto skip_search;

    nya_dll_foreach (arena, region) {
        if (region->free_list != nullptr && region->free_list->average_free_size >= (f32)size) {
            void* ptr = _nya_arena_free_list_find(region->free_list, size);
            if (ptr != nullptr) {
                asan_unpoison_memory_region(ptr, size - ASAN_PADDING);
                asan_poison_memory_region((u8*)ptr + size - ASAN_PADDING, ASAN_PADDING);

                return ptr;
            }
        }

        if (region->capacity - region->used >= size) {
            u8* ptr = region->memory + region->used;

            uintptr_t aligned_ptr = ((uintptr_t)ptr + (arena->options.alignment - 1)) & ~(arena->options.alignment - 1);
            u64       padding     = aligned_ptr - (uintptr_t)ptr;

            // enough space after alignment padding too?
            if (region->capacity - region->used - padding >= size) {
                ptr           = (u8*)aligned_ptr;
                region->used += size + padding;

                asan_unpoison_memory_region(ptr, size - ASAN_PADDING);
                asan_poison_memory_region(ptr + size - ASAN_PADDING, ASAN_PADDING);

                return ptr;
            }
        }

        continue;
    }

skip_search:
    /*
     * No region had space, or the search was skipped. The region must hold the allocation *plus*
     * alignment padding; nya_malloc guarantees only max_align_t (16 bytes), so sizing the region to
     * `size` alone and setting `used = size + initial_padding` put `used` past `capacity` — the
     * write ran off the malloc'd block and every later `capacity - used` check underflowed. Only
     * reachable when a single allocation larger than region_size decides the region size, on an
     * arena whose alignment exceeds malloc's.
     */
    u64              new_region_size   = nya_max(arena->options.region_size, size + arena->options.alignment - 1);
    NYA_ArenaRegion* new_region        = nya_malloc(sizeof(NYA_ArenaRegion));
    u8*              new_region_memory = nya_malloc(new_region_size);
    nya_assert(new_region != nullptr);
    nya_assert(new_region_memory != nullptr);

    uintptr_t aligned_memory  = ((uintptr_t)new_region_memory + (arena->options.alignment - 1)) & ~(arena->options.alignment - 1);
    u64       initial_padding = aligned_memory - (uintptr_t)new_region_memory;

    *new_region = (NYA_ArenaRegion){
        .used       = size + initial_padding,
        .capacity   = new_region_size,
        .memory     = new_region_memory,
        .gc_counter = 0,
        .free_list  = nullptr,
        .next       = nullptr,
        .prev       = nullptr,
    };
    nya_dll_node_push_back(arena, new_region);

    u8* ptr = (u8*)aligned_memory;

    asan_unpoison_memory_region(ptr, size - ASAN_PADDING);

    // The tail runs `capacity - initial_padding` bytes from `ptr` (capacity is from `memory`).
    // Leaving padding out of the subtraction poisoned `initial_padding` bytes past the block's end.
    asan_poison_memory_region(ptr + size - ASAN_PADDING, new_region->capacity - initial_padding - size + ASAN_PADDING);

    return ptr;
}

void* _nya_arena_nodebug_realloc(NYA_Arena* arena, void* ptr, u64 old_size, u64 new_size) {
    nya_assert(arena != nullptr);

    if (ptr == nullptr) return nullptr;
    if (new_size == old_size) return ptr;
    if (new_size == 0) {
        _nya_arena_nodebug_free(arena, ptr, old_size);
        return nullptr;
    }

    _nya_arena_align_and_pad_size(arena, &old_size);
    _nya_arena_align_and_pad_size(arena, &new_size);
    u8* old_ptr = (u8*)ptr;

    // realloc to smaller size = partial free
    if (new_size < old_size) {
        // use old memory as the asan padding
        asan_poison_memory_region(old_ptr + new_size - ASAN_PADDING, ASAN_PADDING);

        // only free the excess if it's larger than ASAN_PADDING
        if (old_size - new_size > ASAN_PADDING) { /**/
            _nya_arena_nodebug_free(arena, old_ptr + new_size, old_size - new_size - ASAN_PADDING);
        }

        return old_ptr;
    }

    nya_dll_foreach (arena, region) {
        if (!(region->memory <= old_ptr && old_ptr < region->memory + region->capacity)) continue;

        // no checking if there is a free slot after in the free list

        // if its the last allocation in the region, we can maybe just extend it
        if (old_ptr + old_size == region->memory + region->used && region->used + (new_size - old_size) <= region->capacity) {
            region->used += new_size - old_size;

            asan_unpoison_memory_region(old_ptr, new_size - ASAN_PADDING);

            return old_ptr;
        }

        // allocate new memory and copy, dont double dipp on the padding
        void* new_ptr = _nya_arena_nodebug_alloc(arena, new_size - ASAN_PADDING);
        nya_memmove(new_ptr, old_ptr, old_size - ASAN_PADDING);
        _nya_arena_nodebug_free(arena, old_ptr, old_size - ASAN_PADDING);

        return new_ptr;
    }

    nya_unreachable();
}

void _nya_arena_nodebug_free(NYA_Arena* arena, void* ptr, u64 size) {
    nya_assert(arena != nullptr);

    if (ptr == nullptr || size == 0) return;

    _nya_arena_align_and_pad_size(arena, &size);
    u8* casted_ptr = (u8*)ptr;

    asan_poison_memory_region(ptr, size);

    nya_dll_foreach (arena, region) {
        if (!(region->memory <= casted_ptr && casted_ptr < region->memory + region->capacity)) continue;

        // last allocation, just move the used pointer back
        if (casted_ptr + size == region->memory + region->used) {
            region->used -= size;
            return;
        }

        // add to free list otherwise
        _nya_arena_free_list_add(region, ptr, size);

        // maybe defragment
        if (arena->options.defragmentation_enabled && region->free_list->defragmentation_counter >= arena->options.defragmentation_threshold) {
            _nya_arena_free_list_defragment(region->free_list);
        }

        return;
    }

    nya_unreachable();
}

void _nya_arena_nodebug_free_all(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    for (NYA_ArenaRegion* region = arena->head; region != nullptr;) {
        NYA_ArenaRegion* next = region->next;

        // deallocate region if unused long enough
        if (arena->options.defragmentation_enabled && region->used == 0 && region->gc_counter++ >= arena->options.garbage_collection_threshold) {
            _nya_arena_region_destroy(arena, region);
            region = next;
            continue;
        }

        /*
         * Poison what was handed out, not the whole region: poisoning the full capacity scales with
         * region size, not usage, and a region defaults to a gibyte — so resetting an arena holding
         * a few kilobytes wrote 128 MiB of shadow memory every time, faulting in shadow pages that
         * are never released and reading as unbounded growth no leak checker attributes to anything.
         * The tail beyond `used` was never handed out, so only the prefix needs poisoning again.
         */
        u64 used_before = region->used;

        region->used = 0;

        asan_poison_memory_region(region->memory, used_before);

        if (region->free_list != nullptr) {
            _nya_arena_free_list_destroy(region->free_list);
            region->free_list = nullptr;
        }

        region = next;
    }
}

void _nya_arena_nodebug_garbage_collect(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    for (NYA_ArenaRegion* region = arena->head; region != nullptr;) {
        NYA_ArenaRegion* next = region->next;
        if (region->used > 0) {
            region = next;
            continue;
        }

        _nya_arena_region_destroy(arena, region);
        region = next;
    }
}

void _nya_arena_nodebug_destroy(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    // Before the memory goes away, so a report racing this never dereferences a freed arena.
    _nya_arena_registry_remove(arena);

    _nya_arena_nodebug_destroy_on_stack(arena);

    nya_free(arena);
}

void _nya_arena_nodebug_destroy_on_stack(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    for (NYA_ArenaRegion* region = arena->head; region != nullptr;) {
        NYA_ArenaRegion* next = region->next;
        _nya_arena_region_destroy(arena, region);
        region = next;
    }

    arena->head = nullptr;
    arena->tail = nullptr;
}

void* _nya_arena_nodebug_copy(NYA_Arena* dst, void* ptr, u64 size) {
    nya_assert(dst != nullptr);
    if (ptr == nullptr || size == 0) return nullptr;

    void* new_ptr = nya_arena_alloc(dst, size);
    if (!new_ptr) return nullptr;

    nya_memmove(new_ptr, ptr, size);
    return new_ptr;
}

void* _nya_arena_nodebug_move(NYA_Arena* src, NYA_Arena* dst, void* ptr, u64 size) {
    nya_assert(src != nullptr);
    nya_assert(dst != nullptr);

    if (ptr == nullptr || size == 0) return nullptr;

    void* new_ptr = nya_arena_alloc(dst, size);
    if (!new_ptr) return nullptr;

    nya_memmove(new_ptr, ptr, size);
    nya_arena_free(src, ptr, size);
    return new_ptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DEBUG API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Arena* _nya_arena_debug_create_with_options(NYA_ArenaOptions options, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    NYA_ArenaAction action = {
        .type          = NYA_ARENA_ACTION_ARENA_NEW,
        .arena_name    = options.name,
        .file_name     = file,
        .line_number   = line,
        .function_name = function,
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    return _nya_arena_nodebug_create_with_options(options);
}

NYA_Arena _nya_arena_debug_create_with_options_on_stack(NYA_ArenaOptions options, const char* function, const char* file, u32 line) {
    NYA_ArenaAction action = {
        .type          = NYA_ARENA_ACTION_ARENA_NEW,
        .arena_name    = options.name,
        .file_name     = file,
        .line_number   = line,
        .function_name = function,
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    return _nya_arena_nodebug_create_with_options_on_stack(options);
}

void* _nya_arena_debug_alloc(NYA_Arena* arena, u64 size, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    void* ptr = _nya_arena_nodebug_alloc(arena, size);
    if (ptr == nullptr) return nullptr;

    NYA_ArenaAction action = {
      .type          = NYA_ARENA_ACTION_ALLOC,
      .arena_name    = arena->options.name,
      .file_name     = file,
      .line_number   = line,
      .function_name = function,
      .as_alloc      = {
          .ptr  = ptr,
          .size = size,
      },
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);
    _nya_arena_callsite_record_alloc(arena->options.name, file, line, function, size);

    return ptr;
}

void* _nya_arena_debug_realloc(NYA_Arena* arena, void* ptr, u64 old_size, u64 new_size, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    void* new_ptr = _nya_arena_nodebug_realloc(arena, ptr, old_size, new_size);
    if (new_ptr == nullptr && new_size != 0) return nullptr;

    NYA_ArenaAction action = {
      .type          = NYA_ARENA_ACTION_REALLOC,
      .arena_name    = arena->options.name,
      .file_name     = file,
      .line_number   = line,
      .function_name = function,
      .as_realloc    = {
          .old_ptr  = ptr,
          .old_size = old_size,
          .new_ptr  = new_ptr,
          .new_size = new_size,
      },
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    // Booked as a free of the old block plus an alloc of the new one, so live_bytes tracks the delta
    // — a growing array reallocating repeatedly shows its current size, not the sum of every size.
    _nya_arena_callsite_record_free(arena->options.name, file, line, function, old_size);
    _nya_arena_callsite_record_alloc(arena->options.name, file, line, function, new_size);

    return new_ptr;
}

void _nya_arena_debug_free(NYA_Arena* arena, void* ptr, u64 size, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    NYA_ArenaAction action = {
      .type          = NYA_ARENA_ACTION_FREE,
      .arena_name    = arena->options.name,
      .file_name     = file,
      .line_number   = line,
      .function_name = function,
      .as_free       = {
          .ptr  = ptr,
          .size = size,
      },
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);
    _nya_arena_callsite_record_free(arena->options.name, file, line, function, size);

    _nya_arena_nodebug_free(arena, ptr, size);
}

void _nya_arena_debug_free_all(NYA_Arena* arena, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    NYA_ArenaAction action = {
        .type          = NYA_ARENA_ACTION_FREE_ALL,
        .arena_name    = arena->options.name,
        .file_name     = file,
        .line_number   = line,
        .function_name = function,
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    _nya_arena_nodebug_free_all(arena);
}

void _nya_arena_debug_garbage_collect(NYA_Arena* arena, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    NYA_ArenaAction action = {
        .type          = NYA_ARENA_ACTION_GARBAGE_COLLECT,
        .arena_name    = arena->options.name,
        .file_name     = file,
        .line_number   = line,
        .function_name = function,
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    _nya_arena_nodebug_garbage_collect(arena);
}

void _nya_arena_debug_destroy(NYA_Arena* arena, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    NYA_ArenaAction action = {
        .type          = NYA_ARENA_ACTION_ARENA_DESTROY,
        .arena_name    = arena->options.name,
        .file_name     = file,
        .line_number   = line,
        .function_name = function,
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    _nya_arena_nodebug_destroy(arena);
}

void _nya_arena_debug_destroy_on_stack(NYA_Arena* arena, const char* function, const char* file, u32 line) {
    NYA_ArenaAction action = {
        .type          = NYA_ARENA_ACTION_ARENA_DESTROY,
        .arena_name    = arena->options.name,
        .file_name     = file,
        .line_number   = line,
        .function_name = function,
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    _nya_arena_nodebug_destroy_on_stack(arena);
}

void* _nya_arena_debug_copy(NYA_Arena* dst, void* ptr, u64 size, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    void* copy_ptr = _nya_arena_nodebug_copy(dst, ptr, size);
    if (copy_ptr == nullptr) return nullptr;

    NYA_ArenaAction action = {
      .type          = NYA_ARENA_ACTION_COPY,
      .arena_name    = dst->options.name,
      .file_name     = file,
      .line_number   = line,
      .function_name = function,
      .as_copy       = {
          .ptr      = ptr,
          .size     = size,
          .copy_ptr = copy_ptr,
      },
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    return copy_ptr;
}

void* _nya_arena_debug_move(NYA_Arena* src, NYA_Arena* dst, void* ptr, u64 size, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    void* move_ptr = _nya_arena_nodebug_move(src, dst, ptr, size);
    if (move_ptr == nullptr) return nullptr;

    NYA_ArenaAction action = {
      .type          = NYA_ARENA_ACTION_MOVE,
      .arena_name    = dst->options.name,
      .file_name     = file,
      .line_number   = line,
      .function_name = function,
      .as_move       = {
          .ptr             = ptr,
          .size            = size,
          .move_arena_name = dst->options.name,
          .move_ptr        = move_ptr,
      },
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    return move_ptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_arena_actions_set_callback(NYA_ArenaActionCallback callback) {
    _nya_arena_action_callback = callback;
}

u64 nya_arena_memory_usage_bytes(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    u64 total_usage = 0;
    nya_dll_foreach (arena, region) total_usage += region->used;

    return total_usage;
}

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

NYA_ArenaStats nya_arena_stats(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    NYA_ArenaStats stats = { .name = arena->options.name };

    nya_dll_foreach (arena, region) {
        stats.region_count++;
        stats.reserved_bytes += region->capacity;
        stats.used_bytes     += region->used;

        if (region->free_list == nullptr) continue;

        for (NYA_ArenaFreeListNode* node = region->free_list->head; node != nullptr; node = node->next) {
            stats.free_list_nodes++;
            stats.free_list_bytes += node->size;
            if (node->size > stats.largest_free_block) stats.largest_free_block = node->size;
        }
    }

    // Zero when nothing is free: an arena with an empty free list is full, not fragmented, and
    // reporting 1.0 there would send someone looking for a defragmentation bug that is not present.
    if (stats.free_list_bytes > 0) {
        stats.fragmentation = 1.0F - ((f32)stats.largest_free_block / (f32)stats.free_list_bytes);
    }

    return stats;
}

u32 nya_arena_registry_count(void) {
    u32 count = 0;
    for (u32 i = 0; i < NYA_ARENA_REGISTRY_MAX; i++) {
        if (atomic_load(&_nya_arena_registry[i]) != nullptr) count++;
    }
    return count;
}

NYA_Arena* nya_arena_registry_at(u32 index) {
    // Indexes the occupied slots rather than the raw table, so a caller walking 0..count-1 sees
    // every arena. The table is sparse: a destroyed arena leaves a hole that the next create fills.
    u32 seen = 0;
    for (u32 i = 0; i < NYA_ARENA_REGISTRY_MAX; i++) {
        NYA_Arena* arena = atomic_load(&_nya_arena_registry[i]);
        if (arena == nullptr) continue;
        if (seen == index) return arena;
        seen++;
    }
    return nullptr;
}

void nya_arena_stats_report(void) {
    u32 count = nya_arena_registry_count();
    if (count == 0) {
        nya_log_info("Arena: no registered arenas.");
        return;
    }

    nya_log_info("Arena: %-28s %8s %12s %12s %8s %12s %6s", "name", "regions", "used", "reserved", "free n", "free bytes", "frag");

    u64 total_used     = 0;
    u64 total_reserved = 0;

    for (u32 i = 0; i < count; i++) {
        NYA_Arena* arena = nya_arena_registry_at(i);
        if (arena == nullptr) continue;

        NYA_ArenaStats stats = nya_arena_stats(arena);
        total_used          += stats.used_bytes;
        total_reserved      += stats.reserved_bytes;

        nya_log_info(
            "Arena: %-28s %8" PRIu64 " %12" PRIu64 " %12" PRIu64 " %8" PRIu64 " %12" PRIu64 " %5.1f%%",
            stats.name != nullptr ? stats.name : "(unnamed)",
            stats.region_count,
            stats.used_bytes,
            stats.reserved_bytes,
            stats.free_list_nodes,
            stats.free_list_bytes,
            (f64)stats.fragmentation * 100.0
        );
    }

    nya_log_info("Arena: %-28s %8s %12" PRIu64 " %12" PRIu64, "TOTAL", "", total_used, total_reserved);
}

/*
 * ─────────────────────────────────────────────────────────
 * CALLSITES
 * ─────────────────────────────────────────────────────────
 */

u32 nya_arena_callsite_count(void) {
    u32 count = atomic_load(&_nya_arena_callsite_count);
    return count > _NYA_ARENA_CALLSITE_MAX ? _NYA_ARENA_CALLSITE_MAX : count;
}

NYA_ArenaCallsiteStats nya_arena_callsite_at(u32 index) {
    if (index >= nya_arena_callsite_count()) return (NYA_ArenaCallsiteStats){ 0 };
    return _nya_arena_callsite_snapshot(&_nya_arena_callsites[index]);
}

void nya_arena_callsites_reset(void) {
    // Field by field rather than nya_memset, which has nothing to say about atomics. Count first, so
    // a thread recording concurrently reserves from the start of the table rather than writing into
    // a row this is in the middle of clearing.
    atomic_store(&_nya_arena_callsite_count, 0);

    for (u32 i = 0; i < _NYA_ARENA_CALLSITE_MAX; i++) {
        _NYA_ArenaCallsiteRow* row = &_nya_arena_callsites[i];

        atomic_store(&row->alloc_count, 0);
        atomic_store(&row->free_count, 0);
        atomic_store(&row->allocated_bytes, 0);
        atomic_store(&row->freed_bytes, 0);
        atomic_store(&row->live_bytes, 0);
        atomic_store(&row->line_number, 0);
        atomic_store(&row->function_name, nullptr);
        atomic_store(&row->arena_name, nullptr);

        // Last, matching the publication order in _nya_arena_callsite_for.
        atomic_store_explicit(&row->file_name, nullptr, memory_order_release);
    }

    atomic_store(&_nya_arena_callsite_overflow_reported, false);
}

void nya_arena_callsites_report(u32 limit) {
    // Sampled once and worked from, rather than re-read: the table can grow underneath a report and
    // the ordering below has to be against a fixed set of rows.
    NYA_ArenaCallsiteStats rows[_NYA_ARENA_CALLSITE_MAX];
    u32                    row_count = 0;

    for (u32 i = 0; i < nya_arena_callsite_count(); i++) {
        NYA_ArenaCallsiteStats row = _nya_arena_callsite_snapshot(&_nya_arena_callsites[i]);
        if (row.file_name == nullptr) continue; // reserved but not yet published

        rows[row_count++] = row;
    }

    if (row_count == 0) {
        // Says which, because "no allocations" and "not a debug build" look identical in a log and
        // only one of them is worth investigating.
        nya_log_info("Arena: no callsites recorded (debug builds only).");
        return;
    }

    /*
     * Selection sort over indices, taking only the top `limit`. The snapshot itself is not reordered
     * — rows are addressed by index through nya_arena_callsite_at, and sorting in place would change
     * that ordering under a caller between two calls.
     */
    u32 count = row_count;
    if (limit == 0 || limit > count) limit = count;

    u32 order[_NYA_ARENA_CALLSITE_MAX];
    for (u32 i = 0; i < count; i++) order[i] = i;

    for (u32 i = 0; i < limit; i++) {
        u32 best = i;
        for (u32 j = i + 1; j < count; j++) {
            if (rows[order[j]].live_bytes > rows[order[best]].live_bytes) best = j;
        }
        u32 swap    = order[i];
        order[i]    = order[best];
        order[best] = swap;
    }

    nya_log_info("Arena: top %" PRIu32 " callsites by live bytes, of %" PRIu32 " recorded", limit, count);
    nya_log_info("Arena: %-40s %-20s %10s %12s %12s %12s", "site", "arena", "allocs", "allocated", "freed", "live");

    for (u32 i = 0; i < limit; i++) {
        const NYA_ArenaCallsiteStats* row = &rows[order[i]];

        // Trimmed to the basename: the full path is the same prefix for every row and pushes the
        // numbers off the side of a terminal.
        const char* file = row->file_name != nullptr ? row->file_name : "?";
        const char* slash = strrchr(file, '/');
        if (slash != nullptr) file = slash + 1;

        char site[64];
        (void)snprintf(site, sizeof(site), "%s:%" PRIu32 " %s", file, row->line_number, row->function_name != nullptr ? row->function_name : "");

        nya_log_info(
            "Arena: %-40s %-20s %10" PRIu64 " %12" PRIu64 " %12" PRIu64 " %12" PRId64,
            site,
            row->arena_name != nullptr ? row->arena_name : "(unnamed)",
            row->alloc_count,
            row->allocated_bytes,
            row->freed_bytes,
            row->live_bytes
        );
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_arena_registry_add(NYA_Arena* arena) {
    for (u32 i = 0; i < NYA_ARENA_REGISTRY_MAX; i++) {
        NYA_Arena* expected = nullptr;
        if (atomic_compare_exchange_strong(&_nya_arena_registry[i], &expected, arena)) {
            atomic_fetch_add(&_nya_arena_registry_live_count, 1);

#ifndef NYA_NO_SDL
            // Registered once, on the first arena the process ever creates — there is no dedicated
            // init to hang this on, since a zeroed CAS table is already a valid empty registry. Base
            // has no ceiling registry of its own — see core_ceiling.h — so this is skipped in a
            // -DNYA_NO_SDL build, which excludes core entirely (the build tool itself is one).
            // The cast is safe in practice though not strictly conforming: a lock-free atomic u32
            // has the same representation as a plain one on every compiler this engine targets, and
            // the ceiling registry only ever reads through the pointer, never writes.
            static atomic b8 ceiling_registered = false;
            if (!atomic_exchange(&ceiling_registered, true)) {
                nya_ceiling_register("arenas", NYA_ARENA_REGISTRY_MAX, (const u32*)&_nya_arena_registry_live_count);
            }
#endif

            return;
        }
    }

    // Not fatal. The arena works; it is simply not listed, and a registry that refused to create
    // arenas past a limit would turn a debugging aid into a failure mode.
    if (!atomic_exchange(&_nya_arena_registry_overflow_reported, true)) {
        nya_log_warn("Arena registry is full at %d entries, further arenas will not be listed.", NYA_ARENA_REGISTRY_MAX);
    }
}

void _nya_arena_registry_remove(NYA_Arena* arena) {
    for (u32 i = 0; i < NYA_ARENA_REGISTRY_MAX; i++) {
        NYA_Arena* expected = arena;
        if (atomic_compare_exchange_strong(&_nya_arena_registry[i], &expected, nullptr)) {
            atomic_fetch_sub(&_nya_arena_registry_live_count, 1);
            return;
        }
    }
}

NYA_ArenaCallsiteStats _nya_arena_callsite_snapshot(const _NYA_ArenaCallsiteRow* row) {
    // Acquire on the publication field, so everything below is the filled row rather than whatever
    // a reserved-but-unwritten one happens to hold.
    const char* file = atomic_load_explicit(&row->file_name, memory_order_acquire);
    if (file == nullptr) return (NYA_ArenaCallsiteStats){ 0 };

    return (NYA_ArenaCallsiteStats){
        .file_name       = file,
        .line_number     = atomic_load(&row->line_number),
        .function_name   = atomic_load(&row->function_name),
        .arena_name      = atomic_load(&row->arena_name),
        .alloc_count     = atomic_load(&row->alloc_count),
        .free_count      = atomic_load(&row->free_count),
        .allocated_bytes = atomic_load(&row->allocated_bytes),
        .freed_bytes     = atomic_load(&row->freed_bytes),
        .live_bytes      = atomic_load(&row->live_bytes),
    };
}

_NYA_ArenaCallsiteRow* _nya_arena_callsite_for(const char* arena_name, const char* file, u32 line, const char* function) {
#ifndef NYA_NO_SDL
    // Registered once, on the first callsite this process ever records. See _nya_arena_registry_add
    // for why this cannot hang off a dedicated init instead, why it is skipped under -DNYA_NO_SDL,
    // and why the counter is read through a plain pointer despite being atomic.
    static atomic b8 ceiling_registered = false;
    if (!atomic_exchange(&ceiling_registered, true)) {
        nya_ceiling_register("arena_callsites", _NYA_ARENA_CALLSITE_MAX, (const u32*)&_nya_arena_callsite_count);
    }
#endif

    u32 count = atomic_load(&_nya_arena_callsite_count);
    if (count > _NYA_ARENA_CALLSITE_MAX) count = _NYA_ARENA_CALLSITE_MAX;

    for (u32 i = 0; i < count; i++) {
        _NYA_ArenaCallsiteRow* row = &_nya_arena_callsites[i];

        // Reserved by another thread but not filled in yet. Skipping it may cost a duplicate row,
        // which is the trade below.
        const char* row_file = atomic_load_explicit(&row->file_name, memory_order_acquire);
        if (row_file == nullptr) continue;

        // Pointer comparison on file: both sides come from __FILE__ at the same site, which the
        // compiler pools into one literal. The arena name is compared by content because two arenas
        // may be named by different literals and still be the same subsystem.
        if (atomic_load(&row->line_number) != line) continue;
        if (row_file != file) continue;

        const char* row_arena = atomic_load(&row->arena_name);
        if (row_arena != arena_name && !(row_arena && arena_name && strcmp(row_arena, arena_name) == 0)) continue;

        return row;
    }

    /*
     * A slot reserved outright, not claimed with a compare-exchange against the row: two threads
     * touching the same site simultaneously can both reserve, leaving two rows whose totals must be
     * added to read the truth. Accepted cost — the window is only the first allocation from a given
     * line, and the alternative is a lock on the hot path of every debug-build allocation.
     */
    u32 index = atomic_fetch_add(&_nya_arena_callsite_count, 1);

    if (index >= _NYA_ARENA_CALLSITE_MAX) {
        // Pinned, so a long run does not carry the counter away from the table size and back around.
        atomic_store(&_nya_arena_callsite_count, _NYA_ARENA_CALLSITE_MAX);

        if (!atomic_exchange(&_nya_arena_callsite_overflow_reported, true)) {
            nya_log_warn("Arena callsite table is full at %d entries, further sites will not be tracked.", _NYA_ARENA_CALLSITE_MAX);
        }
        return nullptr;
    }

    _NYA_ArenaCallsiteRow* row = &_nya_arena_callsites[index];

    atomic_store(&row->line_number, line);
    atomic_store(&row->function_name, function);
    atomic_store(&row->arena_name, arena_name);
    atomic_store(&row->alloc_count, 0);
    atomic_store(&row->free_count, 0);
    atomic_store(&row->allocated_bytes, 0);
    atomic_store(&row->freed_bytes, 0);
    atomic_store(&row->live_bytes, 0);

    // Published last: this is what makes the row visible to a reader, and to the scan above.
    atomic_store_explicit(&row->file_name, file, memory_order_release);

    return row;
}

void _nya_arena_callsite_record_alloc(const char* arena_name, const char* file, u32 line, const char* function, u64 size) {
    _NYA_ArenaCallsiteRow* row = _nya_arena_callsite_for(arena_name, file, line, function);
    if (row == nullptr) return;

    atomic_fetch_add(&row->alloc_count, 1);
    atomic_fetch_add(&row->allocated_bytes, size);
    atomic_fetch_add(&row->live_bytes, (s64)size);
}

void _nya_arena_callsite_record_free(const char* arena_name, const char* file, u32 line, const char* function, u64 size) {
    _NYA_ArenaCallsiteRow* row = _nya_arena_callsite_for(arena_name, file, line, function);
    if (row == nullptr) return;

    atomic_fetch_add(&row->free_count, 1);
    atomic_fetch_add(&row->freed_bytes, size);
    atomic_fetch_sub(&row->live_bytes, (s64)size);
}

NYA_INTERNAL void _nya_arena_align_and_pad_size(NYA_Arena* arena, u64* size) {
    nya_assert(arena != nullptr);
    nya_assert(size != nullptr);

    if (*size == 0) return;

    *size = ((*size + (arena->options.alignment - 1)) & ~(arena->options.alignment - 1)) + ASAN_PADDING;
}

NYA_INTERNAL void _nya_arena_region_destroy(NYA_Arena* arena, NYA_ArenaRegion* region) {
    nya_dll_node_unlink(arena, region);

    if (region->free_list != nullptr) _nya_arena_free_list_destroy(region->free_list);
    nya_free(region->memory);
    nya_free(region);
}

NYA_INTERNAL void* _nya_arena_free_list_find(NYA_ArenaFreeList* free_list, u64 size) {
    nya_assert(free_list != nullptr);

    nya_dll_foreach (free_list, node) {
        if (node->size < size) continue;

        // exact fit
        if (node->size == size) {
            void* ptr = node->ptr;

            nya_dll_node_unlink(free_list, node);
            nya_free(node);

            free_list->node_counter--;
            if (free_list->node_counter == 0) {
                free_list->average_free_size = 0.0F;
            } else {
                free_list->average_free_size =
                    (free_list->average_free_size * (f32)(free_list->node_counter + 1) - (f32)size) / (f32)free_list->node_counter;
            }

            return ptr;
        }

        // free node is bigger than we need
        void* ptr = node->ptr;

        node->ptr                     = (u8*)node->ptr + size;
        node->size                   -= size;
        free_list->average_free_size  = (free_list->average_free_size * (f32)free_list->node_counter - (f32)size) / (f32)(free_list->node_counter);

        return ptr;
    }

    return nullptr;
}

NYA_INTERNAL void _nya_arena_free_list_add(NYA_ArenaRegion* region, void* ptr, u64 size) {
    nya_assert(ptr != nullptr);

    if (region->free_list == nullptr) {
        region->free_list = nya_malloc(sizeof(NYA_ArenaFreeList));
        nya_assert(region->free_list != nullptr);

        *region->free_list = (NYA_ArenaFreeList){
            .node_counter            = 0,
            .average_free_size       = 0.0F,
            .defragmentation_counter = 0,
            .head                    = nullptr,
            .tail                    = nullptr,
        };
    }

    NYA_ArenaFreeListNode* new_node = nya_malloc(sizeof(NYA_ArenaFreeListNode));
    nya_assert(new_node != nullptr);

    *new_node = (NYA_ArenaFreeListNode){
        .ptr  = ptr,
        .size = size,
        .prev = nullptr,
        .next = nullptr,
    };

    /*
     * Kept sorted by address — the invariant _nya_arena_free_list_defragment depends on, since it
     * only merges a node with its immediate successor. The walk below always links *after* the node
     * it stopped at, so it can't place a block below the current head; that case is what the push
     * front is for. Freeing a higher block before a lower one hits it, and without this a region
     * that had just released a contiguous span would refuse to allocate out of it and grow instead.
     */
    if (region->free_list->head == nullptr) {
        region->free_list->head = new_node;
        region->free_list->tail = new_node;
    } else if ((u8*)new_node->ptr < (u8*)region->free_list->head->ptr) {
        nya_dll_node_push_front(region->free_list, new_node);
    } else {
        nya_dll_foreach (region->free_list, free_node) {
            if (free_node->next && (u8*)free_node->next->ptr < (u8*)new_node->ptr) continue;

            // we are now in the situation free_node < new_node < free_node->next by address
            nya_dll_node_link(region->free_list, free_node, new_node, free_node->next);
            break;
        }
    }

    if (region->free_list->node_counter == 0) {
        region->free_list->node_counter      = 1;
        region->free_list->average_free_size = (f32)size;
    } else {
        region->free_list->average_free_size =
            ((region->free_list->average_free_size * (f32)region->free_list->node_counter) + (f32)size) / (f32)(region->free_list->node_counter + 1);
        region->free_list->node_counter++;
    }

    /*
     * Counts frees since the last defragmentation, compared against the threshold in
     * _nya_arena_nodebug_free. Previously nothing incremented this, so the comparison was always
     * 0 >= threshold and defragmentation never ran — adjacent free blocks stayed unmerged and a
     * region that had released a contiguous span could not satisfy an allocation that size.
     * Saturating, not wrapping: the counter is a u8, defragmentation can be turned off (so nothing
     * resets it), and FLAGS_SANITIZE treats unsigned wraparound as an error.
     * */
    if (region->free_list->defragmentation_counter < U8_MAX) region->free_list->defragmentation_counter++;
}

NYA_INTERNAL void _nya_arena_free_list_defragment(NYA_ArenaFreeList* free_list) {
    nya_assert(free_list != nullptr);

    for (NYA_ArenaFreeListNode* node = free_list->head; node != nullptr && node->next != nullptr;) {
        // check if current node is directly before the next node
        if ((u8*)node->ptr + node->size == (u8*)node->next->ptr) {
            NYA_ArenaFreeListNode* next      = node->next;
            u64                    next_size = next->size;

            // merge nodes
            node->size += next_size;
            nya_dll_node_unlink(free_list, next);
            nya_free(next);
            free_list->node_counter--;

            // continue with the same node
            continue;
        }

        node = node->next;
    }

    /*
     * Recomputed rather than adjusted as nodes merge: the incremental form subtracted the absorbed
     * node's size from the running total, but merging two free blocks releases no bytes — it just
     * means one node describes what two did. Every merge pushed the average further below the truth,
     * and since _nya_arena_nodebug_alloc only searches the free list when average_free_size is at
     * least the requested size, a region that had just coalesced a large span would refuse to
     * allocate out of it and grow instead. Sixteen freed blocks in a row was enough to see it. The
     * walk matches the merge loop's order above, so this costs nothing worth saving.
     * */
    u64 total = 0;
    u64 count = 0;
    for (NYA_ArenaFreeListNode* node = free_list->head; node != nullptr; node = node->next) {
        total += node->size;
        count++;
    }

    free_list->node_counter      = count;
    free_list->average_free_size = count == 0 ? 0.0F : (f32)total / (f32)count;

    free_list->defragmentation_counter = 0;
}

NYA_INTERNAL void _nya_arena_free_list_destroy(NYA_ArenaFreeList* free_list) {
    nya_assert(free_list != nullptr);

    for (NYA_ArenaFreeListNode* node = free_list->head; node != nullptr;) {
        NYA_ArenaFreeListNode* next = node->next;
        nya_free(node);
        node = next;
    }

    nya_free(free_list);
}

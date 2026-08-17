/**
 * @file base_arena.h
 *
 * This is the memory allocator used across everything.
 *
 * Example:
 * ```c
 * NYA_Arena* arena = nya_arena_create(.name = "my_arena", .alignment = 16, ...);
 * defer nya_arena_destroy(arena);
 * u8* data  = nya_arena_alloc(arena, 256);
 * ```
 *
 * In debug, or when forced, all arena functions are proxied and logged. Register a callback with
 * `nya_arena_actions_set_callback` to receive them.
 *
 * For the question that callback does not answer — "what does this arena look like right now" —
 * see NYA_ArenaStats and nya_arena_stats, and nya_arena_registry_* for walking every live arena at
 * once.
 *
 * ## An arena is not thread safe
 *
 * There is no lock anywhere in here. Allocating, freeing, resetting or destroying one arena from two
 * threads at once corrupts its region list, and nothing detects it — the region walk simply follows
 * a pointer another thread is in the middle of changing. That is the deliberate trade: an arena is a
 * bump allocator and a lock would be most of its cost.
 *
 * So give a thread its own arena. `nya_arena_global` and `nya_arena_temp` belong to the main thread,
 * and a job that wants scratch memory creates one, uses it and destroys it inside the job. See
 * tests/nyangine/core/test_job.c, which does exactly that and says why.
 *
 * The things that *are* safe to touch from anywhere are the process wide tables rather than any
 * arena: the registry behind nya_arena_registry_count / nya_arena_registry_at, and the callsite table
 * behind nya_arena_callsite_count / nya_arena_callsite_at. Both are built out of atomics, because
 * arenas on different threads all record into them.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_memory.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_ArenaActionType       NYA_ArenaActionType;
typedef struct NYA_Arena               NYA_Arena;
typedef struct NYA_ArenaCallsiteStats  NYA_ArenaCallsiteStats;
typedef struct NYA_ArenaFreeList       NYA_ArenaFreeList;
typedef struct NYA_ArenaFreeListNode   NYA_ArenaFreeListNode;
typedef struct NYA_ArenaOptions        NYA_ArenaOptions;
typedef struct NYA_ArenaRegion         NYA_ArenaRegion;
typedef struct NYA_ArenaStats          NYA_ArenaStats;
typedef struct NYA_ArneaAction         NYA_ArenaAction;

/*
 * ─────────────────────────────────────────────────────────
 * ARENA STRUCTS
 * ─────────────────────────────────────────────────────────
 */

/**
 * A region is *allocated*, not reserved: see _nya_arena_nodebug_alloc, which calls nya_malloc for
 * max(region_size, size). The region size is therefore real memory the moment it is touched, not
 * address space, and every cost that scales with it — allocating it, freeing it, poisoning it on
 * reset — is paid whether or not the arena holds anything.
 *
 * Sixty-four mebibytes, down from a gibyte. The gibyte was chosen as "an arena should never have to
 * grow", but measured against the engine actually running, the largest live arena holds a little
 * over one mebibyte: it bought nothing and cost a gibyte-sized malloc per arena, a gibyte-sized
 * free whenever garbage collection reclaimed a region, and — until that was fixed — a gibyte of
 * sanitizer shadow written on every reset.
 *
 * This is a floor, not a limit. A single allocation larger than a region gets a region of its own,
 * so nothing breaks if a subsystem outgrows it; it simply grows in sixty-four mebibyte steps.
 * */
#define _NYA_ARENA_DEFAULT_OPTIONS                                                                                                                   \
    .name = nullptr, .alignment = 16, .region_size = nya_mebyte_to_byte(64UL), .defragmentation_enabled = true, .defragmentation_threshold = 16,     \
    .garbage_collection_enabled = true, .garbage_collection_threshold = 3

/**
 * The same, for a stack arena, whose region is sized for scratch rather than for a subsystem.
 *
 * A stack arena is per-call scratch by construction — it is returned by value, it cannot be
 * registered, and it is destroyed before the function returns. Sharing the heap default meant every
 * one of them malloc'd a gibyte on its first allocation and freed it moments later. In a per-frame
 * caller that is a gibyte allocated and freed sixty times a second: under AddressSanitizer, whose
 * secondary allocator quarantines large freed blocks rather than returning them, it read as a
 * runaway memory leak that no leak checker would report, because nothing was actually leaked.
 *
 * Sixty-four kibibytes covers the scratch use — a few arrays sized by node or field count — and
 * anything larger still works: a single allocation bigger than the region gets a region of its own,
 * because the region size is a floor rather than a limit.
 * */
#define _NYA_ARENA_DEFAULT_OPTIONS_ON_STACK                                                                                                          \
    .name = nullptr, .alignment = 16, .region_size = nya_kibyte_to_byte(64UL), .defragmentation_enabled = true, .defragmentation_threshold = 16,     \
    .garbage_collection_enabled = true, .garbage_collection_threshold = 3

struct NYA_ArenaOptions {
    const char* name;

    u8 alignment;

    u64 region_size;

    /**
     * When enabled, the free list will merge adjacent free blocks after `defragmentation_threshold` number of frees.
     * */
    b8 defragmentation_enabled;
    u8 defragmentation_threshold;

    /**
     * Every free_all will increase a counter for unused regions.
     * When the counter reaches this value, the region will be freed.
     * Calling nya_arena_garbage_collect will free all unused regions regardless of the counter.
     * */
    b8 garbage_collection_enabled;
    u8 garbage_collection_threshold;
};

struct NYA_Arena {
    NYA_ArenaOptions options;
    NYA_ArenaRegion *head, *tail;
};

struct NYA_ArenaRegion {
    u64 used;
    u64 capacity;
    u8* memory;

    u8                 gc_counter;
    NYA_ArenaFreeList* free_list;

    NYA_ArenaRegion *next, *prev;
};

struct NYA_ArenaFreeList {
    u32 node_counter;
    f32 average_free_size;
    u8  defragmentation_counter;

    NYA_ArenaFreeListNode *head, *tail;
};

struct NYA_ArenaFreeListNode {
    void* ptr;
    u64   size;

    NYA_ArenaFreeListNode *prev, *next;
};

/*
 * ─────────────────────────────────────────────────────────
 * MEMORY DEBUGGING STRUCTS
 * ─────────────────────────────────────────────────────────
 */

typedef void (*NYA_ArenaActionCallback)(NYA_ArenaAction action);

enum NYA_ArenaActionType {
    NYA_ARENA_ACTION_ARENA_NEW,
    NYA_ARENA_ACTION_ALLOC,
    NYA_ARENA_ACTION_REALLOC,
    NYA_ARENA_ACTION_FREE,
    NYA_ARENA_ACTION_FREE_ALL,
    NYA_ARENA_ACTION_GARBAGE_COLLECT,
    NYA_ARENA_ACTION_ARENA_DESTROY,
    NYA_ARENA_ACTION_COPY,
    NYA_ARENA_ACTION_MOVE,
    NYA_ARENA_ACTION_COUNT,
};

struct NYA_ArneaAction {
    NYA_ArenaActionType type;

    const char* arena_name;
    const char* file_name;
    u32         line_number;
    const char* function_name;

    union {
        struct {
            u8* ptr;
            u64 size;
        } as_alloc, as_free;

        struct {
            u8* old_ptr;
            u64 old_size;
            u8* new_ptr;
            u64 new_size;
        } as_realloc;

        struct {
            u8* ptr;
            u64 size;
            u8* copy_ptr;
        } as_copy;

        struct {
            u8*         ptr;
            u64         size;
            const char* move_arena_name;
            u8*         move_ptr;
        } as_move;
    };
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Both arenas are initialized and deinitialized automatically for the main thread.
 * The temp arena is meant for short lived throw-away allocations and should be cleared once done with.
 * */
NYA_API NYA_Arena* nya_arena_global;
NYA_API NYA_Arena* nya_arena_temp;

NYA_API void nya_arena_actions_set_callback(NYA_ArenaActionCallback callback);

/**
 * In debug builds, all arena functions are proxied through a debug variant which logs that action.
 * Register a callback with nya_arena_actions_set_callback to receive them; there is no history kept
 * on your behalf, so a callback installed after the fact sees nothing that already happened.
 * */
// clang-format off
#if (NYA_DEBUG || defined(NYA_ARENA_FORCE_DEBUG)) && !defined(NYA_ARENA_FORCE_NODEBUG)
#define nya_arena_create(...)                             _nya_arena_debug_create_with_options((NYA_ArenaOptions){ _NYA_ARENA_DEFAULT_OPTIONS, __VA_ARGS__ }, __FUNCTION__, __FILE__, __LINE__)
#define nya_arena_create_on_stack(...)                    _nya_arena_debug_create_with_options_on_stack((NYA_ArenaOptions){ _NYA_ARENA_DEFAULT_OPTIONS_ON_STACK, __VA_ARGS__ }, __FUNCTION__, __FILE__, __LINE__)
#define nya_arena_create_with_options(options)            _nya_arena_debug_create_with_options(options, __FUNCTION__, __FILE__, __LINE__)
#define nya_arena_create_with_options_on_stack(options)   _nya_arena_debug_create_with_options_on_stack(options, __FUNCTION__, __FILE__, __LINE__)
#define nya_arena_alloc(arena, size)                      _nya_arena_debug_alloc(arena, size, __FUNCTION__, __FILE__, __LINE__)
#define nya_arena_realloc(arena, ptr, old_size, new_size) _nya_arena_debug_realloc(arena, ptr, old_size, new_size, __FUNCTION__, __FILE__, __LINE__)
#define nya_arena_free(arena, ptr, size)                  _nya_arena_debug_free(arena, ptr, size, __FUNCTION__, __FILE__, __LINE__)
#define nya_arena_free_all(arena)                         _nya_arena_debug_free_all(arena, __FUNCTION__, __FILE__, __LINE__)
#define nya_arena_garbage_collect(arena)                  _nya_arena_debug_garbage_collect(arena, __FUNCTION__, __FILE__, __LINE__)
#define nya_arena_destroy(arena)                          _nya_arena_debug_destroy(arena, __FUNCTION__, __FILE__, __LINE__)
#define nya_arena_destroy_on_stack(arena)                 _nya_arena_debug_destroy_on_stack(arena, __FUNCTION__, __FILE__, __LINE__)
#define nya_arena_copy(dst, ptr, size)                    _nya_arena_debug_copy(dst, ptr, size, __FUNCTION__, __FILE__, __LINE__)
#define nya_arena_move(src, dst, ptr, size)               _nya_arena_debug_move(src, dst, ptr, size, __FUNCTION__, __FILE__, __LINE__)
#else
#define nya_arena_create(...)                  _nya_arena_nodebug_create_with_options((NYA_ArenaOptions){ _NYA_ARENA_DEFAULT_OPTIONS, __VA_ARGS__ })
#define nya_arena_create_on_stack(...)         _nya_arena_nodebug_create_with_options_on_stack((NYA_ArenaOptions){ _NYA_ARENA_DEFAULT_OPTIONS_ON_STACK, __VA_ARGS__ })
#define nya_arena_create_with_options          _nya_arena_nodebug_create_with_options
#define nya_arena_create_with_options_on_stack _nya_arena_nodebug_create_with_options_on_stack
#define nya_arena_alloc                        _nya_arena_nodebug_alloc
#define nya_arena_realloc                      _nya_arena_nodebug_realloc
#define nya_arena_free                         _nya_arena_nodebug_free
#define nya_arena_free_all                     _nya_arena_nodebug_free_all
#define nya_arena_garbage_collect              _nya_arena_nodebug_garbage_collect
#define nya_arena_destroy                      _nya_arena_nodebug_destroy
#define nya_arena_destroy_on_stack             _nya_arena_nodebug_destroy_on_stack
#define nya_arena_copy                         _nya_arena_nodebug_copy
#define nya_arena_move                         _nya_arena_nodebug_move
#endif // (NYA_DEBUG || defined(NYA_ARENA_FORCE_DEBUG)) && !defined(NYA_ARENA_FORCE_NODEBUG)
// clang-format on

NYA_API u64 nya_arena_memory_usage_bytes(NYA_Arena* arena);

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

/**
 * What an arena looks like right now, in one struct.
 *
 * nya_arena_memory_usage_bytes answers only "how many bytes are handed out", which is not enough to
 * act on: an arena that has handed out 4 MiB might hold one region or forty, and might have nothing
 * free or a free list so chopped up that the next allocation still grows the arena. The difference
 * between those decides whether the fix is a bigger region_size, a defragmentation pass, or nothing.
 *
 * Computed by walking the arena, so nothing is tracked on the hot path and this is as accurate in a
 * release build as in a debug one. Cost is proportional to regions plus free list nodes; do not put
 * it in an inner loop.
 * */
struct NYA_ArenaStats {
    /** From the arena's options, so a report can name it. Null when the arena was created unnamed. */
    const char* name;

    u64 region_count;

    /** Total capacity of every region: what the arena has taken from the system. */
    u64 reserved_bytes;

    /** Bytes handed out, the same number nya_arena_memory_usage_bytes returns. Includes freed blocks not yet reused. */
    u64 used_bytes;

    u64 free_list_nodes;

    /** Bytes sitting in free lists: used_bytes that has been given back and is available again. */
    u64 free_list_bytes;

    /** The biggest single free block. An allocation larger than this cannot be served from the free list. */
    u64 largest_free_block;

    /**
     * How broken up the free space is, from 0 to 1.
     *
     * 1 - largest_free_block / free_list_bytes. Zero means every free byte is in one block, so the
     * free list is as useful as it can be. Approaching one means the same total is scattered across
     * many small blocks and most allocations will grow the arena anyway.
     *
     * Zero when nothing is free, which is the honest answer: an arena with no free list is not
     * fragmented, it is simply full.
     * */
    f32 fragmentation;
};

NYA_API NYA_ArenaStats nya_arena_stats(NYA_Arena* arena) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * REGISTRY
 * ─────────────────────────────────────────────────────────
 */

/**
 * Every arena alive right now.
 *
 * Exists for the question a single arena cannot answer: memory is climbing, and which of the twenty
 * arenas in the process is responsible. Without this the only way to find out is to already hold a
 * pointer to the guilty one, which is precisely what you do not have.
 *
 * Arenas add themselves on create and remove themselves on destroy, heap and stack variants alike.
 * The table is fixed size and slots are claimed atomically, so this is safe to call while other
 * threads are creating arenas — what it cannot promise is that an arena is not destroyed between
 * nya_arena_registry_at handing it back and the caller dereferencing it. Registered arenas outlive
 * that window in every current use, and a registry that owned lifetimes would be a different thing.
 *
 * Overflowing NYA_ARENA_REGISTRY_MAX is not fatal: the arena works, it is simply not listed, and a
 * warning says so once.
 * */
#define NYA_ARENA_REGISTRY_MAX 256

NYA_API u32        nya_arena_registry_count(void) __attr_no_discard;
NYA_API NYA_Arena* nya_arena_registry_at(u32 index) __attr_no_discard;

/** Logs one line per live arena: name, regions, used of reserved, free list and fragmentation. */
NYA_API void nya_arena_stats_report(void);

/*
 * ─────────────────────────────────────────────────────────
 * CALLSITES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Allocation totals per source location, which is the drill down the per arena view cannot give.
 *
 * The registry answers "the asset system is holding 40 MiB". This answers the next question — which
 * line of the asset system — by aggregating every alloc, realloc and free the debug proxies already
 * see into one row per file, line and arena.
 *
 * `live_bytes` is the column to sort by. Total allocated is dominated by whatever runs every frame
 * and frees immediately; what has been allocated and *not* given back is what grows a process.
 *
 * Debug builds only, because the numbers come from the same proxies that record NYA_ArenaAction and
 * those are compiled out otherwise. A release build reports zero rows rather than wrong ones.
 * */
struct NYA_ArenaCallsiteStats {
    const char* file_name;
    u32         line_number;
    const char* function_name;

    /** Which arena this line allocated from. The same line billing two arenas gets a row for each. */
    const char* arena_name;

    u64 alloc_count;
    u64 free_count;

    u64 allocated_bytes;
    u64 freed_bytes;

    /**
     * allocated_bytes - freed_bytes: what this line is still holding.
     *
     * Signed, and permitted to go negative rather than clamped. A line that frees what another line
     * allocated is a real pattern, and hiding it behind a floor of zero would turn a legitimate
     * "this is the release site" into a silent zero.
     * */
    s64 live_bytes;
};

/** Number of distinct callsites recorded so far. Zero outside debug builds. */
NYA_API u32 nya_arena_callsite_count(void) __attr_no_discard;

/** Row `index`, or a zeroed struct past the end. Order is first-seen, so it is stable across calls. */
NYA_API NYA_ArenaCallsiteStats nya_arena_callsite_at(u32 index) __attr_no_discard;

/** Forgets every row. For measuring one frame, or one level load, rather than the whole process. */
NYA_API void nya_arena_callsites_reset(void);

/** Logs the `limit` callsites holding the most live bytes. The report you actually want when memory is climbing. */
NYA_API void nya_arena_callsites_report(u32 limit);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// clang-format off
NYA_API NYA_Arena* _nya_arena_debug_create_with_options(NYA_ArenaOptions options, const char* function, const char* file, u32 line) __attr_no_discard;
NYA_API NYA_Arena  _nya_arena_debug_create_with_options_on_stack(NYA_ArenaOptions options, const char* function, const char* file, u32 line) __attr_no_discard;
NYA_API void*      _nya_arena_debug_alloc(NYA_Arena* arena, u64 size, const char* function, const char* file, u32 line) __attr_malloc __attr_no_discard;
NYA_API void*      _nya_arena_debug_realloc(NYA_Arena* arena, void* ptr, u64 old_size, u64 new_size, const char* function, const char* file, u32 line) __attr_no_discard;
NYA_API void       _nya_arena_debug_free(NYA_Arena* arena, void* ptr, u64 size, const char* function, const char* file, u32 line);
NYA_API void       _nya_arena_debug_free_all(NYA_Arena* arena, const char* function, const char* file, u32 line);
NYA_API void       _nya_arena_debug_garbage_collect(NYA_Arena* arena, const char* function, const char* file, u32 line);
NYA_API void       _nya_arena_debug_destroy(NYA_Arena* arena, const char* function, const char* file, u32 line);
NYA_API void       _nya_arena_debug_destroy_on_stack(NYA_Arena* arena, const char* function, const char* file, u32 line);
NYA_API void*      _nya_arena_debug_copy(NYA_Arena* dst, void* ptr, u64 size, const char* function, const char* file, u32 line) __attr_no_discard;
NYA_API void*      _nya_arena_debug_move(NYA_Arena* src, NYA_Arena* dst, void* ptr, u64 size, const char* function, const char* file, u32 line) __attr_no_discard;

NYA_API NYA_Arena* _nya_arena_nodebug_create_with_options(NYA_ArenaOptions options) __attr_no_discard;
NYA_API NYA_Arena  _nya_arena_nodebug_create_with_options_on_stack(NYA_ArenaOptions options) __attr_no_discard;
NYA_API void*      _nya_arena_nodebug_alloc(NYA_Arena* arena, u64 size) __attr_malloc __attr_no_discard;
NYA_API void*      _nya_arena_nodebug_realloc(NYA_Arena* arena, void* ptr, u64 old_size, u64 new_size) __attr_no_discard;
NYA_API void       _nya_arena_nodebug_free(NYA_Arena* arena, void* ptr, u64 size);
NYA_API void       _nya_arena_nodebug_free_all(NYA_Arena* arena);
NYA_API void       _nya_arena_nodebug_garbage_collect(NYA_Arena* arena);
NYA_API void       _nya_arena_nodebug_destroy(NYA_Arena* arena);
NYA_API void       _nya_arena_nodebug_destroy_on_stack(NYA_Arena* arena);
NYA_API void*      _nya_arena_nodebug_copy(NYA_Arena* dst, void* ptr, u64 size) __attr_no_discard;
NYA_API void*      _nya_arena_nodebug_move(NYA_Arena* src, NYA_Arena* dst, void* ptr, u64 size) __attr_no_discard;
// clang-format on

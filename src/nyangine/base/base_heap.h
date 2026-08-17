/**
 * @file base_heap.h
 *
 * API Overview:
 * - nya_heap_create(arena_ptr, item_type, compare_fn)
 * - nya_heap_create_with_capacity(arena_ptr, item_type, compare_fn, initial_capacity
 * - nya_heap_from_carray(arena_ptr, item_type, carray, carray_length, compare_fn)
 * - nya_heap_resize(heap_ptr, new_capacity)
 * - nya_heap_reserve(heap_ptr, min_capacity)
 * - nya_heap_clear(heap_ptr)
 * - nya_heap_destroy(heap_ptr)
 * - nya_heap_peek(heap_ptr)
 * - nya_heap_push(heap_ptr, item)
 * - nya_heap_pop(heap_ptr)
 * - nya_heap_length(heap_ptr)
 *
 * Example:
 * ```c
 * typedef struct {
 *  u32   id;
 *  char* name;
 * } Player;
 * nya_derive_heap(Player);
 *
 * s32 compare_fn(const Player* a, const Player* b) {
 *   if (a->id < b->id) { return -1; }
 *   else if (a->id > b->id) { return 1; }
 *   else { return 0; }
 * }
 *
 * NYA_Arena* arena = nya_arena_create(...);
 * NYA_HeapᐸPlayerᐳ* players = nya_heap_create(arena, Player, &compare_fn);
 *
 * nya_heap_push(players, (Player){ .id = 2, .name = "Alice" });
 * nya_heap_push(players, (Player){ .id = 1, .name = "Bob" });
 *
 * Player top_player = nya_heap_peek(players); // top_player.id == 1
 *
 * nya_arena_destroy(arena);
 * ```
 * */
#pragma once

#include "nyangine/base/base_arena.h"
// For nya_array_swap and _nya_array_access_guard, which nya_heap_push, nya_heap_pop and
// nya_heap_peek expand to. This header did not name it, so including base_heap.h on its own and
// pushing anything failed with "use of undeclared identifier 'nya_array_swap'" — it worked only
// because base.h happens to include base_array.h first. The same problem build.h's docblock
// describes, and the reason the build headers were split up.
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_memory.h"
#include "nyangine/base/base_template.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_scalar.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Name of the heap type derived for `type`, e.g. NYA_Heapᐸs32ᐳ. */
#define _nya_derive_heap_name(type) nya_template(NYA_Heap, type)

#define nya_derive_heap(type)                                                                                                                        \
    typedef struct {                                                                                                                                 \
        u64        length;                                                                                                                           \
        u64        capacity;                                                                                                                         \
        type*      items;                                                                                                                            \
        NYA_Arena* arena;                                                                                                                            \
        s32 (*compare)(const type* a, const type* b);                                                                                                \
    } _nya_derive_heap_name(type)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CREATION MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_HEAP_DEFAULT_CAPACITY 16

/**
 * Compare function: s32 compare_fn(const T* a, const T* b);
 * Return: -1 if a < b, 0 if a == b, 1 if a > b
 * */
#define nya_heap_create(arena_ptr, item_type, compare_fn) nya_heap_create_with_capacity(arena_ptr, item_type, compare_fn, _NYA_HEAP_DEFAULT_CAPACITY)
#define nya_heap_create_with_capacity(arena_ptr, item_type, compare_fn, initial_capacity)                                                            \
    ({                                                                                                                                               \
        _nya_derive_heap_name(item_type)* heap_ptr = nya_arena_alloc(arena_ptr, sizeof(_nya_derive_heap_name(item_type)));                           \
        *heap_ptr                                  = nya_heap_create_with_capacity_on_stack(arena_ptr, item_type, compare_fn, initial_capacity);     \
        heap_ptr;                                                                                                                                    \
    })

#define nya_heap_create_on_stack(arena_ptr, item_type, compare_fn)                                                                                   \
    nya_heap_create_with_capacity_on_stack(arena_ptr, item_type, compare_fn, _NYA_HEAP_DEFAULT_CAPACITY)
#define nya_heap_create_with_capacity_on_stack(arena_ptr, item_type, compare_fn, initial_capacity)                                                   \
    ({                                                                                                                                               \
        _nya_derive_heap_name(item_type) heap = {                                                                                                    \
            .items    = (initial_capacity) == 0 ? nullptr : nya_arena_alloc(arena_ptr, (initial_capacity) * sizeof(item_type)),                      \
            .length   = 0,                                                                                                                           \
            .capacity = (initial_capacity),                                                                                                          \
            .arena    = (arena_ptr),                                                                                                                 \
            .compare  = (compare_fn),                                                                                                                \
        };                                                                                                                                           \
        heap;                                                                                                                                        \
    })

/**
 * Builds a heap from a plain C array. Returns a pointer, like every other heap constructor here.
 *
 * `carray_length` is read once, since it serves as both the initial capacity and the loop bound.
 * */
#define nya_heap_from_carray(arena_ptr, item_type, carray, carray_length, compare_fn)                                                                \
    ({                                                                                                                                               \
        nya_assert_type_match(arena_ptr, (NYA_Arena*)0);                                                                                             \
        nya_assert_type_match(carray_length, (u64)0);                                                                                                \
        /* Assigned rather than nya_assert_type_match'd: that is __builtin_types_compatible_p, which \
         * does not apply array-to-pointer decay and so rejected the `item_type[N]` this is for.  */ \
        item_type*                        _heap_from_items = (carray);                                                                               \
        u64                               _heap_from_count = (carray_length);                                                                        \
        _nya_derive_heap_name(item_type)* _heap_from_ptr = nya_heap_create_with_capacity(arena_ptr, item_type, compare_fn, _heap_from_count);        \
        for (u64 _heap_from_i = 0; _heap_from_i < _heap_from_count; _heap_from_i++) nya_heap_push(_heap_from_ptr, _heap_from_items[_heap_from_i]);   \
        _heap_from_ptr;                                                                                                                              \
    })

#define nya_heap_resize(heap_ptr, new_capacity)                                                                                                      \
    ({                                                                                                                                               \
        /* Same fix, and same reason, as nya_array_resize: nya_arena_realloc returns null for a null \
         * pointer by design, so a heap that started at capacity zero could never grow.  */          \
        (heap_ptr)->items = (heap_ptr)->items == nullptr                                                                                             \
                              ? nya_arena_alloc((heap_ptr)->arena, (new_capacity) * sizeof(*(heap_ptr)->items))                                     \
                              : nya_arena_realloc(                                                                                                  \
                                    (heap_ptr)->arena,                                                                                              \
                                    (heap_ptr)->items,                                                                                              \
                                    (heap_ptr)->capacity * sizeof(*(heap_ptr)->items),                                                              \
                                    (new_capacity) * sizeof(*(heap_ptr)->items)                                                                     \
                                );                                                                                                                  \
        (heap_ptr)->capacity = new_capacity;                                                                                                         \
    })

#define nya_heap_reserve(heap_ptr, min_capacity)                                                                                                     \
    ({                                                                                                                                               \
        if ((heap_ptr)->capacity < (min_capacity)) { /**/                                                                                            \
            nya_heap_resize((heap_ptr), nya_cast_to_u64(nya_max((u64)2 * (heap_ptr)->capacity, (u64)(min_capacity))));                                       \
        }                                                                                                                                            \
    })

#define nya_heap_clear(heap_ptr) ({ (heap_ptr)->length = 0; })

#define nya_heap_destroy(heap_ptr)                                                                                                                   \
    ({                                                                                                                                               \
        nya_arena_free((heap_ptr)->arena, (heap_ptr)->items, sizeof(*(heap_ptr)->items) * (heap_ptr)->capacity);                                     \
        nya_arena_free((heap_ptr)->arena, heap_ptr, sizeof(*(heap_ptr)));                                                                            \
        (heap_ptr) = nullptr;                                                                                                                        \
    })

/*
 * Note this takes the heap by value, where nya_heap_destroy takes a pointer.
 *
 * The on-stack destructors are not consistent with each other across base, and the split is: heap
 * and hmap take a value, array, ring and hset take a pointer. This note used to put hset on the
 * by-value side, which is the wrong half. Left as they are rather than changed underneath a caller.
 *
 * The capacity and the length are reset alongside `items`. Clearing only the pointer left the heap
 * claiming to own a block it no longer had, so a second destroy handed the arena a null pointer with
 * a non-zero size, and a push onto the destroyed heap saw length < capacity and wrote through null
 * rather than reallocating.
 */
#define nya_heap_destroy_on_stack(heap_ptr)                                                                                                          \
    ({                                                                                                                                               \
        nya_arena_free((heap_ptr).arena, (heap_ptr).items, sizeof(*(heap_ptr).items) * (heap_ptr).capacity);                                         \
        (heap_ptr).items    = nullptr;                                                                                                               \
        (heap_ptr).length   = 0;                                                                                                                     \
        (heap_ptr).capacity = 0;                                                                                                                     \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ACCESS MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_heap_peek(heap_ptr)                                                                                                                      \
    ({                                                                                                                                               \
        _nya_array_access_guard(0, (heap_ptr)->length);                                                                                              \
        (heap_ptr)->items[0];                                                                                                                        \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ADD / INSERT / REMOVE MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_heap_push(heap_ptr, item)                                                                                                                \
    ({                                                                                                                                               \
        nya_assert((heap_ptr)->compare != nullptr);                                                                                                  \
        nya_assert_type_match(item, (heap_ptr)->items[0]);                                                                                           \
        if ((heap_ptr)->length == (heap_ptr)->capacity) {                                                                                            \
            nya_heap_resize(heap_ptr, (heap_ptr)->capacity == 0 ? 1 : nya_cast_to_u64(2UL * (heap_ptr)->capacity));                                  \
        }                                                                                                                                            \
        u64 index                = (heap_ptr)->length++;                                                                                             \
        (heap_ptr)->items[index] = item;                                                                                                             \
        while (index != 0) {                                                                                                                         \
            u64 parent_index = (index - 1) / 2;                                                                                                      \
            if ((heap_ptr)->compare(&(heap_ptr)->items[index], &(heap_ptr)->items[parent_index]) < 0) {                                              \
                nya_array_swap(heap_ptr, index, parent_index);                                                                                       \
                index = parent_index;                                                                                                                \
            } else {                                                                                                                                 \
                break;                                                                                                                               \
            }                                                                                                                                        \
        }                                                                                                                                            \
    })

#define nya_heap_pop(heap_ptr)                                                                                                                       \
    ({                                                                                                                                               \
        nya_assert((heap_ptr)->compare != nullptr);                                                                                                  \
        nya_assert((heap_ptr)->length > 0);                                                                                                          \
        typeof(*(heap_ptr)->items) item = (heap_ptr)->items[0];                                                                                      \
        (heap_ptr)->items[0]            = (heap_ptr)->items[--(heap_ptr)->length];                                                                   \
        u64 index                       = 0;                                                                                                         \
        while (true) {                                                                                                                               \
            u64 left_child_index     = 2 * index + 1;                                                                                                \
            u64 right_child_index    = 2 * index + 2;                                                                                                \
            u64 smallest_child_index = index;                                                                                                        \
            if (left_child_index < (heap_ptr)->length &&                                                                                             \
                (heap_ptr)->compare(&(heap_ptr)->items[left_child_index], &(heap_ptr)->items[smallest_child_index]) < 0) {                           \
                smallest_child_index = left_child_index;                                                                                             \
            }                                                                                                                                        \
            if (right_child_index < (heap_ptr)->length &&                                                                                            \
                (heap_ptr)->compare(&(heap_ptr)->items[right_child_index], &(heap_ptr)->items[smallest_child_index]) < 0) {                          \
                smallest_child_index = right_child_index;                                                                                            \
            }                                                                                                                                        \
            if (smallest_child_index != index) {                                                                                                     \
                nya_array_swap(heap_ptr, index, smallest_child_index);                                                                               \
                index = smallest_child_index;                                                                                                        \
            } else {                                                                                                                                 \
                break;                                                                                                                               \
            }                                                                                                                                        \
        }                                                                                                                                            \
        item;                                                                                                                                        \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MISC MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_heap_length(heap_ptr) ((heap_ptr)->length)

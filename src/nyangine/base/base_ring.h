/**
 * @file base_ring.h
 *
 * Ring buffer.
 *
 * API Overview:
 * - nya_ring_create(arena_ptr, item_type)
 * - nya_ring_create_with_capacity(arena_ptr, item_type, capacity)
 * - nya_ring_create_on_stack(arena_ptr, item_type)
 * - nya_ring_create_with_capacity_on_stack(arena_ptr, item_type, capacity)
 * - nya_ring_resize(ring_ptr, new_capacity)
 * - nya_ring_clear(ring_ptr)
 * - nya_ring_destroy(ring_ptr)
 * - nya_ring_destroy_on_stack(ring_ptr)
 * - nya_ring_push(ring_ptr, item)
 * - nya_ring_pop(ring_ptr)
 * - nya_ring_push_many(ring_ptr, ...)
 * - nya_ring_pop_many(ring_ptr, count)
 * - nya_ring_front(ring_ptr)
 * - nya_ring_back(ring_ptr)
 * - nya_ring_at(ring_ptr, index)
 * - nya_ring_is_empty(ring_ptr)
 * - nya_ring_length(ring_ptr)
 * - nya_ring_capacity(ring_ptr)
 * - nya_ring_available_space(ring_ptr)
 * - nya_ring_peek(ring_ptr, offset)
 * - nya_ring_copy(ring_ptr)
 * - nya_ring_move(ring_ptr, new_arena_ptr)
 * - nya_ring_foreach(ring_ptr, item_name)
 * - nya_ring_foreach_reverse(ring_ptr, item_name)
 *
 * Example:
 * ```c
 * typedef struct {
 *  u32   id;
 *  char* name;
 * } Player;
 * nya_derive_ring(Player);
 *
 * NYA_Arena* arena = nya_arena_create(...);
 * NYA_RingᐸPlayerᐳ* players = nya_ring_create(arena, Player, 8);
 *
 * nya_ring_push(players, (Player){ .id = 1, .name = "Alice" });
 * nya_ring_push(players, (Player){ .id = 2, .name = "Bob" });
 *
 * while (!nya_ring_is_empty(players)) {
 *   Player* player = nya_ring_pop(players);
 *   nya_info("Player %u: %s", player->id, player->name);
 * }
 *
 * nya_arena_destroy(arena);
 * ```
 * */
#pragma once

#include "nyangine/base/base_arena.h"
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

/** Name of the ring buffer type derived for `type`, e.g. NYA_Ringᐸu32ᐳ. */
#define _nya_derive_ring_name(type) nya_template(NYA_Ring, type)

#define nya_derive_ring(type)                                                                                                                        \
    typedef struct {                                                                                                                                 \
        u64        length;                                                                                                                           \
        u64        capacity;                                                                                                                         \
        type*      items;                                                                                                                            \
        u64        head;                                                                                                                             \
        u64        tail;                                                                                                                             \
        NYA_Arena* arena;                                                                                                                            \
    } _nya_derive_ring_name(type);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CREATION MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_RING_DEFAULT_CAPACITY 8

#define nya_ring_create(arena_ptr, item_type) nya_ring_create_with_capacity(arena_ptr, item_type, _NYA_RING_DEFAULT_CAPACITY)
#define nya_ring_create_with_capacity(arena_ptr, item_type, initial_capacity)                                                                        \
    ({                                                                                                                                               \
        nya_assert_type_match(arena_ptr, (NYA_Arena*)0);                                                                                             \
        _nya_derive_ring_name(item_type)* ring_ptr = nya_arena_alloc(arena_ptr, sizeof(_nya_derive_ring_name(item_type)));                           \
        *ring_ptr                                  = nya_ring_create_with_capacity_on_stack(arena_ptr, item_type, initial_capacity);                 \
        ring_ptr;                                                                                                                                    \
    })

#define nya_ring_create_on_stack(arena_ptr, item_type) nya_ring_create_with_capacity_on_stack(arena_ptr, item_type, _NYA_RING_DEFAULT_CAPACITY)
#define nya_ring_create_with_capacity_on_stack(arena_ptr, item_type, initial_capacity)                                                               \
    ({                                                                                                                                               \
        nya_assert_type_match(arena_ptr, (NYA_Arena*)0);                                                                                             \
        _nya_derive_ring_name(item_type) ring = {                                                                                                    \
            .items    = (initial_capacity) == 0 ? nullptr : nya_arena_alloc(arena_ptr, (initial_capacity) * sizeof(item_type)),                      \
            .head     = 0,                                                                                                                           \
            .tail     = 0,                                                                                                                           \
            .capacity = (initial_capacity),                                                                                                          \
            .length   = 0,                                                                                                                           \
            .arena    = (arena_ptr),                                                                                                                 \
        };                                                                                                                                           \
        ring;                                                                                                                                        \
    })

#define nya_ring_resize(ring_ptr, new_capacity)                                                                                                      \
    ({                                                                                                                                               \
        u64                         _ring_old_cap  = (ring_ptr)->capacity;                                                                           \
        u64                         _ring_old_head = (ring_ptr)->head;                                                                               \
        u64                         _ring_old_len  = (ring_ptr)->length;                                                                             \
        u64                         _ring_new_cap  = (new_capacity);                                                                                 \
        /* The copy below writes _ring_old_len entries, so a smaller capacity would run off the end  */ \
        /* of the new buffer rather than dropping the excess.                                        */ \
        nya_assert(_ring_new_cap >= _ring_old_len, "Cannot resize a ring below its length.");                                                        \
        typeof(*(ring_ptr)->items)* _ring_new      = nya_arena_alloc((ring_ptr)->arena, _ring_new_cap * sizeof(*(ring_ptr)->items));                 \
        for (u64 _i = 0; _i < _ring_old_len; _i++) { _ring_new[_i] = (ring_ptr)->items[(_ring_old_head + _i) % _ring_old_cap]; }                     \
        nya_arena_free((ring_ptr)->arena, (ring_ptr)->items, _ring_old_cap * sizeof(*(ring_ptr)->items));                                            \
        (ring_ptr)->items    = _ring_new;                                                                                                            \
        (ring_ptr)->head     = 0;                                                                                                                    \
        /* Wrapped, because resizing to exactly the length leaves tail one past the last slot: a    */ \
        /* valid length but not a valid index, and the next push wrote off the end of the buffer.   */ \
        (ring_ptr)->tail     = _ring_old_len % _ring_new_cap;                                                                                        \
        (ring_ptr)->capacity = _ring_new_cap;                                                                                                        \
    })

#define nya_ring_clear(ring_ptr)                                                                                                                     \
    ({                                                                                                                                               \
        (ring_ptr)->head   = 0;                                                                                                                      \
        (ring_ptr)->tail   = 0;                                                                                                                      \
        (ring_ptr)->length = 0;                                                                                                                      \
    })

#define nya_ring_destroy(ring_ptr)                                                                                                                   \
    ({                                                                                                                                               \
        nya_arena_free((ring_ptr)->arena, (ring_ptr)->items, (ring_ptr)->capacity * sizeof(*(ring_ptr)->items));                                     \
        nya_arena_free((ring_ptr)->arena, ring_ptr, sizeof(*(ring_ptr)));                                                                            \
        (ring_ptr) = nullptr;                                                                                                                        \
    })

#define nya_ring_destroy_on_stack(ring_ptr)                                                                                                          \
    ({                                                                                                                                               \
        nya_arena_free((ring_ptr)->arena, (ring_ptr)->items, (ring_ptr)->capacity * sizeof(*(ring_ptr)->items));                                     \
        (ring_ptr)->head     = 0;                                                                                                                    \
        (ring_ptr)->tail     = 0;                                                                                                                    \
        (ring_ptr)->length   = 0;                                                                                                                    \
        (ring_ptr)->capacity = 0;                                                                                                                    \
        (ring_ptr)->items    = nullptr;                                                                                                              \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ACCESS MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _nya_ring_access_guard(index, length)                                                                                                        \
    nya_assert(0 <= (index) && (index) < (length), "Ring index " FMTs64 " (length " FMTu64 ") out of bounds.", (s64)(index), length);

#define nya_ring_at(ring_ptr, index)                                                                                                                 \
    ({                                                                                                                                               \
        _nya_ring_access_guard(index, (ring_ptr)->length);                                                                                           \
        &(ring_ptr)->items[((ring_ptr)->head + (index)) % (ring_ptr)->capacity];                                                                     \
    })

#define nya_ring_front(ring_ptr)                                                                                                                     \
    ({                                                                                                                                               \
        nya_assert((ring_ptr)->length > 0, "Cannot access front of empty ring buffer");                                                              \
        &(ring_ptr)->items[(ring_ptr)->head];                                                                                                        \
    })

#define nya_ring_back(ring_ptr)                                                                                                                      \
    ({                                                                                                                                               \
        nya_assert((ring_ptr)->length > 0, "Cannot access back of empty ring buffer");                                                               \
        &(ring_ptr)->items[((ring_ptr)->tail + (ring_ptr)->capacity - 1) % (ring_ptr)->capacity];                                                    \
    })

#define nya_ring_peek(ring_ptr, offset)                                                                                                              \
    ({                                                                                                                                               \
        nya_assert((offset) < (ring_ptr)->length, "Peek offset " FMTu64 " exceeds ring length " FMTu64, (u64)(offset), (ring_ptr)->length);          \
        &(ring_ptr)->items[((ring_ptr)->head + (offset)) % (ring_ptr)->capacity];                                                                    \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUSH / POP MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_ring_push(ring_ptr, item)                                                                                                                \
    ({                                                                                                                                               \
        nya_assert_type_match(item, (ring_ptr)->items[0]);                                                                                           \
        /*                                                                                                                                          \
         * A ring with no slots has nowhere to put this. `items` is null at capacity zero and the    \
         * wrap below is a modulo by it, so the write went through a null pointer and the wrap was   \
         * a division by zero. A ring is fixed capacity by construction — a full one overwrites its  \
         * oldest entry rather than growing — so an empty one is a caller mistake, not a reason to   \
         * allocate.                                                                                 \
         */                                                                                         \
        nya_assert((ring_ptr)->capacity > 0, "Cannot push onto a ring buffer with zero capacity.");                                                  \
        (ring_ptr)->items[(ring_ptr)->tail] = item;                                                                                                  \
        (ring_ptr)->tail                    = ((ring_ptr)->tail + 1) % (ring_ptr)->capacity;                                                         \
        if ((ring_ptr)->length < (ring_ptr)->capacity) {                                                                                             \
            (ring_ptr)->length++;                                                                                                                    \
        } else {                                                                                                                                     \
            (ring_ptr)->head = ((ring_ptr)->head + 1) % (ring_ptr)->capacity;                                                                        \
        }                                                                                                                                            \
    })

#define nya_ring_pop(ring_ptr)                                                                                                                       \
    ({                                                                                                                                               \
        nya_assert((ring_ptr)->length > 0, "Cannot pop from empty ring buffer");                                                                     \
        typeof(*(ring_ptr)->items) item = (ring_ptr)->items[(ring_ptr)->head];                                                                       \
        (ring_ptr)->head                = ((ring_ptr)->head + 1) % (ring_ptr)->capacity;                                                             \
        (ring_ptr)->length--;                                                                                                                        \
        item;                                                                                                                                        \
    })

#define nya_ring_push_many(ring_ptr, ...)                                                                                                            \
    ({                                                                                                                                               \
        typeof(*(ring_ptr)->items) items_to_push[]     = { __VA_ARGS__ };                                                                            \
        u64                        items_to_push_count = sizeof(items_to_push) / sizeof(items_to_push[0]);                                           \
                                                                                                                                                     \
        for (u64 i = 0; i < items_to_push_count; i++) {                                                                                              \
            nya_assert_type_match(items_to_push[i], (ring_ptr)->items[0]);                                                                           \
            nya_ring_push(ring_ptr, items_to_push[i]);                                                                                               \
        }                                                                                                                                            \
    })

/*
 * The count is read once, before anything is popped.
 *
 * Popping is exactly what shrinks `length`, and the bound was re-read every iteration — so
 * `nya_ring_pop_many(ring, nya_ring_length(ring))`, which is the obvious way to drain one, stopped
 * at the half way point where the shrinking length met the climbing index.
 * */
#define nya_ring_pop_many(ring_ptr, count)                                                                                                           \
    ({                                                                                                                                               \
        u64 _ring_pop_many_count = (count);                                                                                                          \
        nya_assert(_ring_pop_many_count <= (ring_ptr)->length, "Cannot pop more items than ring buffer contains");                                   \
        for (u64 _ring_pop_many_i = 0; _ring_pop_many_i < _ring_pop_many_count; _ring_pop_many_i++) { (void)nya_ring_pop(ring_ptr); }                \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_ring_is_empty(ring_ptr) ((ring_ptr)->length == 0)
#define nya_ring_length(ring_ptr)   ((ring_ptr)->length)
#define nya_ring_capacity(ring_ptr) ((ring_ptr)->capacity)

/**
 * Slots that can be pushed before the ring starts overwriting its oldest entry.
 *
 * Listed in the API overview at the top of this file since it was written, and never defined, so
 * anything following that list got an implicit function declaration rather than a macro. Zero means
 * the next push evicts, not that the push fails — a ring is fixed capacity and overwrites.
 * */
#define nya_ring_available_space(ring_ptr) ((ring_ptr)->capacity - (ring_ptr)->length)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MISC MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_ring_copy(ring_ptr)                                                                                                                      \
    ({                                                                                                                                               \
        typeof(*(ring_ptr)) copy = { .items =                                                                                                        \
                                         nya_arena_copy((ring_ptr)->arena, (ring_ptr)->items, sizeof(*(ring_ptr)->items) * (ring_ptr)->capacity),    \
                                     .head     = (ring_ptr)->head,                                                                                   \
                                     .tail     = (ring_ptr)->tail,                                                                                   \
                                     .length   = (ring_ptr)->length,                                                                                 \
                                     .capacity = (ring_ptr)->capacity,                                                                               \
                                     .arena    = (ring_ptr)->arena };                                                                                \
        copy;                                                                                                                                        \
    })

#define nya_ring_move(ring_ptr, new_arena_ptr)                                                                                                       \
    ({                                                                                                                                               \
        nya_assert_type_match(new_arena_ptr, (ring_ptr)->arena);                                                                                     \
        NYA_Arena*          _ring_move_old_arena = (ring_ptr)->arena;                                                                                \
        typeof(*(ring_ptr)) _ring_move_tmp       = {                                                                                                 \
            .items    = nya_arena_move(_ring_move_old_arena, new_arena_ptr, (ring_ptr)->items, sizeof(*(ring_ptr)->items) * (ring_ptr)->capacity),   \
            .head     = (ring_ptr)->head,                                                                                                            \
            .tail     = (ring_ptr)->tail,                                                                                                            \
            .length   = (ring_ptr)->length,                                                                                                          \
            .capacity = (ring_ptr)->capacity,                                                                                                        \
            .arena    = (new_arena_ptr)                                                                                                              \
        };                                                                                                                                           \
        typeof(ring_ptr) _ring_move_new_ptr = nya_arena_alloc(new_arena_ptr, sizeof(*(ring_ptr)));                                                   \
        *_ring_move_new_ptr                 = _ring_move_tmp;                                                                                        \
        nya_arena_free(_ring_move_old_arena, ring_ptr, sizeof(*(ring_ptr)));                                                                         \
        (ring_ptr) = _ring_move_new_ptr;                                                                                                             \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ITERATOR MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_ring_foreach(ring_ptr, item_name)                                                                                                        \
    for (u64 _nya_ring_idx = 0;                                                                                                                      \
         _nya_ring_idx < (ring_ptr)->length && ((item_name) = &(ring_ptr)->items[((ring_ptr)->head + _nya_ring_idx) % (ring_ptr)->capacity], 1);     \
         _nya_ring_idx++)

#define nya_ring_foreach_reverse(ring_ptr, item_name)                                                                                                \
    for (u64 _nya_ring_idx = (ring_ptr)->length;                                                                                                     \
         _nya_ring_idx > 0 && ((item_name) = &(ring_ptr)->items[((ring_ptr)->head + _nya_ring_idx - 1) % (ring_ptr)->capacity], 1);                  \
         _nya_ring_idx--)

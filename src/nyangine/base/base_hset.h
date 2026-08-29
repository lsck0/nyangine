/**
 * @file base_hset.h
 *
 * API Overview:
 * - nya_hset_create(arena_ptr, item_type)
 * - nya_hset_create_with_capacity(arena_ptr, item_type, initial_capacity)
 * - nya_hset_clear(hset_ptr)
 * - nya_hset_destroy(hset_ptr)
 * - nya_hset_resize_and_rehash(hset_ptr, new_capacity)
 * - nya_hset_contains(hset_ptr, item)
 * - nya_hset_insert(hset_ptr, item)
 * - nya_hset_remove(hset_ptr, item)
 * - nya_hset_union(dest_hset_ptr, src_hset_ptr)
 * - nya_hset_intersection(dest_hset_ptr, src_hset_ptr)
 * - nya_hset_difference(dest_hset_ptr, src_hset_ptr)
 * - nya_hset_symmetric_difference(dest_hset_ptr, src_hset_ptr)
 * - nya_hset_copy(hset_ptr)
 * - nya_hset_move(hset_ptr, new_arena_ptr)
 * - nya_hset_foreach(hset_ptr, item_name)
 *
 * Example:
 * ```c
 * typedef struct {
 *  u32   id;
 *  char* name;
 * } Player;
 * nya_derive_hset(Player);
 *
 * NYA_Arena* arena = nya_arena_create(...);
 * NYA_HSetᐸPlayerᐳ* player_set = nya_hset_create(arena, Player);
 *
 * nya_hset_insert(player_set, (Player){ .id = 1, .name = "Alice" });
 * nya_hset_insert(player_set, (Player){ .id = 2, .name = "Bob" });
 *
 * nya_hset_foreach (player_set, player) nya_log_info("Player %u: %s", player->id, player->name);
 *
 * nya_arena_destroy(arena);
 * ```c
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_template.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_HASHSET_DEFAULT_CAPACITY 64
#define _NYA_HASHSET_LOAD_FACTOR      0.75F

/** Name of the hash set type derived for `item_type`, e.g. NYA_HSetᐸu32ᐳ. */
#define _nya_derive_hset_name(item_type) nya_template(NYA_HSet, item_type)

#define nya_derive_hset(item_type)                                                                                                                   \
    typedef struct {                                                                                                                                 \
        u64        length;                                                                                                                           \
        u64        capacity;                                                                                                                         \
        item_type* items;                                                                                                                            \
        b8*        occupied;                                                                                                                         \
        NYA_Arena* arena;                                                                                                                            \
    } _nya_derive_hset_name(item_type);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CREATION MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_hset_create(arena_ptr, item_type) nya_hset_create_with_capacity(arena_ptr, item_type, _NYA_HASHSET_DEFAULT_CAPACITY)
#define nya_hset_create_with_capacity(arena_ptr, item_type, initial_capacity)                                                                        \
    ({                                                                                                                                               \
        _nya_derive_hset_name(item_type)* _hset_ptr =                                                                                                \
            (_nya_derive_hset_name(item_type)*)nya_arena_alloc(arena_ptr, sizeof(_nya_derive_hset_name(item_type)));                                 \
        *_hset_ptr = nya_hset_create_with_capacity_on_stack(arena_ptr, item_type, initial_capacity);                                                 \
        _hset_ptr;                                                                                                                                   \
    })

#define nya_hset_create_on_stack(arena_ptr, item_type) nya_hset_create_with_capacity_on_stack(arena_ptr, item_type, _NYA_HASHSET_DEFAULT_CAPACITY)
#define nya_hset_create_with_capacity_on_stack(arena_ptr, item_type, initial_capacity)                                                               \
    ({                                                                                                                                               \
        _nya_derive_hset_name(item_type) _hset = {                                                                                                   \
            .length   = 0,                                                                                                                           \
            .capacity = (initial_capacity),                                                                                                          \
            .arena    = (arena_ptr),                                                                                                                 \
            .items    = nya_arena_alloc(arena_ptr, (initial_capacity) * sizeof(item_type)),                                                          \
            .occupied = nya_arena_alloc(arena_ptr, (initial_capacity) * sizeof(b8)),                                                                 \
        };                                                                                                                                           \
        nya_memset(_hset.occupied, 0, (initial_capacity) * sizeof(b8));                                                                              \
        _hset;                                                                                                                                       \
    })

#define nya_hset_clear(hset_ptr)                                                                                                                     \
    ({                                                                                                                                               \
        nya_memset((hset_ptr)->occupied, 0, (hset_ptr)->capacity * sizeof(b8));                                                                      \
        (hset_ptr)->length = 0;                                                                                                                      \
    })

#define nya_hset_destroy(hset_ptr)                                                                                                                   \
    ({                                                                                                                                               \
        nya_arena_free((hset_ptr)->arena, (hset_ptr)->items, sizeof(*(hset_ptr)->items) * (hset_ptr)->capacity);                                     \
        nya_arena_free((hset_ptr)->arena, (hset_ptr)->occupied, sizeof(*(hset_ptr)->occupied) * (hset_ptr)->capacity);                               \
        nya_arena_free((hset_ptr)->arena, hset_ptr, sizeof(*(hset_ptr)));                                                                            \
        (hset_ptr) = nullptr;                                                                                                                        \
    })

#define nya_hset_destroy_on_stack(hset_ptr)                                                                                                          \
    ({                                                                                                                                               \
        nya_arena_free((hset_ptr)->arena, (hset_ptr)->items, sizeof(*(hset_ptr)->items) * (hset_ptr)->capacity);                                     \
        nya_arena_free((hset_ptr)->arena, (hset_ptr)->occupied, sizeof(*(hset_ptr)->occupied) * (hset_ptr)->capacity);                               \
        nya_memset(hset_ptr, 0, sizeof(*(hset_ptr)));                                                                                                \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RESIZE AND REHASH MACRO
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _nya_hset_insert_unchecked(hset_ptr, item)                                                                                                   \
    ({                                                                                                                                               \
        nya_assert_type_match(item, (hset_ptr)->items[0]);                                                                                           \
        typeof(item) item_var   = item;                                                                                                              \
        u64          index      = (hset_ptr)->capacity == 0 ? 0 : nya_hash_fnv1a(&item_var, sizeof(item_var)) % (hset_ptr)->capacity;                \
        u64          iterations = 0;                                                                                                                 \
        b8           found      = false;                                                                                                             \
        while (iterations < (hset_ptr)->capacity) {                                                                                                  \
            if (!(hset_ptr)->occupied[index]) {                                                                                                      \
                (hset_ptr)->items[index]    = item_var;                                                                                              \
                (hset_ptr)->occupied[index] = true;                                                                                                  \
                (hset_ptr)->length++;                                                                                                                \
                break;                                                                                                                               \
            }                                                                                                                                        \
            if (nya_memcmp(&(hset_ptr)->items[index], &item_var, sizeof(item_var)) == 0) {                                                           \
                found = true;                                                                                                                        \
                break;                                                                                                                               \
            }                                                                                                                                        \
            index = (index + 1) % (hset_ptr)->capacity;                                                                                              \
            iterations++;                                                                                                                            \
        }                                                                                                                                            \
        /* Storing and matching both break out early, so a loop that ran all the way to the bound is                                                 \
         * exactly the one that found nowhere to put the item. The load factor is supposed to make                                                   \
         * that unreachable; dropping the item silently surfaced as a failed lookup much later.  */                                                  \
        nya_assert(iterations < (hset_ptr)->capacity, "Hash set is full; the item was dropped rather than stored.");                                 \
        (void)found;                                                                                                                                 \
    })

#define nya_hset_resize_and_rehash(hset_ptr, new_capacity)                                                                                           \
    ({                                                                                                                                               \
        nya_assert((new_capacity) >= (hset_ptr)->length);                                                                                            \
        typeof(*(hset_ptr)) old_hset = *(hset_ptr);                                                                                                  \
                                                                                                                                                     \
        (hset_ptr)->items    = nya_arena_alloc((hset_ptr)->arena, (new_capacity) * sizeof(*(hset_ptr)->items));                                      \
        (hset_ptr)->occupied = nya_arena_alloc((hset_ptr)->arena, (new_capacity) * sizeof(b8));                                                      \
        nya_memset((hset_ptr)->occupied, 0, (new_capacity) * sizeof(b8));                                                                            \
        (hset_ptr)->capacity = (new_capacity);                                                                                                       \
        (hset_ptr)->length   = 0;                                                                                                                    \
                                                                                                                                                     \
        for (u64 i = 0; i < old_hset.capacity; i++) {                                                                                                \
            if (!old_hset.occupied[i]) continue;                                                                                                     \
            _nya_hset_insert_unchecked(hset_ptr, old_hset.items[i]);                                                                                 \
        }                                                                                                                                            \
                                                                                                                                                     \
        nya_arena_free((hset_ptr)->arena, old_hset.items, sizeof(*old_hset.items) * old_hset.capacity);                                              \
        nya_arena_free((hset_ptr)->arena, old_hset.occupied, sizeof(*old_hset.occupied) * old_hset.capacity);                                        \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ACCESS MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_hset_contains(hset_ptr, item)                                                                                                            \
    ({                                                                                                                                               \
        nya_assert_type_match(item, (hset_ptr)->items[0]);                                                                                           \
        typeof(item) item_var   = item;                                                                                                              \
        bool         contains   = false;                                                                                                             \
        u64          index      = (hset_ptr)->capacity == 0 ? 0 : nya_hash_fnv1a(&item_var, sizeof(item_var)) % (hset_ptr)->capacity;                \
        u64          iterations = 0;                                                                                                                 \
        while (iterations < (hset_ptr)->capacity) {                                                                                                  \
            if (!(hset_ptr)->occupied[index]) break;                                                                                                 \
            if (nya_memcmp(&(hset_ptr)->items[index], &item_var, sizeof(item_var)) == 0) {                                                           \
                contains = true;                                                                                                                     \
                break;                                                                                                                               \
            }                                                                                                                                        \
            index = (index + 1) % (hset_ptr)->capacity;                                                                                              \
            iterations++;                                                                                                                            \
        }                                                                                                                                            \
        contains;                                                                                                                                    \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INSERT / REMOVE MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_hset_insert(hset_ptr, item)                                                                                                              \
    ({                                                                                                                                               \
        nya_assert_type_match(item, (hset_ptr)->items[0]);                                                                                           \
        /* Same zero capacity case as nya_hmap_set; see the note there. */                                                                           \
        if ((hset_ptr)->capacity == 0) {                                                                                                             \
            nya_hset_resize_and_rehash(hset_ptr, _NYA_HASHSET_DEFAULT_CAPACITY);                                                                     \
        } else if (((f32)((hset_ptr)->length + 1) / (f32)(hset_ptr)->capacity) > _NYA_HASHSET_LOAD_FACTOR) {                                         \
            nya_hset_resize_and_rehash(hset_ptr, (hset_ptr)->capacity * 2);                                                                          \
        }                                                                                                                                            \
        /* The probe loop lives in _nya_hset_insert_unchecked and nowhere else. This used to spell                                                   \
         * out its own copy — same probe, same duplicate check, same "full" message — which is two                                               \
         * places to keep in step for no gain. nya_hmap_set and nya_dict_set already delegate.  */                                                   \
        _nya_hset_insert_unchecked(hset_ptr, item);                                                                                                  \
    })

#define nya_hset_remove(hset_ptr, item)                                                                                                              \
    ({                                                                                                                                               \
        nya_assert_type_match(item, (hset_ptr)->items[0]);                                                                                           \
        typeof(item) item_var   = item;                                                                                                              \
        u64          index      = (hset_ptr)->capacity == 0 ? 0 : nya_hash_fnv1a(&item_var, sizeof(item_var)) % (hset_ptr)->capacity;                \
        u64          iterations = 0;                                                                                                                 \
        while (iterations < (hset_ptr)->capacity) {                                                                                                  \
            if (!(hset_ptr)->occupied[index]) break;                                                                                                 \
            if (nya_memcmp(&(hset_ptr)->items[index], &item_var, sizeof(item_var)) == 0) {                                                           \
                (hset_ptr)->occupied[index] = false;                                                                                                 \
                (hset_ptr)->length--;                                                                                                                \
                /* Rehash subsequent entries in the probe chain */                                                                                   \
                u64 _rehash_idx = (index + 1) % (hset_ptr)->capacity;                                                                                \
                while ((hset_ptr)->occupied[_rehash_idx]) {                                                                                          \
                    typeof((hset_ptr)->items[0]) _rehash_item = (hset_ptr)->items[_rehash_idx];                                                      \
                    (hset_ptr)->occupied[_rehash_idx]         = false;                                                                               \
                    (hset_ptr)->length--;                                                                                                            \
                    _nya_hset_insert_unchecked(hset_ptr, _rehash_item);                                                                              \
                    _rehash_idx = (_rehash_idx + 1) % (hset_ptr)->capacity;                                                                          \
                }                                                                                                                                    \
                break;                                                                                                                               \
            }                                                                                                                                        \
            index = (index + 1) % (hset_ptr)->capacity;                                                                                              \
            iterations++;                                                                                                                            \
        }                                                                                                                                            \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SET OPERATION MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * All four set operations walk one set and mutate another, and every one of them has to tolerate
 * being handed the *same* set twice — `nya_hset_union(a, a)`, `a \ a`, `a △ a` and `a ∩ a` are all
 * things a caller writes, and test_hset.c writes them.
 *
 * Iterating a table while mutating it does not work here, for two separate reasons:
 *
 *  - nya_hset_remove is a backward shift deletion. After clearing a slot it reinserts the rest of
 *    the probe chain at the first free slot from each entry's own hash, frequently an index *below*
 *    the cursor. An entry moved there is never looked at again, so it survives an operation that
 *    should have removed it. Nine keys in a table of sixteen is enough to see it.
 *  - nya_hset_insert checks the load factor before it knows whether the item is a duplicate, so it
 *    can call nya_hset_resize_and_rehash, which frees `items` and `occupied`. A loop reading those
 *    is then reading freed memory — which is how an aliased union, a no-op by definition, can fault.
 *
 * So each operation resolves its input to a flat array first and iterates that. `_nya_hset_snapshot`
 * takes a copy of the source's items; nya_hset_intersection instead collects the doomed subset of
 * the destination, since it is the one operation whose predicate is evaluated against the source
 * while the destination is what shrinks.
 *
 * Cost is one arena allocation per call, freed before returning. The +1 keeps an empty set from
 * asking the arena for nothing.
 */
#define _nya_hset_snapshot(src_hset_ptr, items_name, count_name, bytes_name)                                                                         \
    u64                               bytes_name = ((src_hset_ptr)->length + 1) * sizeof(*(src_hset_ptr)->items);                                    \
    typeof((src_hset_ptr)->items[0])* items_name = nya_arena_alloc((src_hset_ptr)->arena, bytes_name);                                               \
    u64                               count_name = 0;                                                                                                \
    for (u64 _snapshot_idx = 0; _snapshot_idx < (src_hset_ptr)->capacity; _snapshot_idx++) {                                                         \
        if ((src_hset_ptr)->occupied[_snapshot_idx]) items_name[count_name++] = (src_hset_ptr)->items[_snapshot_idx];                                \
    }

#define nya_hset_union(dest_hset_ptr, src_hset_ptr)                                                                                                  \
    do {                                                                                                                                             \
        NYA_Arena* _union_arena = (src_hset_ptr)->arena;                                                                                             \
        _nya_hset_snapshot(src_hset_ptr, _union_items, _union_count, _union_bytes);                                                                  \
                                                                                                                                                     \
        for (u64 _union_i = 0; _union_i < _union_count; _union_i++) nya_hset_insert(dest_hset_ptr, _union_items[_union_i]);                          \
                                                                                                                                                     \
        nya_arena_free(_union_arena, _union_items, _union_bytes);                                                                                    \
    } while (0)

#define nya_hset_intersection(dest_hset_ptr, src_hset_ptr)                                                                                           \
    do {                                                                                                                                             \
        NYA_Arena*                         _inter_arena        = (dest_hset_ptr)->arena;                                                             \
        u64                                _inter_bytes        = ((dest_hset_ptr)->length + 1) * sizeof(*(dest_hset_ptr)->items);                    \
        typeof((dest_hset_ptr)->items[0])* _inter_doomed       = nya_arena_alloc(_inter_arena, _inter_bytes);                                        \
        u64                                _inter_doomed_count = 0;                                                                                  \
                                                                                                                                                     \
        for (u64 _inter_idx = 0; _inter_idx < (dest_hset_ptr)->capacity; _inter_idx++) {                                                             \
            if (!(dest_hset_ptr)->occupied[_inter_idx]) continue;                                                                                    \
                                                                                                                                                     \
            typeof((dest_hset_ptr)->items[0]) _inter_item = (dest_hset_ptr)->items[_inter_idx];                                                      \
            if (!nya_hset_contains(src_hset_ptr, _inter_item)) _inter_doomed[_inter_doomed_count++] = _inter_item;                                   \
        }                                                                                                                                            \
                                                                                                                                                     \
        for (u64 _inter_i = 0; _inter_i < _inter_doomed_count; _inter_i++) nya_hset_remove(dest_hset_ptr, _inter_doomed[_inter_i]);                  \
                                                                                                                                                     \
        nya_arena_free(_inter_arena, _inter_doomed, _inter_bytes);                                                                                   \
    } while (0)

#define nya_hset_difference(dest_hset_ptr, src_hset_ptr)                                                                                             \
    do {                                                                                                                                             \
        NYA_Arena* _diff_arena = (src_hset_ptr)->arena;                                                                                              \
        _nya_hset_snapshot(src_hset_ptr, _diff_items, _diff_count, _diff_bytes);                                                                     \
                                                                                                                                                     \
        for (u64 _diff_i = 0; _diff_i < _diff_count; _diff_i++) {                                                                                    \
            if (nya_hset_contains(dest_hset_ptr, _diff_items[_diff_i])) nya_hset_remove(dest_hset_ptr, _diff_items[_diff_i]);                        \
        }                                                                                                                                            \
                                                                                                                                                     \
        nya_arena_free(_diff_arena, _diff_items, _diff_bytes);                                                                                       \
    } while (0)

#define nya_hset_symmetric_difference(dest_hset_ptr, src_hset_ptr)                                                                                   \
    do {                                                                                                                                             \
        NYA_Arena* _sym_arena = (src_hset_ptr)->arena;                                                                                               \
        _nya_hset_snapshot(src_hset_ptr, _sym_items, _sym_count, _sym_bytes);                                                                        \
                                                                                                                                                     \
        for (u64 _sym_i = 0; _sym_i < _sym_count; _sym_i++) {                                                                                        \
            if (nya_hset_contains(dest_hset_ptr, _sym_items[_sym_i]))                                                                                \
                nya_hset_remove(dest_hset_ptr, _sym_items[_sym_i]);                                                                                  \
            else                                                                                                                                     \
                nya_hset_insert(dest_hset_ptr, _sym_items[_sym_i]);                                                                                  \
        }                                                                                                                                            \
                                                                                                                                                     \
        nya_arena_free(_sym_arena, _sym_items, _sym_bytes);                                                                                          \
    } while (0)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MEMORY MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_hset_copy(hset_ptr)                                                                                                                      \
    ({                                                                                                                                               \
        typeof(*(hset_ptr)) copy = {                                                                                                                 \
            .length   = (hset_ptr)->length,                                                                                                          \
            .capacity = (hset_ptr)->capacity,                                                                                                        \
            .arena    = (hset_ptr)->arena,                                                                                                           \
            .items    = nya_arena_copy((hset_ptr)->arena, (hset_ptr)->items, sizeof(*(hset_ptr)->items) * (hset_ptr)->capacity),                     \
            .occupied = nya_arena_copy((hset_ptr)->arena, (hset_ptr)->occupied, sizeof(*(hset_ptr)->occupied) * (hset_ptr)->capacity),               \
        };                                                                                                                                           \
        copy;                                                                                                                                        \
    })

#define nya_hset_move(hset_ptr, new_arena_ptr)                                                                                                       \
    ({                                                                                                                                               \
        nya_assert_type_match(new_arena_ptr, (hset_ptr)->arena);                                                                                     \
        NYA_Arena*          _hset_move_old_arena = (hset_ptr)->arena;                                                                                \
        typeof(*(hset_ptr)) _hset_move_tmp       = {                                                                                                 \
            .items = nya_arena_move(_hset_move_old_arena, new_arena_ptr, (hset_ptr)->items, sizeof(*(hset_ptr)->items) * (hset_ptr)->capacity),      \
            .occupied =                                                                                                                              \
                nya_arena_move(_hset_move_old_arena, new_arena_ptr, (hset_ptr)->occupied, sizeof(*(hset_ptr)->occupied) * (hset_ptr)->capacity),     \
            .length   = (hset_ptr)->length,                                                                                                          \
            .capacity = (hset_ptr)->capacity,                                                                                                        \
            .arena    = new_arena_ptr                                                                                                                \
        };                                                                                                                                           \
        typeof(hset_ptr) _hset_move_new_ptr = nya_arena_alloc(new_arena_ptr, sizeof(*(hset_ptr)));                                                   \
        *_hset_move_new_ptr                 = _hset_move_tmp;                                                                                        \
        nya_arena_free(_hset_move_old_arena, hset_ptr, sizeof(*(hset_ptr)));                                                                         \
        (hset_ptr) = _hset_move_new_ptr;                                                                                                             \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ITERATOR MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_hset_foreach(hset_ptr, item_name)                                                                                                        \
    for (u64 _hset_foreach_idx = 0; _hset_foreach_idx < (hset_ptr)->capacity; _hset_foreach_idx++)                                                   \
        if ((hset_ptr)->occupied[_hset_foreach_idx])                                                                                                 \
            for (int _hset_foreach_once = 1; _hset_foreach_once; _hset_foreach_once = 0)                                                             \
                for (typeof((hset_ptr)->items[0]) item_name = (hset_ptr)->items[_hset_foreach_idx]; _hset_foreach_once; _hset_foreach_once = 0)

/**
 * @file base_hmap.h
 *
 * API Overview:
 * - nya_hmap_create(arena_ptr, key_type, value_type)
 * - nya_hmap_create_with_capacity(arena_ptr, key_type, value_type, initial
 * - nya_hmap_clear(hmap_ptr)
 * - nya_hmap_destroy(hmap_ptr)
 * - nya_hmap_resize_and_rehash(hmap_ptr, new_capacity)
 * - nya_hmap_contains(hmap_ptr, key)
 * - nya_hmap_get(hmap_ptr, key)
 * - nya_hmap_set(hmap_ptr, key, value)
 * - nya_hmap_remove(hmap_ptr, key)
 * - nya_hmap_copy(hmap_ptr)
 * - nya_hmap_move(hmap_ptr, new_arena_ptr)
 * - nya_hmap_foreach_key(hmap_ptr, key_name)
 * - nya_hmap_foreach_value(hmap_ptr, value_name)
 *
 * Example:
 * ```c
 * typedef struct {
 *  u32   id;
 *  char* name;
 * } Player;
 * nya_derive_hmap(u32, Player);
 *
 * NYA_Arena* arena = nya_arena_create(...);
 * NYA_HMapᐸu32ˏPlayerᐳ* players = nya_hmap_create(arena, u32, Player);
 *
 * nya_hmap_set(players, 1, (Player){ .id = 1, .name = "Alice" });
 * nya_hmap_set(players, 2, (Player){ .id = 2, .name = "Bob" });
 *
 * Player* alice = nya_hmap_get(players, 1);
 * if (alice != nullptr) {
 *  ...
 * }
 *
 * nya_arena_destroy(arena);
 * ```
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

#define _NYA_HASHMAP_DEFAULT_CAPACITY 64
#define _NYA_HASHMAP_LOAD_FACTOR      0.75F

/** Name of the hash map type derived for a key/value pair, e.g. NYA_HMapᐸu32ˏu64ᐳ. */
#define _nya_derive_hmap_name(key_type, value_type) nya_template(NYA_HMap, key_type, value_type)

/** Names of the byte-wise default hash/equality functions derived alongside each hash map. */
#define _nya_hmap_hash_bytes_name(key_type, value_type)   nya_template(_nya_hmap_hash_bytes, key_type, value_type)
#define _nya_hmap_equals_bytes_name(key_type, value_type) nya_template(_nya_hmap_equals_bytes, key_type, value_type)

#define nya_derive_hmap(key_type, value_type)                                                                                                        \
    typedef struct {                                                                                                                                 \
        u64         length;                                                                                                                          \
        u64         capacity;                                                                                                                        \
        key_type*   keys;                                                                                                                            \
        value_type* values;                                                                                                                          \
        b8*         occupied;                                                                                                                        \
        NYA_Arena*  arena;                                                                                                                           \
        u64 (*hash)(const key_type* key);                                                                                                            \
        b8 (*equals)(const key_type* a, const key_type* b);                                                                                          \
    } _nya_derive_hmap_name(key_type, value_type);                                                                                                   \
    __attr_allow_unused static u64 _nya_hmap_hash_bytes_name(key_type, value_type)(const key_type* key) {                                            \
        return nya_hash_fnv1a(key, sizeof(*key));                                                                                                    \
    }                                                                                                                                                \
    __attr_allow_unused static b8 _nya_hmap_equals_bytes_name(key_type, value_type)(const key_type* a, const key_type* b) {                          \
        return nya_memcmp(a, b, sizeof(*a)) == 0;                                                                                                    \
    }

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CREATION MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The byte-wise defaults hash and compare the key's raw bytes, which is right for scalars and
 * wrong for anything holding a pointer: two equal strings at different addresses would not match.
 * Such keys must pass their own functions, which is what nya_dict does for NYA_CString.
 */
#define nya_hmap_create(arena_ptr, key_type, value_type)                                                                                             \
    nya_hmap_create_with_fns(                                                                                                                        \
        arena_ptr,                                                                                                                                   \
        key_type,                                                                                                                                    \
        value_type,                                                                                                                                  \
        _nya_hmap_hash_bytes_name(key_type, value_type),                                                                                             \
        _nya_hmap_equals_bytes_name(key_type, value_type)                                                                                            \
    )
#define nya_hmap_create_with_fns(arena_ptr, key_type, value_type, hash_fn, equals_fn)                                                                \
    nya_hmap_create_with_capacity_and_fns(arena_ptr, key_type, value_type, _NYA_HASHMAP_DEFAULT_CAPACITY, hash_fn, equals_fn)
#define nya_hmap_create_with_capacity(arena_ptr, key_type, value_type, initial_capacity)                                                             \
    nya_hmap_create_with_capacity_and_fns(                                                                                                           \
        arena_ptr,                                                                                                                                   \
        key_type,                                                                                                                                    \
        value_type,                                                                                                                                  \
        initial_capacity,                                                                                                                            \
        _nya_hmap_hash_bytes_name(key_type, value_type),                                                                                             \
        _nya_hmap_equals_bytes_name(key_type, value_type)                                                                                            \
    )
#define nya_hmap_create_with_capacity_and_fns(arena_ptr, key_type, value_type, initial_capacity, hash_fn, equals_fn)                                 \
    ({                                                                                                                                               \
        _nya_derive_hmap_name(key_type, value_type)* _hmap = nya_arena_alloc(arena_ptr, sizeof(_nya_derive_hmap_name(key_type, value_type)));        \
        *_hmap = nya_hmap_create_with_capacity_and_fns_on_stack(arena_ptr, key_type, value_type, initial_capacity, hash_fn, equals_fn);              \
        _hmap;                                                                                                                                       \
    })

#define nya_hmap_create_on_stack(arena_ptr, key_type, value_type)                                                                                    \
    nya_hmap_create_with_capacity_and_fns_on_stack(                                                                                                  \
        arena_ptr,                                                                                                                                   \
        key_type,                                                                                                                                    \
        value_type,                                                                                                                                  \
        _NYA_HASHMAP_DEFAULT_CAPACITY,                                                                                                               \
        _nya_hmap_hash_bytes_name(key_type, value_type),                                                                                             \
        _nya_hmap_equals_bytes_name(key_type, value_type)                                                                                            \
    )
#define nya_hmap_create_with_capacity_on_stack(arena_ptr, key_type, value_type, initial_capacity)                                                    \
    nya_hmap_create_with_capacity_and_fns_on_stack(                                                                                                  \
        arena_ptr,                                                                                                                                   \
        key_type,                                                                                                                                    \
        value_type,                                                                                                                                  \
        initial_capacity,                                                                                                                            \
        _nya_hmap_hash_bytes_name(key_type, value_type),                                                                                             \
        _nya_hmap_equals_bytes_name(key_type, value_type)                                                                                            \
    )
#define nya_hmap_create_with_capacity_and_fns_on_stack(arena_ptr, key_type, value_type, initial_capacity, hash_fn, equals_fn)                        \
    ({                                                                                                                                               \
        _nya_derive_hmap_name(key_type, value_type) _hmap = {                                                                                        \
            .length   = 0,                                                                                                                           \
            .capacity = (initial_capacity),                                                                                                          \
            .arena    = (arena_ptr),                                                                                                                 \
            .keys     = nya_arena_alloc(arena_ptr, (initial_capacity) * sizeof(key_type)),                                                           \
            .values   = nya_arena_alloc(arena_ptr, (initial_capacity) * sizeof(value_type)),                                                         \
            .occupied = nya_arena_alloc(arena_ptr, (initial_capacity) * sizeof(b8)),                                                                 \
            .hash     = (hash_fn),                                                                                                                   \
            .equals   = (equals_fn),                                                                                                                 \
        };                                                                                                                                           \
        nya_memset(_hmap.occupied, 0, (initial_capacity) * sizeof(b8));                                                                              \
        _hmap;                                                                                                                                       \
    })

#define nya_hmap_clear(hmap_ptr)                                                                                                                     \
    ({                                                                                                                                               \
        nya_memset((hmap_ptr)->occupied, 0, (hmap_ptr)->capacity * sizeof(b8));                                                                      \
        (hmap_ptr)->length = 0;                                                                                                                      \
    })

#define nya_hmap_destroy(hmap_ptr)                                                                                                                   \
    ({                                                                                                                                               \
        nya_arena_free((hmap_ptr)->arena, (hmap_ptr)->keys, sizeof(*(hmap_ptr)->keys) * (hmap_ptr)->capacity);                                       \
        nya_arena_free((hmap_ptr)->arena, (hmap_ptr)->values, sizeof(*(hmap_ptr)->values) * (hmap_ptr)->capacity);                                   \
        nya_arena_free((hmap_ptr)->arena, (hmap_ptr)->occupied, sizeof(*(hmap_ptr)->occupied) * (hmap_ptr)->capacity);                               \
        nya_arena_free((hmap_ptr)->arena, hmap_ptr, sizeof(*(hmap_ptr)));                                                                            \
        (hmap_ptr) = nullptr;                                                                                                                        \
    })

#define nya_hmap_destroy_on_stack(hmap_ptr)                                                                                                          \
    ({                                                                                                                                               \
        nya_arena_free((hmap_ptr).arena, (hmap_ptr).keys, sizeof(*(hmap_ptr).keys) * (hmap_ptr).capacity);                                           \
        nya_arena_free((hmap_ptr).arena, (hmap_ptr).values, sizeof(*(hmap_ptr).values) * (hmap_ptr).capacity);                                       \
        nya_arena_free((hmap_ptr).arena, (hmap_ptr).occupied, sizeof(*(hmap_ptr).occupied) * (hmap_ptr).capacity);                                   \
        (hmap_ptr).keys     = nullptr;                                                                                                               \
        (hmap_ptr).values   = nullptr;                                                                                                               \
        (hmap_ptr).occupied = nullptr;                                                                                                               \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RESIZE AND REHASH MACRO
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _nya_hmap_set_unchecked(hmap_ptr, key, value)                                                                                                \
    ({                                                                                                                                               \
        nya_assert_type_match(key, (hmap_ptr)->keys[0]);                                                                                             \
        nya_assert_type_match(value, (hmap_ptr)->values[0]);                                                                                         \
        typeof(key)   key_var    = key;                                                                                                              \
        typeof(value) value_var  = value;                                                                                                            \
        u64           index      = (hmap_ptr)->hash(&key_var) % (hmap_ptr)->capacity;                                                                \
        u64           iterations = 0;                                                                                                                \
        b8            updated    = false;                                                                                                            \
        while (iterations < (hmap_ptr)->capacity) {                                                                                                  \
            if (!(hmap_ptr)->occupied[index]) {                                                                                                      \
                (hmap_ptr)->keys[index]     = key_var;                                                                                               \
                (hmap_ptr)->values[index]   = value_var;                                                                                             \
                (hmap_ptr)->occupied[index] = true;                                                                                                  \
                (hmap_ptr)->length++;                                                                                                                \
                break;                                                                                                                               \
            }                                                                                                                                        \
            if ((hmap_ptr)->equals(&(hmap_ptr)->keys[index], &key_var)) {                                                                            \
                (hmap_ptr)->values[index] = value_var;                                                                                               \
                updated                   = true;                                                                                                    \
                break;                                                                                                                               \
            }                                                                                                                                        \
            index = (index + 1) % (hmap_ptr)->capacity;                                                                                              \
            iterations++;                                                                                                                            \
        }                                                                                                                                            \
        (void)updated;                                                                                                                               \
    })

#define nya_hmap_resize_and_rehash(hmap_ptr, new_capacity)                                                                                           \
    ({                                                                                                                                               \
        typeof((hmap_ptr)->keys[0])*   old_keys     = (hmap_ptr)->keys;                                                                              \
        typeof((hmap_ptr)->values[0])* old_values   = (hmap_ptr)->values;                                                                            \
        b8*                            old_occupied = (hmap_ptr)->occupied;                                                                          \
        u64                            old_capacity = (hmap_ptr)->capacity;                                                                          \
                                                                                                                                                     \
        (hmap_ptr)->keys     = nya_arena_alloc((hmap_ptr)->arena, (new_capacity) * sizeof(*(hmap_ptr)->keys));                                       \
        (hmap_ptr)->values   = nya_arena_alloc((hmap_ptr)->arena, (new_capacity) * sizeof(*(hmap_ptr)->values));                                     \
        (hmap_ptr)->occupied = nya_arena_alloc((hmap_ptr)->arena, (new_capacity) * sizeof(b8));                                                      \
        nya_memset((hmap_ptr)->occupied, 0, (new_capacity) * sizeof(b8));                                                                            \
        (hmap_ptr)->capacity = (new_capacity);                                                                                                       \
        (hmap_ptr)->length   = 0;                                                                                                                    \
                                                                                                                                                     \
        for (u64 i = 0; i < old_capacity; i++) {                                                                                                     \
            if (!old_occupied[i]) continue;                                                                                                          \
            _nya_hmap_set_unchecked(hmap_ptr, old_keys[i], old_values[i]);                                                                           \
        }                                                                                                                                            \
                                                                                                                                                     \
        nya_arena_free((hmap_ptr)->arena, old_keys, sizeof(*old_keys) * old_capacity);                                                               \
        nya_arena_free((hmap_ptr)->arena, old_values, sizeof(*old_values) * old_capacity);                                                           \
        nya_arena_free((hmap_ptr)->arena, old_occupied, sizeof(*old_occupied) * old_capacity);                                                       \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ACCESS MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_hmap_contains(hmap_ptr, key)                                                                                                             \
    ({                                                                                                                                               \
        nya_assert_type_match(key, (hmap_ptr)->keys[0]);                                                                                             \
        typeof(key) key_var    = key;                                                                                                                \
        bool        contains   = false;                                                                                                              \
        u64         index      = (hmap_ptr)->hash(&key_var) % (hmap_ptr)->capacity;                                                                  \
        u64         iterations = 0;                                                                                                                  \
        while (iterations < (hmap_ptr)->capacity) {                                                                                                  \
            if (!(hmap_ptr)->occupied[index]) break;                                                                                                 \
            if ((hmap_ptr)->equals(&(hmap_ptr)->keys[index], &key_var)) {                                                                            \
                contains = true;                                                                                                                     \
                break;                                                                                                                               \
            }                                                                                                                                        \
            index = (index + 1) % (hmap_ptr)->capacity;                                                                                              \
            iterations++;                                                                                                                            \
        }                                                                                                                                            \
        contains;                                                                                                                                    \
    })

#define nya_hmap_get(hmap_ptr, key)                                                                                                                  \
    ({                                                                                                                                               \
        nya_assert_type_match(key, (hmap_ptr)->keys[0]);                                                                                             \
        typeof(key) key_var    = key;                                                                                                                \
        bool        contains   = false;                                                                                                              \
        u64         index      = (hmap_ptr)->hash(&key_var) % (hmap_ptr)->capacity;                                                                  \
        u64         iterations = 0;                                                                                                                  \
        while (iterations < (hmap_ptr)->capacity) {                                                                                                  \
            if (!(hmap_ptr)->occupied[index]) break;                                                                                                 \
            if ((hmap_ptr)->equals(&(hmap_ptr)->keys[index], &key_var)) {                                                                            \
                contains = true;                                                                                                                     \
                break;                                                                                                                               \
            }                                                                                                                                        \
            index = (index + 1) % (hmap_ptr)->capacity;                                                                                              \
            iterations++;                                                                                                                            \
        }                                                                                                                                            \
        contains ? &(hmap_ptr)->values[index] : nullptr;                                                                                             \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INSERT / REMOVE MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_hmap_set(hmap_ptr, key, value)                                                                                                           \
    ({                                                                                                                                               \
        nya_assert_type_match(key, (hmap_ptr)->keys[0]);                                                                                             \
        nya_assert_type_match(value, (hmap_ptr)->values[0]);                                                                                         \
        if (((f32)((hmap_ptr)->length + 1) / (f32)(hmap_ptr)->capacity) > _NYA_HASHMAP_LOAD_FACTOR) {                                                \
            nya_hmap_resize_and_rehash(hmap_ptr, (hmap_ptr)->capacity * 2);                                                                          \
        }                                                                                                                                            \
        _nya_hmap_set_unchecked(hmap_ptr, key, value);                                                                                               \
    })

#define nya_hmap_remove(hmap_ptr, key)                                                                                                               \
    ({                                                                                                                                               \
        nya_assert_type_match(key, (hmap_ptr)->keys[0]);                                                                                             \
        typeof(key) key_var    = key;                                                                                                                \
        u64         index      = (hmap_ptr)->hash(&key_var) % (hmap_ptr)->capacity;                                                                  \
        u64         iterations = 0;                                                                                                                  \
        while (iterations < (hmap_ptr)->capacity) {                                                                                                  \
            if (!(hmap_ptr)->occupied[index]) break;                                                                                                 \
            if ((hmap_ptr)->equals(&(hmap_ptr)->keys[index], &key_var)) {                                                                            \
                (hmap_ptr)->occupied[index] = false;                                                                                                 \
                (hmap_ptr)->length--;                                                                                                                \
                /* Rehash subsequent entries in the probe chain */                                                                                   \
                u64 _rehash_idx = (index + 1) % (hmap_ptr)->capacity;                                                                                \
                while ((hmap_ptr)->occupied[_rehash_idx]) {                                                                                          \
                    typeof((hmap_ptr)->keys[0])   _rehash_key   = (hmap_ptr)->keys[_rehash_idx];                                                     \
                    typeof((hmap_ptr)->values[0]) _rehash_value = (hmap_ptr)->values[_rehash_idx];                                                   \
                    (hmap_ptr)->occupied[_rehash_idx]           = false;                                                                             \
                    (hmap_ptr)->length--;                                                                                                            \
                    u64 _new_idx  = (hmap_ptr)->hash(&_rehash_key) % (hmap_ptr)->capacity;                                                           \
                    u64 _new_iter = 0;                                                                                                               \
                    while (_new_iter < (hmap_ptr)->capacity) {                                                                                       \
                        if (!(hmap_ptr)->occupied[_new_idx]) {                                                                                       \
                            (hmap_ptr)->keys[_new_idx]     = _rehash_key;                                                                            \
                            (hmap_ptr)->values[_new_idx]   = _rehash_value;                                                                          \
                            (hmap_ptr)->occupied[_new_idx] = true;                                                                                   \
                            (hmap_ptr)->length++;                                                                                                    \
                            break;                                                                                                                   \
                        }                                                                                                                            \
                        _new_idx = (_new_idx + 1) % (hmap_ptr)->capacity;                                                                            \
                        _new_iter++;                                                                                                                 \
                    }                                                                                                                                \
                    _rehash_idx = (_rehash_idx + 1) % (hmap_ptr)->capacity;                                                                          \
                }                                                                                                                                    \
                break;                                                                                                                               \
            }                                                                                                                                        \
            index = (index + 1) % (hmap_ptr)->capacity;                                                                                              \
            iterations++;                                                                                                                            \
        }                                                                                                                                            \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MEMORY MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_hmap_copy(hmap_ptr)                                                                                                                      \
    ({                                                                                                                                               \
        typeof(*(hmap_ptr)) copy = {                                                                                                                 \
            .length   = (hmap_ptr)->length,                                                                                                          \
            .capacity = (hmap_ptr)->capacity,                                                                                                        \
            .arena    = (hmap_ptr)->arena,                                                                                                           \
            .keys     = nya_arena_copy((hmap_ptr)->arena, (hmap_ptr)->keys, sizeof(*(hmap_ptr)->keys) * (hmap_ptr)->capacity),                       \
            .values   = nya_arena_copy((hmap_ptr)->arena, (hmap_ptr)->values, sizeof(*(hmap_ptr)->values) * (hmap_ptr)->capacity),                   \
            .occupied = nya_arena_copy((hmap_ptr)->arena, (hmap_ptr)->occupied, sizeof(*(hmap_ptr)->occupied) * (hmap_ptr)->capacity),               \
        };                                                                                                                                           \
        copy;                                                                                                                                        \
    })

#define nya_hmap_move(hmap_ptr, new_arena_ptr)                                                                                                       \
    ({                                                                                                                                               \
        nya_assert_type_match(new_arena_ptr, (hmap_ptr)->arena);                                                                                     \
        NYA_Arena*          _hmap_move_old_arena = (hmap_ptr)->arena;                                                                                \
        typeof(*(hmap_ptr)) _hmap_move_tmp       = {                                                                                                 \
            .keys   = nya_arena_move(_hmap_move_old_arena, new_arena_ptr, (hmap_ptr)->keys, sizeof(*(hmap_ptr)->keys) * (hmap_ptr)->capacity),       \
            .values = nya_arena_move(_hmap_move_old_arena, new_arena_ptr, (hmap_ptr)->values, sizeof(*(hmap_ptr)->values) * (hmap_ptr)->capacity),   \
            .occupied =                                                                                                                              \
                nya_arena_move(_hmap_move_old_arena, new_arena_ptr, (hmap_ptr)->occupied, sizeof(*(hmap_ptr)->occupied) * (hmap_ptr)->capacity),     \
            .length   = (hmap_ptr)->length,                                                                                                          \
            .capacity = (hmap_ptr)->capacity,                                                                                                        \
            .arena    = (new_arena_ptr)                                                                                                              \
        };                                                                                                                                           \
        typeof(hmap_ptr) _hmap_move_new_ptr = nya_arena_alloc(new_arena_ptr, sizeof(*(hmap_ptr)));                                                   \
        *_hmap_move_new_ptr                 = _hmap_move_tmp;                                                                                        \
        nya_arena_free(_hmap_move_old_arena, hmap_ptr, sizeof(*(hmap_ptr)));                                                                         \
        (hmap_ptr) = _hmap_move_new_ptr;                                                                                                             \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ITERATOR MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_hmap_foreach_key(hmap_ptr, key_name)                                                                                                     \
    for (u64 _nya_hmap_foreach_index = 0; _nya_hmap_foreach_index < (hmap_ptr)->capacity; _nya_hmap_foreach_index++)                                 \
        if ((hmap_ptr)->occupied[_nya_hmap_foreach_index])                                                                                           \
            for (typeof((hmap_ptr)->keys[0])* key_name = &(hmap_ptr)->keys[_nya_hmap_foreach_index]; key_name != NULL; key_name = NULL)

#define nya_hmap_foreach_value(hmap_ptr, value_name)                                                                                                 \
    for (u64 _nya_hmap_foreach_index = 0; _nya_hmap_foreach_index < (hmap_ptr)->capacity; _nya_hmap_foreach_index++)                                 \
        if ((hmap_ptr)->occupied[_nya_hmap_foreach_index])                                                                                           \
            for (typeof((hmap_ptr)->values[0])* value_name = &(hmap_ptr)->values[_nya_hmap_foreach_index]; value_name != NULL; value_name = NULL)

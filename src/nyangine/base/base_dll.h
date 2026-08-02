/**
 * @file base_dll.h
 *
 * To use those macros:
 * - the dll structure needs to have 'head' and 'tail' pointers
 * - the node structure needs to have 'prev' and 'next' pointers
 *
 * API Overview:
 * - nya_dll_node_link(dll_ptr, prev_ptr, node_ptr, next_ptr)
 * - nya_dll_node_unlink(dll_ptr, node_ptr)
 * - nya_dll_node_push_back(dll_ptr, node_ptr)
 * - nya_dll_node_pop_back(dll_ptr)
 * - nya_dll_node_push_front(dll_ptr, node_ptr)
 * - nya_dll_node_pop_front(dll_ptr)
 * - nya_dll_foreach(dll_ptr, node_name)
 * - nya_dll_foreach_reverse(dll_ptr, node_name)
 * */
#pragma once

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LINKING MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_dll_node_link(dll_ptr, prev_ptr, node_ptr, next_ptr)                                                                                     \
    ({                                                                                                                                               \
        typeof(node_ptr) _prev_ptr = (prev_ptr);                                                                                                     \
        typeof(node_ptr) _next_ptr = (next_ptr);                                                                                                     \
                                                                                                                                                     \
        (node_ptr)->prev = _prev_ptr;                                                                                                                \
        (node_ptr)->next = _next_ptr;                                                                                                                \
        if (_prev_ptr) {                                                                                                                             \
            (_prev_ptr)->next = node_ptr;                                                                                                            \
        } else {                                                                                                                                     \
            (dll_ptr)->head = node_ptr;                                                                                                              \
        }                                                                                                                                            \
        if (_next_ptr) {                                                                                                                             \
            (_next_ptr)->prev = node_ptr;                                                                                                            \
        } else {                                                                                                                                     \
            (dll_ptr)->tail = node_ptr;                                                                                                              \
        }                                                                                                                                            \
    })

#define nya_dll_node_unlink(dll_ptr, node_ptr)                                                                                                       \
    ({                                                                                                                                               \
        if ((node_ptr)->prev) {                                                                                                                      \
            (node_ptr)->prev->next = (node_ptr)->next;                                                                                               \
        } else {                                                                                                                                     \
            (dll_ptr)->head = (node_ptr)->next;                                                                                                      \
        }                                                                                                                                            \
        if ((node_ptr)->next) {                                                                                                                      \
            (node_ptr)->next->prev = (node_ptr)->prev;                                                                                               \
        } else {                                                                                                                                     \
            (dll_ptr)->tail = (node_ptr)->prev;                                                                                                      \
        }                                                                                                                                            \
        (node_ptr)->prev = nullptr;                                                                                                                  \
        (node_ptr)->next = nullptr;                                                                                                                  \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUSH/POP MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_dll_node_push_back(dll_ptr, node_ptr) nya_dll_node_link((dll_ptr), (dll_ptr)->tail, (node_ptr), nullptr);
#define nya_dll_node_pop_back(dll_ptr)                                                                                                               \
    ({                                                                                                                                               \
        typeof((dll_ptr)->tail)(tail_ptr) = (dll_ptr)->tail;                                                                                         \
        nya_dll_node_unlink((dll_ptr), tail_ptr);                                                                                                    \
        tail_ptr;                                                                                                                                    \
    })

#define nya_dll_node_push_front(dll_ptr, node_ptr) nya_dll_node_link((dll_ptr), nullptr, (node_ptr), (dll_ptr)->head);
#define nya_dll_node_pop_front(dll_ptr)                                                                                                              \
    ({                                                                                                                                               \
        typeof((dll_ptr)->head)(head_ptr) = (dll_ptr)->head;                                                                                         \
        nya_dll_node_unlink((dll_ptr), head_ptr);                                                                                                    \
        head_ptr;                                                                                                                                    \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ITERATOR MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_dll_foreach(dll_ptr, node_name)                                                                                                          \
    for (typeof((dll_ptr)->head)(node_name) = (dll_ptr)->head; (node_name) != nullptr; (node_name) = (node_name)->next)

#define nya_dll_foreach_reverse(dll_ptr, node_name)                                                                                                  \
    for (typeof((dll_ptr)->tail)(node_name) = (dll_ptr)->tail; (node_name) != nullptr; (node_name) = (node_name)->prev)

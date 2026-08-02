#pragma once

#include "nyangine/base/base_array.h"
#include "nyangine/base/base_string.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Callback       NYA_Callback;
typedef struct NYA_CallbackSystem NYA_CallbackSystem;
typedef u64                       NYA_CallbackHandle;
nya_derive_array(NYA_Callback);

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_CallbackSystem {
    NYA_Arena*               allocator;
    NYA_ArrayᐸNYA_Callbackᐳ* callbacks;
};

/*
 * ─────────────────────────────────────────────────────────
 * CALLBACK STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_Callback {
    NYA_ConstCString name;
    void*            fn;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API void nya_system_callback_init(void);
NYA_API void nya_system_callback_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * CALLBACK FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/*
 * Named callbacks exist for hot reloading: when the game DLL is swapped, a raw function pointer
 * stored before the swap points into the old image, so callbacks are stored by name and re-resolved
 * against the new one. A shipping build never swaps anything, so it stores the pointer directly.
 *
 * This was `#ifdef NYA_DEBUG`, which is always true — NYA_DEBUG is *defined* in every mode, it just
 * evaluates to 0 in most of them. Shipping builds were therefore paying for the registry, the string
 * names and a lookup per callback, and the direct path below had never been compiled.
 */
#if NYA_CODE_HOT_RELOAD
#define nya_callback(callback) _nya_callback((NYA_Callback){ .name = #callback, .fn = (void*)(callback) })
NYA_API void* nya_callback_get(NYA_CallbackHandle handle);
#else
#define nya_callback(callback)   ((NYA_CallbackHandle)(callback))
#define nya_callback_get(handle) ((void*)(handle))
#endif // NYA_CODE_HOT_RELOAD

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_CallbackHandle _nya_callback(NYA_Callback callback);

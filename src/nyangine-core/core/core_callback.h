#pragma once

#include "nyangine-std/base/base_array.h"
#include "nyangine-std/base/base_string.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Callback       NYA_Callback;
typedef struct NYA_CallbackSystem NYA_CallbackSystem;
typedef u64                       NYA_CallbackHandle;
nya_derive_array(NYA_Callback);

/**
 * The one handle that names no function, so a zeroed struct means "nothing registered here".
 *
 * Both builds agree on it: the registry reserves index zero for a null entry, and a build without the
 * registry carries the pointer itself, which is null. nya_callback_get resolves it to nullptr either
 * way, so a caller can either compare against this or test what it resolved to.
 * */
#define NYA_CALLBACK_HANDLE_NONE ((NYA_CallbackHandle)0)

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
 * Named callbacks exist for hot reloading: a raw function pointer stored before a DLL swap points into
 * the old image, so callbacks are stored by name and re-resolved against the new one; a shipping build
 * never swaps anything and stores the pointer directly.
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

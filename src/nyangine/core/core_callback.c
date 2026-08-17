#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_system_callback_init(void) {
    NYA_App* app = nya_app_get();

    app->callback_system = (NYA_CallbackSystem){
        .allocator = nya_arena_create(.name = "callback_system_allocator"),
    };

    app->callback_system.callbacks = nya_array_create(app->callback_system.allocator, NYA_Callback);

    // zero initialized handles are 0 --> map them to nullptr to indicate missing callback
    nya_array_push_back(
        app->callback_system.callbacks,
        ((NYA_Callback){
            .name = nullptr,
            .fn   = nullptr,
        })
    );

    nya_info("Callback system initialized.");
}

void nya_system_callback_deinit(void) {
    NYA_App* app = nya_app_get();

    nya_array_destroy(app->callback_system.callbacks);
    nya_arena_destroy(app->callback_system.allocator);

    nya_info("Callback system deinitialized.");
}

/*
 * ─────────────────────────────────────────────────────────
 * CALLBACK FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/*
 * Only the hot reloading builds have a registry. In a shipping build nya_callback and
 * nya_callback_get are macros that carry the function pointer directly, so defining these would both
 * be dead weight and collide with the macro of the same name.
 */
#if NYA_CODE_HOT_RELOAD

NYA_CallbackHandle _nya_callback(NYA_Callback callback) {
    NYA_App* app = nya_app_get();

    /*
     * The name is copied into the registry's own arena rather than kept as the caller's pointer.
     *
     * nya_callback stringifies its argument, so the name is a literal living in whichever module
     * compiled the call — and for anything the game registers, that module is the hot reloaded DLL.
     * Reloading dlcloses it, which unmaps the .rodata that literal sits in, and the pointer held
     * here is left dangling into an unmapped page.
     *
     * That is precisely the pointer update_callback_pointers hands to dlsym to find the symbol
     * again, so the first reload faulted with SIGSEGV inside dlsym — reading a name that no longer
     * existed, in order to look up a function that did. The engine's own callbacks survived because
     * they are compiled into the executable, which is why it only ever happened once the game had
     * registered one.
     *
     * The registry outlives every DLL it points into, so the string has to as well.
     * */
    if (callback.name != nullptr) {
        NYA_String* owned = nya_string_from(app->callback_system.allocator, callback.name);
        callback.name     = nya_string_to_cstring(app->callback_system.allocator, owned);
    }

    nya_array_push_back(app->callback_system.callbacks, callback);

    return (NYA_CallbackHandle)(app->callback_system.callbacks->length - 1);
}

void* nya_callback_get(NYA_CallbackHandle handle) {
    NYA_App* app = nya_app_get();

    /*
     * Bounds checked, because a handle is an index and nothing stops a caller inventing one.
     *
     * The registry only exists in hot reloading builds, and the handles a caller holds outlive a
     * reload by design — that is the point of it. A stale handle from before a reload, a
     * default-initialised zero on an empty registry, or a handle from a different NYA_App all
     * indexed straight into the array. Returning null instead makes the failure a null function
     * pointer at the call site rather than a plausible-looking pointer read out of arbitrary
     * memory, and the assertion names the handle while the frame that produced it is still up.
     */
    nya_assert(
        handle < app->callback_system.callbacks->length,
        "Callback handle " FMTu64 " is out of range (only " FMTu64 " are registered)",
        (u64)handle,
        app->callback_system.callbacks->length
    );
    if (handle >= app->callback_system.callbacks->length) return nullptr;

    return app->callback_system.callbacks->items[handle].fn;
}

#endif // NYA_CODE_HOT_RELOAD

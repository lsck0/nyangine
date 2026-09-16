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

    nya_log_info("Callback system initialized.");
}

void nya_system_callback_deinit(void) {
    NYA_App* app = nya_app_get();

    nya_array_destroy(app->callback_system.callbacks);
    nya_arena_destroy(app->callback_system.allocator);

    nya_log_info("Callback system deinitialized.");
}

/*
 * ─────────────────────────────────────────────────────────
 * CALLBACK FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/*
 * Only hot reloading builds have a registry. Shipping builds define these as macros carrying the
 * function pointer, which would collide with definitions here.
 */
#if NYA_CODE_HOT_RELOAD

NYA_CallbackHandle _nya_callback(NYA_Callback callback) {
    NYA_App* app = nya_app_get();

    /*
     * The name is copied into the registry's own arena rather than kept as the caller's pointer.
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
     * Bounds checked because a handle is an index and nothing stops a caller inventing one: a stale
     * handle from before a reload, a zeroed default, or one from a different NYA_App would otherwise
     * index straight into the array. Returning null turns that into a null function pointer at the
     * call site instead of a plausible-looking read out of arbitrary memory.
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

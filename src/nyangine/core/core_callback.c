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

    nya_array_push_back(app->callback_system.callbacks, callback);

    return (NYA_CallbackHandle)(app->callback_system.callbacks->length - 1);
}

void* nya_callback_get(NYA_CallbackHandle handle) {
    NYA_App* app = nya_app_get();

    return app->callback_system.callbacks->items[handle].fn;
}

#endif // NYA_CODE_HOT_RELOAD

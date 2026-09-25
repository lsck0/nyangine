#include "SDL3/SDL_events.h"
#include "SDL3/SDL_video.h"

#include "nyangine-core/nyangine.h"

#if OS_WASM
/*
 * A wasm build is single-threaded and links no os_thread implementation: the event queue is only ever
 * touched from the one thread the module runs on, so its mutex has nothing to lock against and is a
 * no-op, exactly as os_wasm's sleep is. Shadowing the four os-mutex calls here keeps the queue
 * machinery below byte-identical to the native code without a threading dependency a wasm module cannot
 * satisfy; the DOM event source that feeds this queue on the web synthesises NYA_Events directly (see
 * wasm_ui.c), so the SDL-event translation further down is compiled out entirely rather than shimmed.
 */
#define nya_os_mutex_init(m)   (NYA_OS_THREAD_OK) // never an error, since nya_system_events_init checks it
#define nya_os_mutex_deinit(m) ((void)(m))
#define nya_os_mutex_lock(m)   ((void)(m))
#define nya_os_mutex_unlock(m) ((void)(m))
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_WindowHandle _nya_event_window_from_sdl_id(SDL_WindowID sdl_window_id);

/** Packs SDL's per-device instance id into an NYA_InputSource. See NYA_InputSource for what zero means. */
NYA_INTERNAL NYA_InputSource _nya_event_source_from_sdl(NYA_InputDeviceKind kind, u32 which) __attr_no_discard;
NYA_INTERNAL NYA_Event        _nya_event_from_sdl_event(SDL_Event sdl_event);

/**
 * Copies a string SDL owns into memory that outlives the event queue.
 * */
NYA_INTERNAL NYA_ConstCString _nya_event_copy_transient_string(NYA_ConstCString text);
NYA_INTERNAL void             _nya_event_notify_deferred_listeners(NYA_Event* event);
NYA_INTERNAL void             _nya_event_notify_immediate_listeners(NYA_Event* event);

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

NYA_Error nya_system_events_init(void) {
    NYA_App* app = nya_app_get();

    app->event_system = (NYA_EventSystem){
        .allocator              = nya_arena_create(.name = "event_system_allocator"),
        .event_queue_read_index = 0,
    };

    // The queue is filled from callbacks on other threads and drained here, so the mutex is what keeps
    // that safe. Initialize it in place — a mutex lives where it was made — and treat a failure as a
    // clean error rather than a no-op lock that becomes a data race only under load.
    if (nya_os_mutex_init(&app->event_system.event_queue_mutex) != NYA_OS_THREAD_OK) {
        nya_arena_destroy(app->event_system.allocator);
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the event queue mutex failed to initialize");
    }

    app->event_system.event_queue           = nya_array_create(app->event_system.allocator, NYA_Event);
    app->event_system.deferred_event_hooks  = nya_hmap_create(app->event_system.allocator, NYA_EventType, NYA_ArrayᐸNYA_EventHookᐳ);
    app->event_system.immediate_event_hooks = nya_hmap_create(app->event_system.allocator, NYA_EventType, NYA_ArrayᐸNYA_EventHookᐳ);

    nya_log_info("Event system initialized.");
    return NYA_OK;
}

void nya_system_events_deinit(void) {
    NYA_App* app = nya_app_get();

    nya_os_mutex_deinit(&app->event_system.event_queue_mutex);
    nya_array_destroy(app->event_system.event_queue);
    nya_hmap_destroy(app->event_system.deferred_event_hooks);
    nya_hmap_destroy(app->event_system.immediate_event_hooks);

    nya_arena_destroy(app->event_system.allocator);

    nya_log_info("Event system deinitialized.");
}

#if !OS_WASM
void nya_system_event_drain_sdl_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        /*
         * Gamepad events are consumed here rather than converted into an NYA_Event.
         */
        if (nya_system_gamepad_handle_sdl_event(&event)) continue;

        NYA_Event nya_event = _nya_event_from_sdl_event(event);
        if (nya_event.type == NYA_EVENT_INVALID) continue;

        nya_event_dispatch(nya_event);
    }
}
#endif // !OS_WASM

b8 nya_system_event_poll(OUT NYA_Event* out_event) {
    nya_assert(out_event);

    NYA_App* app = nya_app_get();

    nya_os_mutex_lock(&app->event_system.event_queue_mutex);
    NYA_ArrayᐸNYA_Eventᐳ* nya_array = app->event_system.event_queue;

    if (app->event_system.event_queue_read_index >= nya_array->length) {
        nya_array_clear(nya_array);
        app->event_system.event_queue_read_index = 0;
        nya_os_mutex_unlock(&app->event_system.event_queue_mutex);
        return false;
    }

    *out_event = *nya_array_get(nya_array, app->event_system.event_queue_read_index);
    app->event_system.event_queue_read_index++;
    nya_os_mutex_unlock(&app->event_system.event_queue_mutex);

    if (!out_event->was_handled) _nya_event_notify_deferred_listeners(out_event);

    return true;
}

/*
 * ─────────────────────────────────────────────────────────
 * EVENT FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_event_dispatch(NYA_Event event) {
    NYA_App* app = nya_app_get();

    event.timestamp = nya_clock_get_timestamp_ms();

    // The lock covers only the queue push. Immediate listeners run after it is released, because a
    // listener may itself dispatch an event (an agent's frame hook moves the mouse, which dispatches):
    // notifying under the lock would re-enter this on the same thread and deadlock the non-recursive
    // NYA_OsMutex. The deferred path already notifies unlocked for the same reason.
    nya_os_mutex_lock(&app->event_system.event_queue_mutex);
    nya_array_push_back(app->event_system.event_queue, event);
    nya_os_mutex_unlock(&app->event_system.event_queue_mutex);

    _nya_event_notify_immediate_listeners(&event);

    if (NYA_EVENT_LIFECYCLE_EVENTS_BEGIN <= event.type && event.type <= NYA_EVENT_LIFECYCLE_EVENTS_END) return;
    nya_log_trace("Event dispatched: %s", NYA_EVENT_NAME_MAP[event.type]);
}

void nya_event_hook_register(NYA_EventHook hook) {
    NYA_App* app = nya_app_get();

    NYA_ArrayᐸNYA_EventHookᐳ* hook_array = nullptr;
    switch (hook.hook_type) {
        case NYA_EVENT_HOOK_TYPE_DEFERRED: {
            hook_array = nya_hmap_get(app->event_system.deferred_event_hooks, hook.event_type);

            if (!hook_array) {
                NYA_ArrayᐸNYA_EventHookᐳ new_hook_array = nya_array_create_on_stack(app->event_system.allocator, NYA_EventHook);
                nya_hmap_add(app->event_system.deferred_event_hooks, hook.event_type, new_hook_array);
                hook_array = nya_hmap_get(app->event_system.deferred_event_hooks, hook.event_type);
            }
        } break;

        case NYA_EVENT_HOOK_TYPE_IMMEDIATE: {
            hook_array = nya_hmap_get(app->event_system.immediate_event_hooks, hook.event_type);

            if (!hook_array) {
                NYA_ArrayᐸNYA_EventHookᐳ new_hook_array = nya_array_create_on_stack(app->event_system.allocator, NYA_EventHook);
                nya_hmap_add(app->event_system.immediate_event_hooks, hook.event_type, new_hook_array);
                hook_array = nya_hmap_get(app->event_system.immediate_event_hooks, hook.event_type);
            }
        } break;

        default: nya_unreachable();
    }
    static_assert(NYA_EVENT_HOOK_TYPE_COUNT == 2, "Unhandled NYA_EventHookType enum value.");

    nya_array_push_back(hook_array, hook);
}

void nya_event_hook_register_once(NYA_EventHook hook) {
    hook.one_shot = true;
    nya_event_hook_register(hook);
}

void nya_event_hook_unregister(NYA_EventHook hook) {
    NYA_App* app = nya_app_get();

    NYA_ArrayᐸNYA_EventHookᐳ* hook_array = nullptr;
    switch (hook.hook_type) {
        case NYA_EVENT_HOOK_TYPE_DEFERRED: {
            hook_array = nya_hmap_get(app->event_system.deferred_event_hooks, hook.event_type);
        } break;

        case NYA_EVENT_HOOK_TYPE_IMMEDIATE: {
            hook_array = nya_hmap_get(app->event_system.immediate_event_hooks, hook.event_type);
        } break;

        default: nya_unreachable();
    }
    static_assert(NYA_EVENT_HOOK_TYPE_COUNT == 2, "Unhandled NYA_EventHookType enum value.");
    nya_assert(hook_array != nullptr, "Cannot unregister hook that was not registered.");

    nya_array_remove_item(hook_array, hook);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_ConstCString _nya_event_copy_transient_string(NYA_ConstCString text) {
    if (text == nullptr) return nullptr;

    NYA_Arena* arena = nya_app_get()->frame_allocator;

    // Only reachable from the frame loop's drain, which cannot run before the app is up. Handing
    // back SDL's pointer rather than asserting keeps a caller that gets here some other way with
    // the behaviour it had, instead of turning a missing arena into a crash inside the conversion.
    if (arena == nullptr) return text;

    u64         length = strlen(text);
    NYA_CString copy   = nya_arena_alloc(arena, length + 1);
    nya_memcpy(copy, text, length);
    copy[length] = '\0';

    return copy;
}

// The SDL-event translation: how a raw SDL_Event becomes an NYA_Event. A wasm build has no SDL event
// pump — the browser is the event source and wasm_ui.c synthesises NYA_Events directly — so this whole
// block is compiled out there. It is the only code in this file that reads SDL_Event fields.
#if !OS_WASM
NYA_InputSource _nya_event_source_from_sdl(NYA_InputDeviceKind kind, u32 which) {
    /*
     * Carried through untouched, zero included.
     */
    return (NYA_InputSource){ .kind = kind, .id = which };
}

NYA_INTERNAL NYA_WindowHandle _nya_event_window_from_sdl_id(SDL_WindowID sdl_window_id) {
    for (u32 i = 0; i < NYA_WINDOW_MAX; i++) {
        NYA_Window* window = nya_window_at_slot(i);
        if (window == nullptr) continue;
        if (SDL_GetWindowID(window->sdl_window) == sdl_window_id) return window->handle;
    }

    return NYA_WINDOW_HANDLE_NONE;
}

NYA_INTERNAL NYA_Event _nya_event_from_sdl_event(SDL_Event sdl_event) {
    nya_unused(sdl_event);

    NYA_Event event = { 0 };

    switch (sdl_event.type) {
        case SDL_EVENT_CLIPBOARD_UPDATE: {
            event.type = NYA_EVENT_CLIPBOARD_UPDATE;
        } break;

        case SDL_EVENT_DISPLAY_ADDED: {
            event.type                        = NYA_EVENT_DISPLAY_ADDED;
            event.as_display_event.display_id = (u32)sdl_event.display.displayID;
        } break;

        case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED: {
            event.type                        = NYA_EVENT_DISPLAY_CONTENT_SCALE_CHANGED;
            event.as_display_event.display_id = (u32)sdl_event.display.displayID;
        } break;

        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED: {
            event.type                        = NYA_EVENT_DISPLAY_CURRENT_MODE_CHANGED;
            event.as_display_event.display_id = (u32)sdl_event.display.displayID;
        } break;

        case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED: {
            event.type                        = NYA_EVENT_DISPLAY_DESKTOP_MODE_CHANGED;
            event.as_display_event.display_id = (u32)sdl_event.display.displayID;
        } break;

        case SDL_EVENT_DISPLAY_MOVED: {
            event.type                        = NYA_EVENT_DISPLAY_MOVED;
            event.as_display_event.display_id = (u32)sdl_event.display.displayID;
            event.as_display_event.data1      = sdl_event.display.data1;
            event.as_display_event.data2      = sdl_event.display.data2;
        } break;

        case SDL_EVENT_DISPLAY_ORIENTATION: {
            event.type                        = NYA_EVENT_DISPLAY_ORIENTATION;
            event.as_display_event.display_id = (u32)sdl_event.display.displayID;
            event.as_display_event.data1      = sdl_event.display.data1;
        } break;

        case SDL_EVENT_DISPLAY_REMOVED: {
            event.type                        = NYA_EVENT_DISPLAY_REMOVED;
            event.as_display_event.display_id = (u32)sdl_event.display.displayID;
        } break;

        case SDL_EVENT_DROP_BEGIN: {
            event.type                 = NYA_EVENT_DROP_BEGIN;
            event.as_drop_event.window = _nya_event_window_from_sdl_id(sdl_event.drop.windowID);
        } break;

        case SDL_EVENT_DROP_COMPLETE: {
            event.type                 = NYA_EVENT_DROP_COMPLETE;
            event.as_drop_event.window = _nya_event_window_from_sdl_id(sdl_event.drop.windowID);
        } break;

        case SDL_EVENT_DROP_FILE: {
            event.type                 = NYA_EVENT_DROP_FILE;
            event.as_drop_event.window = _nya_event_window_from_sdl_id(sdl_event.drop.windowID);
            event.as_drop_event.path   = _nya_event_copy_transient_string(sdl_event.drop.data);
        } break;

        case SDL_EVENT_DROP_POSITION: {
            event.type                          = NYA_EVENT_DROP_POSITION;
            event.as_drop_position_event.window = _nya_event_window_from_sdl_id(sdl_event.drop.windowID);
            event.as_drop_position_event.x      = sdl_event.drop.x;
            event.as_drop_position_event.y      = sdl_event.drop.y;
        } break;

        case SDL_EVENT_DROP_TEXT: {
            event.type                 = NYA_EVENT_DROP_TEXT;
            event.as_drop_event.window = _nya_event_window_from_sdl_id(sdl_event.drop.windowID);
            event.as_drop_event.path   = _nya_event_copy_transient_string(sdl_event.drop.data);
        } break;

        case SDL_EVENT_KEY_DOWN: {
            event.type                        = NYA_EVENT_KEY_DOWN;
            event.as_key_event.window         = _nya_event_window_from_sdl_id(sdl_event.key.windowID);
            event.as_key_event.source         = _nya_event_source_from_sdl(NYA_INPUT_DEVICE_KIND_KEYBOARD, sdl_event.key.which);
            event.as_key_event.is_down        = true;
            event.as_key_event.is_repeat      = sdl_event.key.repeat != 0;
            event.as_key_event.key            = sdl_event.key.key;
            event.as_key_event.scancode       = (NYA_Scancode)sdl_event.key.scancode;
            event.as_key_event.modifier_flags = sdl_event.key.mod;
            event.as_key_event.raw            = sdl_event.key.raw;
        } break;

        case SDL_EVENT_KEY_UP: {
            event.type                        = NYA_EVENT_KEY_UP;
            event.as_key_event.window         = _nya_event_window_from_sdl_id(sdl_event.key.windowID);
            event.as_key_event.source         = _nya_event_source_from_sdl(NYA_INPUT_DEVICE_KIND_KEYBOARD, sdl_event.key.which);
            event.as_key_event.is_down        = false;
            event.as_key_event.is_repeat      = sdl_event.key.repeat != 0;
            event.as_key_event.key            = sdl_event.key.key;
            event.as_key_event.scancode       = (NYA_Scancode)sdl_event.key.scancode;
            event.as_key_event.modifier_flags = sdl_event.key.mod;
            event.as_key_event.raw            = sdl_event.key.raw;
        } break;

        case SDL_EVENT_KEYMAP_CHANGED: {
            event.type = NYA_EVENT_KEYMAP_CHANGED;
        } break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            event.type                          = NYA_EVENT_MOUSE_BUTTON_DOWN;
            event.as_mouse_button_event.window  = _nya_event_window_from_sdl_id(sdl_event.button.windowID);
            event.as_mouse_button_event.source  = _nya_event_source_from_sdl(NYA_INPUT_DEVICE_KIND_MOUSE, sdl_event.button.which);
            event.as_mouse_button_event.is_down = true;
            event.as_mouse_button_event.button  = sdl_event.button.button;
            event.as_mouse_button_event.x       = sdl_event.button.x;
            event.as_mouse_button_event.y       = sdl_event.button.y;
            event.as_mouse_button_event.clicks  = sdl_event.button.clicks;
        } break;

        case SDL_EVENT_MOUSE_BUTTON_UP: {
            event.type                          = NYA_EVENT_MOUSE_BUTTON_UP;
            event.as_mouse_button_event.window  = _nya_event_window_from_sdl_id(sdl_event.button.windowID);
            event.as_mouse_button_event.source  = _nya_event_source_from_sdl(NYA_INPUT_DEVICE_KIND_MOUSE, sdl_event.button.which);
            event.as_mouse_button_event.is_down = false;
            event.as_mouse_button_event.button  = sdl_event.button.button;
            event.as_mouse_button_event.x       = sdl_event.button.x;
            event.as_mouse_button_event.y       = sdl_event.button.y;
            event.as_mouse_button_event.clicks  = sdl_event.button.clicks;
        } break;

        case SDL_EVENT_MOUSE_MOTION: {
            event.type                         = NYA_EVENT_MOUSE_MOVED;
            event.as_mouse_moved_event.window  = _nya_event_window_from_sdl_id(sdl_event.motion.windowID);
            event.as_mouse_moved_event.source  = _nya_event_source_from_sdl(NYA_INPUT_DEVICE_KIND_MOUSE, sdl_event.motion.which);
            event.as_mouse_moved_event.state   = sdl_event.motion.state;
            event.as_mouse_moved_event.x       = sdl_event.motion.x;
            event.as_mouse_moved_event.y       = sdl_event.motion.y;
            event.as_mouse_moved_event.delta_x = sdl_event.motion.xrel;
            event.as_mouse_moved_event.delta_y = sdl_event.motion.yrel;
        } break;

        case SDL_EVENT_MOUSE_WHEEL: {
            event.type                                  = NYA_EVENT_MOUSE_WHEEL_MOVED;
            event.as_mouse_wheel_event.window           = _nya_event_window_from_sdl_id(sdl_event.wheel.windowID);
            event.as_mouse_wheel_event.source           = _nya_event_source_from_sdl(NYA_INPUT_DEVICE_KIND_MOUSE, sdl_event.wheel.which);
            event.as_mouse_wheel_event.direction        = (NYA_MouseWheelDirection)sdl_event.wheel.direction;
            event.as_mouse_wheel_event.amount_x         = sdl_event.wheel.x;
            event.as_mouse_wheel_event.amount_y         = sdl_event.wheel.y;
            event.as_mouse_wheel_event.mouse_x          = sdl_event.wheel.mouse_x;
            event.as_mouse_wheel_event.mouse_y          = sdl_event.wheel.mouse_y;
            event.as_mouse_wheel_event.integer_amount_x = sdl_event.wheel.integer_x;
            event.as_mouse_wheel_event.integer_amount_y = sdl_event.wheel.integer_y;
        } break;

        case SDL_EVENT_QUIT: {
            event.type = NYA_EVENT_QUIT;
        } break;

        case SDL_EVENT_TEXT_EDITING: {
            event.type                         = NYA_EVENT_TEXT_EDITING;
            event.as_text_editing_event.window = _nya_event_window_from_sdl_id(sdl_event.edit.windowID);
            event.as_text_editing_event.text   = _nya_event_copy_transient_string(sdl_event.edit.text);
            event.as_text_editing_event.start  = sdl_event.edit.start;
            event.as_text_editing_event.length = sdl_event.edit.length;
        } break;

        case SDL_EVENT_TEXT_INPUT: {
            event.type                       = NYA_EVENT_TEXT_INPUT;
            event.as_text_input_event.window = _nya_event_window_from_sdl_id(sdl_event.text.windowID);
            event.as_text_input_event.text   = _nya_event_copy_transient_string(sdl_event.text.text);
        } break;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
            event.type                   = NYA_EVENT_WINDOW_CLOSE_REQUESTED;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_DESTROYED: {
            event.type                   = NYA_EVENT_WINDOW_DESTROYED;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_DISPLAY_CHANGED: {
            event.type                   = NYA_EVENT_WINDOW_DISPLAY_CHANGED;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
            event.type                   = NYA_EVENT_WINDOW_DISPLAY_SCALE_CHANGED;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: {
            event.type                   = NYA_EVENT_WINDOW_ENTER_FULLSCREEN;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_EXPOSED: {
            event.type                   = NYA_EVENT_WINDOW_EXPOSED;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_FOCUS_GAINED: {
            event.type                   = NYA_EVENT_WINDOW_FOCUS_GAINED;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            event.type                   = NYA_EVENT_WINDOW_FOCUS_LOST;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_HDR_STATE_CHANGED: {
            event.type                   = NYA_EVENT_WINDOW_HDR_STATE_CHANGED;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_HIDDEN: {
            event.type                   = NYA_EVENT_WINDOW_HIDDEN;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: {
            event.type                   = NYA_EVENT_WINDOW_LEAVE_FULLSCREEN;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_MAXIMIZED: {
            event.type                   = NYA_EVENT_WINDOW_MAXIMIZED;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_MINIMIZED: {
            event.type                   = NYA_EVENT_WINDOW_MINIMIZED;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_MOUSE_ENTER: {
            event.type                   = NYA_EVENT_WINDOW_MOUSE_ENTER;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_MOUSE_LEAVE: {
            event.type                   = NYA_EVENT_WINDOW_MOUSE_LEAVE;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_MOVED: {
            event.type                         = NYA_EVENT_WINDOW_MOVED;
            event.as_window_moved_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
            event.as_window_moved_event.x      = sdl_event.window.data1;
            event.as_window_moved_event.y      = sdl_event.window.data2;
        } break;

        case SDL_EVENT_WINDOW_OCCLUDED: {
            event.type                   = NYA_EVENT_WINDOW_OCCLUDED;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            event.type                           = NYA_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
            event.as_window_resized_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
            event.as_window_resized_event.width  = (u32)sdl_event.window.data1;
            event.as_window_resized_event.height = (u32)sdl_event.window.data2;
        } break;

        case SDL_EVENT_WINDOW_RESIZED: {
            event.type                           = NYA_EVENT_WINDOW_RESIZED;
            event.as_window_resized_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
            event.as_window_resized_event.width  = sdl_event.window.data1;
            event.as_window_resized_event.height = sdl_event.window.data2;
        } break;

        case SDL_EVENT_WINDOW_RESTORED: {
            event.type                   = NYA_EVENT_WINDOW_RESTORED;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED: {
            event.type                   = NYA_EVENT_WINDOW_SAFE_AREA_CHANGED;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        case SDL_EVENT_WINDOW_SHOWN: {
            event.type                   = NYA_EVENT_WINDOW_SHOWN;
            event.as_window_event.window = _nya_event_window_from_sdl_id(sdl_event.window.windowID);
        } break;

        default: {
            event.type = NYA_EVENT_INVALID;
        } break;
    }

    return event;
}
#endif // !OS_WASM

/**
 * Runs every hook registered for the event, dropping the one shots that fired.
 * */
NYA_INTERNAL void _nya_event_notify_listeners(NYA_HMapᐸNYA_EventTypeˏNYA_ArrayᐸNYA_EventHookᐳᐳ* hooks, NYA_Event* event) {
    nya_assert(event != nullptr);

    NYA_App* app = nya_app_get();

    // read for the early return and initial count, then not kept. See below.
    NYA_ArrayᐸNYA_EventHookᐳ* hook_array = nya_hmap_get(hooks, event->type);
    if (hook_array == nullptr) return;

    u64 hook_count = hook_array->length;

    NYA_ArrayᐸNYA_EventHookᐳ finished_oneshot_hooks = nya_array_create_on_stack(app->event_system.allocator, NYA_EventHook);
    defer                    nya_array_destroy_on_stack(&finished_oneshot_hooks);

    /*
     * Nothing may be cached across a handler: a hook can register another from inside dispatch (asset hot
     * reload and job completion both do), which moves memory.
     */
    for (u64 i = 0; i < hook_count; i++) {
        // Re-read every iteration, and re-checked against the current length: a handler may have
        // unregistered every hook for this type, or shrunk the array past where we are.
        NYA_ArrayᐸNYA_EventHookᐳ* current = nya_hmap_get(hooks, event->type);
        if (current == nullptr || i >= current->length) break;

        // Copied out before the handler runs, since the handler is what may move the array.
        NYA_EventHook hook = current->items[i];

        NYA_EventHookFn          fn           = nya_callback_get(hook.fn);
        NYA_EventHookConditionFn condition_fn = nya_callback_get(hook.condition_fn);

        if (fn == nullptr) continue;
        if (condition_fn != nullptr && !condition_fn(event)) continue;

        // Collected before the call, not after: a hook that unregisters itself, or one whose
        // handler marks the event handled and breaks the loop, must still be spent.
        if (hook.one_shot) nya_array_push_back(&finished_oneshot_hooks, hook);

        fn(event);

        if (event->was_handled) break;
    }

    // Looked up once more for the same reason: the handlers above may have moved it since.
    if (finished_oneshot_hooks.length > 0) {
        NYA_ArrayᐸNYA_EventHookᐳ* current = nya_hmap_get(hooks, event->type);
        if (current != nullptr) nya_array_foreach (&finished_oneshot_hooks, hook_to_remove) nya_array_remove_item(current, *hook_to_remove);
    }
}

NYA_INTERNAL void _nya_event_notify_deferred_listeners(NYA_Event* event) {
    NYA_App* app = nya_app_get();
    _nya_event_notify_listeners(app->event_system.deferred_event_hooks, event);
}

NYA_INTERNAL void _nya_event_notify_immediate_listeners(NYA_Event* event) {
    NYA_App* app = nya_app_get();
    _nya_event_notify_listeners(app->event_system.immediate_event_hooks, event);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TERMINAL INPUT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_TERMINAL_ENABLED

/**
 * NYA_TerminalKey to NYA_Keycode, which is the one place the two vocabularies meet.
 *
 * The terminal module is in platform/ and cannot see core's keycodes, so its decoder emits its own
 * small enum and this table translates. That is the whole reason the enum exists: without it either
 * platform/ would include core/, or the escape parser would carry keycodes it has no business
 * knowing.
 * */
NYA_INTERNAL const NYA_Keycode _NYA_EVENT_TERMINAL_KEYS[NYA_TERMINAL_KEY_COUNT] = {
    [NYA_TERMINAL_KEY_NONE]      = NYA_KEY_UNKNOWN,
    [NYA_TERMINAL_KEY_ESCAPE]    = NYA_KEY_ESCAPE,
    [NYA_TERMINAL_KEY_ENTER]     = NYA_KEY_RETURN,
    [NYA_TERMINAL_KEY_TAB]       = NYA_KEY_TAB,
    [NYA_TERMINAL_KEY_BACKSPACE] = NYA_KEY_BACKSPACE,
    [NYA_TERMINAL_KEY_DELETE]    = NYA_KEY_DELETE,
    [NYA_TERMINAL_KEY_INSERT]    = NYA_KEY_INSERT,
    [NYA_TERMINAL_KEY_UP]        = NYA_KEY_UP,
    [NYA_TERMINAL_KEY_DOWN]      = NYA_KEY_DOWN,
    [NYA_TERMINAL_KEY_LEFT]      = NYA_KEY_LEFT,
    [NYA_TERMINAL_KEY_RIGHT]     = NYA_KEY_RIGHT,
    [NYA_TERMINAL_KEY_HOME]      = NYA_KEY_HOME,
    [NYA_TERMINAL_KEY_END]       = NYA_KEY_END,
    [NYA_TERMINAL_KEY_PAGE_UP]   = NYA_KEY_PAGEUP,
    [NYA_TERMINAL_KEY_PAGE_DOWN] = NYA_KEY_PAGEDOWN,
    [NYA_TERMINAL_KEY_F1]        = NYA_KEY_F1,
    [NYA_TERMINAL_KEY_F2]        = NYA_KEY_F2,
    [NYA_TERMINAL_KEY_F3]        = NYA_KEY_F3,
    [NYA_TERMINAL_KEY_F4]        = NYA_KEY_F4,
    [NYA_TERMINAL_KEY_F5]        = NYA_KEY_F5,
    [NYA_TERMINAL_KEY_F6]        = NYA_KEY_F6,
    [NYA_TERMINAL_KEY_F7]        = NYA_KEY_F7,
    [NYA_TERMINAL_KEY_F8]        = NYA_KEY_F8,
    [NYA_TERMINAL_KEY_F9]        = NYA_KEY_F9,
    [NYA_TERMINAL_KEY_F10]       = NYA_KEY_F10,
    [NYA_TERMINAL_KEY_F11]       = NYA_KEY_F11,
    [NYA_TERMINAL_KEY_F12]       = NYA_KEY_F12,
};

/** The terminal's modifier bits as core's. Two vocabularies again, and one table. */
NYA_INTERNAL NYA_KeyModFlag _nya_event_terminal_modifiers(u16 modifiers) {
    NYA_KeyModFlag flags = NYA_KEYMOD_NONE;

    // the left variants, because a terminal says shift and never which shift.
    if ((modifiers & NYA_TERMINAL_MODIFIER_SHIFT) != 0) flags |= NYA_KEYMOD_LSHIFT;
    if ((modifiers & NYA_TERMINAL_MODIFIER_ALT) != 0) flags |= NYA_KEYMOD_LALT;
    if ((modifiers & NYA_TERMINAL_MODIFIER_CTRL) != 0) flags |= NYA_KEYMOD_LCTRL;

    return flags;
}

/**
 * The UTF-8 for one code point, in storage that outlives the dispatch.
 *
 * A ring of its own rather than the frame allocator: this drain is reachable without the app loop,
 * and _nya_event_copy_transient_string hands the caller's pointer back when there is no frame arena,
 * which for a stack buffer would be a dangling pointer sitting in the event queue. The text is valid
 * until the next drain, which is the lifetime SDL's own text events promise.
 * */
NYA_INTERNAL NYA_ConstCString _nya_event_terminal_text(u32 codepoint) {
    // one slot per input a drain can produce, so no two events from one drain share a slot.
    static char text[NYA_TERMINAL_INPUT_MAX][5];
    static u32  next = 0;

    char* slot = text[next % NYA_TERMINAL_INPUT_MAX];
    next      += 1;

    u32 at = 0;

    if (codepoint < 0x80U) {
        slot[at++] = (char)codepoint;
    } else if (codepoint < 0x800U) {
        slot[at++] = (char)(0xC0U | (codepoint >> 6U));
        slot[at++] = (char)(0x80U | (codepoint & 0x3FU));
    } else if (codepoint < 0x10000U) {
        slot[at++] = (char)(0xE0U | (codepoint >> 12U));
        slot[at++] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
        slot[at++] = (char)(0x80U | (codepoint & 0x3FU));
    } else {
        slot[at++] = (char)(0xF0U | (codepoint >> 18U));
        slot[at++] = (char)(0x80U | ((codepoint >> 12U) & 0x3FU));
        slot[at++] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
        slot[at++] = (char)(0x80U | (codepoint & 0x3FU));
    }

    slot[at] = '\0';

    return slot;
}

/** A key, as the down and the up a terminal does not distinguish, plus the text it typed. */
NYA_INTERNAL void _nya_event_terminal_key(const NYA_TerminalInput* input, NYA_WindowHandle window) {
    nya_assert((u32)input->key < (u32)NYA_TERMINAL_KEY_COUNT, "the decoder produced key %d", (s32)input->key);

    // a key that is only a character carries no NYA_TerminalKey, and for ASCII its code point is its
    // keycode. Anything above that is text and not a key anyone binds.
    NYA_Keycode keycode = _NYA_EVENT_TERMINAL_KEYS[input->key];
    if (keycode == NYA_KEY_UNKNOWN && input->codepoint > 0 && input->codepoint < 0x80U) keycode = (NYA_Keycode)input->codepoint;

    if (keycode != NYA_KEY_UNKNOWN) {
        NYA_KeyEvent key = {
            .window         = window,
            .is_down        = true,
            .key            = keycode,
            .modifier_flags = _nya_event_terminal_modifiers(input->modifiers),
        };

        nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_KEY_DOWN, .as_key_event = key });

        // a terminal never says a key came up, so it is released in the same drain. See
        // nya_system_event_drain_terminal_events for what that means for a caller.
        key.is_down = false;
        nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_KEY_UP, .as_key_event = key });
    }

    // a printable character is also text, which is what a text field reads. A control chord is not:
    // ctrl+c is a key, and typing it must not insert a 'c'.
    if (input->codepoint >= ' ' && (input->modifiers & (NYA_TERMINAL_MODIFIER_CTRL | NYA_TERMINAL_MODIFIER_ALT)) == 0) {
        nya_event_dispatch((NYA_Event){
            .type                = NYA_EVENT_TEXT_INPUT,
            .as_text_input_event = { .window = window, .text = _nya_event_terminal_text(input->codepoint) },
        });
    }
}

/** One decoded terminal input, as the events the SDL backend would have produced for it. */
NYA_INTERNAL void _nya_event_terminal_dispatch(const NYA_TerminalInput* input, NYA_WindowHandle window) {
    nya_assert(input != nullptr);

    // cells to pixels, at the cell's centre, so a handler reads the coordinates a window would give.
    f32 x = ((f32)input->column + 0.5F) * (f32)NYA_TERMINAL_CELL_WIDTH_PX;
    f32 y = ((f32)input->row + 0.5F) * (f32)NYA_TERMINAL_CELL_HEIGHT_PX;

    switch (input->kind) {
        case NYA_TERMINAL_INPUT_KEY: _nya_event_terminal_key(input, window); break;

        case NYA_TERMINAL_INPUT_MOUSE_BUTTON: {
            const NYA_MouseButton buttons[NYA_TERMINAL_MOUSE_BUTTON_COUNT] = {
                [NYA_TERMINAL_MOUSE_BUTTON_NONE]   = 0,
                [NYA_TERMINAL_MOUSE_BUTTON_LEFT]   = NYA_MOUSE_BUTTON_LEFT,
                [NYA_TERMINAL_MOUSE_BUTTON_MIDDLE] = NYA_MOUSE_BUTTON_MIDDLE,
                [NYA_TERMINAL_MOUSE_BUTTON_RIGHT]  = NYA_MOUSE_BUTTON_RIGHT,
            };

            nya_assert(input->button > NYA_TERMINAL_MOUSE_BUTTON_NONE && input->button < NYA_TERMINAL_MOUSE_BUTTON_COUNT);

            // the move first, so a handler reading nya_input_mouse_position on the click sees where
            // the click was and not where the pointer last passed.
            nya_event_dispatch((NYA_Event){
                .type                 = NYA_EVENT_MOUSE_MOVED,
                .as_mouse_moved_event = { .window = window, .x = x, .y = y },
            });

            nya_event_dispatch((NYA_Event){
                .type                  = input->is_down ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP,
                .as_mouse_button_event = {
                                          .window  = window,
                                          .is_down = input->is_down,
                                          .button  = buttons[input->button],
                                          .clicks  = 1,
                                          .x       = x,
                                          .y       = y,
                                          },
            });
        } break;

        case NYA_TERMINAL_INPUT_MOUSE_MOVED: {
            nya_event_dispatch((NYA_Event){
                .type                 = NYA_EVENT_MOUSE_MOVED,
                .as_mouse_moved_event = { .window = window, .x = x, .y = y },
            });
        } break;

        case NYA_TERMINAL_INPUT_MOUSE_WHEEL: {
            nya_event_dispatch((NYA_Event){
                .type                 = NYA_EVENT_MOUSE_WHEEL_MOVED,
                .as_mouse_wheel_event = {
                                         .window           = window,
                                         .direction        = NYA_MOUSE_WHEEL_DIRECTION_NORMAL,
                                         .amount_y         = (f32)input->wheel,
                                         .mouse_x          = x,
                                         .mouse_y          = y,
                                         .integer_amount_y = input->wheel,
                                         },
            });
        } break;

        case NYA_TERMINAL_INPUT_RESIZE: {
            // in pixels, like every other size the engine reports, so a layout needs no second unit
            // for this one backend.
            nya_event_dispatch((NYA_Event){
                .type                    = NYA_EVENT_WINDOW_RESIZED,
                .as_window_resized_event = {
                                            .window = window,
                                            .width  = (u32)input->column * NYA_TERMINAL_CELL_WIDTH_PX,
                                            .height = (u32)input->row * NYA_TERMINAL_CELL_HEIGHT_PX,
                                            },
            });
        } break;

        case NYA_TERMINAL_INPUT_NONE:
        case NYA_TERMINAL_INPUT_KIND_COUNT:
        default:                            nya_unreachable();
    }
}

void nya_system_event_drain_terminal_events(void) {
    if (!nya_terminal_is_open()) return;

    NYA_TerminalInput input[NYA_TERMINAL_INPUT_MAX];

    u32              count  = nya_terminal_poll(input, nya_carray_length(input));
    NYA_WindowHandle window = nya_render2d_terminal_window()->handle;

    for (u32 i = 0; i < count; i++) _nya_event_terminal_dispatch(&input[i], window);
}

#endif // NYA_TERMINAL_ENABLED

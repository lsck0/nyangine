#include "SDL3/SDL_events.h"
#include "SDL3/SDL_video.h"

#include "nyangine/nyangine.h"

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
 *
 * The dropped path and the text input strings are SDL's "temporary memory": SDL_PumpEvents frees
 * everything handed out before it, and SDL_PollEvent pumps. nya_system_event_drain_sdl_events polls
 * SDL's whole queue before nya_system_event_poll hands any of it to the layers, so storing SDL's
 * pointer meant every one of those strings was already freed by the time anything read it — a use
 * after free on any drag and drop or any text field.
 *
 * The frame allocator, because that is exactly the lifetime wanted: the event is consumed later in
 * the same frame, and the arena is reset once the frame ends. Anything that needs a dropped path
 * beyond the frame it arrived in has to copy it, which was already true.
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

    // Was unchecked. A null mutex makes every SDL_LockMutex on the event queue a no-op, which turns
    // a clean failure here into a data race that only shows up under load.
    SDL_Mutex* event_queue_mutex = SDL_CreateMutex();
    if (event_queue_mutex == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "SDL_CreateMutex() failed for the event queue: %s", SDL_GetError());

    app->event_system = (NYA_EventSystem){
        .allocator              = nya_arena_create(.name = "event_system_allocator"),
        .event_queue_mutex      = event_queue_mutex,
        .event_queue_read_index = 0,
    };

    app->event_system.event_queue           = nya_array_create(app->event_system.allocator, NYA_Event);
    app->event_system.deferred_event_hooks  = nya_hmap_create(app->event_system.allocator, NYA_EventType, NYA_ArrayᐸNYA_EventHookᐳ);
    app->event_system.immediate_event_hooks = nya_hmap_create(app->event_system.allocator, NYA_EventType, NYA_ArrayᐸNYA_EventHookᐳ);

    nya_info("Event system initialized.");
    return NYA_OK;
}

void nya_system_events_deinit(void) {
    NYA_App* app = nya_app_get();

    SDL_DestroyMutex(app->event_system.event_queue_mutex);
    nya_array_destroy(app->event_system.event_queue);
    nya_hmap_destroy(app->event_system.deferred_event_hooks);
    nya_hmap_destroy(app->event_system.immediate_event_hooks);

    nya_arena_destroy(app->event_system.allocator);

    nya_info("Event system deinitialized.");
}

void nya_system_event_drain_sdl_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        NYA_Event nya_event = _nya_event_from_sdl_event(event);
        if (nya_event.type == NYA_EVENT_INVALID) continue;

        nya_event_dispatch(nya_event);
    }
}

b8 nya_system_event_poll(OUT NYA_Event* out_event) {
    nya_assert(out_event);

    NYA_App* app = nya_app_get();

    SDL_LockMutex(app->event_system.event_queue_mutex);
    NYA_ArrayᐸNYA_Eventᐳ* nya_array = app->event_system.event_queue;

    if (app->event_system.event_queue_read_index >= nya_array->length) {
        nya_array_clear(nya_array);
        app->event_system.event_queue_read_index = 0;
        SDL_UnlockMutex(app->event_system.event_queue_mutex);
        return false;
    }

    *out_event = *nya_array_get(nya_array, app->event_system.event_queue_read_index);
    app->event_system.event_queue_read_index++;
    SDL_UnlockMutex(app->event_system.event_queue_mutex);

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

    SDL_LockMutex(app->event_system.event_queue_mutex);
    nya_array_push_back(app->event_system.event_queue, event);
    _nya_event_notify_immediate_listeners(&event);
    SDL_UnlockMutex(app->event_system.event_queue_mutex);

    if (NYA_EVENT_LIFECYCLE_EVENTS_BEGIN <= event.type && event.type <= NYA_EVENT_LIFECYCLE_EVENTS_END) return;
    nya_trace("Event dispatched: %s", NYA_EVENT_NAME_MAP[event.type]);
}

void nya_event_hook_register(NYA_EventHook hook) {
    NYA_App* app = nya_app_get();

    NYA_ArrayᐸNYA_EventHookᐳ* hook_array = nullptr;
    switch (hook.hook_type) {
        case NYA_EVENT_HOOK_TYPE_DEFERRED: {
            hook_array = nya_hmap_get(app->event_system.deferred_event_hooks, hook.event_type);

            if (!hook_array) {
                NYA_ArrayᐸNYA_EventHookᐳ new_hook_array = nya_array_create_on_stack(app->event_system.allocator, NYA_EventHook);
                nya_hmap_set(app->event_system.deferred_event_hooks, hook.event_type, new_hook_array);
                hook_array = nya_hmap_get(app->event_system.deferred_event_hooks, hook.event_type);
            }
        } break;

        case NYA_EVENT_HOOK_TYPE_IMMEDIATE: {
            hook_array = nya_hmap_get(app->event_system.immediate_event_hooks, hook.event_type);

            if (!hook_array) {
                NYA_ArrayᐸNYA_EventHookᐳ new_hook_array = nya_array_create_on_stack(app->event_system.allocator, NYA_EventHook);
                nya_hmap_set(app->event_system.immediate_event_hooks, hook.event_type, new_hook_array);
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

NYA_InputSource _nya_event_source_from_sdl(NYA_InputDeviceKind kind, u32 which) {
    /*
     * Carried through untouched, zero included.
     *
     * Zero is what SDL reports when it cannot separate devices — every keyboard on a platform
     * without per-device keyboard support, and every mouse outside relative mode. It is the ordinary
     * single-player reading rather than an error, and it routes like any other source, so there is
     * nothing here to normalise or reject. See NYA_InputSource.
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

/**
 * Runs every hook registered for the event, dropping the one shots that fired.
 *
 * The deferred and immediate paths differ only in which map they read, so they share this. They
 * were duplicated line for line, which is how the two came to disagree in the first place.
 *
 * A one shot is collected while iterating and removed afterwards rather than in place, because
 * removing from the array being walked would shuffle the elements out from under the loop. It is
 * only collected when its function actually ran, so a condition that rejects an event does not
 * spend the hook.
 * */
NYA_INTERNAL void _nya_event_notify_listeners(NYA_HMapᐸNYA_EventTypeˏNYA_ArrayᐸNYA_EventHookᐳᐳ* hooks, NYA_Event* event) {
    nya_assert(event != nullptr);

    NYA_App* app = nya_app_get();

    // Read for the early return and the initial count, then deliberately not kept — see below.
    NYA_ArrayᐸNYA_EventHookᐳ* hook_array = nya_hmap_get(hooks, event->type);
    if (hook_array == nullptr) return;

    u64 hook_count = hook_array->length;

    NYA_ArrayᐸNYA_EventHookᐳ finished_oneshot_hooks = nya_array_create_on_stack(app->event_system.allocator, NYA_EventHook);
    defer                    nya_array_destroy_on_stack(&finished_oneshot_hooks);

    /*
     * Nothing about this walk may be cached across a handler, because a hook may register another
     * one from inside the dispatch — the asset system's hot reload path and the job system's
     * completion handlers both do — and that moves memory in two places.
     *
     * The hook array is one: a push into a full one reallocates `items` and hands the old block back
     * to the arena. The hash map is the other, and the one that is easy to miss — `hook_array` points
     * *into* the map's `values` block, and registering for an event type nothing has hooked yet is an
     * insert, so crossing the load factor reallocates that block and frees the old one. The map starts
     * at capacity 64 against 63 event types, so the threshold of 48 is inside what an application
     * registers. Re-reading costs a hash and a probe per hook, nothing beside the handler about to
     * run, and it covers the array case as a side effect.
     *
     * The count is sampled once, so a hook registered during this dispatch runs on the next matching
     * event rather than the one that created it — which is also what stops a hook that registers
     * itself from running forever.
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

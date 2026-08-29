#pragma once

#include "SDL3/SDL_events.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_mutex.h"

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_hmap.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_callback.h"
#include "nyangine/core/core_job.h"
#include "nyangine/core/core_keys.h"
#include "nyangine/core/core_mouse.h"
#include "nyangine/core/core_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_EventType            NYA_EventType;
typedef enum NYA_EventHookType        NYA_EventHookType;
typedef struct NYA_AssetEvent         NYA_AssetEvent;
typedef struct NYA_DisplayEvent       NYA_DisplayEvent;
typedef struct NYA_DropEvent          NYA_DropEvent;
typedef struct NYA_DropPositionEvent  NYA_DropPositionEvent;
typedef struct NYA_Event              NYA_Event;
typedef struct NYA_EventHook          NYA_EventHook;
typedef struct NYA_EventSystem        NYA_EventSystem;
typedef struct NYA_JobEvent           NYA_JobEvent;
typedef struct NYA_KeyEvent           NYA_KeyEvent;
typedef struct NYA_MouseButtonEvent   NYA_MouseButtonEvent;
typedef struct NYA_MouseMovedEvent    NYA_MouseMovedEvent;
typedef struct NYA_MouseWheelEvent    NYA_MouseWheelEvent;
typedef struct NYA_TextEditingEvent   NYA_TextEditingEvent;
typedef struct NYA_TextInputEvent     NYA_TextInputEvent;
typedef struct NYA_WindowEvent        NYA_WindowEvent;
typedef struct NYA_WindowMovedEvent   NYA_WindowMovedEvent;
typedef struct NYA_WindowResizedEvent NYA_WindowResizedEvent;
/*
 * ─────────────────────────────────────────────────────────
 * EVENT TYPE ENUM
 * ─────────────────────────────────────────────────────────
 */

enum NYA_EventType {
    NYA_EVENT_INVALID,

    NYA_EVENT_LIFECYCLE_EVENTS_BEGIN,
    NYA_EVENT_FRAME_STARTED,
    NYA_EVENT_FRAME_ENDED,
    NYA_EVENT_HANDLING_STARTED,
    NYA_EVENT_HANDLING_ENDED,
    NYA_EVENT_UPDATING_STARTED,
    NYA_EVENT_UPDATING_ENDED,
    NYA_EVENT_RENDERING_STARTED,
    NYA_EVENT_RENDERING_ENDED,
    NYA_EVENT_LIFECYCLE_EVENTS_END,

    NYA_EVENT_CLIPBOARD_UPDATE,

    NYA_EVENT_DISPLAY_ADDED,
    NYA_EVENT_DISPLAY_CONTENT_SCALE_CHANGED,
    NYA_EVENT_DISPLAY_CURRENT_MODE_CHANGED,
    NYA_EVENT_DISPLAY_DESKTOP_MODE_CHANGED,
    NYA_EVENT_DISPLAY_MOVED,
    NYA_EVENT_DISPLAY_ORIENTATION,
    NYA_EVENT_DISPLAY_REMOVED,

    NYA_EVENT_DROP_FILE,
    NYA_EVENT_DROP_TEXT,
    NYA_EVENT_DROP_BEGIN,
    NYA_EVENT_DROP_COMPLETE,
    NYA_EVENT_DROP_POSITION,

    NYA_EVENT_JOB_STARTED,
    NYA_EVENT_JOB_COMPLETED,

    /** An asset could not be loaded. Carries the handle that failed; the reason was already logged. */
    NYA_EVENT_ASSET_LOAD_FAILED,

    NYA_EVENT_KEY_DOWN,
    NYA_EVENT_KEY_UP,
    NYA_EVENT_KEYMAP_CHANGED,

    NYA_EVENT_MOUSE_BUTTON_DOWN,
    NYA_EVENT_MOUSE_BUTTON_UP,
    NYA_EVENT_MOUSE_MOVED,
    NYA_EVENT_MOUSE_WHEEL_MOVED,

    NYA_EVENT_QUIT,

    NYA_EVENT_TEXT_INPUT,
    NYA_EVENT_TEXT_EDITING,

    NYA_EVENT_WINDOW_CLOSE_REQUESTED,
    NYA_EVENT_WINDOW_DESTROYED,
    NYA_EVENT_WINDOW_DISPLAY_CHANGED,
    NYA_EVENT_WINDOW_DISPLAY_SCALE_CHANGED,
    NYA_EVENT_WINDOW_ENTER_FULLSCREEN,
    NYA_EVENT_WINDOW_EXPOSED,
    NYA_EVENT_WINDOW_FOCUS_GAINED,
    NYA_EVENT_WINDOW_FOCUS_LOST,
    NYA_EVENT_WINDOW_HDR_STATE_CHANGED,
    NYA_EVENT_WINDOW_HIDDEN,
    NYA_EVENT_WINDOW_LEAVE_FULLSCREEN,
    NYA_EVENT_WINDOW_MAXIMIZED,
    NYA_EVENT_WINDOW_MINIMIZED,
    NYA_EVENT_WINDOW_MOUSE_ENTER,
    NYA_EVENT_WINDOW_MOUSE_LEAVE,
    NYA_EVENT_WINDOW_MOVED,
    NYA_EVENT_WINDOW_OCCLUDED,
    NYA_EVENT_WINDOW_PIXEL_SIZE_CHANGED,
    NYA_EVENT_WINDOW_RESIZED,
    NYA_EVENT_WINDOW_RESTORED,
    NYA_EVENT_WINDOW_SAFE_AREA_CHANGED,
    NYA_EVENT_WINDOW_SHOWN,

    NYA_EVENT_COUNT,
};

nya_derive_array(NYA_Event);
nya_derive_array(NYA_EventHook);
nya_derive_hmap(NYA_EventType, NYA_ArrayᐸNYA_EventHookᐳ);

typedef void (*NYA_EventHookFn)(NYA_Event*);
typedef b8 (*NYA_EventHookConditionFn)(NYA_Event*);

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_EventSystem {
    NYA_Arena* allocator;

    SDL_Mutex*            event_queue_mutex;
    NYA_ArrayᐸNYA_Eventᐳ* event_queue;
    u64                   event_queue_read_index;

    NYA_HMapᐸNYA_EventTypeˏNYA_ArrayᐸNYA_EventHookᐳᐳ* deferred_event_hooks;
    NYA_HMapᐸNYA_EventTypeˏNYA_ArrayᐸNYA_EventHookᐳᐳ* immediate_event_hooks;
};

/*
 * Sized to NYA_EVENT_COUNT, not left for the initialisers to size.
 *
 * It was declared `[]`, which makes the length one past the highest index anyone happened to write
 * rather than the number of event types. Five entries were missing: the two lifecycle range
 * sentinels, and — the ones that mattered — JOB_STARTED, JOB_COMPLETED and ASSET_LOAD_FAILED.
 *
 * nya_event_dispatch does `nya_log_trace("Event dispatched: %s", NYA_EVENT_NAME_MAP[event.type])` on
 * every dispatch, so each of those passed a null pointer to a %s conversion. That is undefined
 * behaviour, and the job events are dispatched on every job start and finish; it survived only
 * because glibc prints "(null)" rather than faulting.
 *
 * The explicit size is what stops the next omission being an out of bounds read instead of a null,
 * and tests/nyangine/core/test_bug_event_name_map.c fails on any entry left empty. Every other name
 * map in the tree — NYA_INTEGRITY_STATUS_NAME_MAP, NYA_FILETYPE_NAME_MAP — was already spelled this
 * way; this one was the exception.
 */
__attr_allow_unused static NYA_ConstCString NYA_EVENT_NAME_MAP[NYA_EVENT_COUNT] = {
    [NYA_EVENT_INVALID] = "INVALID",

    // The range markers are not dispatched, but they are valid indices and a hole is still a hole.
    [NYA_EVENT_LIFECYCLE_EVENTS_BEGIN] = "LIFECYCLE_EVENTS_BEGIN",
    [NYA_EVENT_LIFECYCLE_EVENTS_END]   = "LIFECYCLE_EVENTS_END",

    [NYA_EVENT_FRAME_STARTED]     = "FRAME_STARTED",
    [NYA_EVENT_FRAME_ENDED]       = "FRAME_ENDED",
    [NYA_EVENT_HANDLING_STARTED]  = "HANDLING_STARTED",
    [NYA_EVENT_HANDLING_ENDED]    = "HANDLING_ENDED",
    [NYA_EVENT_UPDATING_STARTED]  = "UPDATING_STARTED",
    [NYA_EVENT_UPDATING_ENDED]    = "UPDATING_ENDED",
    [NYA_EVENT_RENDERING_STARTED] = "RENDERING_STARTED",
    [NYA_EVENT_RENDERING_ENDED]   = "RENDERING_ENDED",

    [NYA_EVENT_CLIPBOARD_UPDATE] = "CLIPBOARD_UPDATE",

    [NYA_EVENT_DISPLAY_ADDED]                 = "DISPLAY_ADDED",
    [NYA_EVENT_DISPLAY_CONTENT_SCALE_CHANGED] = "DISPLAY_CONTENT_SCALE_CHANGED",
    [NYA_EVENT_DISPLAY_CURRENT_MODE_CHANGED]  = "DISPLAY_CURRENT_MODE_CHANGED",
    [NYA_EVENT_DISPLAY_DESKTOP_MODE_CHANGED]  = "DISPLAY_DESKTOP_MODE_CHANGED",
    [NYA_EVENT_DISPLAY_MOVED]                 = "DISPLAY_MOVED",
    [NYA_EVENT_DISPLAY_ORIENTATION]           = "DISPLAY_ORIENTATION",
    [NYA_EVENT_DISPLAY_REMOVED]               = "DISPLAY_REMOVED",

    [NYA_EVENT_DROP_BEGIN]    = "DROP_BEGIN",
    [NYA_EVENT_DROP_COMPLETE] = "DROP_COMPLETE",
    [NYA_EVENT_DROP_FILE]     = "DROP_FILE",
    [NYA_EVENT_DROP_POSITION] = "DROP_POSITION",

    [NYA_EVENT_JOB_STARTED]   = "JOB_STARTED",
    [NYA_EVENT_JOB_COMPLETED] = "JOB_COMPLETED",

    [NYA_EVENT_ASSET_LOAD_FAILED] = "ASSET_LOAD_FAILED",
    [NYA_EVENT_DROP_TEXT]     = "DROP_TEXT",

    [NYA_EVENT_KEY_DOWN]       = "KEY_DOWN",
    [NYA_EVENT_KEY_UP]         = "KEY_UP",
    [NYA_EVENT_KEYMAP_CHANGED] = "KEYMAP_CHANGED",

    [NYA_EVENT_MOUSE_BUTTON_DOWN] = "MOUSE_BUTTON_DOWN",
    [NYA_EVENT_MOUSE_BUTTON_UP]   = "MOUSE_BUTTON_UP",
    [NYA_EVENT_MOUSE_MOVED]       = "MOUSE_MOVED",
    [NYA_EVENT_MOUSE_WHEEL_MOVED] = "MOUSE_WHEEL_MOVED",

    [NYA_EVENT_QUIT] = "QUIT",

    [NYA_EVENT_TEXT_INPUT]   = "TEXT_INPUT",
    [NYA_EVENT_TEXT_EDITING] = "TEXT_EDITING",

    [NYA_EVENT_WINDOW_CLOSE_REQUESTED]       = "WINDOW_CLOSE_REQUESTED",
    [NYA_EVENT_WINDOW_DESTROYED]             = "WINDOW_DESTROYED",
    [NYA_EVENT_WINDOW_DISPLAY_CHANGED]       = "WINDOW_DISPLAY_CHANGED",
    [NYA_EVENT_WINDOW_DISPLAY_SCALE_CHANGED] = "WINDOW_DISPLAY_SCALE_CHANGED",
    [NYA_EVENT_WINDOW_ENTER_FULLSCREEN]      = "WINDOW_ENTER_FULLSCREEN",
    [NYA_EVENT_WINDOW_EXPOSED]               = "WINDOW_EXPOSED",
    [NYA_EVENT_WINDOW_FOCUS_GAINED]          = "WINDOW_FOCUS_GAINED",
    [NYA_EVENT_WINDOW_FOCUS_LOST]            = "WINDOW_FOCUS_LOST",
    [NYA_EVENT_WINDOW_HDR_STATE_CHANGED]     = "WINDOW_HDR_STATE_CHANGED",
    [NYA_EVENT_WINDOW_HIDDEN]                = "WINDOW_HIDDEN",
    [NYA_EVENT_WINDOW_LEAVE_FULLSCREEN]      = "WINDOW_LEAVE_FULLSCREEN",
    [NYA_EVENT_WINDOW_MAXIMIZED]             = "WINDOW_MAXIMIZED",
    [NYA_EVENT_WINDOW_MINIMIZED]             = "WINDOW_MINIMIZED",
    [NYA_EVENT_WINDOW_MOUSE_ENTER]           = "WINDOW_MOUSE_ENTER",
    [NYA_EVENT_WINDOW_MOUSE_LEAVE]           = "WINDOW_MOUSE_LEAVE",
    [NYA_EVENT_WINDOW_MOVED]                 = "WINDOW_MOVED",
    [NYA_EVENT_WINDOW_OCCLUDED]              = "WINDOW_OCCLUDED",
    [NYA_EVENT_WINDOW_PIXEL_SIZE_CHANGED]    = "WINDOW_PIXEL_SIZE_CHANGED",
    [NYA_EVENT_WINDOW_RESIZED]               = "WINDOW_RESIZED",
    [NYA_EVENT_WINDOW_RESTORED]              = "WINDOW_RESTORED",
    [NYA_EVENT_WINDOW_SAFE_AREA_CHANGED]     = "WINDOW_SAFE_AREA_CHANGED",
    [NYA_EVENT_WINDOW_SHOWN]                 = "WINDOW_SHOWN",
};

/*
 * ─────────────────────────────────────────────────────────
 * ASSET EVENT STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_AssetEvent {
    NYA_CString asset_handle;
};

/*
 * ─────────────────────────────────────────────────────────
 * DISPLAY EVENT STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_DisplayEvent {
    u32 display_id;
    s32 data1;
    s32 data2;
};

/*
 * ─────────────────────────────────────────────────────────
 * DROP EVENT STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_DropEvent {
    NYA_WindowHandle window;
    NYA_ConstCString path;
};

struct NYA_DropPositionEvent {
    NYA_WindowHandle window;
    f32              x;
    f32              y;
};

/*
 * ─────────────────────────────────────────────────────────
 * JOB EVENT STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_JobEvent {
    NYA_Job job;
};

/*
 * ─────────────────────────────────────────────────────────
 * KEYBOARD EVENT STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_KeyEvent {
    NYA_WindowHandle window;

    /**
     * Which keyboard produced this. See NYA_InputSource.
     *
     * Zero-id and NYA_INPUT_DEVICE_KIND_KEYBOARD on any platform that does not separate keyboards,
     * which is most of them — that is the ordinary case, not a failure. It is what lets two players
     * on one machine, or several remote players over Steam Remote Play Together, be told apart when
     * the platform can tell them apart.
     * */
    NYA_InputSource source;

    b8             is_down;
    b8             is_repeat;
    NYA_Keycode    key;
    NYA_Scancode   scancode;
    NYA_KeyModFlag modifier_flags;
    u16            raw;
};

/*
 * ─────────────────────────────────────────────────────────
 * MOUSE EVENT STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_MouseButtonEvent {
    NYA_WindowHandle window;

    /** Which mouse produced this. SDL fills the id in only in relative mode; see NYA_InputSource. */
    NYA_InputSource source;

    b8              is_down;
    NYA_MouseButton button;
    u8              clicks;
    f32             x;
    f32             y;
};

struct NYA_MouseMovedEvent {
    NYA_WindowHandle window;

    /** Which mouse produced this. SDL fills the id in only in relative mode; see NYA_InputSource. */
    NYA_InputSource source;

    NYA_MouseButtonFlags state;
    f32                  x;
    f32                  y;
    f32                  delta_x;
    f32                  delta_y;
};

struct NYA_MouseWheelEvent {
    NYA_WindowHandle window;

    /** Which mouse produced this. SDL fills the id in only in relative mode; see NYA_InputSource. */
    NYA_InputSource source;

    NYA_MouseWheelDirection direction;
    f32                     amount_x;
    f32                     amount_y;
    f32                     mouse_x;
    f32                     mouse_y;
    s32                     integer_amount_x;
    s32                     integer_amount_y;
};

/*
 * ─────────────────────────────────────────────────────────
 * WINDOW EVENT STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_WindowEvent {
    NYA_WindowHandle window;
};

struct NYA_WindowMovedEvent {
    NYA_WindowHandle window;
    u32              x;
    u32              y;
};

struct NYA_WindowResizedEvent {
    NYA_WindowHandle window;
    u32              width;
    u32              height;
};

/*
 * ─────────────────────────────────────────────────────────
 * TEXT INPUT EVENT STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_TextInputEvent {
    NYA_WindowHandle window;
    NYA_ConstCString text;
};

struct NYA_TextEditingEvent {
    NYA_WindowHandle window;
    NYA_ConstCString text;
    s32              start;
    s32              length;
};

/*
 * ─────────────────────────────────────────────────────────
 * EVENT STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_Event {
    NYA_EventType type;
    b8            was_handled;
    u64           timestamp;

    union {
        NYA_AssetEvent         as_asset_event;
        NYA_DisplayEvent       as_display_event;
        NYA_DropEvent          as_drop_event;
        NYA_DropPositionEvent  as_drop_position_event;
        NYA_JobEvent           as_job_event;
        NYA_KeyEvent           as_key_event;
        NYA_MouseButtonEvent   as_mouse_button_event;
        NYA_MouseMovedEvent    as_mouse_moved_event;
        NYA_MouseWheelEvent    as_mouse_wheel_event;
        NYA_TextEditingEvent   as_text_editing_event;
        NYA_TextInputEvent     as_text_input_event;
        NYA_WindowEvent        as_window_event;
        NYA_WindowMovedEvent   as_window_moved_event;
        NYA_WindowResizedEvent as_window_resized_event;
    };
};

enum NYA_EventHookType {
    /** Deferred hooks are executed when the event is polled. */
    NYA_EVENT_HOOK_TYPE_DEFERRED,

    /**
     * Immediate hooks are executed when the event is dispatched.
     * Used to dynamically run code in different stages of the frame.
     * */
    NYA_EVENT_HOOK_TYPE_IMMEDIATE,

    NYA_EVENT_HOOK_TYPE_COUNT,
};

struct NYA_EventHook {
    NYA_EventType      event_type;
    NYA_EventHookType  hook_type;
    NYA_CallbackHandle fn;
    NYA_CallbackHandle condition_fn;

    /**
     * Removes itself after it runs once.
     *
     * "Runs" means the hook's function was actually called, so a one shot with a condition waits
     * for the first event that *passes* the condition rather than being spent on the first event
     * that merely has the right type.
     * */
    b64 one_shot;
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

NYA_API NYA_Error nya_system_events_init(void) __attr_no_discard;
NYA_API void      nya_system_events_deinit(void);
NYA_API void      nya_system_event_drain_sdl_events(void);
NYA_API b8        nya_system_event_poll(OUT NYA_Event* out_event);

/*
 * ─────────────────────────────────────────────────────────
 * EVENT FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API void nya_event_dispatch(NYA_Event event);
NYA_API void nya_event_hook_register(NYA_EventHook hook);

/**
 * Registers a hook that fires on the next matching event and then unregisters itself.
 *
 * For the "wait for the next X, then do Y" shape: the first frame after an asset finishes loading,
 * the next keypress, the next time a window is resized. Without it every such case grows a static
 * bool that the hook checks and sets, which is the same logic written badly once per site.
 *
 * ```c
 * nya_event_hook_register_once((NYA_EventHook){
 *     .event_type = NYA_EVENT_WINDOW_RESIZED,
 *     .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
 *     .fn         = nya_callback(on_first_resize),
 * });
 * ```
 *
 * Sets one_shot for you; anything already passing `.one_shot = true` to nya_event_hook_register
 * behaves identically. A one shot that never sees its event is never removed, so it costs one array
 * slot until the event system is torn down.
 * */
NYA_API void nya_event_hook_register_once(NYA_EventHook hook);

NYA_API void nya_event_hook_unregister(NYA_EventHook hook);

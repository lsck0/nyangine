/**
 * @file core_window.h
 * */
#pragma once

#include "SDL3/SDL_gpu.h"

#include "nyangine/base/base_array.h"
#include "nyangine/base/base_string.h"
#include "nyangine/core/core_callback.h"
#include "nyangine/core/core_event.h"
#include "nyangine/core/core_types.h"
#include "nyangine/renderer/renderer.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Window slots.
 * */
#define NYA_WINDOW_MAX 16

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_WindowFlags    NYA_WindowFlags;
typedef enum NYA_FlashOperation NYA_FlashOperation;
typedef struct NYA_Layer        NYA_Layer;
typedef enum NYA_CursorShape NYA_CursorShape;
typedef enum NYA_WindowRegion   NYA_WindowRegion;
typedef struct NYA_Window       NYA_Window;
typedef struct NYA_WindowSystem NYA_WindowSystem;
typedef struct NYA_DisplayMode  NYA_DisplayMode;
typedef struct NYA_Rect         NYA_Rect;
nya_derive_array(NYA_Layer);

typedef void (*NYA_LayerOnCreateFn)(NYA_Window* window);
typedef void (*NYA_LayerOnDestroyFn)(NYA_Window* window);
typedef void (*NYA_LayerOnUpdateFn)(NYA_Window* window, f32 delta_time_s);
typedef void (*NYA_LayerOnEventFn)(NYA_Window* window, NYA_Event* event);
typedef void (*NYA_LayerOnRenderFn)(NYA_Window* window);

struct NYA_Rect {
    s32 x;
    s32 y;
    s32 width;
    s32 height;
};

struct NYA_DisplayMode {
    s32 width;
    s32 height;
    f32 refresh_rate;
    f32 pixel_density;
};

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_WindowSystem {
    NYA_Arena* allocator;

    /* Fixed table. `occupied` says whether a slot holds a window; `generations` outlives the window. */
    NYA_Window* windows;
    b8*         occupied;
    u32*        generations;
    u32         count;

    /**
     * Whether this platform lets a client choose its own size and position.
     * */
    b8 client_controlled_geometry;
};

/*
 * ─────────────────────────────────────────────────────────
 * WINDOW STRUCT
 * ─────────────────────────────────────────────────────────
 */

enum NYA_WindowFlags {
    NYA_WINDOW_NONE               = 0,
    NYA_WINDOW_ALWAYS_ON_TOP      = SDL_WINDOW_ALWAYS_ON_TOP,
    NYA_WINDOW_BORDERLESS         = SDL_WINDOW_BORDERLESS,
    NYA_WINDOW_FULLSCREEN         = SDL_WINDOW_FULLSCREEN,
    NYA_WINDOW_HIDDEN             = SDL_WINDOW_HIDDEN,
    NYA_WINDOW_HIGH_PIXEL_DENSITY = SDL_WINDOW_HIGH_PIXEL_DENSITY,
    NYA_WINDOW_MAXIMIZED          = SDL_WINDOW_MAXIMIZED,
    NYA_WINDOW_MINIMIZED          = SDL_WINDOW_MINIMIZED,
    NYA_WINDOW_MODAL              = SDL_WINDOW_MODAL,
    NYA_WINDOW_MOUSE_GRABBED      = SDL_WINDOW_MOUSE_GRABBED,
    NYA_WINDOW_NOT_FOCUSABLE      = SDL_WINDOW_NOT_FOCUSABLE,
    NYA_WINDOW_OCCLUDED           = SDL_WINDOW_OCCLUDED,
    NYA_WINDOW_POPUP_MENU         = SDL_WINDOW_POPUP_MENU,
    NYA_WINDOW_RESIZABLE          = SDL_WINDOW_RESIZABLE,
    NYA_WINDOW_TOOLTIP            = SDL_WINDOW_TOOLTIP,
    NYA_WINDOW_TRANSPARENT        = SDL_WINDOW_TRANSPARENT,
    NYA_WINDOW_UTILITY            = SDL_WINDOW_UTILITY,
};

/**
 * What a point in a window is, when the window is asked. See nya_window_region_set.
 *
 * The values are SDL's, so the two never drift.
 * */
enum NYA_WindowRegion {
    /** Ordinary content: clicks go to the app. */
    NYA_WINDOW_REGION_NORMAL = SDL_HITTEST_NORMAL,

    /** Dragging here moves the whole window, the way a title bar does. */
    NYA_WINDOW_REGION_DRAGGABLE = SDL_HITTEST_DRAGGABLE,

    NYA_WINDOW_REGION_RESIZE_TOP_LEFT     = SDL_HITTEST_RESIZE_TOPLEFT,
    NYA_WINDOW_REGION_RESIZE_TOP          = SDL_HITTEST_RESIZE_TOP,
    NYA_WINDOW_REGION_RESIZE_TOP_RIGHT    = SDL_HITTEST_RESIZE_TOPRIGHT,
    NYA_WINDOW_REGION_RESIZE_RIGHT        = SDL_HITTEST_RESIZE_RIGHT,
    NYA_WINDOW_REGION_RESIZE_BOTTOM_RIGHT = SDL_HITTEST_RESIZE_BOTTOMRIGHT,
    NYA_WINDOW_REGION_RESIZE_BOTTOM       = SDL_HITTEST_RESIZE_BOTTOM,
    NYA_WINDOW_REGION_RESIZE_BOTTOM_LEFT  = SDL_HITTEST_RESIZE_BOTTOMLEFT,
    NYA_WINDOW_REGION_RESIZE_LEFT         = SDL_HITTEST_RESIZE_LEFT,
};

/**
 * Asked what the point `x`, `y` in `window` is. Window coordinates, origin at the top left.
 *
 * Called by the platform while the pointer moves, not on the frame loop, so it does what a hit test does
 * and nothing else: no allocation, no drawing, no state a frame also writes.
 * */
typedef NYA_WindowRegion (*NYA_WindowRegionFn)(NYA_WindowHandle window, s32 x, s32 y, void* user_data);

enum NYA_FlashOperation {
    NYA_FLASH_CANCEL        = SDL_FLASH_CANCEL,
    NYA_FLASH_BRIEFLY       = SDL_FLASH_BRIEFLY,
    NYA_FLASH_UNTIL_FOCUSED = SDL_FLASH_UNTIL_FOCUSED,
};

struct NYA_Window {
    NYA_WindowHandle handle;
    NYA_ConstCString title;
    SDL_Window*      sdl_window;

    /**
     * Size in logical units, as last reported by the platform.
     * */
    u32 width;
    u32 height;

    /**
     * Size of the swapchain image, in pixels.
     * */
    u32 screen_width;
    u32 screen_height;

    NYA_ArrayᐸNYA_Layerᐳ* layer_stack;

    /**
     * What the platform asks when it wants to know what a point in this window is, and the data it is
     * handed. Zero until nya_window_region_set installs one. See NYA_WindowRegionFn.
     * */
    NYA_CallbackHandle on_region;
    void*              region_user_data;

    NYA_RenderSystemWindow render_system;
};

/*
 * ─────────────────────────────────────────────────────────
 * LAYER STRUCT
 * ─────────────────────────────────────────────────────────
 */

/** The longest layer id, terminator included. Ids are short literals naming the layer. */
#ifndef NYA_LAYER_ID_MAX
#define NYA_LAYER_ID_MAX 64
#endif

struct NYA_Layer {
    /** Copied, and compared by content, so a layer pushed by a game DLL is still found after a reload. */
    char id[NYA_LAYER_ID_MAX];
    b8   enabled;

    NYA_CallbackHandle on_create;
    NYA_CallbackHandle on_destroy;
    NYA_CallbackHandle on_event;
    NYA_CallbackHandle on_update;
    NYA_CallbackHandle on_render;

    /** Set by nya_layer_push. */
    NYA_WindowHandle window;
};

/**
 * A layer whose five hooks are `prefix##_on_create` and friends, enabled, identified by `layer_id`.
 *
 * ```c
 * GNY_LAYER_PAUSE_MENU = nya_layer_of(gny_layer_pause_menu, GNY_LAYER_PAUSE_MENU_ID);
 * ```
 *
 * Derives the hook names from one prefix, so a layer cannot be wired to another layer's `on_update`
 * by a paste mistake. A missing hook fails to compile, naming it. A layer that wants fewer hooks
 * builds the struct by hand.
 * */
#define nya_layer_of(prefix, layer_id)                                                                                                       \
    _nya_layer_with_id((NYA_Layer){                                                                                                          \
        .enabled    = true,                                                                                                                  \
        .on_create  = nya_callback(prefix##_on_create),                                                                                       \
        .on_destroy = nya_callback(prefix##_on_destroy),                                                                                      \
        .on_event   = nya_callback(prefix##_on_event),                                                                                        \
        .on_update  = nya_callback(prefix##_on_update),                                                                                       \
        .on_render  = nya_callback(prefix##_on_render),                                                                                       \
    }, (layer_id))

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

NYA_API void nya_system_window_init(void);
NYA_API void nya_system_window_deinit(void);
NYA_API void nya_system_window_handle_event(NYA_Event* event);

/** Iterating the slot table. Skips empty slots, so `index` is not a count. */
NYA_API NYA_Window* nya_window_at_slot(u32 index) __attr_no_discard;
NYA_API u32         nya_window_count(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

/** Returns NYA_WINDOW_HANDLE_NONE if the slot table is full. The size is a request, see the file comment. */
NYA_API NYA_WindowHandle nya_window_create(NYA_ConstCString title, u32 requested_width, u32 requested_height, NYA_WindowFlags flags)
    __attr_no_discard;

NYA_API void nya_window_destroy(NYA_WindowHandle window);

/** Null once the window is gone, which is the whole reason handles exist. Never store the result. */
NYA_API NYA_Window* nya_window_get(NYA_WindowHandle window) __attr_no_discard;
NYA_API b8          nya_window_is_valid(NYA_WindowHandle window) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * GEOMETRY, READ
 * ─────────────────────────────────────────────────────────
 */

/*
 * These read back what the platform actually did, which is the only trustworthy answer. Prefer them
 * over remembering what was requested.
 */

/** Logical size. What layout and input coordinates are in. */
NYA_API void nya_window_size(NYA_WindowHandle window, OUT u32* width, OUT u32* height);

/** Size in pixels. What a viewport and a render target are in. Differs from logical when scaled. */
NYA_API void nya_window_size_in_pixels(NYA_WindowHandle window, OUT u32* width, OUT u32* height);

NYA_API void nya_window_position(NYA_WindowHandle window, OUT s32* x, OUT s32* y);
NYA_API f32  nya_window_display_scale(NYA_WindowHandle window) __attr_no_discard;
NYA_API f32  nya_window_pixel_density(NYA_WindowHandle window) __attr_no_discard;

/** The part of the window not covered by notches, rounded corners or system UI. */
NYA_API NYA_Rect nya_window_safe_area(NYA_WindowHandle window) __attr_no_discard;

NYA_API NYA_WindowFlags  nya_window_flags(NYA_WindowHandle window) __attr_no_discard;
NYA_API NYA_ConstCString nya_window_title(NYA_WindowHandle window) __attr_no_discard;
NYA_API f32              nya_window_opacity(NYA_WindowHandle window) __attr_no_discard;

NYA_API b8 nya_window_has_focus(NYA_WindowHandle window) __attr_no_discard;
NYA_API b8 nya_window_is_fullscreen(NYA_WindowHandle window) __attr_no_discard;
NYA_API b8 nya_window_is_maximized(NYA_WindowHandle window) __attr_no_discard;
NYA_API b8 nya_window_is_minimized(NYA_WindowHandle window) __attr_no_discard;
NYA_API b8 nya_window_is_visible(NYA_WindowHandle window) __attr_no_discard;
NYA_API b8 nya_window_is_occluded(NYA_WindowHandle window) __attr_no_discard;
NYA_API b8 nya_window_is_resizable(NYA_WindowHandle window) __attr_no_discard;
NYA_API b8 nya_window_is_borderless(NYA_WindowHandle window) __attr_no_discard;
NYA_API b8 nya_window_is_always_on_top(NYA_WindowHandle window) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * GEOMETRY, REQUEST
 * ─────────────────────────────────────────────────────────
 */

/**
 * True when the platform lets a client choose its own size and position.
 * */
NYA_API b8 nya_window_geometry_is_client_controlled(void) __attr_no_discard;

/* Each returns whether the platform accepts client geometry at all, not whether it obeyed. */

NYA_API b8 nya_window_request_size(NYA_WindowHandle window, u32 width, u32 height);
NYA_API b8 nya_window_request_position(NYA_WindowHandle window, s32 x, s32 y);

/**
 * A floor and a ceiling for the window manager to enforce.
 * */
NYA_API b8 nya_window_request_minimum_size(NYA_WindowHandle window, u32 min_width, u32 min_height);
NYA_API b8 nya_window_request_maximum_size(NYA_WindowHandle window, u32 max_width, u32 max_height);

/** Locks the window to `width : height`. 0 for both removes the constraint. */
NYA_API b8 nya_window_request_aspect_ratio(NYA_WindowHandle window, f32 min_aspect, f32 max_aspect);

/*
 * ─────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────
 */

NYA_API void nya_window_show(NYA_WindowHandle window);
NYA_API void nya_window_hide(NYA_WindowHandle window);
NYA_API void nya_window_raise(NYA_WindowHandle window);
NYA_API void nya_window_maximize(NYA_WindowHandle window);
NYA_API void nya_window_minimize(NYA_WindowHandle window);
NYA_API void nya_window_restore(NYA_WindowHandle window);

NYA_API void nya_window_set_title(NYA_WindowHandle window, NYA_ConstCString title);

/** Sets the window's icon from encoded image bytes in any format SDL_image reads: PNG, BMP, ICO. */
NYA_API NYA_Error nya_window_set_icon(NYA_WindowHandle window, const u8* data, u64 size) __attr_no_discard;
NYA_API void nya_window_set_fullscreen(NYA_WindowHandle window, b8 fullscreen);
NYA_API void nya_window_set_borderless(NYA_WindowHandle window, b8 borderless);

/**
 * Installs the function the platform asks what a point in the window is. Null removes it.
 *
 * ```c
 * // a borderless widget dragged by its whole surface.
 * NYA_INTERNAL NYA_WindowRegion pet_region(NYA_WindowHandle window, s32 x, s32 y, void* user_data) {
 *     nya_unused(window, x, y, user_data);
 *     return NYA_WINDOW_REGION_DRAGGABLE;
 * }
 *
 * nya_window_region_set(window, nya_callback(pet_region), nullptr);
 * ```
 *
 * A NYA_WINDOW_BORDERLESS window has no title bar, so the app says which pixels drag it.
 *
 * The platform calls this while the pointer moves, on the window system's thread, so it must not
 * allocate, draw or touch frame state. Answer from geometry the callback already has.
 *
 * A callback handle, so it survives a code hot reload.
 * */
NYA_API void nya_window_region_set(NYA_WindowHandle window, NYA_CallbackHandle on_region, void* user_data);
NYA_API void nya_window_set_resizable(NYA_WindowHandle window, b8 resizable);
NYA_API void nya_window_set_always_on_top(NYA_WindowHandle window, b8 always_on_top);
NYA_API void nya_window_set_opacity(NYA_WindowHandle window, f32 opacity);
NYA_API void nya_window_set_focusable(NYA_WindowHandle window, b8 focusable);

/** Keeps the pointer inside the window. Mouse grab is a request the compositor may refuse. */
NYA_API void nya_window_set_mouse_grabbed(NYA_WindowHandle window, b8 grabbed);
NYA_API b8   nya_window_is_mouse_grabbed(NYA_WindowHandle window) __attr_no_discard;

/** Hides the cursor and reports motion as deltas. What a first person camera wants. */
NYA_API void nya_window_set_relative_mouse(NYA_WindowHandle window, b8 relative);
NYA_API b8   nya_window_is_relative_mouse(NYA_WindowHandle window) __attr_no_discard;

/** Asks the window manager for attention. */
NYA_API void nya_window_flash(NYA_WindowHandle window, NYA_FlashOperation operation);

/** Blocks until pending changes have been acknowledged by the window system. */
NYA_API void nya_window_sync(NYA_WindowHandle window);

/*
 * ─────────────────────────────────────────────────────────
 * DISPLAY
 * ─────────────────────────────────────────────────────────
 */

NYA_API NYA_ConstCString nya_window_display_name(NYA_WindowHandle window) __attr_no_discard;

/** Full extent of the display the window is on. */
NYA_API NYA_Rect nya_window_display_bounds(NYA_WindowHandle window) __attr_no_discard;

/** The part of the display not occupied by taskbars and docks. */
NYA_API NYA_Rect nya_window_display_usable_bounds(NYA_WindowHandle window) __attr_no_discard;

NYA_API NYA_DisplayMode nya_window_display_mode(NYA_WindowHandle window) __attr_no_discard;

/** Which video driver SDL selected: "wayland", "x11", "windows", ... */
NYA_API NYA_ConstCString nya_video_driver(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * LAYER FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API NYA_Layer* nya_layer_get(NYA_WindowHandle window, NYA_ConstCString layer_id) __attr_no_discard;
NYA_API void       nya_layer_enable(NYA_WindowHandle window, NYA_ConstCString layer_id);
NYA_API void       nya_layer_disable(NYA_WindowHandle window, NYA_ConstCString layer_id);

/** `layer` with `id` copied in. What nya_layer_of expands to. */
NYA_API NYA_Layer _nya_layer_with_id(NYA_Layer layer, NYA_ConstCString id) __attr_no_discard;
NYA_API void       nya_layer_push(NYA_WindowHandle window, NYA_Layer layer);
NYA_API NYA_Layer  nya_layer_pop(NYA_WindowHandle window);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MOUSE CURSOR
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Which shape the pointer takes.
 * */
enum NYA_CursorShape {
    NYA_CURSOR_DEFAULT,

    /** An I-beam. Over any text a click would put a caret in. */
    NYA_CURSOR_TEXT,

    NYA_CURSOR_POINTER,
    NYA_CURSOR_CROSSHAIR,
    NYA_CURSOR_MOVE,

    /** Splitters, and the edges of a resizable panel. */
    NYA_CURSOR_RESIZE_HORIZONTAL,
    NYA_CURSOR_RESIZE_VERTICAL,
    NYA_CURSOR_RESIZE_NWSE,
    NYA_CURSOR_RESIZE_NESW,

    NYA_CURSOR_NOT_ALLOWED,
    NYA_CURSOR_WAIT,

    NYA_CURSOR_COUNT,
};

/**
 * Sets the pointer's shape. Cheap to call every frame with the same value.
 * */
NYA_API void nya_cursor_set(NYA_CursorShape shape);

/** The shape currently set. */
NYA_API NYA_CursorShape nya_cursor(void) __attr_no_discard;

/** Shows or hides the pointer entirely. What a first person camera does while it has the mouse. */
NYA_API void nya_cursor_visible_set(b8 visible);

NYA_API b8 nya_cursor_visible(void) __attr_no_discard;

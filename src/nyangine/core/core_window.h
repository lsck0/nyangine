/**
 * @file core_window.h
 *
 * Windows, and the layer stacks that live on them.
 *
 * **Windows are addressed by handle, not by pointer.** NYA_Window lives in a fixed slot table, so a
 * pointer stays valid for as long as that window exists, but it says nothing about whether the
 * window still exists. A handle carries a generation, so a lookup after the window closed returns
 * null instead of a live pointer to whatever took the slot. Hold handles; borrow pointers.
 *
 * **Geometry belongs to the compositor, not to the app.** Under Wayland a client cannot set its own
 * size or position at all; it is *told* what it got and must draw to that. The setters here are
 * therefore named `request` and return whether the platform even entertains the idea. The
 * authoritative size is the one that arrives on NYA_EVENT_WINDOW_RESIZED, and the one
 * nya_window_size reads back. Anything that computes layout from a size it asked for rather than
 * the size it was given will be wrong on half the systems it runs on.
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
 *
 * Fixed rather than growable on purpose: a growable array reallocates, and every NYA_Window* handed
 * to a layer or held across a frame would dangle the moment a second window opened.
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
     *
     * False under Wayland, where the compositor decides and the app is informed. Queried once at
     * init rather than per call, since it cannot change while the process runs.
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
     *
     * Not what was asked for at creation. Updated from NYA_EVENT_WINDOW_RESIZED, which is the only
     * source that is right everywhere.
     * */
    u32 width;
    u32 height;

    /**
     * Size of the swapchain image, in pixels.
     *
     * Differs from width/height whenever the display is scaled, and it is this pair that a viewport
     * or a projection matrix wants.
     * */
    u32 screen_width;
    u32 screen_height;

    NYA_ArrayᐸNYA_Layerᐳ* layer_stack;

    NYA_RenderSystemWindow render_system;
};

/*
 * ─────────────────────────────────────────────────────────
 * LAYER STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_Layer {
    void* id;
    b8    enabled;

    NYA_CallbackHandle on_create;
    NYA_CallbackHandle on_destroy;
    NYA_CallbackHandle on_event;
    NYA_CallbackHandle on_update;
    NYA_CallbackHandle on_render;

    /** Set by nya_layer_push. */
    NYA_WindowHandle window;
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
 *
 * False under Wayland. Branch on this rather than assuming the requests below did anything: they
 * are hints, and a compositor is free to ignore them or, as observed, to react to them in ways that
 * are worse than being ignored.
 * */
NYA_API b8 nya_window_geometry_is_client_controlled(void) __attr_no_discard;

/* Each returns whether the platform accepts client geometry at all, not whether it obeyed. */

NYA_API b8 nya_window_request_size(NYA_WindowHandle window, u32 width, u32 height);
NYA_API b8 nya_window_request_position(NYA_WindowHandle window, s32 x, s32 y);

/**
 * A floor and a ceiling for the window manager to enforce.
 *
 * Setting a minimum right after creating a window has been observed to make a tiling Wayland
 * compositor collapse the surface to exactly that minimum and maximize the frame around it. Clamp in
 * a NYA_EVENT_WINDOW_RESIZED handler instead if the floor actually matters.
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

/**
 * Sets the window's icon from encoded image bytes — whatever SDL_image reads: PNG, BMP, ICO.
 *
 * Bytes rather than an asset handle because the window system sits below the asset system, so it
 * cannot ask it for anything. Load the image as NYA_ASSET_TYPE_TEXT and hand over what it read:
 *
 * ```c
 * NYA_Asset* icon = nya_asset_get(NYA_ASSET_ICON_ICON_BMP);
 * nya_asset_with(icon) NYA_EXPECT(nya_window_set_icon(window, icon->as_text.data, icon->as_text.size));
 * ```
 *
 * SDL converts the image into its own surface, so the bytes are only needed for the call and the
 * asset can be released straight after.
 *
 * Returns an error rather than asserting, because failing is not a programming mistake: on Wayland
 * this needs the compositor to support xdg_toplevel_icon_v1, and one that does not will refuse.
 * Windows, X11 and macOS set it directly. Note that on Windows the icon shown before the process
 * starts comes from the resource the build compiles into the executable, not from this.
 * */
NYA_API NYA_Error nya_window_set_icon(NYA_WindowHandle window, const u8* data, u64 size) __attr_no_discard;
NYA_API void nya_window_set_fullscreen(NYA_WindowHandle window, b8 fullscreen);
NYA_API void nya_window_set_borderless(NYA_WindowHandle window, b8 borderless);
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

NYA_API NYA_Layer* nya_layer_get(NYA_WindowHandle window, void* layer_id) __attr_no_discard;
NYA_API void       nya_layer_enable(NYA_WindowHandle window, void* layer_id);
NYA_API void       nya_layer_disable(NYA_WindowHandle window, void* layer_id);
NYA_API void       nya_layer_push(NYA_WindowHandle window, NYA_Layer layer);
NYA_API NYA_Layer  nya_layer_pop(NYA_WindowHandle window);

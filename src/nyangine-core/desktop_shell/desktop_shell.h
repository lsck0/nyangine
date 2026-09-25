/**
 * @file desktop_shell.h
 *
 * The desktop's own furniture, behind one component so a game or a headless server does not pay for
 * it: native file dialogs, a message box, and a system tray icon with a menu. Each is a thin skin over
 * SDL's dialog, message-box and tray subsystems, given a `nya_*` API with the engine's error handling
 * and bounded strings.
 *
 * ```c
 * NYA_TRY(nya_desktop_shell_init());
 *
 * // A file dialog is asynchronous: it returns at once and answers on a later frame, once SDL has
 * // pumped its events, so it never blocks the loop.
 * static void on_pick(void* user, const NYA_DesktopFileResult* result) {
 *     if (result->cancelled) return;
 *     for (u32 i = 0; i < result->count; i++) open_document(result->paths[i]);
 * }
 * NYA_DesktopFileFilter filters[] = { { "Images", "png;jpg;jpeg" }, { "All files", "*" } };
 * nya_desktop_open_files(filters, 2, nullptr, true, on_pick, nullptr);
 *
 * // A tray icon with a menu; its entries answer through callbacks on the main thread.
 * NYA_DesktopTray* tray = nullptr;
 * nya_desktop_tray_create(nullptr, "My app", &tray);
 * nya_desktop_tray_add_button(tray, "Quit", on_quit, nullptr);
 * ```
 *
 * Everything here needs SDL and a display. Under a headless or server build (NYA_NO_SDL) the whole
 * component is compiled out; on a machine with no display the subsystem fails to come up and every call
 * answers NYA_ERROR_NOT_SUPPORTED rather than crashing, so the same code runs on a build farm.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

enum {
    /** Longest path a dialog hands back, terminator included. A choice that bounds a fixed buffer, so an enum. */
    NYA_DESKTOP_PATH_MAX = 1024,

    /** How many files one open dialog may return. Past this the extra selection is dropped, not overrun. */
    NYA_DESKTOP_DIALOG_MAX_FILES = 64,

    /** How many filters one dialog may carry. */
    NYA_DESKTOP_DIALOG_MAX_FILTERS = 16,

    /** Longest filter name and pattern, terminator included. */
    NYA_DESKTOP_FILTER_NAME_MAX    = 64,
    NYA_DESKTOP_FILTER_PATTERN_MAX = 128,

    /** How many buttons a message-box prompt may offer. */
    NYA_DESKTOP_MESSAGE_MAX_BUTTONS = 4,

    /** How many entries one tray menu holds. */
    NYA_DESKTOP_TRAY_MAX_ENTRIES = 32,

    /** Longest tray tooltip and entry label, terminator included. */
    NYA_DESKTOP_TOOLTIP_MAX    = 128,
    NYA_DESKTOP_TRAY_LABEL_MAX = 128,

    /** How many file dialogs may be waiting for their answer at once. */
    NYA_DESKTOP_MAX_PENDING_DIALOGS = 4,

    /** How many trays may exist at once. One is the usual case; a couple leaves room. */
    NYA_DESKTOP_MAX_TRAYS = 4,
};

/** What a message-box prompt returns when the box was dismissed without a button (escape, close). */
#define NYA_DESKTOP_NO_BUTTON U32_MAX

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A message box's severity, which picks its icon. */
typedef enum NYA_DesktopMessageLevel {
    NYA_DESKTOP_MESSAGE_INFO,
    NYA_DESKTOP_MESSAGE_WARNING,
    NYA_DESKTOP_MESSAGE_ERROR,
} NYA_DesktopMessageLevel;

/** One dialog filter: a human name and a semicolon-separated list of extensions, SDL's own form ("png;jpg"). */
typedef struct NYA_DesktopFileFilter {
    NYA_ConstCString name;
    NYA_ConstCString pattern;
} NYA_DesktopFileFilter;

/**
 * What a file dialog hands its callback. Valid only for the duration of that call: the paths point into
 * a shared buffer the next dialog reuses, so copy anything kept past the callback.
 * */
typedef struct NYA_DesktopFileResult {
    /** The user closed the dialog without choosing, or it failed; `count` is zero. */
    b8 cancelled;

    /** How many paths were chosen, at most NYA_DESKTOP_DIALOG_MAX_FILES. */
    u32 count;

    /** The chosen paths, `count` of them. */
    NYA_ConstCString paths[NYA_DESKTOP_DIALOG_MAX_FILES];
} NYA_DesktopFileResult;

/** Answers a file dialog once the user has chosen or cancelled. Runs on the main thread, during the event pump. */
typedef void (*NYA_DesktopFileCallback)(void* user, const NYA_DesktopFileResult* result);

/** Runs when a tray entry is chosen. On the main thread, during the event pump. */
typedef void (*NYA_DesktopTrayCallback)(void* user);

/**
 * An RGBA8 image for a tray icon, row-major, one byte per channel. The pixels are copied at create, so
 * the caller's buffer need not outlive the call.
 * */
typedef struct NYA_DesktopTrayIcon {
    const void* rgba;
    u32         width;
    u32         height;
} NYA_DesktopTrayIcon;

/** A system tray icon and its menu. Owned by the component; freed by nya_desktop_tray_destroy. */
typedef struct NYA_DesktopTray NYA_DesktopTray;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Brings up the video subsystem the dialogs and the tray rest on. Idempotent, and cheap when SDL's video
 * is already up (a windowed app), because it only takes a reference. Returns NYA_ERROR_NOT_SUPPORTED where
 * there is no display, which is the answer a headless build and a build farm both get.
 * */
NYA_API NYA_Error nya_desktop_shell_init(void) __attr_no_discard;

/** Releases the reference init took and destroys any tray still open, so the process leaves nothing behind. */
NYA_API void nya_desktop_shell_deinit(void);

/** Whether init succeeded and the desktop is available. Every other call answers NYA_ERROR_NOT_SUPPORTED when it is not. */
NYA_API b8 nya_desktop_shell_ready(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MESSAGE BOX
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Shows a modal message box with a single dismiss button and blocks until it is closed. For the
 * occasional "this failed" the user must acknowledge; the long-running work of a program does not go here.
 * `title` and `message` are required.
 * */
NYA_API NYA_Error nya_desktop_message_show(NYA_DesktopMessageLevel level, NYA_ConstCString title, NYA_ConstCString message) __attr_no_discard;

/**
 * Shows a modal message box offering `button_count` buttons (at most NYA_DESKTOP_MESSAGE_MAX_BUTTONS) and
 * blocks until one is chosen. Writes the chosen button's index to `out_chosen`, or NYA_DESKTOP_NO_BUTTON
 * if the box was dismissed without one. The first button is the default for the return key.
 * */
NYA_API NYA_Error nya_desktop_message_prompt(NYA_DesktopMessageLevel level, NYA_ConstCString title, NYA_ConstCString message,
                                             const NYA_ConstCString* buttons, u32 button_count, OUT u32* out_chosen) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FILE DIALOGS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * All three are asynchronous: they return at once and call `callback(user, result)` on a later frame,
 * once SDL has pumped its events, so the loop is never blocked. `filters` and `default_location` may be
 * null; `default_location` is a starting folder or file.
 */

/** Opens a dialog to choose one existing file, or several when `allow_many`. */
NYA_API NYA_Error nya_desktop_open_files(const NYA_DesktopFileFilter* filters, u32 filter_count, NYA_ConstCString default_location, b8 allow_many,
                                         NYA_DesktopFileCallback callback, void* user) __attr_no_discard;

/** Opens a dialog to choose a destination file to save to. */
NYA_API NYA_Error nya_desktop_save_file(const NYA_DesktopFileFilter* filters, u32 filter_count, NYA_ConstCString default_location,
                                        NYA_DesktopFileCallback callback, void* user) __attr_no_discard;

/** Opens a dialog to choose one folder, or several when `allow_many`. */
NYA_API NYA_Error nya_desktop_open_folder(NYA_ConstCString default_location, b8 allow_many, NYA_DesktopFileCallback callback, void* user)
    __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TRAY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Creates a tray icon with an empty menu. `icon` may be null for the platform default; `tooltip` is the
 * hover text. Writes the tray to `out_tray`, owned by the component until nya_desktop_tray_destroy.
 * */
NYA_API NYA_Error nya_desktop_tray_create(const NYA_DesktopTrayIcon* icon, NYA_ConstCString tooltip, OUT NYA_DesktopTray** out_tray) __attr_no_discard;

/** Destroys a tray and frees its menu. Passing null is a no-op. */
NYA_API void nya_desktop_tray_destroy(NYA_DesktopTray* tray);

/** Appends a clickable menu entry that runs `callback(user)` when chosen. */
NYA_API NYA_Error nya_desktop_tray_add_button(NYA_DesktopTray* tray, NYA_ConstCString label, NYA_DesktopTrayCallback callback, void* user)
    __attr_no_discard;

/**
 * Appends a checkable menu entry, initially `checked`. The check toggles on its own when chosen, and
 * `callback(user)` runs on each toggle.
 * */
NYA_API NYA_Error nya_desktop_tray_add_checkbox(NYA_DesktopTray* tray, NYA_ConstCString label, b8 checked, NYA_DesktopTrayCallback callback, void* user)
    __attr_no_discard;

/** Appends a separator line to the menu. */
NYA_API NYA_Error nya_desktop_tray_add_separator(NYA_DesktopTray* tray) __attr_no_discard;

/**
 * Pumps the trays so their menus repaint and their callbacks fire. Some platforms drive the tray from
 * the main loop rather than the event queue, so call this once a frame from a program that has a tray.
 * */
NYA_API void nya_desktop_tray_update(void);

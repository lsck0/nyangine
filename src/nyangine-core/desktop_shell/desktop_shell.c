#include "nyangine-core/nyangine.h"

#include <stdio.h>

#include "SDL3/SDL_dialog.h"
#include "SDL3/SDL_error.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_keyboard.h"
#include "SDL3/SDL_messagebox.h"
#include "SDL3/SDL_pixels.h"
#include "SDL3/SDL_surface.h"
#include "SDL3/SDL_tray.h"
#include "SDL3/SDL_video.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One file dialog waiting for its answer. The filters and location are copied here so they outlive the async call. */
typedef struct {
    b8                      in_use;
    NYA_DesktopFileCallback callback;
    void*                   user;

    u32                  filter_count;
    SDL_DialogFileFilter sdl_filters[NYA_DESKTOP_DIALOG_MAX_FILTERS];
    char                 filter_names[NYA_DESKTOP_DIALOG_MAX_FILTERS][NYA_DESKTOP_FILTER_NAME_MAX];
    char                 filter_patterns[NYA_DESKTOP_DIALOG_MAX_FILTERS][NYA_DESKTOP_FILTER_PATTERN_MAX];

    b8   has_location;
    char default_location[NYA_DESKTOP_PATH_MAX];
} _NYA_DesktopDialog;

/** One tray menu entry's callback and its user pointer. Lives in the tray, so its address is stable while the tray is. */
typedef struct {
    NYA_DesktopTrayCallback callback;
    void*                   user;
} _NYA_DesktopTrayEntry;

struct NYA_DesktopTray {
    b8            in_use;
    SDL_Tray*     handle;
    SDL_TrayMenu* menu;

    // the icon surface owns its own pixels (copied at create) and is freed with the tray.
    SDL_Surface* icon;

    _NYA_DesktopTrayEntry entries[NYA_DESKTOP_TRAY_MAX_ENTRIES];
    u32                   entry_count;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL struct {
    b8                 ready;
    _NYA_DesktopDialog dialogs[NYA_DESKTOP_MAX_PENDING_DIALOGS];
    NYA_DesktopTray    trays[NYA_DESKTOP_MAX_TRAYS];
} _nya_desktop_shell;

/**
 * Where a dialog's chosen paths are copied for the callback. One shared buffer: SDL delivers dialog and
 * tray callbacks on the main thread one at a time, so no two are ever marshalled at once.
 * */
NYA_INTERNAL char _nya_desktop_paths[NYA_DESKTOP_DIALOG_MAX_FILES][NYA_DESKTOP_PATH_MAX];

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The message-box flags for a severity. */
NYA_INTERNAL SDL_MessageBoxFlags _nya_desktop_message_flags(NYA_DesktopMessageLevel level) {
    switch (level) {
        case NYA_DESKTOP_MESSAGE_WARNING: return SDL_MESSAGEBOX_WARNING;
        case NYA_DESKTOP_MESSAGE_ERROR:   return SDL_MESSAGEBOX_ERROR;
        case NYA_DESKTOP_MESSAGE_INFO:
        default:                          return SDL_MESSAGEBOX_INFORMATION;
    }
}

/** The window a dialog or box should sit over: whichever of the program's windows has focus, or none. */
NYA_INTERNAL SDL_Window* _nya_desktop_parent(void) {
    return SDL_GetKeyboardFocus();
}

/** A free dialog slot, or null when NYA_DESKTOP_MAX_PENDING_DIALOGS are already waiting. */
NYA_INTERNAL _NYA_DesktopDialog* _nya_desktop_dialog_acquire(void) {
    for (u32 i = 0; i < NYA_DESKTOP_MAX_PENDING_DIALOGS; i++) {
        if (!_nya_desktop_shell.dialogs[i].in_use) return &_nya_desktop_shell.dialogs[i];
    }
    return nullptr;
}

/** Copies the caller's filters into `slot`, so the pointers SDL keeps stay valid until the answer. */
NYA_INTERNAL void _nya_desktop_dialog_set_filters(_NYA_DesktopDialog* slot, const NYA_DesktopFileFilter* filters, u32 filter_count) {
    slot->filter_count = filter_count;
    for (u32 i = 0; i < filter_count; i++) {
        (void)snprintf(slot->filter_names[i], sizeof(slot->filter_names[i]), "%s", filters[i].name != nullptr ? filters[i].name : "");
        (void)snprintf(slot->filter_patterns[i], sizeof(slot->filter_patterns[i]), "%s", filters[i].pattern != nullptr ? filters[i].pattern : "*");
        slot->sdl_filters[i] = (SDL_DialogFileFilter){ .name = slot->filter_names[i], .pattern = slot->filter_patterns[i] };
    }
}

/** SDL's answer to a file dialog: marshal it into an NYA_DesktopFileResult and hand it to the caller's callback. */
NYA_INTERNAL_CALLBACK void _nya_desktop_dialog_answer(void* userdata, const char* const* filelist, int filter) {
    (void)filter;

    _NYA_DesktopDialog*   slot   = (_NYA_DesktopDialog*)userdata;
    NYA_DesktopFileResult result = { .cancelled = true, .count = 0 };

    if (filelist == nullptr) {
        // A null list is an error, not a cancel; the report is the most a headless caller can act on.
        nya_log_warn("Desktop file dialog failed: %s", SDL_GetError());
    } else if (filelist[0] == nullptr) {
        // An empty list is a plain cancel; `cancelled` already says so.
    } else {
        result.cancelled = false;
        for (u32 i = 0; filelist[i] != nullptr && result.count < NYA_DESKTOP_DIALOG_MAX_FILES; i++) {
            (void)snprintf(_nya_desktop_paths[result.count], NYA_DESKTOP_PATH_MAX, "%s", filelist[i]);
            result.paths[result.count] = _nya_desktop_paths[result.count];
            result.count++;
        }
    }

    // Freed before the callback runs, so the callback may open the next dialog into this same slot.
    NYA_DesktopFileCallback callback = slot->callback;
    void*                   user     = slot->user;
    *slot                            = (_NYA_DesktopDialog){ 0 };

    if (callback != nullptr) callback(user, &result);
}

/** SDL's notice that a tray entry was chosen: forward it to the entry's own callback. */
NYA_INTERNAL_CALLBACK void _nya_desktop_tray_entry_chosen(void* userdata, SDL_TrayEntry* entry) {
    (void)entry;

    _NYA_DesktopTrayEntry* slot = (_NYA_DesktopTrayEntry*)userdata;
    if (slot != nullptr && slot->callback != nullptr) slot->callback(slot->user);
}

/** A free tray slot, or null when NYA_DESKTOP_MAX_TRAYS already exist. */
NYA_INTERNAL NYA_DesktopTray* _nya_desktop_tray_acquire(void) {
    for (u32 i = 0; i < NYA_DESKTOP_MAX_TRAYS; i++) {
        if (!_nya_desktop_shell.trays[i].in_use) return &_nya_desktop_shell.trays[i];
    }
    return nullptr;
}

/** Inserts a tray entry with the given label and flags, wiring `callback` to it. Shared by button and checkbox. */
NYA_INTERNAL NYA_Error _nya_desktop_tray_entry_add(NYA_DesktopTray* tray, NYA_ConstCString label, SDL_TrayEntryFlags flags,
                                                   NYA_DesktopTrayCallback callback, void* user) {
    nya_assert(tray != nullptr);
    if (!_nya_desktop_shell.ready || tray->handle == nullptr) return nya_error(NYA_ERROR_NOT_SUPPORTED, "the desktop shell is not available");
    if (label == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a tray entry needs a label");
    if (tray->entry_count >= NYA_DESKTOP_TRAY_MAX_ENTRIES) return nya_error(NYA_ERROR_NOT_OK, "the tray menu is full");

    _NYA_DesktopTrayEntry* slot = &tray->entries[tray->entry_count];
    *slot                       = (_NYA_DesktopTrayEntry){ .callback = callback, .user = user };

    // SDL copies the label into its own storage, so the caller's string need not outlive this call.
    SDL_TrayEntry* entry = SDL_InsertTrayEntryAt(tray->menu, -1, label, flags);
    if (entry == nullptr) {
        *slot = (_NYA_DesktopTrayEntry){ 0 };
        return nya_error(NYA_ERROR_NOT_OK, "SDL_InsertTrayEntryAt failed: %s", SDL_GetError());
    }

    SDL_SetTrayEntryCallback(entry, _nya_desktop_tray_entry_chosen, slot);
    tray->entry_count++;
    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_desktop_shell_init(void) {
    if (_nya_desktop_shell.ready) return NYA_OK;

    // Refcounted in SDL, so this is cheap when a windowed app already holds the video subsystem, and a
    // matching quit in deinit only drops our reference rather than tearing down someone else's window.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "no display for the desktop shell: %s", SDL_GetError());
    }

    _nya_desktop_shell.ready = true;
    nya_log_info("Desktop shell initialized (native dialogs and tray available).");
    return NYA_OK;
}

void nya_desktop_shell_deinit(void) {
    if (!_nya_desktop_shell.ready) return;

    for (u32 i = 0; i < NYA_DESKTOP_MAX_TRAYS; i++) {
        if (_nya_desktop_shell.trays[i].in_use) nya_desktop_tray_destroy(&_nya_desktop_shell.trays[i]);
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    _nya_desktop_shell = (typeof(_nya_desktop_shell)){ 0 };
    nya_log_info("Desktop shell deinitialized.");
}

b8 nya_desktop_shell_ready(void) {
    return _nya_desktop_shell.ready;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MESSAGE BOX
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_desktop_message_show(NYA_DesktopMessageLevel level, NYA_ConstCString title, NYA_ConstCString message) {
    if (title == nullptr || message == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message box needs a title and a message");
    if (!_nya_desktop_shell.ready) return nya_error(NYA_ERROR_NOT_SUPPORTED, "the desktop shell is not available");

    if (!SDL_ShowSimpleMessageBox(_nya_desktop_message_flags(level), title, message, _nya_desktop_parent())) {
        return nya_error(NYA_ERROR_NOT_OK, "SDL_ShowSimpleMessageBox failed: %s", SDL_GetError());
    }
    return NYA_OK;
}

NYA_Error nya_desktop_message_prompt(NYA_DesktopMessageLevel level, NYA_ConstCString title, NYA_ConstCString message, const NYA_ConstCString* buttons,
                                     u32 button_count, OUT u32* out_chosen) {
    if (title == nullptr || message == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message box needs a title and a message");
    if (buttons == nullptr || button_count == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a prompt needs at least one button");
    if (button_count > NYA_DESKTOP_MESSAGE_MAX_BUTTONS) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "at most %d buttons", NYA_DESKTOP_MESSAGE_MAX_BUTTONS);
    if (out_chosen == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a prompt needs somewhere to write the chosen button");
    if (!_nya_desktop_shell.ready) return nya_error(NYA_ERROR_NOT_SUPPORTED, "the desktop shell is not available");

    SDL_MessageBoxButtonData button_data[NYA_DESKTOP_MESSAGE_MAX_BUTTONS] = { 0 };
    for (u32 i = 0; i < button_count; i++) {
        button_data[i] = (SDL_MessageBoxButtonData){
            // the first button answers the return key, so a one-button box acknowledges with enter.
            .flags    = i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0U,
            .buttonID = (int)i,
            .text     = buttons[i] != nullptr ? buttons[i] : "",
        };
    }

    SDL_MessageBoxData data = {
        .flags      = _nya_desktop_message_flags(level),
        .window     = _nya_desktop_parent(),
        .title      = title,
        .message    = message,
        .numbuttons = (int)button_count,
        .buttons    = button_data,
    };

    int chosen = -1;
    if (!SDL_ShowMessageBox(&data, &chosen)) return nya_error(NYA_ERROR_NOT_OK, "SDL_ShowMessageBox failed: %s", SDL_GetError());

    *out_chosen = chosen < 0 ? NYA_DESKTOP_NO_BUTTON : (u32)chosen;
    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FILE DIALOGS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Shared setup for the three dialogs: validate, take a slot, copy the filters and location. */
NYA_INTERNAL NYA_Error _nya_desktop_dialog_begin(const NYA_DesktopFileFilter* filters, u32 filter_count, NYA_ConstCString default_location,
                                                 NYA_DesktopFileCallback callback, void* user, OUT _NYA_DesktopDialog** out_slot) {
    if (callback == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a file dialog needs a callback for its answer");
    if (filter_count > NYA_DESKTOP_DIALOG_MAX_FILTERS) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "at most %d filters", NYA_DESKTOP_DIALOG_MAX_FILTERS);
    if (filter_count > 0 && filters == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "filter_count is set but filters is null");
    if (!_nya_desktop_shell.ready) return nya_error(NYA_ERROR_NOT_SUPPORTED, "the desktop shell is not available");

    _NYA_DesktopDialog* slot = _nya_desktop_dialog_acquire();
    if (slot == nullptr) return nya_error(NYA_ERROR_NOT_OK, "too many file dialogs open at once");

    *slot          = (_NYA_DesktopDialog){ .in_use = true, .callback = callback, .user = user };
    slot->has_location = default_location != nullptr;
    if (slot->has_location) (void)snprintf(slot->default_location, sizeof(slot->default_location), "%s", default_location);
    _nya_desktop_dialog_set_filters(slot, filters, filter_count);

    *out_slot = slot;
    return NYA_OK;
}

NYA_Error nya_desktop_open_files(const NYA_DesktopFileFilter* filters, u32 filter_count, NYA_ConstCString default_location, b8 allow_many,
                                 NYA_DesktopFileCallback callback, void* user) {
    _NYA_DesktopDialog* slot = nullptr;
    NYA_TRY(_nya_desktop_dialog_begin(filters, filter_count, default_location, callback, user, &slot));

    SDL_ShowOpenFileDialog(_nya_desktop_dialog_answer, slot, _nya_desktop_parent(), filter_count > 0 ? slot->sdl_filters : nullptr, (int)filter_count,
                           slot->has_location ? slot->default_location : nullptr, allow_many);
    return NYA_OK;
}

NYA_Error nya_desktop_save_file(const NYA_DesktopFileFilter* filters, u32 filter_count, NYA_ConstCString default_location,
                                NYA_DesktopFileCallback callback, void* user) {
    _NYA_DesktopDialog* slot = nullptr;
    NYA_TRY(_nya_desktop_dialog_begin(filters, filter_count, default_location, callback, user, &slot));

    SDL_ShowSaveFileDialog(_nya_desktop_dialog_answer, slot, _nya_desktop_parent(), filter_count > 0 ? slot->sdl_filters : nullptr, (int)filter_count,
                           slot->has_location ? slot->default_location : nullptr);
    return NYA_OK;
}

NYA_Error nya_desktop_open_folder(NYA_ConstCString default_location, b8 allow_many, NYA_DesktopFileCallback callback, void* user) {
    _NYA_DesktopDialog* slot = nullptr;
    NYA_TRY(_nya_desktop_dialog_begin(nullptr, 0, default_location, callback, user, &slot));

    SDL_ShowOpenFolderDialog(_nya_desktop_dialog_answer, slot, _nya_desktop_parent(), slot->has_location ? slot->default_location : nullptr, allow_many);
    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TRAY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_desktop_tray_create(const NYA_DesktopTrayIcon* icon, NYA_ConstCString tooltip, OUT NYA_DesktopTray** out_tray) {
    if (out_tray == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a tray needs somewhere to be written");
    if (!_nya_desktop_shell.ready) return nya_error(NYA_ERROR_NOT_SUPPORTED, "the desktop shell is not available");

    NYA_DesktopTray* tray = _nya_desktop_tray_acquire();
    if (tray == nullptr) return nya_error(NYA_ERROR_NOT_OK, "too many trays");

    *tray = (NYA_DesktopTray){ .in_use = true };

    // The icon surface owns its pixels: a copy taken row by row, so the caller's buffer may go once create returns.
    if (icon != nullptr && icon->rgba != nullptr && icon->width > 0 && icon->height > 0) {
        tray->icon = SDL_CreateSurface((int)icon->width, (int)icon->height, SDL_PIXELFORMAT_RGBA32);
        if (tray->icon == nullptr) {
            *tray = (NYA_DesktopTray){ 0 };
            return nya_error(NYA_ERROR_OUT_OF_MEMORY, "SDL_CreateSurface failed: %s", SDL_GetError());
        }
        const u8* source = (const u8*)icon->rgba;
        u8*       target = (u8*)tray->icon->pixels;
        for (u32 row = 0; row < icon->height; row++) {
            nya_memcpy(target + (u64)row * (u64)tray->icon->pitch, source + (u64)row * (u64)icon->width * 4U, (u64)icon->width * 4U);
        }
    }

    tray->handle = SDL_CreateTray(tray->icon, tooltip);
    if (tray->handle == nullptr) {
        if (tray->icon != nullptr) SDL_DestroySurface(tray->icon);
        *tray = (NYA_DesktopTray){ 0 };
        return nya_error(NYA_ERROR_NOT_OK, "SDL_CreateTray failed: %s", SDL_GetError());
    }

    tray->menu = SDL_CreateTrayMenu(tray->handle);
    if (tray->menu == nullptr) {
        SDL_DestroyTray(tray->handle);
        if (tray->icon != nullptr) SDL_DestroySurface(tray->icon);
        *tray = (NYA_DesktopTray){ 0 };
        return nya_error(NYA_ERROR_NOT_OK, "SDL_CreateTrayMenu failed: %s", SDL_GetError());
    }

    *out_tray = tray;
    return NYA_OK;
}

void nya_desktop_tray_destroy(NYA_DesktopTray* tray) {
    if (tray == nullptr || !tray->in_use) return;

    // SDL_DestroyTray frees the menu and every entry under it; the icon surface is ours to free.
    if (tray->handle != nullptr) SDL_DestroyTray(tray->handle);
    if (tray->icon != nullptr) SDL_DestroySurface(tray->icon);
    *tray = (NYA_DesktopTray){ 0 };
}

NYA_Error nya_desktop_tray_add_button(NYA_DesktopTray* tray, NYA_ConstCString label, NYA_DesktopTrayCallback callback, void* user) {
    if (tray == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no tray");
    return _nya_desktop_tray_entry_add(tray, label, SDL_TRAYENTRY_BUTTON, callback, user);
}

NYA_Error nya_desktop_tray_add_checkbox(NYA_DesktopTray* tray, NYA_ConstCString label, b8 checked, NYA_DesktopTrayCallback callback, void* user) {
    if (tray == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no tray");
    SDL_TrayEntryFlags flags = SDL_TRAYENTRY_CHECKBOX | (checked ? SDL_TRAYENTRY_CHECKED : 0U);
    return _nya_desktop_tray_entry_add(tray, label, flags, callback, user);
}

NYA_Error nya_desktop_tray_add_separator(NYA_DesktopTray* tray) {
    if (tray == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no tray");
    if (!_nya_desktop_shell.ready || tray->handle == nullptr) return nya_error(NYA_ERROR_NOT_SUPPORTED, "the desktop shell is not available");
    if (tray->entry_count >= NYA_DESKTOP_TRAY_MAX_ENTRIES) return nya_error(NYA_ERROR_NOT_OK, "the tray menu is full");

    // A null label is SDL's separator; it carries no callback, but it still takes a menu position.
    SDL_TrayEntry* entry = SDL_InsertTrayEntryAt(tray->menu, -1, nullptr, SDL_TRAYENTRY_BUTTON);
    if (entry == nullptr) return nya_error(NYA_ERROR_NOT_OK, "SDL_InsertTrayEntryAt failed: %s", SDL_GetError());

    tray->entries[tray->entry_count] = (_NYA_DesktopTrayEntry){ 0 };
    tray->entry_count++;
    return NYA_OK;
}

void nya_desktop_tray_update(void) {
    if (!_nya_desktop_shell.ready) return;
    SDL_UpdateTrays();
}

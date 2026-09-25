/**
 * The desktop shell with no one at the keyboard: the calls a program makes are all reachable, reject bad
 * arguments up front, and — on a build farm with no display — answer NYA_ERROR_NOT_SUPPORTED rather than
 * crashing or blocking. Where a display does exist the tray is built and torn down, which is non-blocking;
 * the modal message boxes and file dialogs are left for a human to drive, so the suite never hangs on one.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_error.h"
#include "SDL3/SDL_init.h"

static void on_pick(void* user, const NYA_DesktopFileResult* result) {
    (void)user;
    (void)result;
}

static void on_click(void* user) {
    (void)user;
}

s32 main(void) {
    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    // Argument checks answer before the display is ever consulted, so they run the same with or without one
    // and never pop a window.
    nya_assert(nya_desktop_message_show(NYA_DESKTOP_MESSAGE_INFO, nullptr, "body").kind == NYA_ERROR_INVALID_ARGUMENT, "a box needs a title");
    nya_assert(nya_desktop_message_show(NYA_DESKTOP_MESSAGE_INFO, "title", nullptr).kind == NYA_ERROR_INVALID_ARGUMENT, "a box needs a message");

    u32 chosen = 0;
    nya_assert(nya_desktop_message_prompt(NYA_DESKTOP_MESSAGE_INFO, "t", "m", nullptr, 0, &chosen).kind == NYA_ERROR_INVALID_ARGUMENT,
               "a prompt needs at least one button");
    NYA_ConstCString too_many[] = { "a", "b", "c", "d", "e" };
    nya_assert(nya_desktop_message_prompt(NYA_DESKTOP_MESSAGE_INFO, "t", "m", too_many, 5, &chosen).kind == NYA_ERROR_INVALID_ARGUMENT,
               "past NYA_DESKTOP_MESSAGE_MAX_BUTTONS is refused");

    nya_assert(nya_desktop_open_files(nullptr, 0, nullptr, false, nullptr, nullptr).kind == NYA_ERROR_INVALID_ARGUMENT, "a dialog needs a callback");

    // init either brings the desktop up or says there is no display; both are a clean answer.
    NYA_Error init = nya_desktop_shell_init();
    nya_assert(init.ok || init.kind == NYA_ERROR_NOT_SUPPORTED, "init succeeds or reports no display, got %s", NYA_ERRORKIND_NAME_MAP[init.kind]);
    nya_assert(nya_desktop_shell_ready() == init.ok, "ready tracks whether init took");

    if (!nya_desktop_shell_ready()) {
        // The build farm's path: every entry point answers NOT_SUPPORTED, nothing blocks, nothing crashes.
        NYA_DesktopFileFilter filters[] = { { "Text", "txt" } };
        nya_assert(nya_desktop_open_files(filters, 1, nullptr, true, on_pick, nullptr).kind == NYA_ERROR_NOT_SUPPORTED, "no display, no open dialog");
        nya_assert(nya_desktop_save_file(filters, 1, "out.txt", on_pick, nullptr).kind == NYA_ERROR_NOT_SUPPORTED, "no display, no save dialog");
        nya_assert(nya_desktop_open_folder(nullptr, false, on_pick, nullptr).kind == NYA_ERROR_NOT_SUPPORTED, "no display, no folder dialog");
        nya_assert(nya_desktop_message_show(NYA_DESKTOP_MESSAGE_ERROR, "t", "m").kind == NYA_ERROR_NOT_SUPPORTED, "no display, no message box");
        NYA_ConstCString ok_button[] = { "OK" };
        nya_assert(nya_desktop_message_prompt(NYA_DESKTOP_MESSAGE_INFO, "t", "m", ok_button, 1, &chosen).kind == NYA_ERROR_NOT_SUPPORTED,
                   "no display, no prompt");
    }

    // The tray is non-blocking, so it is safe to attempt in either state: it either builds or fails cleanly.
    NYA_DesktopTray* tray   = nullptr;
    NYA_Error        create = nya_desktop_tray_create(nullptr, "test", &tray);
    if (create.ok) {
        nya_assert(tray != nullptr, "a successful create hands back a tray");
        nya_assert(nya_desktop_tray_add_button(tray, "Open", on_click, nullptr).ok, "a button goes onto the menu");
        nya_assert(nya_desktop_tray_add_checkbox(tray, "Mute", true, on_click, nullptr).ok, "a checkbox goes onto the menu");
        nya_assert(nya_desktop_tray_add_separator(tray).ok, "a separator goes onto the menu");
        nya_desktop_tray_update();
        nya_desktop_tray_destroy(tray);
    } else {
        nya_assert(tray == nullptr, "a failed create hands back nothing");
        nya_assert(create.kind == NYA_ERROR_NOT_SUPPORTED || create.kind == NYA_ERROR_NOT_OK, "and a clean error: %s", NYA_ERRORKIND_NAME_MAP[create.kind]);
        nya_assert(nya_desktop_tray_add_button(nullptr, "x", on_click, nullptr).kind == NYA_ERROR_INVALID_ARGUMENT, "no tray to add a button to");
        nya_desktop_tray_update();
    }

    // Destroying a null tray is a no-op, and deinit is safe whether or not init took, and twice over.
    nya_desktop_tray_destroy(nullptr);
    nya_desktop_shell_deinit();
    nya_desktop_shell_deinit();

    nya_log_info("desktop_shell test passed (ready=%s).", init.ok ? "true" : "false");
    return 0;
}

#include "gnyame/gnyame.h"
#include "generated/assets.h"

NYA_WindowHandle GNY_WINDOW_MAIN = NYA_WINDOW_HANDLE_NONE;

void gny_window_main_create(void) {
    GNY_WINDOW_MAIN = nya_window_create(GNY_WINDOW_MAIN_TITLE, GNY_WINDOW_MAIN_WIDTH, GNY_WINDOW_MAIN_HEIGHT, GNY_WINDOW_MAIN_FLAGS);
    nya_assert(nya_window_is_valid(GNY_WINDOW_MAIN), "Failed to create the main window.");

    // No minimum size on purpose.
    //
    // Calling SDL_SetWindowMinimumSize right after creating the window makes at least one Wayland
    // compositor collapse the surface to exactly that minimum while maximizing the frame around it:
    // measured here as a 320x240 surface inside a full tile at 2304,0, which renders as a small
    // image floating in a large empty window. Without the call the window stays at the size it was
    // created with and never churns.
    //
    // If a floor is genuinely needed, clamp in NYA_EVENT_WINDOW_RESIZED rather than asking the
    // window manager to enforce it.

    // Read, handed to SDL, released. Nothing stays resident: SDL keeps its own converted copy.
    NYA_Error icon = nya_asset_set_window_icon(GNY_WINDOW_MAIN, NYA_ASSET_ICON_ICON_BMP);

    // Not fatal. A Wayland compositor without xdg_toplevel_icon_v1 refuses this, and a default icon
    // is a far better outcome than refusing to open the window.
    if (!icon.ok) nya_log_warn("%s", (NYA_ConstCString)icon.message);

    /*
     * The background and the title screen, and nothing else.
     *
     * The game and the HUD are pushed by "start" and popped again by "main menu", so the world does
     * not exist until it is asked for. Push order is draw order and there is no depth test, so the
     * background goes first; it is also the layer that is never popped, which is why the music lives
     * on it rather than on the game.
     *
     * Event order is the reverse of this, so the menu on top sees input first and can be modal
     * without anything underneath having to check whether a menu is open.
     */
    nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_BACKGROUND);
    nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU);
}

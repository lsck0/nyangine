#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * One action's name and up to two default keys, in one row.
 *
 * A table rather than twenty-odd lines of paired calls, because the two things that must not drift
 * apart — the name a settings file writes and the key it defaults to — are then physically adjacent.
 * */
typedef struct {
    NYA_InputAction  action;
    NYA_ConstCString name;
    NYA_Keycode      primary;

    /** NYA_KEY_UNKNOWN for an action with one key. Both slots are what NYA_INPUT_BINDINGS_PER_ACTION allows. */
    NYA_Keycode alternative;
} GNY_ActionDefault;

NYA_INTERNAL const GNY_ActionDefault _GNY_ACTION_DEFAULTS[] = {
    /*
     * ── The engine's menu actions ──
     *
     * Bound here rather than by the engine, which ships them unbound on purpose: which keys drive a
     * menu is a game's decision, and an engine that guessed would be overriding it every startup.
     */
    { .action = NYA_INPUT_ACTION_CONFIRM, .name = "confirm", .primary = NYA_KEY_RETURN, .alternative = NYA_KEY_SPACE },
    { .action = NYA_INPUT_ACTION_CANCEL,  .name = "cancel",  .primary = NYA_KEY_ESCAPE                              },
    { .action = NYA_INPUT_ACTION_PAUSE,   .name = "pause",   .primary = NYA_KEY_ESCAPE                              },
    { .action = NYA_INPUT_ACTION_UP,      .name = "menu_up", .primary = NYA_KEY_UP,     .alternative = NYA_KEY_W     },
    { .action = NYA_INPUT_ACTION_DOWN,  .name = "menu_down",  .primary = NYA_KEY_DOWN,  .alternative = NYA_KEY_S },
    { .action = NYA_INPUT_ACTION_LEFT,  .name = "menu_left",  .primary = NYA_KEY_LEFT,  .alternative = NYA_KEY_A },
    { .action = NYA_INPUT_ACTION_RIGHT, .name = "menu_right", .primary = NYA_KEY_RIGHT, .alternative = NYA_KEY_D },

    /*
     * ── The game's own ──
     *
     * Movement duplicates the menu's keys and is deliberately a separate action anyway. See
     * actions.h: sharing one would make "rebind walking without rebinding the menu" unsayable.
     */
    { .action = GNY_ACTION_MOVE_LEFT,  .name = "move_left",  .primary = NYA_KEY_LEFT,  .alternative = NYA_KEY_A },
    { .action = GNY_ACTION_MOVE_RIGHT, .name = "move_right", .primary = NYA_KEY_RIGHT, .alternative = NYA_KEY_D },
    { .action = GNY_ACTION_MOVE_UP,    .name = "move_up",    .primary = NYA_KEY_UP,    .alternative = NYA_KEY_W },
    { .action = GNY_ACTION_MOVE_DOWN,  .name = "move_down",  .primary = NYA_KEY_DOWN,  .alternative = NYA_KEY_S },

    { .action = GNY_ACTION_SPAWN_BURST,          .name = "spawn_burst",          .primary = NYA_KEY_SPACE },
    { .action = GNY_ACTION_CLEAR_BOXES,          .name = "clear_boxes",          .primary = NYA_KEY_C     },
    { .action = GNY_ACTION_REGENERATE_TERRAIN,   .name = "regenerate_terrain",   .primary = NYA_KEY_R     },
    { .action = GNY_ACTION_TOGGLE_PHYSICS,       .name = "toggle_physics",       .primary = NYA_KEY_P     },
    { .action = GNY_ACTION_TOGGLE_BLOOM,         .name = "toggle_bloom",         .primary = NYA_KEY_B     },
    { .action = GNY_ACTION_TOGGLE_MUSIC,         .name = "toggle_music",         .primary = NYA_KEY_M     },
    { .action = GNY_ACTION_TOGGLE_TRACE,         .name = "toggle_trace",         .primary = NYA_KEY_T     },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_actions_init(void) {
    for (u32 i = 0; i < nya_carray_length(_GNY_ACTION_DEFAULTS); i++) {
        const GNY_ActionDefault* entry = &_GNY_ACTION_DEFAULTS[i];

        // The engine already named its own seven, and naming two actions the same thing is refused —
        // so these rows deliberately use different names for the menu directions than the engine's
        // "up"/"down"/"left"/"right", which is also what a player reading the file wants to see.
        nya_input_action_name_set(entry->action, entry->name);

        // Rebind rather than bind: this runs again on a hot reload, and binding appends.
        nya_input_action_rebind(entry->action, entry->primary);
        if (entry->alternative != NYA_KEY_UNKNOWN) nya_input_action_bind(entry->action, entry->alternative);
    }

    // Music quieter than effects by default, which is a taste the player can now override — and the
    // override survives, which is the whole point of doing it here rather than in constants.h.
    nya_settings_volume_set(NYA_VOLUME_CHANNEL_MUSIC, GNY_MUSIC_VOLUME_DEFAULT);

    /*
     * Over the top of the defaults, and not fatal when there is nothing there.
     *
     * NOT_FOUND is the first run, which is the common case and wants exactly what just happened. Any
     * other failure is a settings file that exists and did not parse, which is worth saying out loud
     * — the player is about to lose their bindings and should be able to find out why.
     */
    NYA_Error loaded = nya_settings_load();
    if (!loaded.ok && loaded.kind != NYA_ERROR_NOT_FOUND) {
        u8 message[256];
        (void)nya_error_format(&loaded, message, sizeof(message));
        nya_warn("Could not read the settings file, continuing with defaults: %s", (NYA_CString)message);
    }
}

void gny_actions_deinit(void) {
    NYA_Error saved = nya_settings_save();
    if (saved.ok) return;

    u8 message[256];
    (void)nya_error_format(&saved, message, sizeof(message));
    nya_warn("Could not write the settings file: %s", (NYA_CString)message);
}

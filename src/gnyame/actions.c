/**
 * @file actions.c
 *
 * The game's input actions: names, default bindings, and the settings file that overrides them.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * One action's name and up to two default keys, in one row.
 * */
typedef struct {
    NYA_InputAction  action;
    NYA_ConstCString name;
    NYA_Keycode      primary;

    /** NYA_KEY_UNKNOWN for an action with one key. */
    NYA_Keycode alternative;

    /** Modifiers both keys need held. NYA_KEYMOD_NONE for the ordinary case of a bare key. */
    NYA_KeyModFlag modifiers;

    /** A gamepad button and a stick direction, zeroed for an action the pad does not reach. */
    NYA_InputBinding button;
    NYA_InputBinding stick;
} GNY_ActionDefault;

#define GNY_PAD(pad_button) ((NYA_InputBinding){ .kind = NYA_INPUT_BINDING_GAMEPAD_BUTTON, .button = (pad_button) })

/** Past half deflection, so a resting stick with drift does not scroll a menu. */
#define GNY_STICK(pad_axis, sign) ((NYA_InputBinding){ .kind = NYA_INPUT_BINDING_GAMEPAD_AXIS, .axis = (pad_axis), .axis_threshold = (sign) * 0.5F })

NYA_INTERNAL const GNY_ActionDefault _GNY_ACTION_DEFAULTS[] = {
    /*
     * The engine's menu actions
     *
     * The engine ships them unbound, since which keys drive a menu is the game's decision.
     */
    { .action = NYA_INPUT_ACTION_CONFIRM, .name = "confirm", .primary = NYA_KEY_RETURN, .alternative = NYA_KEY_SPACE, .button = GNY_PAD(NYA_GAMEPAD_BUTTON_SOUTH) },
    { .action = NYA_INPUT_ACTION_CANCEL,  .name = "cancel",  .primary = NYA_KEY_ESCAPE, .button = GNY_PAD(NYA_GAMEPAD_BUTTON_EAST)  },
    { .action = NYA_INPUT_ACTION_PAUSE,   .name = "pause",   .primary = NYA_KEY_ESCAPE, .button = GNY_PAD(NYA_GAMEPAD_BUTTON_START) },

    { .action = NYA_INPUT_ACTION_UP,    .name = "menu_up",    .primary = NYA_KEY_UP,    .alternative = NYA_KEY_W,
      .button = GNY_PAD(NYA_GAMEPAD_BUTTON_DPAD_UP),    .stick = GNY_STICK(NYA_GAMEPAD_AXIS_LEFT_Y, -1.0F) },
    { .action = NYA_INPUT_ACTION_DOWN,  .name = "menu_down",  .primary = NYA_KEY_DOWN,  .alternative = NYA_KEY_S,
      .button = GNY_PAD(NYA_GAMEPAD_BUTTON_DPAD_DOWN),  .stick = GNY_STICK(NYA_GAMEPAD_AXIS_LEFT_Y, 1.0F)  },
    { .action = NYA_INPUT_ACTION_LEFT,  .name = "menu_left",  .primary = NYA_KEY_LEFT,  .alternative = NYA_KEY_A,
      .button = GNY_PAD(NYA_GAMEPAD_BUTTON_DPAD_LEFT),  .stick = GNY_STICK(NYA_GAMEPAD_AXIS_LEFT_X, -1.0F) },
    { .action = NYA_INPUT_ACTION_RIGHT, .name = "menu_right", .primary = NYA_KEY_RIGHT, .alternative = NYA_KEY_D,
      .button = GNY_PAD(NYA_GAMEPAD_BUTTON_DPAD_RIGHT), .stick = GNY_STICK(NYA_GAMEPAD_AXIS_LEFT_X, 1.0F)  },

    /*
     * The game's own
     *
     * Movement uses the menu's keys but is a separate action, so walking can be rebound without the menu.
     * See actions.h.
     */
    { .action = GNY_ACTION_MOVE_LEFT,  .name = "move_left",  .primary = NYA_KEY_LEFT,  .alternative = NYA_KEY_A,
      .button = GNY_PAD(NYA_GAMEPAD_BUTTON_DPAD_LEFT),  .stick = GNY_STICK(NYA_GAMEPAD_AXIS_LEFT_X, -1.0F) },
    { .action = GNY_ACTION_MOVE_RIGHT, .name = "move_right", .primary = NYA_KEY_RIGHT, .alternative = NYA_KEY_D,
      .button = GNY_PAD(NYA_GAMEPAD_BUTTON_DPAD_RIGHT), .stick = GNY_STICK(NYA_GAMEPAD_AXIS_LEFT_X, 1.0F)  },
    { .action = GNY_ACTION_MOVE_UP,    .name = "move_up",    .primary = NYA_KEY_UP,    .alternative = NYA_KEY_W,
      .button = GNY_PAD(NYA_GAMEPAD_BUTTON_DPAD_UP),    .stick = GNY_STICK(NYA_GAMEPAD_AXIS_LEFT_Y, -1.0F) },
    { .action = GNY_ACTION_MOVE_DOWN,  .name = "move_down",  .primary = NYA_KEY_DOWN,  .alternative = NYA_KEY_S,
      .button = GNY_PAD(NYA_GAMEPAD_BUTTON_DPAD_DOWN),  .stick = GNY_STICK(NYA_GAMEPAD_AXIS_LEFT_Y, 1.0F)  },

    { .action = GNY_ACTION_SPAWN_BURST,          .name = "spawn_burst",          .primary = NYA_KEY_SPACE },
    { .action = GNY_ACTION_CLEAR_BOXES,          .name = "clear_boxes",          .primary = NYA_KEY_C     },
    { .action = GNY_ACTION_REGENERATE_TERRAIN,   .name = "regenerate_terrain",   .primary = NYA_KEY_R     },
    { .action = GNY_ACTION_TOGGLE_PHYSICS,       .name = "toggle_physics",       .primary = NYA_KEY_P     },
    { .action = GNY_ACTION_TOGGLE_BLOOM,         .name = "toggle_bloom",         .primary = NYA_KEY_B     },
    { .action = GNY_ACTION_TOGGLE_MUSIC,         .name = "toggle_music",         .primary = NYA_KEY_M     },
    { .action = GNY_ACTION_TOGGLE_OVERLAY,       .name = "toggle_overlay",        .primary = NYA_KEY_T     },
    { .action = GNY_ACTION_CYCLE_OVERLAY_PAGE,   .name = "cycle_overlay_page",   .primary = NYA_KEY_Y     },
    { .action = GNY_ACTION_CYCLE_TRACE_SORT,     .name = "cycle_trace_sort",     .primary = NYA_KEY_U     },
    { .action = GNY_ACTION_TRACE_REPORT,         .name = "trace_report",         .primary = NYA_KEY_L     },
    { .action = GNY_ACTION_TRACE_CAPTURE,        .name = "trace_capture",        .primary = NYA_KEY_K     },
    { .action = GNY_ACTION_TOGGLE_INK,           .name = "toggle_ink",           .primary = NYA_KEY_1     },
    { .action = GNY_ACTION_TOGGLE_OCCLUSION,     .name = "toggle_occlusion",     .primary = NYA_KEY_2     },
    { .action = GNY_ACTION_TOGGLE_ANTIALIAS,     .name = "toggle_antialias",     .primary = NYA_KEY_3     },
    { .action = GNY_ACTION_TOGGLE_GRADE,         .name = "toggle_grade",         .primary = NYA_KEY_4     },
    { .action = GNY_ACTION_CYCLE_DEBUG_VIEW,     .name = "cycle_debug_view",     .primary = NYA_KEY_V     },
    { .action = GNY_ACTION_CYCLE_FOCUS,          .name = "cycle_focus",          .primary = NYA_KEY_5     },
    { .action = GNY_ACTION_TOGGLE_SPEED_LINES,   .name = "toggle_speed_lines",   .primary = NYA_KEY_6     },
    { .action = GNY_ACTION_TOGGLE_DECALS,        .name = "toggle_decals",        .primary = NYA_KEY_7     },
    { .action = GNY_ACTION_TOGGLE_HDR,           .name = "toggle_hdr",           .primary = NYA_KEY_8     },
    { .action = GNY_ACTION_DROP_THROUGH,         .name = "drop_through",         .primary = NYA_KEY_G     },
    { .action = GNY_ACTION_FREEZE_ANIMATION,     .name = "freeze_animation",     .primary = NYA_KEY_F     },

    // Ctrl and Shift together, because this one ends the process: every other row here is a bare key, so
    // a bare key would be pressed by accident eventually.
    { .action = GNY_ACTION_TEST_CRASH, .name = "test_crash", .primary = NYA_KEY_F12, .modifiers = NYA_KEYMOD_CTRL | NYA_KEYMOD_SHIFT },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_actions_init(void) {
    for (u32 i = 0; i < nya_carray_length(_GNY_ACTION_DEFAULTS); i++) {
        const GNY_ActionDefault* entry = &_GNY_ACTION_DEFAULTS[i];

        // the engine already uses "up"/"down"/"left"/"right" and duplicate names are refused, so these rows
        // use different names for the menu directions.
        nya_input_action_name_set(entry->action, entry->name);

        // Rebind rather than bind: this runs again on a hot reload, and binding appends.
        nya_input_action_rebind(entry->action, entry->primary, entry->modifiers);
        if (entry->alternative != NYA_KEY_UNKNOWN) nya_input_action_bind(entry->action, entry->alternative, entry->modifiers);

        // the settings file below replaces keys only, so these stay whatever the player rebinds.
        if (entry->button.kind != NYA_INPUT_BINDING_NONE) nya_input_action_bind_button(entry->action, entry->button.button);
        if (entry->stick.kind != NYA_INPUT_BINDING_NONE) nya_input_action_bind_axis(entry->action, entry->stick.axis, entry->stick.axis_threshold);
    }

    // music quieter than effects by default. Set here rather than in constants.h so a player override
    // persists.
    nya_settings_volume_set(NYA_VOLUME_CHANNEL_MUSIC, GNY_MUSIC_VOLUME_DEFAULT);

    /*
     * Over the top of the defaults, and not fatal when there is nothing there.
     */
    NYA_Error loaded = nya_settings_load();
    if (!loaded.ok && loaded.kind != NYA_ERROR_NOT_FOUND) {
        u8 message[256];
        (void)nya_error_format(&loaded, message, sizeof(message));
        nya_log_warn("Could not read the settings file, continuing with defaults: %s", (NYA_CString)message);
    }
}

void gny_actions_deinit(void) {
    NYA_Error saved = nya_settings_save();
    if (saved.ok) return;

    u8 message[256];
    (void)nya_error_format(&saved, message, sizeof(message));
    nya_log_warn("Could not write the settings file: %s", (NYA_CString)message);
}

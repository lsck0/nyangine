#include "SDL3/SDL_keyboard.h"

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * What each volume channel is called in the settings file.
 *
 * Names rather than array indices, for the same reason bindings are keyed by action name: an index
 * is unreadable to the player editing the file, and it changes meaning the day a channel is inserted
 * in the middle of the enum. A name that no longer exists is simply ignored on load.
 * */
NYA_INTERNAL NYA_ConstCString _NYA_VOLUME_CHANNEL_NAMES[NYA_VOLUME_CHANNEL_COUNT] = {
    [NYA_VOLUME_CHANNEL_MASTER] = "master",
    [NYA_VOLUME_CHANNEL_SOUND]  = "sound",
    [NYA_VOLUME_CHANNEL_MUSIC]  = "music",
    [NYA_VOLUME_CHANNEL_VOICE]  = "voice",
    [NYA_VOLUME_CHANNEL_UI]     = "ui",
};

/**
 * A binding as one editable string: `"Space"`, `"Ctrl+S"`, `"Shift+Left Alt+F1"`.
 *
 * Names rather than numbers throughout, and SDL's own names specifically, because SDL_GetKeyName and
 * SDL_GetKeyFromName round-trip — which nya_keycode_to_cstring does not, since that one returns what
 * a key *types* (`" "` for space, `"\t"` for tab) rather than what it is called.
 * */
NYA_INTERNAL NYA_String* _nya_settings_binding_to_string(NYA_Arena* arena, NYA_InputBinding binding);

/** The inverse. Leaves `out_binding` untouched and returns false when the string names no key. */
NYA_INTERNAL b8 _nya_settings_binding_from_string(NYA_ConstCString text, OUT NYA_InputBinding* out_binding);

/** Reads whatever a value holds as an f32, across every numeric type a format could give it back as. */
NYA_INTERNAL b8 _nya_settings_value_as_f32(const NYA_Value* value, OUT f32* out_number);

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

void nya_system_settings_init(void) {
    /*
     * Defaults only. Loading is nya_settings_load, and the game calls it.
     *
     * Not because loading is optional, but because of *when* it can happen: a settings file addresses
     * bindings by action name, and a game's actions are named after nya_app_init returns. Loading
     * here would find nothing to attach the game's bindings to and would drop them silently — and
     * saving on the way out would then write that emptied file back over the player's.
     *
     * So both ends are explicit, and neither can quietly destroy a settings file it did not
     * understand. See core_settings.h.
     */
    nya_settings_reset();

    nya_info("Settings system initialized.");
}

void nya_system_settings_deinit(void) {
    // Nothing is owned: the volumes are floats and the bindings are a fixed array inside NYA_App.
    // Saving is nya_settings_save and is the game's call, for the reason in nya_system_settings_init.
    nya_info("Settings system deinitialized.");
}

/*
 * ─────────────────────────────────────────────────────────
 * PERSISTENCE
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_settings_save(void) {
    NYA_Arena* scratch = nya_arena_create(.name = "settings_save_scratch");
    defer nya_arena_destroy(scratch);

    // PRETTY because this is the one file a player is invited to open. The native format's checksum
    // is over the object tree rather than the bytes, so reformatting it by hand does not break it.
    return nya_save_write(NYA_SETTINGS_FILE, nya_settings_to_object(scratch), NYA_SERDE_PRETTY);
}

NYA_Error nya_settings_load(void) {
    NYA_Arena* scratch = nya_arena_create(.name = "settings_load_scratch");
    defer nya_arena_destroy(scratch);

    NYA_Object* object = nullptr;
    NYA_TRY(nya_save_read(scratch, NYA_SETTINGS_FILE, NYA_SERDE_NONE, &object));

    nya_settings_from_object(object);

    return NYA_OK;
}

NYA_Object* nya_settings_to_object(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    NYA_Object* root = nya_object_create(arena);

    nya_object_set(root, NYA_SAVE_VERSION_KEY, (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = NYA_SETTINGS_VERSION });

    NYA_Object* volumes = nya_object_create(arena);
    for (u32 channel = 0; channel < NYA_VOLUME_CHANNEL_COUNT; channel++) {
        nya_object_set(volumes, (NYA_CString)_NYA_VOLUME_CHANNEL_NAMES[channel], (NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = nya_settings_volume(channel) });
    }

    nya_object_set(root, "volumes", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *volumes });

    NYA_Object* bindings = nya_object_create(arena);
    for (u32 action = 1; action < NYA_INPUT_ACTION_MAX; action++) {
        /*
         * Unnamed actions are skipped rather than written under their number.
         *
         * A number is not a stable name: a game that inserts an action in the middle of its enum
         * renumbers everything after it, and a settings file keyed by number would then hand every
         * one of those players the wrong keys. Skipping loses the binding for an action the game
         * never named, which is a smaller and much more findable problem.
         */
        NYA_ConstCString name = nya_input_action_name((NYA_InputAction)action);
        if (name == nullptr) continue;
        if (!nya_input_action_bound((NYA_InputAction)action)) continue;

        NYA_ArrayᐸNYA_Valueᐳ* keys = nya_array_create(arena, NYA_Value);

        for (u32 slot = 0; slot < NYA_INPUT_BINDINGS_PER_ACTION; slot++) {
            NYA_InputBinding binding = nya_input_action_get((NYA_InputAction)action, slot);
            if (binding.key == NYA_KEY_UNKNOWN) continue;

            NYA_String* text = _nya_settings_binding_to_string(arena, binding);
            if (text == nullptr) continue;

            nya_array_push_back(keys, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = nya_string_to_cstring(arena, text) }));
        }

        nya_object_set(bindings, (NYA_CString)name, (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *keys });
    }

    nya_object_set(root, "bindings", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *bindings });

    return root;
}

void nya_settings_from_object(const NYA_Object* object) {
    if (object == nullptr) return;

    /*
     * The version is read and, today, not acted on.
     *
     * There is exactly one version, so there is nothing to migrate between — but reading it here is
     * what makes the *next* version's migration possible, and a file written by a future build is
     * worth a warning rather than a silent partial load.
     */
    u32 version = nya_save_version(object);
    if (version > NYA_SETTINGS_VERSION) {
        nya_warn("Settings file is version " FMTu32 ", newer than the " FMTu32 " this build understands; loading what it can.", version,
                 (u32)NYA_SETTINGS_VERSION);
    }

    NYA_Value* volumes = nya_object_get(object, "volumes");
    if (volumes != nullptr && volumes->type == NYA_TYPE_OBJECT) {
        for (u32 channel = 0; channel < NYA_VOLUME_CHANNEL_COUNT; channel++) {
            NYA_Value* value = nya_object_get(&volumes->as_object, (NYA_CString)_NYA_VOLUME_CHANNEL_NAMES[channel]);
            if (value == nullptr) continue;

            f32 volume = 0.0F;
            if (!_nya_settings_value_as_f32(value, &volume)) {
                nya_warn("Settings volume '%s' is not a number; leaving it alone.", _NYA_VOLUME_CHANNEL_NAMES[channel]);
                continue;
            }

            // nya_settings_volume_set clamps, which is the validation: a hand-edited 11 becomes 1
            // rather than a mix that clips.
            nya_settings_volume_set((NYA_VolumeChannel)channel, volume);
        }
    }

    NYA_Value* bindings = nya_object_get(object, "bindings");
    if (bindings == nullptr || bindings->type != NYA_TYPE_OBJECT) return;

    nya_dict_foreach_key (&bindings->as_object, key_slot) {
        NYA_ConstCString name   = *key_slot;
        NYA_InputAction  action = nya_input_action_from_name(name);
        if (action == NYA_INPUT_ACTION_NONE) {
            // An action this build does not have. Normal when a settings file outlives a rename, and
            // exactly what skipping unnamed actions on write is meant to keep rare.
            nya_warn("Settings file binds '%s', which is not an action in this build; ignoring it.", name);
            continue;
        }

        NYA_Value* keys = nya_object_get(&bindings->as_object, (NYA_CString)name);
        if (keys == nullptr || keys->type != NYA_TYPE_ARRAY) continue;

        /*
         * Cleared before the first slot is written, and only once the file has actually offered
         * something for this action.
         *
         * Replacing rather than adding, because nya_input_action_bind appends and a file loaded
         * twice would otherwise fill both slots with the same key. Clearing lazily, because an
         * action the file mentions with an empty list should keep its defaults rather than end up
         * unbound — an empty list is much more likely to be a hand-editing accident than an
         * instruction.
         */
        b8 cleared = false;
        u32 slot   = 0;

        nya_array_foreach (&keys->as_array, key) {
            if (key->type != NYA_TYPE_STRING) continue;
            if (slot >= NYA_INPUT_BINDINGS_PER_ACTION) break;

            NYA_InputBinding binding = { 0 };
            if (!_nya_settings_binding_from_string(key->as_string, &binding)) {
                nya_warn("Settings file binds '%s' to '%s', which names no key; ignoring it.", name, key->as_string);
                continue;
            }

            if (!cleared) {
                nya_input_action_unbind(action);
                cleared = true;
            }

            nya_input_action_set(action, slot++, binding.key, binding.modifiers);
        }
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * SETTINGS FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_SettingsSystem* nya_settings(void) {
    return &nya_app_get()->settings_system;
}

f32 nya_settings_volume(NYA_VolumeChannel channel) {
    nya_assert(channel < NYA_VOLUME_CHANNEL_COUNT, "Unknown volume channel %d.", (int)channel);

    return nya_settings()->volumes[channel];
}

void nya_settings_volume_set(NYA_VolumeChannel channel, f32 volume) {
    nya_assert(channel < NYA_VOLUME_CHANNEL_COUNT, "Unknown volume channel %d.", (int)channel);

    nya_settings()->volumes[channel] = nya_clamp(volume, 0.0F, 1.0F);
}

f32 nya_settings_volume_effective(NYA_VolumeChannel channel) {
    nya_assert(channel < NYA_VOLUME_CHANNEL_COUNT, "Unknown volume channel %d.", (int)channel);

    if (channel == NYA_VOLUME_CHANNEL_MASTER) return nya_settings()->volumes[NYA_VOLUME_CHANNEL_MASTER];

    return nya_settings()->volumes[NYA_VOLUME_CHANNEL_MASTER] * nya_settings()->volumes[channel];
}

void nya_settings_reset(void) {
    NYA_SettingsSystem* settings = nya_settings();

    for (u32 channel = 0; channel < NYA_VOLUME_CHANNEL_COUNT; channel++) settings->volumes[channel] = 1.0F;

    // NYA_KEY_UNKNOWN is the unbound marker, and it is zero, so this clears the whole table.
    nya_memset(settings->bindings, 0, sizeof(settings->bindings));
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The modifiers a binding string may name, in the order they are written.
 *
 * Only the side-agnostic ones, and only the chording ones. Writing `Ctrl` rather than `Left Ctrl` is
 * what a player expects to see and to type, and it is also what the matcher actually tests — a
 * binding asking for NYA_KEYMOD_CTRL is satisfied by either physical key. The lock keys are state
 * rather than chords and are never part of a binding, so they have no spelling here.
 * */
NYA_INTERNAL const struct {
    NYA_ConstCString name;
    NYA_KeyModFlag   flag;
} _NYA_SETTINGS_MODIFIER_NAMES[] = {
    { .name = "Ctrl",  .flag = NYA_KEYMOD_CTRL  },
    { .name = "Shift", .flag = NYA_KEYMOD_SHIFT },
    { .name = "Alt",   .flag = NYA_KEYMOD_ALT   },
    { .name = "Gui",   .flag = NYA_KEYMOD_GUI   },
};

NYA_String* _nya_settings_binding_to_string(NYA_Arena* arena, NYA_InputBinding binding) {
    NYA_ConstCString key_name = SDL_GetKeyName((SDL_Keycode)binding.key);

    // SDL answers "" rather than null for a keycode it has no name for. Either way there is nothing
    // to write that could be read back, so the binding is dropped rather than written unloadable.
    if (key_name == nullptr || key_name[0] == '\0') return nullptr;

    NYA_String* text = nya_string_create(arena);

    for (u32 i = 0; i < nya_carray_length(_NYA_SETTINGS_MODIFIER_NAMES); i++) {
        // Any of the flag's bits, not all of them: NYA_KEYMOD_CTRL is both control keys, and a
        // binding holding only the left one still reads as Ctrl.
        if ((binding.modifiers & _NYA_SETTINGS_MODIFIER_NAMES[i].flag) == 0) continue;

        nya_string_extend(text, _NYA_SETTINGS_MODIFIER_NAMES[i].name);
        nya_string_push_back(text, '+');
    }

    nya_string_extend(text, key_name);

    return text;
}

b8 _nya_settings_binding_from_string(NYA_ConstCString text, OUT NYA_InputBinding* out_binding) {
    nya_assert(out_binding != nullptr);

    if (text == nullptr || text[0] == '\0') return false;

    NYA_Arena* scratch = nya_arena_create(.name = "settings_binding_parse");
    defer nya_arena_destroy(scratch);

    NYA_KeyModFlag modifiers = NYA_KEYMOD_NONE;

    /*
     * Scanned from the front, one `Name+` prefix at a time, rather than split on every `+`.
     *
     * Because `+` is also a key. Splitting `Ctrl++` on separators gives three empty-ish pieces and
     * loses the key entirely; consuming known modifier prefixes leaves whatever is left as the key
     * name, and what is left there is exactly `+`.
     */
    NYA_ConstCString cursor = text;

    for (b8 consumed = true; consumed;) {
        consumed = false;

        for (u32 i = 0; i < nya_carray_length(_NYA_SETTINGS_MODIFIER_NAMES); i++) {
            NYA_String* prefix = nya_string_sprintf(scratch, "%s+", _NYA_SETTINGS_MODIFIER_NAMES[i].name);

            if (!nya_string_starts_with(cursor, nya_string_to_cstring(scratch, prefix))) continue;

            modifiers |= _NYA_SETTINGS_MODIFIER_NAMES[i].flag;
            cursor    += prefix->length;
            consumed   = true;
            break;
        }
    }

    // Case sensitive, deliberately: this is SDL's own spelling round-tripping through SDL's own
    // lookup, and accepting variants would mean writing one thing and accepting another.
    SDL_Keycode key = SDL_GetKeyFromName(cursor);
    if (key == SDLK_UNKNOWN) return false;

    *out_binding = (NYA_InputBinding){ .key = (NYA_Keycode)key, .modifiers = modifiers };

    return true;
}

b8 _nya_settings_value_as_f32(const NYA_Value* value, OUT f32* out_number) {
    nya_assert(out_number != nullptr);

    /*
     * Every numeric type, not just F32.
     *
     * A volume written as 1.0 comes back as an F32 from the native format and an F64 from JSON, and
     * a volume a player hand-edited to a bare `1` comes back as an integer from both. All three mean
     * the same thing, and a loader that only accepted its own output would silently ignore the one
     * spelling a human is most likely to type.
     */
    switch (value->type) {
        case NYA_TYPE_F32: *out_number = value->as_f32; return true;
        case NYA_TYPE_F64: *out_number = (f32)value->as_f64; return true;
        case NYA_TYPE_S32: *out_number = (f32)value->as_s32; return true;
        case NYA_TYPE_S64: *out_number = (f32)value->as_s64; return true;
        case NYA_TYPE_U32: *out_number = (f32)value->as_u32; return true;
        case NYA_TYPE_U64: *out_number = (f32)value->as_u64; return true;
        default:           return false;
    }
}

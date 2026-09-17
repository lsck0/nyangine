#include "SDL3/SDL_keyboard.h"

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * What each volume channel is called in the settings file.
 * */
NYA_INTERNAL NYA_ConstCString _NYA_VOLUME_CHANNEL_NAMES[NYA_VOLUME_CHANNEL_COUNT] = {
    [NYA_VOLUME_CHANNEL_MASTER] = "master",
    [NYA_VOLUME_CHANNEL_SOUND]  = "sound",
    [NYA_VOLUME_CHANNEL_MUSIC]  = "music",
    [NYA_VOLUME_CHANNEL_VOICE]  = "voice",
    [NYA_VOLUME_CHANNEL_UI]     = "ui",
};

static_assert(sizeof(NYA_GraphicsQuality) == sizeof(u32), "the graphics quality is written and read as a u32");

/** The graphics settings as the file names them: a b8, a u32 or an f32 each. */
NYA_INTERNAL const struct {
    NYA_ConstCString name;
    NYA_Type         type;
    u64              offset;
} _NYA_SETTINGS_GRAPHICS_FIELDS[] = {
    { "msaa_samples", NYA_TYPE_U32, offsetof(NYA_SettingsGraphics, msaa_samples) },
    { "fxaa", NYA_TYPE_B8, offsetof(NYA_SettingsGraphics, fxaa) },
    { "ambient_occlusion", NYA_TYPE_B8, offsetof(NYA_SettingsGraphics, ambient_occlusion) },
    { "bloom", NYA_TYPE_B8, offsetof(NYA_SettingsGraphics, bloom) },
    { "depth_of_field", NYA_TYPE_B8, offsetof(NYA_SettingsGraphics, depth_of_field) },
    { "eye_adaptation", NYA_TYPE_B8, offsetof(NYA_SettingsGraphics, eye_adaptation) },
    { "light_shafts", NYA_TYPE_B8, offsetof(NYA_SettingsGraphics, light_shafts) },
    { "motion_blur", NYA_TYPE_B8, offsetof(NYA_SettingsGraphics, motion_blur) },
    { "shadows", NYA_TYPE_U32, offsetof(NYA_SettingsGraphics, shadows) },
    { "fov", NYA_TYPE_F32, offsetof(NYA_SettingsGraphics, fov) },
    { "render_scale", NYA_TYPE_F32, offsetof(NYA_SettingsGraphics, render_scale) },
};

/**
 * A binding as one editable string: `"Space"`, `"Ctrl+S"`, `"Shift+Left Alt+F1"`.
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
     */
    nya_settings_reset();

    nya_log_info("Settings system initialized.");
}

void nya_system_settings_deinit(void) {
    // Nothing is owned: the volumes are floats and the bindings are a fixed array inside NYA_App.
    // Saving is nya_settings_save and is the game's call, for the reason in nya_system_settings_init.
    nya_log_info("Settings system deinitialized.");
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

    NYA_ConstCString name = nya_settings_player_name();
    if (name[0] != '\0') nya_object_set(root, "player_name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)name });

    NYA_Object* graphics = nya_object_create(arena);

    for (u32 i = 0; i < nya_carray_length(_NYA_SETTINGS_GRAPHICS_FIELDS); i++) {
        const u8* field = (const u8*)&nya_settings()->graphics + _NYA_SETTINGS_GRAPHICS_FIELDS[i].offset;
        NYA_Value value = { .type = _NYA_SETTINGS_GRAPHICS_FIELDS[i].type };

        // the union's members share its first bytes, so a field copies into it at its own size.
        nya_memcpy(&value.as_u32, field, value.type == NYA_TYPE_B8 ? sizeof(b8) : sizeof(u32));

        nya_object_set(graphics, (NYA_CString)_NYA_SETTINGS_GRAPHICS_FIELDS[i].name, value);
    }

    nya_object_set(root, "graphics", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *graphics });

    return root;
}

void nya_settings_from_object(const NYA_Object* object) {
    if (object == nullptr) return;

    /*
     * The version is read and, today, not acted on.
     */
    u32 version = nya_save_version(object);
    if (version > NYA_SETTINGS_VERSION) {
        nya_log_warn("Settings file is version " FMTu32 ", newer than the " FMTu32 " this build understands; loading what it can.", version,
                 (u32)NYA_SETTINGS_VERSION);
    }

    NYA_Value* volumes = nya_object_get(object, "volumes");
    if (volumes != nullptr && volumes->type == NYA_TYPE_OBJECT) {
        for (u32 channel = 0; channel < NYA_VOLUME_CHANNEL_COUNT; channel++) {
            NYA_Value* value = nya_object_get(&volumes->as_object, (NYA_CString)_NYA_VOLUME_CHANNEL_NAMES[channel]);
            if (value == nullptr) continue;

            f32 volume = 0.0F;
            if (!_nya_settings_value_as_f32(value, &volume)) {
                nya_log_warn("Settings volume '%s' is not a number; leaving it alone.", _NYA_VOLUME_CHANNEL_NAMES[channel]);
                continue;
            }

            // nya_settings_volume_set clamps, which is the validation: a hand-edited 11 becomes 1
            // rather than a mix that clips.
            nya_settings_volume_set((NYA_VolumeChannel)channel, volume);
        }
    }

    NYA_Value* name = nya_object_get(object, "player_name");
    if (name != nullptr && name->type == NYA_TYPE_STRING) nya_settings_player_name_set(name->as_string);

    // over the current options, so a field the file lacks keeps its value, and then through the setter's clamps.
    NYA_Value* graphics_value = nya_object_get(object, "graphics");

    if (graphics_value != nullptr && graphics_value->type == NYA_TYPE_OBJECT) {
        NYA_SettingsGraphics graphics = nya_settings()->graphics;

        for (u32 i = 0; i < nya_carray_length(_NYA_SETTINGS_GRAPHICS_FIELDS); i++) {
            NYA_Value* value = nya_object_get(&graphics_value->as_object, (NYA_CString)_NYA_SETTINGS_GRAPHICS_FIELDS[i].name);
            u8*        field = (u8*)&graphics + _NYA_SETTINGS_GRAPHICS_FIELDS[i].offset;
            f32        number = 0.0F;

            if (value == nullptr) continue;

            if (_NYA_SETTINGS_GRAPHICS_FIELDS[i].type == NYA_TYPE_B8) {
                if (value->type == NYA_TYPE_B8) nya_memcpy(field, &value->as_b8, sizeof(b8));
                continue;
            }

            if (!_nya_settings_value_as_f32(value, &number)) continue;

            u32 whole = (u32)nya_clamp(number, 0.0F, 1024.0F);

            nya_memcpy(field, _NYA_SETTINGS_GRAPHICS_FIELDS[i].type == NYA_TYPE_F32 ? (const void*)&number : (const void*)&whole, sizeof(u32));
        }

        nya_settings_graphics_set(graphics);
    }

    NYA_Value* bindings = nya_object_get(object, "bindings");
    if (bindings == nullptr || bindings->type != NYA_TYPE_OBJECT) return;

    nya_dict_foreach_key (&bindings->as_object, key_slot) {
        NYA_ConstCString name   = *key_slot;
        NYA_InputAction  action = nya_input_action_from_name(name);
        if (action == NYA_INPUT_ACTION_NONE) {
            // An action this build does not have. Normal when a settings file outlives a rename, and
            // exactly what skipping unnamed actions on write is meant to keep rare.
            nya_log_warn("Settings file binds '%s', which is not an action in this build; ignoring it.", name);
            continue;
        }

        NYA_Value* keys = nya_object_get(&bindings->as_object, (NYA_CString)name);
        if (keys == nullptr || keys->type != NYA_TYPE_ARRAY) continue;

        /*
         * Cleared before the first slot is written, and only once the file has actually offered
         * something for this action.
         */
        b8 cleared = false;
        u32 slot   = 0;

        // the file only holds keys, so the gamepad bindings are put back after the keys are replaced.
        NYA_InputBinding previous[NYA_INPUT_BINDINGS_PER_ACTION];
        for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) previous[i] = nya_input_action_get(action, i);

        nya_array_foreach (&keys->as_array, key) {
            if (key->type != NYA_TYPE_STRING) continue;
            if (slot >= NYA_INPUT_BINDINGS_PER_ACTION) break;

            NYA_InputBinding binding = { 0 };
            if (!_nya_settings_binding_from_string(key->as_string, &binding)) {
                nya_log_warn("Settings file binds '%s' to '%s', which names no key; ignoring it.", name, key->as_string);
                continue;
            }

            if (!cleared) {
                nya_input_action_unbind(action);
                cleared = true;
            }

            nya_input_action_set(action, slot++, binding.key, binding.modifiers);
        }

        if (!cleared) continue;

        for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
            if (previous[i].kind == NYA_INPUT_BINDING_GAMEPAD_BUTTON) nya_input_action_bind_button(action, previous[i].button);
            if (previous[i].kind == NYA_INPUT_BINDING_GAMEPAD_AXIS) nya_input_action_bind_axis(action, previous[i].axis, previous[i].axis_threshold);
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

NYA_ConstCString nya_settings_player_name(void) {
    return nya_settings()->player_name;
}

void nya_settings_player_name_set(NYA_ConstCString name) {
    nya_assert(name != nullptr);

    char* stored = nya_settings()->player_name;
    u64   length = nya_min(strlen(name), (u64)(NYA_SETTINGS_NAME_MAX - 1));

    // a cut mid-character would store invalid UTF-8.
    if (name[length] != '\0') {
        while (length > 0 && ((u8)name[length] & 0xC0) == 0x80) length--;
    }

    nya_memcpy(stored, name, length);
    stored[length] = '\0';
}

void nya_settings_reset(void) {
    NYA_SettingsSystem* settings = nya_settings();

    settings->player_name[0] = '\0';

    for (u32 channel = 0; channel < NYA_VOLUME_CHANNEL_COUNT; channel++) settings->volumes[channel] = 1.0F;

    // NYA_KEY_UNKNOWN is the unbound marker, and it is zero, so this clears the whole table.
    nya_memset(settings->bindings, 0, sizeof(settings->bindings));

    settings->graphics = NYA_SETTINGS_GRAPHICS_DEFAULT;
}

NYA_SettingsGraphics nya_settings_graphics(void) {
    return nya_settings()->graphics;
}

void nya_settings_graphics_set(NYA_SettingsGraphics graphics) {
    // the largest power of two sample count at or under the one asked for.
    u32 samples = 1;
    while (samples * 2 <= nya_min(graphics.msaa_samples, 8U)) samples *= 2;

    graphics.msaa_samples = samples;
    graphics.shadows      = (u32)graphics.shadows < NYA_GRAPHICS_QUALITY_COUNT ? graphics.shadows : NYA_GRAPHICS_QUALITY_MEDIUM;
    graphics.fov          = nya_clamp(graphics.fov, 30.0F, 120.0F);
    graphics.render_scale = nya_clamp(graphics.render_scale, 0.25F, 1.0F);

    nya_settings()->graphics = graphics;
}

void nya_settings_graphics_apply(NYA_Window* window) {
    nya_assert(window != nullptr);

    const NYA_SettingsGraphics* graphics = &nya_settings()->graphics;

    nya_render_options_set(
        window,
        (NYA_RenderOptions){ .msaa_samples = graphics->msaa_samples, .fov_y = graphics->fov * (f32)M_PI / 180.0F, .render_scale = graphics->render_scale }
    );

    NYA_PostAntialias antialias = nya_post_antialias(window);
    antialias.enabled           = antialias.enabled && graphics->fxaa;
    nya_post_antialias_set(window, antialias);

    NYA_PostAmbientOcclusion occlusion = nya_post_ambient_occlusion(window);
    occlusion.enabled                  = occlusion.enabled && graphics->ambient_occlusion;
    nya_post_ambient_occlusion_set(window, occlusion);

    NYA_PostBloom bloom = nya_post_bloom(window);
    bloom.enabled       = bloom.enabled && graphics->bloom;
    nya_post_bloom_set(window, bloom);

    NYA_PostEyeAdaptation adaptation = nya_post_eye_adaptation(window);
    adaptation.enabled               = adaptation.enabled && graphics->eye_adaptation;
    nya_post_eye_adaptation_set(window, adaptation);

    NYA_PostLightShafts shafts = nya_post_light_shafts(window);
    shafts.enabled             = shafts.enabled && graphics->light_shafts;
    nya_post_light_shafts_set(window, shafts);

    NYA_PostMotionBlur motion_blur = nya_post_motion_blur(window);
    motion_blur.enabled            = motion_blur.enabled && graphics->motion_blur;
    nya_post_motion_blur_set(window, motion_blur);

    if (!graphics->depth_of_field) {
        NYA_PostDepthOfField depth_of_field = nya_post_depth_of_field(window);
        depth_of_field.focus                = NYA_POST_FOCUS_OFF;
        nya_post_depth_of_field_set(window, depth_of_field);
    }

    if (graphics->shadows == NYA_GRAPHICS_QUALITY_MEDIUM) return;

    if (graphics->shadows == NYA_GRAPHICS_QUALITY_OFF) {
        NYA_Render3DShadowFit fit = nya_render3d_shadow(window);
        fit.strength              = 0.0F;
        nya_render3d_shadow_set(window, fit);
        return;
    }

    // relative to what the game asked for, which is what medium means.
    NYA_Render3DShadowOptions shadow = nya_render3d_shadow_options(window);
    b8                        high   = graphics->shadows == NYA_GRAPHICS_QUALITY_HIGH;

    shadow.cascades = high ? shadow.cascades + 1 : 1;
    shadow.map_size = high ? shadow.map_size * 2 : shadow.map_size / 2;

    nya_render3d_shadow_options_set(window, shadow);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The modifiers a binding string may name, in the order they are written.
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

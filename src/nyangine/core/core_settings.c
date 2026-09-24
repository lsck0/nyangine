#include "SDL3/SDL_keyboard.h"

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

static_assert(sizeof(NYA_GraphicsQuality) == sizeof(u32), "the graphics quality is written and read as a u32");

/**
 * A binding as one editable string: `"Space"`, `"Ctrl+S"`, `"Shift+Left Alt+F1"`.
 * */
NYA_INTERNAL NYA_String* _nya_settings_binding_to_string(NYA_Arena* arena, NYA_InputBinding binding);

/** The inverse. Leaves `out_binding` untouched and returns false when the string names no key. */
NYA_INTERNAL b8 _nya_settings_binding_from_string(NYA_ConstCString text, OUT NYA_InputBinding* out_binding);

/**
 * Reads one described block out of the settings document, saying exactly what it could not use.
 *
 * `instance` is only written where the document is good, and a key that names no field or holds the
 * wrong kind of value is named and skipped. That is the whole contract of this file: one bad line
 * costs the player that line, never the rest of their settings. See nya_reflect_check.
 * */
NYA_INTERNAL void _nya_settings_section_read(NYA_ConstCString section, const NYA_TypeReflection* type, void* instance, const NYA_Object* object);

/** What _nya_settings_report needs to name where a problem is. */
typedef struct {
    NYA_ConstCString section;
} _NYA_SettingsReportContext;

NYA_INTERNAL void _nya_settings_report(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data);

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

    // NYA_SAVE_FLAGS_EDITABLE because this is the one file a player is invited to open. See the note
    // on that constant, and nya_settings_load for the other half of the bargain.
    return nya_save_write(NYA_SETTINGS_FILE, nya_settings_to_object(scratch), NYA_SAVE_FLAGS_EDITABLE);
}

NYA_Error nya_settings_load(void) {
    NYA_Arena* scratch = nya_arena_create(.name = "settings_load_scratch");
    defer nya_arena_destroy(scratch);

    NYA_Object* object = nullptr;

    // NYA_SAVE_FLAGS_EDITABLE, so the checksum is not enforced: this is the one file a player is
    // invited to open, the checksum is over the contents, and an honest edit changes it. Enforcing it
    // would throw away every setting a player had the moment they corrected one of them by hand.
    NYA_TRY(nya_save_read(scratch, NYA_SETTINGS_FILE, NYA_SAVE_FLAGS_EDITABLE, &object));

    nya_settings_from_object(object);

    return NYA_OK;
}

NYA_Object* nya_settings_to_object(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    NYA_Object* root = nya_object_create(arena);

    nya_object_add(root, NYA_SAVE_VERSION_KEY, (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = NYA_SETTINGS_VERSION });

    // Copied rather than cast: the two are the same bytes by static_assert, and a copy says so without
    // asking the compiler to believe an f32 array and a struct of f32 alias.
    NYA_SettingsVolumes volumes = { 0 };
    nya_memcpy(&volumes, nya_settings()->volumes, sizeof(volumes));

    NYA_Object* volumes_object = nya_reflect_to_object(arena, nya_reflect_of(NYA_SettingsVolumes), &volumes);

    nya_object_add(root, "volumes", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *volumes_object });

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

        nya_object_add(bindings, (NYA_CString)name, (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *keys });
    }

    nya_object_add(root, "bindings", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *bindings });

    NYA_ConstCString name = nya_settings_player_name();
    if (name[0] != '\0') nya_object_add(root, "player_name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)name });

    NYA_Object* graphics = nya_reflect_to_object(arena, nya_reflect_of(NYA_SettingsGraphics), &nya_settings()->graphics);

    nya_object_add(root, "graphics", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *graphics });

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

    // Over the values the settings already hold, so a block the file omits keeps its defaults, and
    // then back through the setters, which clamp: a hand-edited volume of 11 becomes 1 rather than a
    // mix that clips, and a field of movement of 400 degrees becomes 120.
    NYA_Value* volumes_value = nya_object_get(object, "volumes");

    if (volumes_value != nullptr && volumes_value->type == NYA_TYPE_OBJECT) {
        NYA_SettingsVolumes volumes = { 0 };
        nya_memcpy(&volumes, nya_settings()->volumes, sizeof(volumes));

        _nya_settings_section_read("volumes", nya_reflect_of(NYA_SettingsVolumes), &volumes, &volumes_value->as_object);

        f32 levels[NYA_VOLUME_CHANNEL_COUNT] = { 0 };
        nya_memcpy(levels, &volumes, sizeof(levels));

        /*
         * The field names come from the reflection rather than a second table here, so a channel
         * renamed in the header cannot be reported under its old name. They are in channel order for
         * the reason NYA_SettingsVolumes exists at all.
         */
        const NYA_TypeReflection* volume_type = nya_reflect_of(NYA_SettingsVolumes);

        for (u32 channel = 0; channel < NYA_VOLUME_CHANNEL_COUNT; channel++) {
            nya_settings_volume_set((NYA_VolumeChannel)channel, levels[channel]);

            // Said rather than silently corrected. A person who typed 5 meant something by it, and a
            // level that quietly becomes 1 is a setting that looks ignored.
            const f32 kept = nya_settings_volume((NYA_VolumeChannel)channel);
            if (kept == levels[channel]) continue;

            nya_log_warn("%s: 'volumes.%s' is %.3f, expected 0 to 1; using %.3f.", NYA_SETTINGS_FILE,
                         channel < volume_type->field_count ? volume_type->fields[channel].name : "?", (f64)levels[channel], (f64)kept);
        }
    } else if (volumes_value != nullptr) {
        nya_log_warn("%s: 'volumes' is not a block of settings; leaving every channel alone.", NYA_SETTINGS_FILE);
    }

    NYA_Value* name = nya_object_get(object, "player_name");

    if (name != nullptr && name->type == NYA_TYPE_STRING) {
        nya_settings_player_name_set(name->as_string);
    } else if (name != nullptr) {
        nya_log_warn("%s: 'player_name' is a %s, expected text; keeping the current name.", NYA_SETTINGS_FILE,
                     NYA_TYPE_NAME_MAP[name->type]);
    }

    NYA_Value* graphics_value = nya_object_get(object, "graphics");

    if (graphics_value != nullptr && graphics_value->type == NYA_TYPE_OBJECT) {
        NYA_SettingsGraphics graphics = nya_settings()->graphics;

        _nya_settings_section_read("graphics", nya_reflect_of(NYA_SettingsGraphics), &graphics, &graphics_value->as_object);

        const NYA_SettingsGraphics asked = graphics;
        nya_settings_graphics_set(graphics);

        /*
         * What was asked for against what was kept. Read back from the setter rather than checked
         * against ranges written out again here: the setter is the one place that decides, and a
         * second copy of its limits would be a second thing to keep in step.
         */
        const NYA_SettingsGraphics kept = nya_settings()->graphics;

        if (kept.msaa_samples != asked.msaa_samples) {
            nya_log_warn("%s: 'graphics.msaa_samples' is " FMTu32 ", expected 1, 2, 4 or 8; using " FMTu32 ".", NYA_SETTINGS_FILE,
                         asked.msaa_samples, kept.msaa_samples);
        }
        if (kept.shadows != asked.shadows) {
            // The variant names come from the reflection, so a quality level added later names itself.
            const NYA_TypeReflection* quality = nya_reflect_of(NYA_GraphicsQuality);
            NYA_ConstCString          using   = (u32)kept.shadows < quality->variant_count ? quality->variants[kept.shadows].name : "?";

            nya_log_warn("%s: 'graphics.shadows' is %d, which is not a quality level in this build; using %s.", NYA_SETTINGS_FILE,
                         (int)asked.shadows, using);
        }
        if (kept.fov != asked.fov) {
            nya_log_warn("%s: 'graphics.fov' is %.1f, expected 30 to 120 degrees; using %.1f.", NYA_SETTINGS_FILE, (f64)asked.fov, (f64)kept.fov);
        }
        if (kept.render_scale != asked.render_scale) {
            nya_log_warn("%s: 'graphics.render_scale' is %.3f, expected 0.25 to 1; using %.3f.", NYA_SETTINGS_FILE, (f64)asked.render_scale,
                         (f64)kept.render_scale);
        }
    } else if (graphics_value != nullptr) {
        nya_log_warn("%s: 'graphics' is not a block of settings; leaving every option alone.", NYA_SETTINGS_FILE);
    }

    NYA_Value* bindings = nya_object_get(object, "bindings");
    if (bindings == nullptr) return;

    if (bindings->type != NYA_TYPE_OBJECT) {
        nya_log_warn("%s: 'bindings' is not a block of bindings; leaving every action alone.", NYA_SETTINGS_FILE);
        return;
    }

    nya_dict_foreach_key (&bindings->as_object, key_slot) {
        NYA_ConstCString name   = *key_slot;
        NYA_InputAction  action = nya_input_action_from_name(name);
        if (action == NYA_INPUT_ACTION_NONE) {
            // An action this build does not have. Normal when a settings file outlives a rename, and
            // exactly what skipping unnamed actions on write is meant to keep rare.
            nya_log_warn("%s: 'bindings.%s' is not an action in this build; ignoring it.", NYA_SETTINGS_FILE, name);
            continue;
        }

        NYA_Value* keys = nya_object_get(&bindings->as_object, (NYA_CString)name);
        if (keys == nullptr) continue;

        if (keys->type != NYA_TYPE_ARRAY) {
            nya_log_warn("%s: 'bindings.%s' is a %s, expected a list of key names such as [\"Space\", \"Ctrl+S\"]; leaving it alone.",
                         NYA_SETTINGS_FILE, name, NYA_TYPE_NAME_MAP[keys->type]);
            continue;
        }

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
            if (slot >= NYA_INPUT_BINDINGS_PER_ACTION) {
                nya_log_warn("%s: 'bindings.%s' lists more than " FMTu32 " keys; the rest are ignored.", NYA_SETTINGS_FILE, name,
                             (u32)NYA_INPUT_BINDINGS_PER_ACTION);
                break;
            }

            if (key->type != NYA_TYPE_STRING) {
                nya_log_warn("%s: 'bindings.%s' holds a %s, expected the name of a key; ignoring it.", NYA_SETTINGS_FILE, name,
                             NYA_TYPE_NAME_MAP[key->type]);
                continue;
            }

            NYA_InputBinding binding = { 0 };
            if (!_nya_settings_binding_from_string(key->as_string, &binding)) {
                nya_log_warn("%s: 'bindings.%s' is \"%s\", which names no key; ignoring it. Keys are SDL's own spelling: "
                             "\"Space\", \"F1\", \"Ctrl+S\".",
                             NYA_SETTINGS_FILE, name, key->as_string);
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

    // classic SSAO answers to the same ambient-occlusion quality switch as the stylised occlusion.
    NYA_PostSsao ssao = nya_post_ssao(window);
    ssao.enabled      = ssao.enabled && graphics->ambient_occlusion;
    nya_post_ssao_set(window, ssao);

    // screen-space reflections answer to their own reflections quality switch.
    NYA_PostSsr ssr = nya_post_ssr(window);
    ssr.enabled     = ssr.enabled && graphics->reflections;
    nya_post_ssr_set(window, ssr);

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

void _nya_settings_report(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data) {
    const _NYA_SettingsReportContext* context = user_data;

    nya_log_warn("%s: '%s.%s' is %s, expected %s; leaving it alone.", NYA_SETTINGS_FILE, context->section, path, found, expected);
}

void _nya_settings_section_read(NYA_ConstCString section, const NYA_TypeReflection* type, void* instance, const NYA_Object* object) {
    nya_assert(section != nullptr);
    nya_assert(type != nullptr);
    nya_assert(instance != nullptr);
    nya_assert(object != nullptr);

    _NYA_SettingsReportContext context = { .section = section };

    // Before the write, not instead of it: the check says what is wrong and the write takes everything
    // that is right. A file with one bad line loses that line and nothing else.
    (void)nya_reflect_check(type, object, _nya_settings_report, &context);

    // Cannot fail on a settings block: these types carry no @on_apply, and a value that will not fit
    // has already been named above and is skipped by the write itself.
    NYA_Error applied = nya_reflect_from_object(type, instance, object);

    if (!applied.ok) {
        u8 message[256];
        (void)nya_error_format(&applied, message, sizeof(message));
        nya_log_warn("%s: could not read the '%s' settings: %s", NYA_SETTINGS_FILE, section, (NYA_CString)message);
    }
}

/**
 * @file ui_style.c
 *
 * The look: resolving a style's zeroes to defaults, the scale every size is multiplied by, and the per pass
 * look built from the two. See ui.h.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


// PRIVATE API DECLARATION

#ifdef NYA_ASSET_HOT_RELOAD
/** The asset system's current modification time for `handle`, or zero. Mirrors _nya_config_modification_time. */
NYA_INTERNAL u64 _nya_ui_theme_modification_time(NYA_CString handle);

/** Puts a theme asset that has died back into a state where it can be watched again. Mirrors _nya_config_rearm. */
NYA_INTERNAL void _nya_ui_theme_rearm(NYA_UI* context);
#endif // NYA_ASSET_HOT_RELOAD


// STYLE

void nya_ui_style_set(NYA_Window* window, NYA_UIStyle style) {
    nya_assert(window != nullptr);

    _nya_ui_context(window)->style = _nya_ui_style_resolve(style);
}

NYA_UIStyle nya_ui_style_get(const NYA_Window* window) {
    nya_assert(window != nullptr);

    return _nya_ui_context(window)->style;
}

void nya_ui_style_push(NYA_UI* ui, NYA_UIStyle style) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.look_depth < NYA_UI_STYLE_DEPTH_MAX, "styles pushed deeper than NYA_UI_STYLE_DEPTH_MAX");

    NYA_UIStyle resolved = _nya_ui_style_resolve(style);

    _nya_ui.look_depth += 1;

    _nya_ui_look_build_at(ui, _nya_ui.look_depth, &resolved);
}

void nya_ui_style_pop(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.look_depth > 0, "nya_ui_style_pop without a push");

    _nya_ui.look_depth -= 1;

    ui->present->look_use(ui->present->state, _nya_ui.look_depth);
}

f32 nya_ui_scale(const NYA_Window* window) {
    nya_assert(window != nullptr);

    const NYA_UI* ui = _nya_ui_context(window);

    return ui->scale > 0.0F ? ui->scale : 1.0F;
}


// THEME

NYA_Error nya_ui_theme_load(NYA_Window* window, NYA_ConstCString path) {
    nya_assert(window != nullptr);
    nya_assert(path != nullptr);

    NYA_UI* context = _nya_ui_context(window);

    // Applied first, so a file that can't be read or parsed reports its error and leaves the style and, under hot reload, any watch already on this window untouched — the same order nya_config_watch uses.
    NYA_TRY(_nya_ui_theme_apply(context, path));

#ifdef NYA_ASSET_HOT_RELOAD
    // The path is the asset handle, and it lives in the window's own storage rather than the caller's, which may be a game DLL's .rodata a code reload can unmap; refused rather than truncated, since a clipped path would name a different file the watch could never resolve.
    u64 length = 0;
    while (path[length] != '\0') length++;
    if (length + 1 > NYA_UI_THEME_PATH_MAX) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "UI theme path is longer than NYA_UI_THEME_PATH_MAX");
    }

    nya_memcpy(context->theme_path, path, length);
    context->theme_path[length]     = '\0';
    context->theme_active           = true;
    context->theme_modification_time = _nya_ui_theme_modification_time(context->theme_path);
    context->theme_next_recovery_ns  = 0;

    // Guarded so a program that loads themes for several windows adds one hook, not one per window.
    static b8 hook_registered = false;
    if (!hook_registered) {
        // After the asset system's own frame-ended hooks, like _nya_config_watch_tick: by the time this runs a reload queued this frame has landed and the timestamp it compares against is settled.
        nya_event_hook_register((NYA_EventHook){
            .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
            .event_type = NYA_EVENT_FRAME_ENDED,
            .fn         = nya_callback(_nya_ui_theme_tick),
        });
        hook_registered = true;
    }

    // Registered as a text asset so the file is watched from here on, mirroring nya_config_watch.
    (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXT, .handle = context->theme_path });
#endif // NYA_ASSET_HOT_RELOAD

    return NYA_OK;
}

void nya_ui_theme_clear(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_UI* context = _nya_ui_context(window);

#ifdef NYA_ASSET_HOT_RELOAD
    if (context->theme_active) (void)nya_asset_unload(context->theme_path);

    context->theme_active            = false;
    context->theme_path[0]           = '\0';
    context->theme_modification_time = 0;
    context->theme_next_recovery_ns  = 0;
#else
    nya_unused(context);
#endif // NYA_ASSET_HOT_RELOAD
}


// INTERNAL

NYA_UIStyle _nya_ui_style_resolve(NYA_UIStyle style) {
    style.font[NYA_UI_FONT_NAME_MAX - 1]       = '\0';
    style.title_font[NYA_UI_FONT_NAME_MAX - 1] = '\0';

    struct {
        f32* value;
        f32  fallback;
    } sizes[] = {
        { &style.body_size,  NYA_UI_BODY_SIZE  },
        { &style.small_size, NYA_UI_SMALL_SIZE },
        { &style.title_size, NYA_UI_TITLE_SIZE },
        { &style.margin,     NYA_UI_MARGIN     },
        { &style.padding,    NYA_UI_PADDING    },
        { &style.spacing,    NYA_UI_SPACING    },
        { &style.radius,     NYA_UI_RADIUS     },
        { &style.outline,    NYA_UI_OUTLINE    },
        { &style.depth,      NYA_UI_DEPTH      },
        { &style.pop,        NYA_UI_POP        },
        { &style.focus_bar,  NYA_UI_FOCUS_BAR  },
    };

    for (u32 i = 0; i < nya_carray_length(sizes); i++) {
        if (*sizes[i].value <= 0.0F) *sizes[i].value = sizes[i].fallback;
    }

    if (style.scale < 0.0F) style.scale = 0.0F;
    if (style.item_height < 0.0F) style.item_height = 0.0F;

    // from a hand edited config, so a bad value falls back instead of asserting.
    if (style.overflow == NYA_UI_OVERFLOW_INHERIT || style.overflow >= NYA_UI_OVERFLOW_COUNT) style.overflow = NYA_UI_OVERFLOW_VISIBLE;

    NYA_UISkin* skins[] = { &style.panel_skin, &style.button_skin.normal, &style.button_skin.focused, &style.button_skin.pressed,
                            &style.button_skin.disabled, &style.track_skin, &style.knob_skin };

    for (u32 i = 0; i < nya_carray_length(skins); i++) skins[i]->texture[NYA_UI_SKIN_TEXTURE_MAX - 1] = '\0';

    struct {
        NYA_Color* color;
        NYA_Color  fallback;
    } colors[] = {
        { &style.scrim,           NYA_UI_SCRIM           },
        { &style.panel,           NYA_UI_PANEL           },
        { &style.ink,             NYA_UI_INK             },
        { &style.track,           NYA_UI_TRACK           },
        { &style.accent,          NYA_UI_ACCENT          },
        { &style.text_dim,        NYA_UI_TEXT_DIM        },
        { &style.button.normal,   NYA_UI_BUTTON          },
        { &style.button.focused,  NYA_UI_BUTTON_FOCUSED  },
        { &style.button.pressed,  NYA_UI_BUTTON_PRESSED  },
        { &style.button.disabled, NYA_UI_BUTTON_DISABLED },
        { &style.text.normal,     NYA_UI_TEXT            },
        { &style.text.focused,    NYA_UI_TEXT_FOCUSED    },
        { &style.text.pressed,    NYA_UI_TEXT_PRESSED    },
        { &style.text.disabled,   NYA_UI_TEXT_DISABLED   },
    };

    for (u32 i = 0; i < nya_carray_length(colors); i++) {
        NYA_Color c = *colors[i].color;
        if (c.r == 0.0F && c.g == 0.0F && c.b == 0.0F && c.a == 0.0F) *colors[i].color = colors[i].fallback;
    }

    return style;
}

// THEME

/** Where a theme validation warning names the file it came from. */
typedef struct {
    NYA_ConstCString path;
    u32              problems;
} _NYA_UIThemeReport;

/**
 * What nya_reflect_check reports for a key that names no field or a value of the wrong type. Mirrors the
 * wording of core_settings.c so a theme reads like every other hand edited file the engine validates.
 * */
NYA_INTERNAL void _nya_ui_theme_report(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data) {
    _NYA_UIThemeReport* report = user_data;
    report->problems++;

    nya_log_warn("%s: '%s' is %s, expected %s; using the built-in default.", report->path, path, found, expected);
}

/**
 * Rejects every numeric field outside its range, reporting it by name and restoring the built-in default
 * in its place. What nya_reflect_check cannot do: reflection describes types, not ranges, so this is where
 * "radius is non-negative" and "an alpha is in [0, 1]" live — the same division of labour as
 * core_settings.c, where the reflected read is followed by the setters that clamp.
 * */
NYA_INTERNAL void _nya_ui_theme_validate(NYA_UIStyle* style, const NYA_UIStyle* fallback, NYA_ConstCString path) {
    // Every size is a length, so a negative one is meaningless rather than merely unusual; a zero is left to _nya_ui_style_resolve, which reads it as "take the default", the documented behaviour of the type.
    struct {
        NYA_ConstCString name;
        f32*             value;
        f32              fallback;
    } sizes[] = {
        { "body_size", &style->body_size, fallback->body_size },       { "small_size", &style->small_size, fallback->small_size },
        { "title_size", &style->title_size, fallback->title_size },    { "scale", &style->scale, fallback->scale },
        { "margin", &style->margin, fallback->margin },                { "padding", &style->padding, fallback->padding },
        { "spacing", &style->spacing, fallback->spacing },             { "radius", &style->radius, fallback->radius },
        { "outline", &style->outline, fallback->outline },             { "depth", &style->depth, fallback->depth },
        { "pop", &style->pop, fallback->pop },                         { "focus_bar", &style->focus_bar, fallback->focus_bar },
        { "item_height", &style->item_height, fallback->item_height }, { "transition_s", &style->transition_s, fallback->transition_s },
        { "appear_s", &style->appear_s, fallback->appear_s },
    };

    for (u32 i = 0; i < nya_carray_length(sizes); i++) {
        if (*sizes[i].value >= 0.0F) continue;

        nya_log_warn("%s: '%s' is %.3f, expected >= 0; using %.3f.", path, sizes[i].name, (f64)*sizes[i].value, (f64)sizes[i].fallback);
        *sizes[i].value = sizes[i].fallback;
    }

    // Every channel of every colour, alpha included, is a fraction in [0, 1]: a colour left at all zeroes is _nya_ui_style_resolve's to fill, and one with a channel out of range is rejected whole, since a partly clamped colour is a colour nobody chose.
    struct {
        NYA_ConstCString name;
        NYA_Color*       color;
        NYA_Color        fallback;
    } colors[] = {
        { "scrim", &style->scrim, fallback->scrim },
        { "panel", &style->panel, fallback->panel },
        { "ink", &style->ink, fallback->ink },
        { "track", &style->track, fallback->track },
        { "accent", &style->accent, fallback->accent },
        { "text_dim", &style->text_dim, fallback->text_dim },
        { "button.normal", &style->button.normal, fallback->button.normal },
        { "button.focused", &style->button.focused, fallback->button.focused },
        { "button.pressed", &style->button.pressed, fallback->button.pressed },
        { "button.disabled", &style->button.disabled, fallback->button.disabled },
        { "text.normal", &style->text.normal, fallback->text.normal },
        { "text.focused", &style->text.focused, fallback->text.focused },
        { "text.pressed", &style->text.pressed, fallback->text.pressed },
        { "text.disabled", &style->text.disabled, fallback->text.disabled },
    };

    for (u32 i = 0; i < nya_carray_length(colors); i++) {
        NYA_Color c        = *colors[i].color;
        f32       channels[] = { c.r, c.g, c.b, c.a };

        b8 out_of_range = false;
        for (u32 k = 0; k < nya_carray_length(channels); k++) {
            if (channels[k] < 0.0F || channels[k] > 1.0F) out_of_range = true;
        }
        if (!out_of_range) continue;

        nya_log_warn("%s: '%s' is (%.3f, %.3f, %.3f, %.3f), expected each channel 0 to 1; using the built-in colour.", path,
                     colors[i].name, (f64)c.r, (f64)c.g, (f64)c.b, (f64)c.a);
        *colors[i].color = colors[i].fallback;
    }
}

NYA_Error _nya_ui_theme_apply(NYA_UI* context, NYA_ConstCString path) {
    nya_assert(context != nullptr && path != nullptr);

    NYA_Arena* scratch = nya_arena_create(.name = "ui_theme_scratch");
    defer      nya_arena_destroy(scratch);

    u8* data = nullptr;
    u64 size = 0;
    NYA_TRY(nya_asset_read(scratch, (NYA_CString)path, &data, &size));

    NYA_Object* object = nullptr;

    // NO_CHECKSUM for the same reason nya_config_load uses it: a theme is meant to be hand edited while the program runs, so a mismatch from an in-progress edit is expected rather than corruption.
    NYA_TRY(nya_deserialize(scratch, data, size, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &object));

    // Reported before anything is written, so a key that names no field or a value of the wrong type is named rather than silently skipped; nya_reflect_from_object then drops exactly what this warned about.
    _NYA_UIThemeReport report = { .path = path };
    (void)nya_reflect_check(nya_reflect_of(NYA_UIStyle), object, _nya_ui_theme_report, &report);

    // The built-in look, fully resolved, is both seed and fallback: a field the file omits keeps its default, a field of the wrong type is skipped and so keeps it, and a field out of range is put back to it by _nya_ui_theme_validate below.
    NYA_UIStyle fallback = _nya_ui_style_resolve((NYA_UIStyle){ 0 });
    NYA_UIStyle style    = fallback;

    // Only reached once the file has fully parsed, so a syntax error above leaves the style as it was.
    NYA_TRY(nya_reflect_from_object(nya_reflect_of(NYA_UIStyle), &style, object));

    _nya_ui_theme_validate(&style, &fallback, path);

    context->style = _nya_ui_style_resolve(style);

    return NYA_OK;
}

#ifdef NYA_ASSET_HOT_RELOAD
void _nya_ui_theme_tick(NYA_Event* event) {
    nya_trace_scope(NYA_TRACE_HOT_RELOAD);

    nya_unused(event);

    for (u32 i = 0; i < NYA_WINDOW_MAX; i++) {
        NYA_UI* context = &_nya_ui.windows[i];
        if (!context->claimed || !context->theme_active) continue;

        // The point of this call, not the comparison below it: reload detection lives inside nya_asset_get, which stats the file at most once per interval and queues it when the timestamp moved. Same note as _nya_config_watch_tick.
        (void)nya_asset_get(context->theme_path);

        // Before the comparison, because a dead asset reports no timestamp and would otherwise look like a file that simply hadn't changed.
        _nya_ui_theme_rearm(context);

        u64 now = _nya_ui_theme_modification_time(context->theme_path);
        if (now == context->theme_modification_time) continue;

        // Not logged on failure: a file caught mid-write fails to parse, nothing changes, and the unmoved timestamp makes the next tick try again. Same as _nya_config_watch_tick.
        if (!_nya_ui_theme_apply(context, context->theme_path).ok) continue;

        context->theme_modification_time = now;

        nya_log_info("Reloaded UI theme '%s'.", context->theme_path);
    }
}

u64 _nya_ui_theme_modification_time(NYA_CString handle) {
    if (handle == nullptr || handle[0] == '\0') return 0;

    NYA_Asset* asset = nya_asset_get(handle);

    // Out of the blob: part of the executable, so there is no file and nothing that could differ.
    if (asset != nullptr && asset->from_blob) return 0;

    // The asset's own timestamp once it has one, and the file's until then. See _nya_config_modification_time.
    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED) return asset->source_modification_time;

    u64 modified = 0;

    // A theme that's missing answers zero, which compares equal to itself and so reads as "nothing changed" rather than a change that can never be resolved.
    if (!nya_filesystem_last_modified(handle, &modified).ok) return 0;

    return modified;
}

void _nya_ui_theme_rearm(NYA_UI* context) {
    NYA_Asset* asset = nya_asset_get(context->theme_path);

    // Both terminal states, as in _nya_config_rearm: UNLOADED is what a file briefly missing during an editor's save-and-rename produces, and it recovers no more on its own than FAILED does.
    b8 stuck = asset == nullptr || asset->status == NYA_ASSET_STATUS_FAILED || asset->status == NYA_ASSET_STATUS_UNLOADED;
    if (!stuck) return;

    u64 now_ns = nya_app_get()->frame_stats.uptime_ns;
    if (now_ns < context->theme_next_recovery_ns) return;

    context->theme_next_recovery_ns = now_ns + _NYA_ASSET_STAT_INTERVAL_NS;

    nya_log_debug("Re-arming the UI theme asset '%s' after a failed load.", context->theme_path);

    (void)nya_asset_unload(context->theme_path);
    (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXT, .handle = context->theme_path });

    // Cleared so the file is re-resolved once it's back, rather than comparing against the last good load's timestamp and finding nothing changed.
    context->theme_modification_time = 0;
}
#endif // NYA_ASSET_HOT_RELOAD

f32 _nya_ui_scale_derive(const NYA_Window* window, const NYA_UIStyle* style) {
    nya_assert(window != nullptr && style != nullptr);
    nya_assert(style->scale >= 0.0F, "a resolved style's scale is never negative");

    f32 scale = style->scale > 0.0F ? style->scale : 1.0F;

    // The window's size is deliberately not in here: each distinct scale rasterises its own glyph atlas per point size, so a scale that follows a drag mints one per step and empties the atlas cache mid-resize. The display scale is opt in and snapped for the same reason — a 1.15 desktop scale would otherwise bake sizes nothing else shares.
    if (style->follow_display_scale && window->sdl_window != nullptr) {
        scale *= roundf(nya_window_display_scale(window->handle) / NYA_UI_SCALE_STEP) * NYA_UI_SCALE_STEP;
    }

    return nya_max(scale, NYA_UI_SCALE_MIN);
}

void _nya_ui_look_build(const NYA_UI* ui, u32 depth) {
    _nya_ui_look_build_at(ui, depth, &ui->style);
}

void _nya_ui_look_build_at(const NYA_UI* ui, u32 depth, const NYA_UIStyle* style) {
    nya_assert(ui != nullptr && style != nullptr && ui->present != nullptr);
    nya_assert(depth <= NYA_UI_STYLE_DEPTH_MAX);

    const NYA_UIPresenter* present = ui->present;

    // the backend turns the style into pixels, so a grid can round a padding to a cell and the layout never knows.
    present->look_build(present->state, depth, style, ui->scale, &_nya_ui.looks[depth]);
    present->look_use(present->state, depth);
}

const NYA_UILook* _nya_ui_look(void) {
    return &_nya_ui.looks[_nya_ui.look_depth];
}

f32 _nya_ui_px(f32 value) {
    return roundf(value * _nya_ui.open->scale);
}

f32 _nya_ui_item_height(const _NYA_UILayout* layout) {
    const NYA_UILook* look = _nya_ui_look();

    return look->item_height > 0.0F ? look->item_height : look->line_heights[layout->text] + roundf(look->padding * 1.5F);
}

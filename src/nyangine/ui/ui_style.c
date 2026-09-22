/**
 * @file ui_style.c
 *
 * The look: resolving a style's zeroes to defaults, the scale every size is multiplied by, and the per pass
 * look built from the two. See ui.h.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STYLE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

f32 _nya_ui_scale_derive(const NYA_Window* window, const NYA_UIStyle* style) {
    nya_assert(window != nullptr && style != nullptr);
    nya_assert(style->scale >= 0.0F, "a resolved style's scale is never negative");

    f32 scale = style->scale > 0.0F ? style->scale : 1.0F;

    /*
     * The window's size is deliberately not in here. Each distinct scale rasterises its own glyph atlas per point
     * size, so a scale that follows a drag mints one per step and empties the atlas cache mid-resize. The display
     * scale is opt in and snapped for the same reason: a 1.15 desktop scale would otherwise bake sizes nothing else
     * shares.
     */
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

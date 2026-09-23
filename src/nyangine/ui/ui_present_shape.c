/**
 * @file ui_present_shape.c
 *
 * The presenter every window starts with: widgets as shapes and glyphs through render2d and the font registry.
 * This is the only file in the UI module that names a drawing primitive or a font, which is what the rest of the
 * module is arranged around. See ui_present.h.
 *
 * Nothing here decides what a widget *is*; it is handed one NYA_UIWidgetDraw at a time and decides what it looks
 * like. Which 2D backend is under render2d is not this file's business either: the GPU one and the terminal one
 * satisfy the same header, so a TUI already draws through here.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One depth of the style stack: what the layout reads back, and the faces only this file needs. */
typedef struct {
    NYA_UILook look;
    NYA_Font   fonts[NYA_UI_TEXT_COUNT];
} _NYA_UIShapeLook;

typedef struct {
    _NYA_UIShapeLook looks[NYA_UI_STYLE_DEPTH_MAX + 1];
    u32              depth;

    f32 scale;

    /** The widget being drawn: its opacity multiplies every colour, and its state picks every colour. */
    f32               opacity;
    NYA_UIWidgetState state;
} _NYA_UIShape;


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/* One window's UI is drawn at a time, as one pass is open at a time, so the presenter's scratch is one of these. */
NYA_INTERNAL _NYA_UIShape _nya_ui_shape = { .opacity = 1.0F };


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void  _nya_ui_shape_look_build(void* state, u32 depth, const NYA_UIStyle* style, f32 scale, NYA_UILook* out);
NYA_INTERNAL void  _nya_ui_shape_look_use(void* state, u32 depth);
NYA_INTERNAL f32x2 _nya_ui_shape_measure(void* state, NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow);
NYA_INTERNAL f32   _nya_ui_shape_measure_bytes(void* state, NYA_UIText role, NYA_ConstCString text, u32 bytes);
NYA_INTERNAL void  _nya_ui_shape_clip_set(void* state, NYA_Window* window, NYA_Rectf clip, b8 whole);
NYA_INTERNAL s32   _nya_ui_shape_layer_get(void* state, NYA_Window* window);
NYA_INTERNAL void  _nya_ui_shape_layer_set(void* state, NYA_Window* window, s32 layer);
NYA_INTERNAL void  _nya_ui_shape_draw(void* state, NYA_Window* window, const NYA_UIWidgetDraw* widget);

/** The look and the faces at the depth the UI last selected. */
NYA_INTERNAL const _NYA_UIShapeLook* _nya_ui_shape_look(void) __attr_no_discard;

/** `value`, in pixels at scale 1, as whole pixels at the pass's scale. */
NYA_INTERNAL f32 _nya_ui_shape_px(f32 value) __attr_no_discard;

/** How wide the first `bytes` of `text` draw at `role`. What measure_bytes answers, reachable from this file. */
NYA_INTERNAL f32 _nya_ui_shape_prefix(NYA_UIText role, NYA_ConstCString text, u32 bytes) __attr_no_discard;

/**
 * One row's height at `role`: what the style asks for, or a line plus padding. The marks inside a widget — a dot, a
 * ring, a pill, a knob — are sized from this rather than from the rectangle, so a row a container stretched keeps
 * the marks the rest of the rows have.
 * */
NYA_INTERNAL f32 _nya_ui_shape_item(NYA_UIText role) __attr_no_discard;

/** `color` with its alpha multiplied by the opacity groups open around the widget being drawn. */
NYA_INTERNAL NYA_Color _nya_ui_shape_fade(NYA_Color color) __attr_no_discard;

/** The colour and the skin for the widget being drawn. */
NYA_INTERNAL NYA_Color  _nya_ui_shape_color(const NYA_UIStateColors* colors) __attr_no_discard;
NYA_INTERNAL NYA_UISkin _nya_ui_shape_skin(const NYA_UIStateSkins* skins) __attr_no_discard;

/** `text` in the shrunk face a SHRINK label falls back to, or the role's own when it fits. */
NYA_INTERNAL NYA_Font _nya_ui_shape_fit(NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow, f32* out_width) __attr_no_discard;

/** Draws `skin` over `rect`, tinted by `tint` when the skin has none. False, drawing nothing, for a flat or unloaded skin. */
NYA_INTERNAL b8 _nya_ui_shape_skin_draw(NYA_Window* window, const NYA_UISkin* skin, NYA_Rectf rect, NYA_Color tint);

/** The shadow, fill, outline and focus mark of a widget, popped when newly focused and sunk when held. The body. */
NYA_INTERNAL NYA_Rectf _nya_ui_shape_body(NYA_Window* window, NYA_Rectf rect);

/** A slider or toggle track, a field's box, or the filled part of a track in `color`. */
NYA_INTERNAL void _nya_ui_shape_track(NYA_Window* window, NYA_Rectf rect, f32 radius, NYA_Color color);

/** `text`, measured `width` wide, centred vertically in `rect` and placed across it by `align`. */
NYA_INTERNAL void _nya_ui_shape_text(NYA_Window* window, NYA_UIText role, NYA_ConstCString text, f32 width, NYA_Rectf rect, NYA_UIAlign align, NYA_Color color);

/** Draws `mark` centred in `rect` from lines and triangles, since chrome must not need a glyph the face may lack. */
NYA_INTERNAL void _nya_ui_shape_mark(NYA_Window* window, NYA_UIMark mark, NYA_Rectf rect, NYA_Color color);

/** Draws `icon` into `rect`, taking its texture and tint from the style when it names none. */
NYA_INTERNAL void _nya_ui_shape_icon(NYA_Window* window, const NYA_UIIcon* icon, NYA_Rectf rect, NYA_Color tint);

/* One per NYA_UIWidgetKind that needs more than a line, in the enum's order. */

NYA_INTERNAL void _nya_ui_shape_panel(NYA_Window* window, const NYA_UIWidgetDraw* widget);
NYA_INTERNAL void _nya_ui_shape_label(NYA_Window* window, const NYA_UIWidgetDraw* widget);
NYA_INTERNAL void _nya_ui_shape_toggle(NYA_Window* window, const NYA_UIWidgetDraw* widget);
NYA_INTERNAL void _nya_ui_shape_slider(NYA_Window* window, const NYA_UIWidgetDraw* widget);
NYA_INTERNAL void _nya_ui_shape_dropdown(NYA_Window* window, const NYA_UIWidgetDraw* widget);
NYA_INTERNAL void _nya_ui_shape_field(NYA_Window* window, const NYA_UIWidgetDraw* widget, const NYA_UIFieldDraw* field);
NYA_INTERNAL void _nya_ui_shape_picker(NYA_Window* window, const NYA_UIWidgetDraw* widget);
NYA_INTERNAL void _nya_ui_shape_chart(NYA_Window* window, const NYA_UIWidgetDraw* widget);


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE PRESENTER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL const NYA_UIPresenter _nya_ui_shape_presenter = {
    .name          = "shape",
    .state         = &_nya_ui_shape,
    .look_build    = _nya_ui_shape_look_build,
    .look_use      = _nya_ui_shape_look_use,
    .measure       = _nya_ui_shape_measure,
    .measure_bytes = _nya_ui_shape_measure_bytes,
    .clip_set      = _nya_ui_shape_clip_set,
    .layer_get     = _nya_ui_shape_layer_get,
    .layer_set     = _nya_ui_shape_layer_set,
    .draw          = _nya_ui_shape_draw,
};

const NYA_UIPresenter* nya_ui_presenter_shape(void) {
    return &_nya_ui_shape_presenter;
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_ui_shape_look_build(void* state, u32 depth, const NYA_UIStyle* style, f32 scale, NYA_UILook* out) {
    nya_assert(state == &_nya_ui_shape && style != nullptr && out != nullptr);
    nya_assert(depth <= NYA_UI_STYLE_DEPTH_MAX, "a look is built at depth %u", depth);

    _NYA_UIShapeLook* built = &_nya_ui_shape.looks[depth];

    _nya_ui_shape.scale = scale;

    nya_ui_look_scale(style, scale, &built->look);

    // by name every pass, since the registry can change under a hot reload.
    NYA_Font body  = nya_font_resolve(style->font[0] != '\0' ? nya_font_named(style->font) : NYA_FONT_NONE);
    NYA_Font title = style->title_font[0] != '\0' ? nya_font_resolve(nya_font_named(style->title_font)) : body;

    const f32 sizes[NYA_UI_TEXT_COUNT] = {
        [NYA_UI_TEXT_INHERIT] = style->body_size,
        [NYA_UI_TEXT_BODY]    = style->body_size,
        [NYA_UI_TEXT_SMALL]   = style->small_size,
        [NYA_UI_TEXT_TITLE]   = style->title_size,
    };

    // rasterised at the scaled size in whole points, so text is as crisp at 4K as at 720p.
    for (u32 i = 0; i < NYA_UI_TEXT_COUNT; i++) {
        NYA_Font face = i == NYA_UI_TEXT_TITLE ? title : body;

        built->fonts[i] = nya_font(face.path, nya_max(roundf(sizes[i] * scale), 1.0F));
        if (!nya_font_equals(built->fonts[i], face) && nya_font_sdf(face) && !nya_font_sdf(built->fonts[i])) (void)nya_font_sdf_set(built->fonts[i], true);

        built->look.line_heights[i] = nya_font_valid(built->fonts[i]) ? ceilf(nya_font_metrics(built->fonts[i]).line_height) : 0.0F;
    }

    // the title's line height and half the padding: enough that the text is not touching both edges, and
    // no more, because the bar is chrome rather than a row — a whole padding made it the thickest thing
    // on screen at a large title size.
    f32 title_line = built->look.line_heights[NYA_UI_TEXT_TITLE];

    built->look.title_bar = title_line > 0.0F ? roundf(title_line + (built->look.padding * 0.5F)) : 0.0F;

    *out = built->look;
}

void _nya_ui_shape_look_use(void* state, u32 depth) {
    nya_assert(state == &_nya_ui_shape);
    nya_assert(depth <= NYA_UI_STYLE_DEPTH_MAX, "a look at depth %u was selected", depth);

    _nya_ui_shape.depth = depth;
}

f32x2 _nya_ui_shape_measure(void* state, NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow) {
    nya_assert(state == &_nya_ui_shape && text != nullptr);
    nya_assert(role < NYA_UI_TEXT_COUNT);

    const _NYA_UIShapeLook* look = _nya_ui_shape_look();

    f32      width = 0.0F;
    NYA_Font font  = _nya_ui_shape_fit(role, text, room, overflow, &width);

    if (overflow == NYA_UI_OVERFLOW_WRAP && room > 0.0F && width > room) return nya_font_measure_wrapped(font, text, room);

    // a shrunk line keeps the line height of the role it shrank from, so a row of labels stays a row.
    return (f32x2){ width, look->look.line_heights[role] };
}

f32 _nya_ui_shape_measure_bytes(void* state, NYA_UIText role, NYA_ConstCString text, u32 bytes) {
    nya_assert(state == &_nya_ui_shape);

    return _nya_ui_shape_prefix(role, text, bytes);
}

void _nya_ui_shape_clip_set(void* state, NYA_Window* window, NYA_Rectf clip, b8 whole) {
    nya_assert(state == &_nya_ui_shape && window != nullptr);

    if (whole) {
        nya_render2d_scissor_end(window);
        return;
    }

    nya_render2d_scissor_begin(window, clip.x, clip.y, clip.width, clip.height);
}

s32 _nya_ui_shape_layer_get(void* state, NYA_Window* window) {
    nya_assert(state == &_nya_ui_shape && window != nullptr);

    return nya_render2d_layer(window);
}

void _nya_ui_shape_layer_set(void* state, NYA_Window* window, s32 layer) {
    nya_assert(state == &_nya_ui_shape && window != nullptr);

    nya_render2d_layer_set(window, layer);
}

void _nya_ui_shape_draw(void* state, NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    nya_assert(state == &_nya_ui_shape && window != nullptr && widget != nullptr);
    nya_assert(widget->kind < NYA_UI_WIDGET_KIND_COUNT);
    nya_assert(widget->label != nullptr, "%s arrived without a label; \"\" is how a widget says it has none", nya_ui_widget_kind_name(widget->kind));

    const _NYA_UIShapeLook* look  = _nya_ui_shape_look();
    const NYA_UIStyle*      style = &look->look.style;

    _nya_ui_shape.opacity = widget->opacity;
    _nya_ui_shape.state   = widget->state;

    switch (widget->kind) {
        case NYA_UI_WIDGET_SCRIM: {
            nya_render2d_rect(window, widget->rect.x, widget->rect.y, widget->rect.width, widget->rect.height, _nya_ui_shape_fade(style->scrim));
        } break;

        case NYA_UI_WIDGET_PANEL: _nya_ui_shape_panel(window, widget); break;
        case NYA_UI_WIDGET_LABEL: _nya_ui_shape_label(window, widget); break;

        case NYA_UI_WIDGET_BUTTON: {
            NYA_Rectf body = _nya_ui_shape_body(window, widget->rect);

            _nya_ui_shape_text(window, widget->text, widget->label, 0.0F, body, NYA_UI_ALIGN_CENTER, _nya_ui_shape_color(&style->text));
        } break;

        case NYA_UI_WIDGET_SELECTABLE: {
            NYA_Rectf body   = _nya_ui_shape_body(window, widget->rect);
            f32       dot    = roundf(_nya_ui_shape_item(widget->text) * 0.14F);
            f32x2     center = { body.x + look->look.padding + dot, body.y + roundf(body.height * 0.5F) };

            // an empty hole when not chosen, so the list reads as a choice even before anything is picked.
            b8 filled = widget->as_choice.on && !widget->state.disabled;
            nya_render2d_circle(window, center, dot, _nya_ui_shape_fade(filled ? style->accent : style->track));

            NYA_Rectf text = { body.x + (dot * 2.0F) + look->look.padding, body.y, body.width - (dot * 2.0F) - look->look.padding, body.height };
            _nya_ui_shape_text(window, widget->text, widget->label, 0.0F, text, NYA_UI_ALIGN_CENTER, _nya_ui_shape_color(&style->text));
        } break;

        case NYA_UI_WIDGET_TOGGLE:   _nya_ui_shape_toggle(window, widget); break;
        case NYA_UI_WIDGET_SLIDER:   _nya_ui_shape_slider(window, widget); break;

        case NYA_UI_WIDGET_RADIO: {
            NYA_Rectf body   = _nya_ui_shape_body(window, widget->rect);
            NYA_Color color  = _nya_ui_shape_color(&style->text);
            f32       ring   = roundf(_nya_ui_shape_item(widget->text) * 0.22F);
            f32x2     center = { body.x + look->look.padding + ring, body.y + roundf(body.height * 0.5F) };

            // a ring with a dot in it, rather than the filled dot a selectable draws, so the two never read as the same.
            nya_render2d_circle(window, center, ring, _nya_ui_shape_fade(color));
            nya_render2d_circle(window, center, ring - nya_max(_nya_ui_shape_px(2.0F), 1.0F), _nya_ui_shape_fade(style->track));

            if (widget->as_choice.on) {
                NYA_Color dot = widget->state.disabled ? style->text.disabled : style->accent;
                nya_render2d_circle(window, center, roundf(ring * 0.5F), _nya_ui_shape_fade(dot));
            }

            NYA_Rectf text = { body.x + (ring * 2.0F) + (look->look.padding * 2.0F), body.y, body.width, body.height };
            _nya_ui_shape_text(window, widget->text, widget->label, 0.0F, text, NYA_UI_ALIGN_START, color);
        } break;

        case NYA_UI_WIDGET_DROPDOWN: _nya_ui_shape_dropdown(window, widget); break;

        case NYA_UI_WIDGET_FIELD: {
            NYA_Rectf body = _nya_ui_shape_body(window, widget->rect);

            // the box follows the body when it sinks, which is why the widget hands over the one its input read.
            NYA_UIFieldDraw field = widget->as_field.field;
            field.box             = nya_rect_translate(field.box, (f32x2){ body.x - widget->rect.x, body.y - widget->rect.y });

            _nya_ui_shape_field(window, widget, &field);
            _nya_ui_shape_text(window, widget->text, widget->label, 0.0F, (NYA_Rectf){ body.x + look->look.padding, body.y, body.width, body.height },
                               NYA_UI_ALIGN_START, _nya_ui_shape_color(&style->text));
        } break;

        case NYA_UI_WIDGET_COLOR_PICKER: _nya_ui_shape_picker(window, widget); break;
        case NYA_UI_WIDGET_CHART:        _nya_ui_shape_chart(window, widget); break;

        case NYA_UI_WIDGET_ICON: {
            NYA_Color tint = widget->state.disabled ? style->text.disabled : style->text.normal;
            _nya_ui_shape_icon(window, widget->as_icon.icon, widget->rect, tint);
        } break;

        case NYA_UI_WIDGET_SECTION: {
            NYA_Rectf body  = _nya_ui_shape_body(window, widget->rect);
            NYA_Color color = _nya_ui_shape_color(&style->text);
            f32       mark  = look->look.line_heights[widget->text];

            NYA_Rectf chevron = { body.x + look->look.padding, roundf(body.y + ((body.height - mark) * 0.5F)), mark, mark };
            _nya_ui_shape_mark(window, widget->as_mark.mark, chevron, color);

            NYA_Rectf text = { body.x + (look->look.padding * 2.0F) + mark, body.y, body.width, body.height };
            _nya_ui_shape_text(window, widget->text, widget->label, 0.0F, text, NYA_UI_ALIGN_START, color);
        } break;

        case NYA_UI_WIDGET_CHROME: {
            // the fill only once it is worth seeing, so a quiet title bar is a title and three marks rather than a row
            // of buttons, and the focus mark still lands because that is drawn whatever the fill does.
            /*
             * Inset from the bar rather than filling it. A chrome square is square to the title bar so
             * that it is an easy target, and a fill that took the whole square turned the bar into a row
             * of slabs; the target stays the square and only what is drawn is smaller.
             */
            NYA_Rectf seat = nya_rect_expand(widget->rect, -roundf(widget->rect.height * 0.16F));
            NYA_Rectf body = widget->as_mark.body ? _nya_ui_shape_body(window, seat) : seat;

            _nya_ui_shape_mark(window, widget->as_mark.mark, nya_rect_expand(body, -roundf(body.height * 0.25F)), _nya_ui_shape_color(&style->text));
        } break;

        // not a button: nothing presses a grip, so it is the mark alone, at the size the corner gave it.
        case NYA_UI_WIDGET_GRIP: {
            _nya_ui_shape_mark(window, widget->as_mark.mark, widget->rect, style->text_dim);
        } break;

        case NYA_UI_WIDGET_SCROLLBAR: {
            f32 radius = nya_min(widget->rect.width, widget->rect.height) * 0.5F;
            nya_render2d_rect_rounded(window, widget->rect.x, widget->rect.y, widget->rect.width, widget->rect.height, radius, _nya_ui_shape_fade(style->text_dim));
        } break;

        case NYA_UI_WIDGET_RULE:
        case NYA_UI_WIDGET_STRIPE:
        case NYA_UI_WIDGET_UNDERLINE: {
            nya_render2d_rect(window, widget->rect.x, widget->rect.y, widget->rect.width, widget->rect.height, _nya_ui_shape_fade(widget->color));
        } break;

        case NYA_UI_WIDGET_KIND_COUNT:
        default:                       nya_unreachable();
    }

    _nya_ui_shape.opacity = 1.0F;
}

const _NYA_UIShapeLook* _nya_ui_shape_look(void) {
    return &_nya_ui_shape.looks[_nya_ui_shape.depth];
}

f32 _nya_ui_shape_px(f32 value) {
    return roundf(value * _nya_ui_shape.scale);
}

f32 _nya_ui_shape_item(NYA_UIText role) {
    const NYA_UILook* look = &_nya_ui_shape_look()->look;

    return look->item_height > 0.0F ? look->item_height : look->line_heights[role] + roundf(look->padding * 1.5F);
}

f32 _nya_ui_shape_prefix(NYA_UIText role, NYA_ConstCString text, u32 bytes) {
    nya_assert(text != nullptr);
    nya_assert(role < NYA_UI_TEXT_COUNT);
    nya_assert(bytes <= NYA_UI_TEXT_INPUT_MAX, "a field measures at most NYA_UI_TEXT_INPUT_MAX bytes, got %u", bytes);

    if (bytes == 0) return 0.0F;

    char prefix[NYA_UI_TEXT_INPUT_MAX];
    u32  size = nya_min(bytes, (u32)sizeof(prefix) - 1);

    nya_memcpy(prefix, text, size);
    prefix[size] = '\0';

    return nya_font_width(_nya_ui_shape_look()->fonts[role], prefix);
}

NYA_Color _nya_ui_shape_fade(NYA_Color color) {
    f32 opacity = _nya_ui_shape.opacity;

    nya_assert(opacity >= 0.0F && opacity <= 1.0F, "an opacity group multiplies by a share, got %f", (f64)opacity);

    color.a *= opacity;

    return color;
}

NYA_Color _nya_ui_shape_color(const NYA_UIStateColors* colors) {
    const NYA_UIWidgetState* state = &_nya_ui_shape.state;

    if (state->disabled) return colors->disabled;

    return nya_color_mix(nya_color_mix(colors->normal, colors->focused, state->focus), colors->pressed, state->press);
}

NYA_UISkin _nya_ui_shape_skin(const NYA_UIStateSkins* skins) {
    const NYA_UIWidgetState* state = &_nya_ui_shape.state;
    const NYA_UISkin*        skin  = &skins->normal;

    if (state->disabled) {
        skin = &skins->disabled;
    } else if (state->held) {
        skin = &skins->pressed;
    } else if (state->focused) {
        skin = &skins->focused;
    }

    if (skin->texture[0] != '\0' || skin->source_width > 0.0F) return *skin;

    NYA_UISkin fallback = skins->normal;
    NYA_Color  tint     = skin->tint;
    if (tint.r != 0.0F || tint.g != 0.0F || tint.b != 0.0F || tint.a != 0.0F) fallback.tint = tint;

    return fallback;
}

NYA_Font _nya_ui_shape_fit(NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow, f32* out_width) {
    const _NYA_UIShapeLook* look  = _nya_ui_shape_look();
    NYA_Font                font  = look->fonts[role];
    f32                     width = nya_font_width(font, text);

    if (overflow != NYA_UI_OVERFLOW_SHRINK || room <= 0.0F || width <= room) {
        *out_width = width;
        return font;
    }

    // whole points, so a line that changes a digit a frame does not bake an atlas a frame.
    font.point_size = nya_max(floorf(font.point_size * (room / width)), 1.0F);
    width           = nya_font_width(font, text);

    if (width > room && font.point_size > 1.0F) {
        font.point_size -= 1.0F;
        width            = nya_font_width(font, text);
    }

    // a size not loaded yet measures zero; holding the room keeps the container from collapsing for that frame.
    if (width <= 0.0F) width = room;

    *out_width = width;

    return font;
}

b8 _nya_ui_shape_skin_draw(NYA_Window* window, const NYA_UISkin* skin, NYA_Rectf rect, NYA_Color tint) {
    // a region alone is cut from the panel's sheet, so one texture skins everything.
    NYA_ConstCString texture = skin->texture;
    if (texture[0] == '\0' && skin->source_width > 0.0F && skin->source_height > 0.0F) texture = _nya_ui_shape_look()->look.style.panel_skin.texture;

    if (texture[0] == '\0') return false;

    NYA_Asset* asset = nya_asset_get((NYA_CString)texture);

    // loaded on first use, and drawn flat until it is.
    if (asset == nullptr) {
        (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = (NYA_CString)texture });
        return false;
    }

    if (asset->status != NYA_ASSET_STATUS_LOADED) return false;

    NYA_Color own = skin->tint;
    b8        set = own.r != 0.0F || own.g != 0.0F || own.b != 0.0F || own.a != 0.0F;

    nya_render2d_nine_slice(
        window,
        texture,
        (NYA_NineSlice){
            .left          = skin->left,
            .right         = skin->right,
            .top           = skin->top,
            .bottom        = skin->bottom,
            .x             = rect.x,
            .y             = rect.y,
            .width         = rect.width,
            .height        = rect.height,
            .source_x      = skin->source_x,
            .source_y      = skin->source_y,
            .source_width  = skin->source_width,
            .source_height = skin->source_height,
            .scale         = _nya_ui_shape.scale,
            .fill          = skin->tile ? NYA_NINE_SLICE_TILE : NYA_NINE_SLICE_STRETCH,
            .hollow        = skin->hollow,
            .tint          = _nya_ui_shape_fade(set ? own : tint),
        }
    );

    return true;
}

NYA_Rectf _nya_ui_shape_body(NYA_Window* window, NYA_Rectf rect) {
    const _NYA_UIShapeLook*  look  = _nya_ui_shape_look();
    const NYA_UIStyle*       style = &look->look.style;
    const NYA_UIWidgetState* state = &_nya_ui_shape.state;

    f32 grow = 0.0F;

    if (look->look.pop > 0.0F) {
        // grows at once and settles back, ease out; and once more, harder, on the click itself, out and back, so the
        // press reads as landing rather than as focus. Both are one at rest, which is how a widget says neither runs.
        f32 pop = 1.0F - state->pop;

        grow = nya_max(look->look.pop * pop * pop, look->look.pop * NYA_UI_BOUNCE * sinf(state->bounce * (f32)M_PI));
    }

    grow = roundf(grow);
    if (grow > 0.0F) rect = (NYA_Rectf){ rect.x - grow, rect.y - grow, rect.width + (grow * 2.0F), rect.height + (grow * 2.0F) };

    // held, the body sinks onto its shadow.
    if (look->look.depth > 0.0F && !state->held) {
        nya_render2d_rect_rounded(window, rect.x, rect.y + look->look.depth, rect.width, rect.height, look->look.radius, _nya_ui_shape_fade(style->ink));
    }

    if (state->held) rect.y += look->look.depth;

    NYA_UISkin skin = _nya_ui_shape_skin(&style->button_skin);
    NYA_Color  fill = _nya_ui_shape_color(&style->button);

    if (!_nya_ui_shape_skin_draw(window, &skin, rect, fill)) {
        nya_render2d_rect_rounded(window, rect.x, rect.y, rect.width, rect.height, look->look.radius, _nya_ui_shape_fade(fill));
        if (look->look.outline > 0.0F) {
            nya_render2d_rect_rounded_outline(window, rect.x, rect.y, rect.width, rect.height, look->look.radius, look->look.outline, _nya_ui_shape_fade(style->ink));
        }
    }

    // clear of the rounded corners, fading with the focus.
    if (state->focus > 0.0F) {
        f32       bar    = look->look.focus_bar;
        NYA_Color accent = { style->accent.r, style->accent.g, style->accent.b, style->accent.a * state->focus };

        nya_render2d_rect(window, rect.x + look->look.outline, rect.y + look->look.radius, bar, nya_max(rect.height - (look->look.radius * 2.0F), 0.0F),
                          _nya_ui_shape_fade(accent));
    }

    return rect;
}

void _nya_ui_shape_track(NYA_Window* window, NYA_Rectf rect, f32 radius, NYA_Color color) {
    const NYA_UILook* look = &_nya_ui_shape_look()->look;

    if (rect.width <= 0.0F || _nya_ui_shape_skin_draw(window, &look->style.track_skin, rect, color)) return;

    nya_render2d_rect_rounded(window, rect.x, rect.y, rect.width, rect.height, radius, _nya_ui_shape_fade(color));
    if (look->outline > 0.0F) {
        nya_render2d_rect_rounded_outline(window, rect.x, rect.y, rect.width, rect.height, radius, look->outline, _nya_ui_shape_fade(look->style.ink));
    }
}

void _nya_ui_shape_text(NYA_Window* window, NYA_UIText role, NYA_ConstCString text, f32 width, NYA_Rectf rect, NYA_UIAlign align, NYA_Color color) {
    const _NYA_UIShapeLook* look = _nya_ui_shape_look();

    if (text[0] == '\0') return;

    // measured here rather than handed in, since only this file knows what a glyph of this face advances by.
    if (width <= 0.0F) width = nya_font_width(look->fonts[role], text);

    f32 x = roundf(rect.x + ((rect.width - width) * (f32)align * 0.5F));
    f32 y = roundf(rect.y + ((rect.height - look->look.line_heights[role]) * 0.5F));

    nya_font_draw(window, look->fonts[role], text, x, y, _nya_ui_shape_fade(color));
}

void _nya_ui_shape_mark(NYA_Window* window, NYA_UIMark mark, NYA_Rectf rect, NYA_Color color) {
    nya_assert(mark < NYA_UI_MARK_COUNT);

    if (rect.width <= 0.0F || rect.height <= 0.0F) return;

    f32       thickness = nya_max(_nya_ui_shape_px(1.5F), 1.0F);
    NYA_Color faded     = _nya_ui_shape_fade(color);

    f32x2 top_left     = { rect.x, rect.y };
    f32x2 bottom_right = { rect.x + rect.width, rect.y + rect.height };
    f32x2 center       = { rect.x + (rect.width * 0.5F), rect.y + (rect.height * 0.5F) };

    switch (mark) {
        case NYA_UI_MARK_CLOSE: {
            nya_render2d_line(window, top_left, bottom_right, thickness, faded);
            nya_render2d_line(window, (f32x2){ bottom_right.x, top_left.y }, (f32x2){ top_left.x, bottom_right.y }, thickness, faded);
        } break;

        // a triangle rather than two lines: a chevron a cell tall in a terminal is one glyph either way, and a
        // filled one survives being rounded to cells where a stroked one falls between them.
        case NYA_UI_MARK_EXPANDED: {
            nya_render2d_triangle(window, top_left, (f32x2){ bottom_right.x, top_left.y }, (f32x2){ center.x, bottom_right.y }, faded);
        } break;

        case NYA_UI_MARK_COLLAPSED: {
            nya_render2d_triangle(window, top_left, (f32x2){ bottom_right.x, center.y }, (f32x2){ top_left.x, bottom_right.y }, faded);
        } break;

        case NYA_UI_MARK_MENU: {
            for (u32 i = 0; i < 3; i++) {
                f32 y = roundf(rect.y + (rect.height * ((f32)i * 0.5F)));

                nya_render2d_rect(window, rect.x, nya_min(y, bottom_right.y - thickness), rect.width, thickness, faded);
            }
        } break;

        // two strokes along the corner, which is what a resize grip is everywhere anyone has seen one.
        case NYA_UI_MARK_GRIP: {
            for (u32 i = 1; i < 3; i++) {
                f32 inset = rect.width * ((f32)i / 3.0F);

                nya_render2d_line(window, (f32x2){ rect.x + inset, bottom_right.y }, (f32x2){ bottom_right.x, rect.y + inset }, thickness, faded);
            }
        } break;

        case NYA_UI_MARK_COUNT:
        default:                nya_unreachable();
    }
}

void _nya_ui_shape_icon(NYA_Window* window, const NYA_UIIcon* icon, NYA_Rectf rect, NYA_Color tint) {
    nya_assert(icon != nullptr);

    const NYA_UIStyle* style = &_nya_ui_shape_look()->look.style;

    // an icon with no texture of its own is cut from the style's sheet, so one sheet holds a game's whole set.
    NYA_ConstCString texture = icon->texture != nullptr && icon->texture[0] != '\0' ? icon->texture : style->icon_sheet;
    if (texture[0] == '\0') return;

    NYA_Asset* asset = nya_asset_get((NYA_CString)texture);

    // loaded on first use, and drawn as nothing until it is.
    if (asset == nullptr) {
        (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = (NYA_CString)texture });
        return;
    }

    if (asset->status != NYA_ASSET_STATUS_LOADED) return;

    NYA_Color own = icon->tint;
    b8        set = own.r != 0.0F || own.g != 0.0F || own.b != 0.0F || own.a != 0.0F;

    nya_render2d_texture_rect(window, texture, icon->source_x, icon->source_y, icon->source_width, icon->source_height, rect.x, rect.y, rect.width, rect.height,
                              _nya_ui_shape_fade(set ? own : tint));
}

void _nya_ui_shape_panel(NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    const _NYA_UIShapeLook* look   = _nya_ui_shape_look();
    const NYA_UIStyle*      style  = &look->look.style;
    const NYA_UIPanel*      panel  = widget->as_panel.options;
    NYA_Rectf               bounds = widget->rect;

    nya_assert(panel != nullptr, "a panel is drawn from the options that opened it");

    if (!panel->frameless) {
        NYA_Color fill    = panel->fill;
        b8        colored = fill.r != 0.0F || fill.g != 0.0F || fill.b != 0.0F || fill.a != 0.0F;

        if (!colored) fill = style->panel;

        if (!_nya_ui_shape_skin_draw(window, &style->panel_skin, bounds, fill)) {
            if (look->look.depth > 0.0F) {
                nya_render2d_rect_rounded(window, bounds.x, bounds.y + look->look.depth, bounds.width, bounds.height, look->look.radius, _nya_ui_shape_fade(style->ink));
            }

            nya_render2d_rect_rounded(window, bounds.x, bounds.y, bounds.width, bounds.height, look->look.radius, _nya_ui_shape_fade(fill));

            if (look->look.outline > 0.0F) {
                nya_render2d_rect_rounded_outline(window, bounds.x, bounds.y, bounds.width, bounds.height, look->look.radius, look->look.outline,
                                                  _nya_ui_shape_fade(style->ink));
            }
        }
    }

    if (widget->label[0] != '\0') {
        /*
         * Centred in what the chrome leaves rather than across the whole bar, and dropped to the largest
         * size that fits in it. A title centred across the whole width slides under the close button as
         * soon as it is long enough, which is the window that looks broken; shrinking is what a label
         * with no room does here too, so a window behaves like the rest of the UI.
         */
        f32 left  = bounds.x + widget->as_panel.inset.x + widget->as_panel.title_room.x;
        f32 right = bounds.x + bounds.width - widget->as_panel.inset.x - widget->as_panel.title_room.y;
        f32 room  = nya_max(right - left, 0.0F);

        f32      width = 0.0F;
        NYA_Font title = _nya_ui_shape_fit(NYA_UI_TEXT_TITLE, widget->label, room, NYA_UI_OVERFLOW_SHRINK, &width);

        f32 x = roundf(left + ((room - width) * 0.5F));
        f32 y = bounds.y + widget->as_panel.inset.y;

        // centred down the bar rather than sitting on its top edge, at whatever size it ended up.
        if (widget->as_panel.bar > 0.0F) {
            f32 line = nya_font_valid(title) ? ceilf(nya_font_metrics(title).line_height) : look->look.line_heights[NYA_UI_TEXT_TITLE];

            y += roundf((widget->as_panel.bar - line) * 0.5F);
        }

        // and never before the room it was given, so a title with nowhere to go starts where the bar does.
        x = nya_max(x, left);

        if (look->look.depth > 0.0F) nya_font_draw(window, title, widget->label, x, y + roundf(look->look.depth * 0.5F), _nya_ui_shape_fade(style->ink));
        nya_font_draw(window, title, widget->label, x, y, _nya_ui_shape_fade(style->text.normal));
    }
}

void _nya_ui_shape_label(NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    const _NYA_UIShapeLook* look  = _nya_ui_shape_look();
    NYA_Rectf               rect  = widget->rect;
    f32                     width = 0.0F;
    NYA_Font                font  = _nya_ui_shape_fit(widget->text, widget->label, widget->as_label.room, widget->as_label.overflow, &width);

    if (widget->as_label.overflow == NYA_UI_OVERFLOW_WRAP && widget->as_label.room > 0.0F && width > widget->as_label.room) {
        // NYA_UIAlign and NYA_TextAlign share their order.
        nya_font_draw_wrapped(window, font, widget->label, rect.x, rect.y, rect.width, (NYA_TextAlign)widget->as_label.align, _nya_ui_shape_fade(widget->color));
        return;
    }

    // a shrunk line keeps the line height of its size, centred in it.
    f32 line = look->look.line_heights[widget->text];
    f32 y    = font.point_size != look->fonts[widget->text].point_size ? roundf(rect.y + ((line - nya_font_metrics(font).line_height) * 0.5F)) : rect.y;

    nya_font_draw(window, font, widget->label, rect.x, y, _nya_ui_shape_fade(widget->color));
}

void _nya_ui_shape_toggle(NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    const _NYA_UIShapeLook* look  = _nya_ui_shape_look();
    const NYA_UIStyle*      style = &look->look.style;

    NYA_Rectf body  = _nya_ui_shape_body(window, widget->rect);
    NYA_Color color = _nya_ui_shape_color(&style->text);

    f32 pill_height = roundf(_nya_ui_shape_item(widget->text) * 0.5F);
    f32 pill_width  = roundf(pill_height * 1.8F);

    NYA_Rectf pill = { body.x + body.width - look->look.padding - pill_width, roundf(body.y + ((body.height - pill_height) * 0.5F)), pill_width, pill_height };
    _nya_ui_shape_track(window, pill, pill_height * 0.5F, widget->as_choice.on && !widget->state.disabled ? style->accent : style->track);

    f32       radius = (pill_height * 0.5F) - nya_max(look->look.outline, _nya_ui_shape_px(2.0F));
    f32x2     center = { widget->as_choice.on ? pill.x + pill_width - (pill_height * 0.5F) : pill.x + (pill_height * 0.5F), pill.y + (pill_height * 0.5F) };
    NYA_Rectf knob   = { center.x - radius, center.y - radius, radius * 2.0F, radius * 2.0F };

    NYA_UISkin skin = style->knob_skin;
    if (!_nya_ui_shape_skin_draw(window, &skin, knob, color)) nya_render2d_circle(window, center, radius, _nya_ui_shape_fade(color));

    // text after every shape, so the shapes of the whole menu can share a draw call.
    _nya_ui_shape_text(window, widget->text, widget->label, 0.0F, (NYA_Rectf){ body.x + look->look.padding, body.y, body.width, body.height }, NYA_UI_ALIGN_START, color);
}

void _nya_ui_shape_slider(NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    const _NYA_UIShapeLook* look  = _nya_ui_shape_look();
    const NYA_UIStyle*      style = &look->look.style;

    NYA_Rectf body  = _nya_ui_shape_body(window, widget->rect);
    NYA_Color color = _nya_ui_shape_color(&style->text);

    f32 radius = roundf(_nya_ui_shape_item(widget->text) * 0.2F);
    f32 t      = widget->as_slider.t;

    // the track the input pass read, moved with the body when a held widget sinks.
    NYA_Rectf track = {
        widget->as_slider.track.x + (body.x - widget->rect.x),
        roundf(body.y + ((body.height - radius) * 0.5F)),
        widget->as_slider.track.width,
        radius,
    };

    _nya_ui_shape_track(window, track, radius * 0.5F, style->track);
    _nya_ui_shape_track(window, (NYA_Rectf){ track.x, track.y, roundf(track.width * t), radius }, radius * 0.5F, widget->state.disabled ? style->track : style->accent);

    f32x2     center = { track.x + roundf(track.width * t), track.y + (radius * 0.5F) };
    NYA_Rectf knob   = { center.x - radius, center.y - radius, radius * 2.0F, radius * 2.0F };

    NYA_UISkin skin = style->knob_skin;
    if (!_nya_ui_shape_skin_draw(window, &skin, knob, color)) {
        if (look->look.outline > 0.0F) nya_render2d_circle(window, center, radius + look->look.outline, _nya_ui_shape_fade(style->ink));
        nya_render2d_circle(window, center, radius, _nya_ui_shape_fade(color));
    }

    _nya_ui_shape_text(window, widget->text, widget->label, 0.0F, (NYA_Rectf){ body.x + look->look.padding, body.y, body.width, body.height }, NYA_UI_ALIGN_START, color);
}

void _nya_ui_shape_dropdown(NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    const _NYA_UIShapeLook* look  = _nya_ui_shape_look();
    const NYA_UIStyle*      style = &look->look.style;

    NYA_Rectf body  = _nya_ui_shape_body(window, widget->rect);
    NYA_Color color = _nya_ui_shape_color(&style->text);
    f32       arrow = roundf(_nya_ui_shape_item(widget->text) * 0.18F);

    _nya_ui_shape_text(window, widget->text, widget->label, 0.0F, (NYA_Rectf){ body.x + look->look.padding, body.y, body.width, body.height }, NYA_UI_ALIGN_START, color);

    NYA_Rectf value = { body.x, body.y, body.width - (look->look.padding * 2.0F) - (arrow * 2.0F), body.height };
    _nya_ui_shape_text(window, widget->text, widget->as_dropdown.shown, 0.0F, value, NYA_UI_ALIGN_END, color);

    // a caret, pointing down when closed and up when the list is showing.
    f32   x    = body.x + body.width - look->look.padding - arrow;
    f32   y    = body.y + roundf(body.height * 0.5F);
    f32   tip  = widget->as_dropdown.open ? -arrow : arrow;
    f32x2 left = { x - arrow, y - (tip * 0.5F) };

    nya_render2d_triangle(window, left, (f32x2){ x + arrow, y - (tip * 0.5F) }, (f32x2){ x, y + (tip * 0.5F) }, _nya_ui_shape_fade(color));
}

void _nya_ui_shape_field(NYA_Window* window, const NYA_UIWidgetDraw* widget, const NYA_UIFieldDraw* field) {
    const _NYA_UIShapeLook* look  = _nya_ui_shape_look();
    const NYA_UIStyle*      style = &look->look.style;

    NYA_Rectf        box    = field->box;
    NYA_ConstCString buffer = field->buffer;
    NYA_UIText       role   = widget->text;
    NYA_Font         font   = look->fonts[role];
    NYA_Color        color  = _nya_ui_shape_color(&style->text);

    nya_assert(buffer != nullptr && field->composing != nullptr, "a field is drawn from a buffer and a composition, both of which may be empty but not absent");

    u32 length = (u32)strlen(buffer);
    f32 line   = look->look.line_heights[role];
    f32 margin = roundf(look->look.padding * 0.5F);
    f32 mark   = _nya_ui_shape_px(2.0F);

    _nya_ui_shape_track(window, box, look->look.radius, style->track);

    // the line runs past its box when the caret is far along it, so the glyphs are cut to the box and put back.
    _nya_ui_shape_clip_set(&_nya_ui_shape, window, nya_rect_intersection(widget->clip, box), false);

    f32 x = box.x + margin - field->shift;
    f32 y = roundf(box.y + ((box.height - line) * 0.5F));

    if (field->editing) {
        nya_render2d_rect(window, box.x, box.y + box.height - mark, box.width, mark, _nya_ui_shape_fade(style->accent));

        // the selection sits behind the glyphs so the text reads through it.
        u32 from = nya_min(field->caret, field->select);
        u32 to   = nya_max(field->caret, field->select);

        if (to > from) {
            f32       left      = _nya_ui_shape_prefix(role, buffer, nya_min(from, length));
            f32       right     = _nya_ui_shape_prefix(role, buffer, nya_min(to, length));
            NYA_Color highlight = { style->accent.r, style->accent.g, style->accent.b, style->accent.a * NYA_UI_SELECTION_ALPHA };

            nya_render2d_rect(window, x + left, y, nya_max(right - left, mark), line, _nya_ui_shape_fade(highlight));
        }
    }

    nya_font_draw(window, font, buffer, x, y, _nya_ui_shape_fade(color));

    if (field->editing) {
        f32              caret_x   = _nya_ui_shape_prefix(role, buffer, nya_min(field->caret, length));
        NYA_ConstCString composing = field->composing;

        // the IME's unfinished text sits at the caret, dimmed, and the caret after it.
        if (composing[0] != '\0') {
            nya_font_draw(window, font, composing, x + caret_x, y, _nya_ui_shape_fade(style->text_dim));

            // the run the IME has selected, the candidate being chosen, is underlined as the part that will change.
            if (field->composing_to > field->composing_from) {
                f32 left  = _nya_ui_shape_prefix(role, composing, field->composing_from);
                f32 right = _nya_ui_shape_prefix(role, composing, field->composing_to);

                nya_render2d_rect(window, x + caret_x + left, y + line - mark, right - left, mark, _nya_ui_shape_fade(style->accent));
            }

            caret_x += roundf(nya_font_width(font, composing));
        }

        if (fmod(nya_app_uptime_s(), NYA_UI_CARET_BLINK_S * 2.0) < NYA_UI_CARET_BLINK_S) {
            nya_render2d_rect(window, roundf(x + caret_x), y, mark, line, _nya_ui_shape_fade(color));
        }
    }

    _nya_ui_shape_clip_set(&_nya_ui_shape, window, widget->clip, widget->clip_whole);
}

void _nya_ui_shape_picker(NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    const _NYA_UIShapeLook* look  = _nya_ui_shape_look();
    const NYA_UIStyle*      style = &look->look.style;

    NYA_Rectf body = _nya_ui_shape_body(window, widget->rect);
    f32x2     sink = { body.x - widget->rect.x, body.y - widget->rect.y };

    NYA_Rectf p = nya_rect_translate(widget->as_picker.plane, sink);
    NYA_Rectf h = nya_rect_translate(widget->as_picker.hue, sink);
    NYA_Rectf a = nya_rect_translate(widget->as_picker.alpha, sink);
    NYA_Rectf w = nya_rect_translate(widget->as_picker.swatch, sink);

    NYA_ColorHSV hsv   = widget->as_picker.hsv;
    NYA_Color    value = widget->as_picker.value;

    // faded up front, since every one of these goes straight into a gradient rather than through a draw helper.
    NYA_Color pure  = _nya_ui_shape_fade(nya_color_from_hsv((NYA_ColorHSV){ hsv.h, 1.0F, 1.0F, 1.0F }));
    NYA_Color white = _nya_ui_shape_fade((NYA_Color){ 1.0F, 1.0F, 1.0F, 1.0F });
    NYA_Color black = _nya_ui_shape_fade((NYA_Color){ 0.0F, 0.0F, 0.0F, 1.0F });
    NYA_Color clear = { 0.0F, 0.0F, 0.0F, 0.0F };
    NYA_Color solid = _nya_ui_shape_fade((NYA_Color){ value.r, value.g, value.b, 1.0F });
    NYA_Color faded = { value.r, value.g, value.b, 0.0F };

    nya_render2d_rect_gradient(window, p.x, p.y, p.width, p.height, (NYA_Color[4]){ white, pure, pure, white });
    nya_render2d_rect_gradient(window, p.x, p.y, p.width, p.height, (NYA_Color[4]){ clear, clear, black, black });

    // six bands of hue, red at both ends.
    for (u32 i = 0; i < 6; i++) {
        NYA_Color top    = nya_color_from_hsv((NYA_ColorHSV){ (f32)i * 60.0F, 1.0F, 1.0F, 1.0F });
        NYA_Color bottom = nya_color_from_hsv((NYA_ColorHSV){ (f32)((i + 1) % 6) * 60.0F, 1.0F, 1.0F, 1.0F });
        f32       y0     = roundf(h.y + (h.height * (f32)i / 6.0F));
        f32       y1     = roundf(h.y + (h.height * (f32)(i + 1) / 6.0F));

        nya_render2d_rect_gradient(window, h.x, y0, h.width, y1 - y0, (NYA_Color[4]){ top, top, bottom, bottom });
    }

    _nya_ui_shape_track(window, a, 0.0F, style->track);
    nya_render2d_rect_gradient(window, a.x, a.y, a.width, a.height, (NYA_Color[4]){ faded, solid, solid, faded });

    _nya_ui_shape_track(window, w, look->look.radius, style->track);
    nya_render2d_rect(window, w.x, w.y, w.width, w.height, _nya_ui_shape_fade(value));

    // where each part stands: a ring on the field, and a notch across each bar.
    f32   mark   = _nya_ui_shape_px(2.0F);
    f32x2 center = { roundf(p.x + (p.width * hsv.s)), roundf(p.y + (p.height * (1.0F - hsv.v))) };

    nya_render2d_circle(window, center, mark * 3.0F, _nya_ui_shape_fade(style->text.normal));
    nya_render2d_circle(window, center, mark * 2.0F, _nya_ui_shape_fade(solid));
    nya_render2d_rect(window, h.x - mark, roundf(h.y + (h.height * hsv.h / 360.0F)) - mark, h.width + (mark * 2.0F), mark * 2.0F, _nya_ui_shape_fade(style->text.normal));
    nya_render2d_rect(window, roundf(a.x + (a.width * hsv.a)) - mark, a.y - mark, mark * 2.0F, a.height + (mark * 2.0F), _nya_ui_shape_fade(style->text.normal));

    NYA_UIFieldDraw field = widget->as_picker.field;
    field.box             = nya_rect_translate(field.box, sink);

    _nya_ui_shape_field(window, widget, &field);

    // the label last, over the body, centred in the picker's top row rather than in the whole of it.
    NYA_Rectf text = { body.x + look->look.padding, body.y, body.width, _nya_ui_shape_item(widget->text) };
    _nya_ui_shape_text(window, widget->text, widget->label, 0.0F, text, NYA_UI_ALIGN_START, _nya_ui_shape_color(&style->text));
}

void _nya_ui_shape_chart(NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    const _NYA_UIShapeLook* look  = _nya_ui_shape_look();
    const NYA_UIChart*      chart = widget->as_chart.chart;
    NYA_Rectf               rect  = widget->rect;

    nya_assert(chart != nullptr, "a chart is drawn from the values that declared it");

    _nya_ui_shape_track(window, rect, look->look.radius, look->look.style.track);

    // the newest points, when there are more than the plot takes; a chart of a history shows its end.
    u32        count  = nya_min(chart->count, (u32)NYA_UI_CHART_POINTS_MAX);
    const f32* values = chart->values + (chart->count - count);

    if (count == 0) return;

    f32 low  = chart->min;
    f32 high = chart->max;

    if (high <= low) {
        low  = values[0];
        high = values[0];

        for (u32 i = 1; i < count; i++) {
            low  = nya_min(low, values[i]);
            high = nya_max(high, values[i]);
        }
    }

    // a flat series would divide by zero and, worse, draw a line at a meaningless height; it sits on the floor.
    f32 span = high - low;
    if (span <= 0.0F) span = 1.0F;

    NYA_Rectf plot = nya_rect_expand(rect, -_nya_ui_shape_px(2.0F));
    if (plot.width <= 0.0F || plot.height <= 0.0F) return;

    NYA_Color color = widget->color;

    if (chart->kind == NYA_UI_CHART_BAR) {
        f32 slot = plot.width / (f32)count;
        f32 bar  = nya_max(roundf(slot * NYA_UI_CHART_BAR_SHARE), 1.0F);

        for (u32 i = 0; i < count; i++) {
            f32 t = nya_clamp((values[i] - low) / span, 0.0F, 1.0F);
            f32 h = roundf(plot.height * t);

            nya_render2d_rect(window, roundf(plot.x + ((f32)i * slot)), plot.y + plot.height - h, bar, h, _nya_ui_shape_fade(color));
        }

        return;
    }

    // one point is a dot, not a line, and polyline wants at least two.
    f32   step = count > 1 ? plot.width / (f32)(count - 1) : 0.0F;
    f32x2 points[NYA_UI_CHART_POINTS_MAX];

    for (u32 i = 0; i < count; i++) {
        f32 t = nya_clamp((values[i] - low) / span, 0.0F, 1.0F);

        points[i] = (f32x2){ plot.x + ((f32)i * step), plot.y + plot.height - roundf(plot.height * t) };
    }

    if (count == 1) {
        nya_render2d_circle(window, points[0], _nya_ui_shape_px(2.0F), _nya_ui_shape_fade(color));
        return;
    }

    nya_render2d_polyline(window, points, count, nya_max(_nya_ui_shape_px(NYA_UI_CHART_LINE_WIDTH), 1.0F), _nya_ui_shape_fade(color));
}

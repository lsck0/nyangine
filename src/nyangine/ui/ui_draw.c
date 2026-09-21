/**
 * @file ui_draw.c
 *
 * Drawing: what a draw pass is allowed to draw, the scissor it draws through, and the shapes every widget is
 * built from. See ui.h.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DRAWING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_ui_scrim(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);

    if (ui->pass != NYA_UI_PASS_DRAW) return;

    nya_render2d_rect(ui->window, 0.0F, 0.0F, (f32)ui->window->screen_width, (f32)ui->window->screen_height, _nya_ui_fade(_nya_ui_look()->style.scrim));
}

void nya_ui_opacity_begin(NYA_UI* ui, f32 opacity) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.opacity_depth < NYA_UI_OPACITY_DEPTH_MAX, "opacity groups nest deeper than NYA_UI_OPACITY_DEPTH_MAX");

    f32 clamped = nya_clamp(opacity, 0.0F, 1.0F);

    // the product, so a faded group inside a faded one fades twice rather than the inner one winning.
    _nya_ui.opacity_depth                       += 1;
    _nya_ui.opacities[_nya_ui.opacity_depth]     = _nya_ui.opacities[_nya_ui.opacity_depth - 1] * clamped;
}

void nya_ui_opacity_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.opacity_depth > 0, "nya_ui_opacity_end without a begin");

    _nya_ui.opacity_depth -= 1;
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_ui_drawing(void) {
    return _nya_ui.open->pass == NYA_UI_PASS_DRAW && !_nya_ui.layouts[_nya_ui.depth - 1].hidden;
}

b8 _nya_ui_drawn(NYA_Rectf rect) {
    if (!_nya_ui_drawing()) return false;

    // a list scrolled far past a widget draws nothing for it.
    const _NYA_UILook* look  = _nya_ui_look();
    NYA_Rectf          reach = nya_rect_expand(rect, look->pop + look->depth + 1.0F);

    return nya_rect_overlaps(reach, _nya_ui.layouts[_nya_ui.depth - 1].clip);
}

s32 _nya_ui_layer_get(const NYA_UI* ui) {
    nya_assert(ui != nullptr && ui->window != nullptr);

    return nya_render2d_layer(ui->window);
}

void _nya_ui_layer_set(const NYA_UI* ui, s32 layer) {
    nya_assert(ui != nullptr && ui->window != nullptr);

    // only a draw pass has geometry to order, and an input pass must not touch the renderer at all.
    if (ui->pass != NYA_UI_PASS_DRAW) return;

    nya_render2d_layer_set(ui->window, layer);
}

void _nya_ui_scissor(const NYA_UI* ui, NYA_Rectf clip) {
    const NYA_Rectf* screen = &_nya_ui.layouts[0].clip;

    if (clip.x == screen->x && clip.y == screen->y && clip.width == screen->width && clip.height == screen->height) {
        nya_render2d_scissor_end(ui->window);
        return;
    }

    nya_render2d_scissor_begin(ui->window, clip.x, clip.y, clip.width, clip.height);
}

b8 _nya_ui_skin_draw(const NYA_UI* ui, const NYA_UISkin* skin, NYA_Rectf rect, NYA_Color tint) {
    // a region alone is cut from the panel's sheet, so one texture skins everything.
    NYA_ConstCString texture = skin->texture;
    if (texture[0] == '\0' && skin->source_width > 0.0F && skin->source_height > 0.0F) texture = _nya_ui_look()->style.panel_skin.texture;

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
        ui->window,
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
            .scale         = ui->scale,
            .fill          = skin->tile ? NYA_NINE_SLICE_TILE : NYA_NINE_SLICE_STRETCH,
            .hollow        = skin->hollow,
            .tint          = _nya_ui_fade(set ? own : tint),
        }
    );

    return true;
}

NYA_Rectf _nya_ui_body_draw(NYA_UI* ui, NYA_Rectf rect, _NYA_UIWidget widget) {
    const _NYA_UILook* look  = _nya_ui_look();
    const NYA_UIStyle* style = &look->style;

    f32 grow = 0.0F;
    f64 now  = nya_app_uptime_s();

    if (widget.focused && look->pop > 0.0F) {
        // grows at once and settles back, ease out.
        f32 t = nya_clamp((f32)(now - ui->focus_changed_s) / NYA_UI_POP_S, 0.0F, 1.0F);
        grow  = look->pop * (1.0F - t) * (1.0F - t);
    }

    // and once more, harder, on the click itself: out and back, so the press reads as landing rather than as focus.
    if (ui->bounce_id == widget.id && look->pop > 0.0F) {
        f32 t = nya_clamp((f32)(now - ui->bounce_s) / NYA_UI_BOUNCE_S, 0.0F, 1.0F);
        grow  = nya_max(grow, look->pop * NYA_UI_BOUNCE * sinf(t * (f32)M_PI));
    }

    grow = roundf(grow);
    if (grow > 0.0F) rect = (NYA_Rectf){ rect.x - grow, rect.y - grow, rect.width + (grow * 2.0F), rect.height + (grow * 2.0F) };

    // held, the body sinks onto its shadow.
    if (look->depth > 0.0F && !widget.held) {
        nya_render2d_rect_rounded(ui->window, rect.x, rect.y + look->depth, rect.width, rect.height, look->radius, _nya_ui_fade(style->ink));
    }

    if (widget.held) rect.y += look->depth;

    NYA_UISkin skin = _nya_ui_skin(&style->button_skin, widget);

    NYA_Color fill = _nya_ui_color(&style->button, widget);

    if (!_nya_ui_skin_draw(ui, &skin, rect, fill)) {
        nya_render2d_rect_rounded(ui->window, rect.x, rect.y, rect.width, rect.height, look->radius, _nya_ui_fade(fill));
        if (look->outline > 0.0F) {
            nya_render2d_rect_rounded_outline(ui->window, rect.x, rect.y, rect.width, rect.height, look->radius, look->outline, _nya_ui_fade(style->ink));
        }
    }

    // clear of the rounded corners, fading with the focus.
    if (widget.focus > 0.0F) {
        f32       bar    = look->focus_bar;
        NYA_Color accent = { style->accent.r, style->accent.g, style->accent.b, style->accent.a * widget.focus };

        nya_render2d_rect(ui->window, rect.x + look->outline, rect.y + look->radius, bar, nya_max(rect.height - (look->radius * 2.0F), 0.0F), _nya_ui_fade(accent));
    }

    return rect;
}

void _nya_ui_track_draw(NYA_UI* ui, NYA_Rectf rect, f32 radius, NYA_Color color) {
    const _NYA_UILook* look = _nya_ui_look();

    if (rect.width <= 0.0F || _nya_ui_skin_draw(ui, &look->style.track_skin, rect, color)) return;

    nya_render2d_rect_rounded(ui->window, rect.x, rect.y, rect.width, rect.height, radius, _nya_ui_fade(color));
    if (look->outline > 0.0F) {
        nya_render2d_rect_rounded_outline(ui->window, rect.x, rect.y, rect.width, rect.height, radius, look->outline, _nya_ui_fade(look->style.ink));
    }
}

void _nya_ui_text_draw(NYA_UI* ui, const _NYA_UILayout* layout, NYA_ConstCString text, f32 width, NYA_Rectf rect, NYA_UIAlign align, NYA_Color color) {
    const _NYA_UILook* look = _nya_ui_look();

    f32 x = roundf(rect.x + ((rect.width - width) * (f32)align * 0.5F));
    f32 y = roundf(rect.y + ((rect.height - look->line_heights[layout->text]) * 0.5F));

    nya_font_draw(ui->window, look->fonts[layout->text], text, x, y, _nya_ui_fade(color));
}

NYA_Color _nya_ui_fade(NYA_Color color) {
    f32 opacity = _nya_ui.opacities[_nya_ui.opacity_depth];

    nya_assert(opacity >= 0.0F && opacity <= 1.0F, "an opacity group multiplies by a share, got %f", (f64)opacity);

    color.a *= opacity;

    return color;
}

void _nya_ui_scrollbar_draw(NYA_UI* ui, const _NYA_UILayout* layout, u32 axis) {
    nya_assert(ui != nullptr && layout != nullptr);
    nya_assert(axis < 2, "a scrollbar is on one of two axes, got %u", axis);

    const _NYA_UILook* look   = _nya_ui_look();
    const NYA_Rectf*   bounds = &layout->bounds;

    // along the container's direction the content is what the children took; across it, the widest of them.
    f32 content = axis == layout->main ? layout->used : layout->across;

    f32 bar   = _nya_ui_px(NYA_UI_SCROLLBAR);
    f32 track = layout->extent[axis];
    f32 total = nya_max(content, track);
    f32 thumb = nya_max(roundf(track * (track / total)), bar * 4.0F);
    f32 t     = total > track ? nya_clamp(layout->scroll[axis] / (total - track), 0.0F, 1.0F) : 0.0F;

    // along the axis it scrolls, in the padding after the content on the other one.
    f32x2 position = { 0.0F, 0.0F };
    f32x2 size     = { 0.0F, 0.0F };

    position[axis]     = roundf(layout->origin[axis] + layout->scroll[axis] + ((track - thumb) * t));
    position[1 - axis] = axis == 0 ? roundf(bounds->y + bounds->height - ((layout->after.y + bar) * 0.5F))
                                   : roundf(bounds->x + bounds->width - ((layout->after.x + bar) * 0.5F));

    size[axis]     = thumb;
    size[1 - axis] = bar;

    nya_render2d_rect_rounded(ui->window, position.x, position.y, size.x, size.y, bar * 0.5F, _nya_ui_fade(look->style.text_dim));
}

b8 _nya_ui_icon_draw(const NYA_UI* ui, const NYA_UIIcon* icon, NYA_Rectf rect, NYA_Color tint) {
    nya_assert(ui != nullptr && icon != nullptr);

    const NYA_UIStyle* style = &_nya_ui_look()->style;

    // an icon with no texture of its own is cut from the style's sheet, so one sheet holds a game's whole set.
    NYA_ConstCString texture = icon->texture != nullptr && icon->texture[0] != '\0' ? icon->texture : style->icon_sheet;
    if (texture[0] == '\0') return false;

    NYA_Asset* asset = nya_asset_get((NYA_CString)texture);

    // loaded on first use, and drawn as nothing until it is.
    if (asset == nullptr) {
        (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = (NYA_CString)texture });
        return false;
    }

    if (asset->status != NYA_ASSET_STATUS_LOADED) return false;

    NYA_Color own = icon->tint;
    b8        set = own.r != 0.0F || own.g != 0.0F || own.b != 0.0F || own.a != 0.0F;

    nya_render2d_texture_rect(ui->window, texture, icon->source_x, icon->source_y, icon->source_width, icon->source_height, rect.x, rect.y, rect.width,
                              rect.height, _nya_ui_fade(set ? own : tint));

    return true;
}

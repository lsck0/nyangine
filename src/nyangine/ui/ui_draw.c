/**
 * @file ui_draw.c
 *
 * What a draw pass is allowed to draw, the clip it draws through, and the opacity it draws at. The shapes
 * themselves belong to whichever presenter is installed; see ui_present.h. See ui.h.
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

    NYA_UIWidgetDraw widget = {
        .kind = NYA_UI_WIDGET_SCRIM,
        .rect = { 0.0F, 0.0F, (f32)ui->window->screen_width, (f32)ui->window->screen_height },
    };

    _nya_ui_draw(ui, &widget);
}

void nya_ui_opacity_begin(NYA_UI* ui, f32 opacity) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.opacity_depth < NYA_UI_OPACITY_DEPTH_MAX, "opacity groups nest deeper than NYA_UI_OPACITY_DEPTH_MAX");

    f32 clamped = nya_clamp(opacity, 0.0F, 1.0F);

    // the product, so a faded group inside a faded one fades twice rather than the inner one winning.
    _nya_ui.opacity_depth                   += 1;
    _nya_ui.opacities[_nya_ui.opacity_depth] = _nya_ui.opacities[_nya_ui.opacity_depth - 1] * clamped;
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
    const NYA_UILook* look  = _nya_ui_look();
    NYA_Rectf         reach = nya_rect_expand(rect, look->pop + look->depth + 1.0F);

    return nya_rect_overlaps(reach, _nya_ui.layouts[_nya_ui.depth - 1].clip);
}

s32 _nya_ui_layer_get(const NYA_UI* ui) {
    nya_assert(ui != nullptr && ui->window != nullptr && ui->present != nullptr);

    return ui->present->layer_get(ui->present->state, ui->window);
}

void _nya_ui_layer_set(const NYA_UI* ui, s32 layer) {
    nya_assert(ui != nullptr && ui->window != nullptr);

    // only a draw pass has geometry to order, and an input pass must not touch the backend at all.
    if (ui->pass != NYA_UI_PASS_DRAW) return;

    ui->present->layer_set(ui->present->state, ui->window, layer);
}

void _nya_ui_scissor(const NYA_UI* ui, NYA_Rectf clip) {
    const NYA_Rectf* screen = &_nya_ui.layouts[0].clip;

    b8 whole = clip.x == screen->x && clip.y == screen->y && clip.width == screen->width && clip.height == screen->height;

    ui->present->clip_set(ui->present->state, ui->window, clip, whole);
}

void _nya_ui_scrollbar_draw(NYA_UI* ui, const _NYA_UILayout* layout, u32 axis) {
    nya_assert(ui != nullptr && layout != nullptr);
    nya_assert(axis < 2, "a scrollbar is on one of two axes, got %u", axis);

    const NYA_Rectf* bounds = &layout->bounds;

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

    NYA_UIWidgetDraw widget = {
        .kind = NYA_UI_WIDGET_SCROLLBAR,
        .rect = { position.x, position.y, size.x, size.y },
    };

    _nya_ui_draw(ui, &widget);
}

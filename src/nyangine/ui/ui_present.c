/**
 * @file ui_present.c
 *
 * The seam itself: which presenter a window's passes go through, and the few calls every widget makes into it. The
 * backends are the ui_present_*.c files beside this one. See ui_present.h.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE SEAM
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_ui_presenter_set(NYA_Window* window, const NYA_UIPresenter* presenter) {
    nya_assert(window != nullptr);
    nya_assert(_nya_ui.open == nullptr, "a presenter was swapped inside an open pass, which would measure its two halves differently");

    if (presenter == nullptr) presenter = nya_ui_presenter_shape();

    // refused here rather than at the first pass that happens to reach the missing one.
    nya_assert(presenter->look_build != nullptr && presenter->look_use != nullptr && presenter->measure != nullptr && presenter->measure_bytes != nullptr &&
                   presenter->clip_set != nullptr && presenter->layer_get != nullptr && presenter->layer_set != nullptr && presenter->draw != nullptr,
               "the presenter \"%s\" left a call unimplemented", presenter->name != nullptr ? presenter->name : "?");

    _nya_ui_context(window)->present = presenter;
}

const NYA_UIPresenter* nya_ui_presenter_get(const NYA_Window* window) {
    nya_assert(window != nullptr);

    return _nya_ui_context(window)->present;
}

NYA_ConstCString nya_ui_widget_kind_name(NYA_UIWidgetKind kind) {
    switch (kind) {
        case NYA_UI_WIDGET_SCRIM:        return "scrim";
        case NYA_UI_WIDGET_PANEL:        return "panel";
        case NYA_UI_WIDGET_LABEL:        return "label";
        case NYA_UI_WIDGET_BUTTON:       return "button";
        case NYA_UI_WIDGET_SELECTABLE:   return "selectable";
        case NYA_UI_WIDGET_TOGGLE:       return "toggle";
        case NYA_UI_WIDGET_SLIDER:       return "slider";
        case NYA_UI_WIDGET_RADIO:        return "radio";
        case NYA_UI_WIDGET_DROPDOWN:     return "dropdown";
        case NYA_UI_WIDGET_FIELD:        return "field";
        case NYA_UI_WIDGET_COLOR_PICKER: return "color_picker";
        case NYA_UI_WIDGET_CHART:        return "chart";
        case NYA_UI_WIDGET_ICON:         return "icon";
        case NYA_UI_WIDGET_SECTION:      return "section";
        case NYA_UI_WIDGET_CHROME:       return "chrome";
        case NYA_UI_WIDGET_GRIP:         return "grip";
        case NYA_UI_WIDGET_SCROLLBAR:    return "scrollbar";
        case NYA_UI_WIDGET_RULE:         return "rule";
        case NYA_UI_WIDGET_STRIPE:       return "stripe";
        case NYA_UI_WIDGET_UNDERLINE:    return "underline";

        case NYA_UI_WIDGET_KIND_COUNT:
        default:                         nya_unreachable();
    }
}

void nya_ui_look_scale(const NYA_UIStyle* style, f32 scale, NYA_UILook* out) {
    nya_assert(style != nullptr && out != nullptr);
    nya_assert(scale > 0.0F, "a look is built at a positive scale, got %f", (f64)scale);

    *out = (NYA_UILook){
        .style       = *style,
        .margin      = roundf(style->margin * scale),
        .padding     = roundf(style->padding * scale),
        .spacing     = roundf(style->spacing * scale),
        .radius      = roundf(style->radius * scale),
        .outline     = roundf(style->outline * scale),
        .depth       = roundf(style->depth * scale),
        .pop         = roundf(style->pop * scale),
        .focus_bar   = nya_max(roundf(style->focus_bar * scale), 1.0F),
        .item_height = roundf(style->item_height * scale),
    };
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

const NYA_UIPresenter* _nya_ui_present(void) {
    nya_assert(_nya_ui.open != nullptr && _nya_ui.open->present != nullptr);

    return _nya_ui.open->present;
}

NYA_UIWidgetState _nya_ui_state(const NYA_UI* ui, _NYA_UIWidget widget) {
    nya_assert(ui != nullptr);

    f64 now = nya_app_uptime_s();

    // both are one at rest, which is how a widget with neither animation running says so without a flag for it.
    f32 pop    = widget.focused ? nya_clamp((f32)(now - ui->focus_changed_s) / NYA_UI_POP_S, 0.0F, 1.0F) : 1.0F;
    f32 bounce = ui->bounce_id == widget.id ? nya_clamp((f32)(now - ui->bounce_s) / NYA_UI_BOUNCE_S, 0.0F, 1.0F) : 1.0F;

    return (NYA_UIWidgetState){
        .id        = widget.id,
        .disabled  = widget.disabled,
        .focused   = widget.focused,
        .held      = widget.held,
        .activated = widget.activated,
        .focus     = widget.focus,
        .press     = widget.press,
        .pop       = pop,
        .bounce    = bounce,
    };
}

void _nya_ui_draw(NYA_UI* ui, NYA_UIWidgetDraw* widget) {
    nya_assert(ui != nullptr && ui == _nya_ui.open && widget != nullptr);
    nya_assert(widget->kind < NYA_UI_WIDGET_KIND_COUNT);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_Rectf*     screen = &_nya_ui.layouts[0].clip;
    const NYA_Rectf*     clip   = &layout->clip;

    // filled here rather than at twenty call sites: none of it is the widget's business, and all of it is the pass's.
    widget->text       = layout->text;
    widget->opacity    = _nya_ui.opacities[_nya_ui.opacity_depth];
    widget->clip       = *clip;
    widget->clip_whole = clip->x == screen->x && clip->y == screen->y && clip->width == screen->width && clip->height == screen->height;

    if (widget->label == nullptr) widget->label = "";

    const NYA_UIPresenter* present = ui->present;

    present->draw(present->state, ui->window, widget);
}

f32x2 _nya_ui_measure(NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow) {
    nya_assert(text != nullptr);
    nya_assert(role < NYA_UI_TEXT_COUNT);

    const NYA_UIPresenter* present = _nya_ui_present();

    return present->measure(present->state, role, text, room, overflow);
}

f32 _nya_ui_text_width(NYA_UIText role, NYA_ConstCString text) {
    return _nya_ui_measure(role, text, 0.0F, NYA_UI_OVERFLOW_VISIBLE).x;
}

f32 _nya_ui_measure_bytes(NYA_UIText role, NYA_ConstCString text, u32 bytes) {
    nya_assert(text != nullptr);
    nya_assert(role < NYA_UI_TEXT_COUNT);

    const NYA_UIPresenter* present = _nya_ui_present();

    return present->measure_bytes(present->state, role, text, bytes);
}

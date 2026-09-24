/**
 * @file ui_present_dom.c
 *
 * The semantic-HTML presenter: one real form control per interactive widget, in declaration order, into a
 * fixed buffer. The accessible sibling of ui_present_html — same seam, same widget stream, same monospace
 * measurement, differing only in `draw`, where this writes an `<input>` where the other writes a positioned
 * `<div>`. See ui_present_dom.h for the element mapping and why both exist.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"
#include "nyangine/ui/ui_present_dom.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void  _nya_ui_dom_look_build(void* state, u32 depth, const NYA_UIStyle* style, f32 scale, NYA_UILook* out);
NYA_INTERNAL void  _nya_ui_dom_look_use(void* state, u32 depth);
NYA_INTERNAL f32x2 _nya_ui_dom_measure(void* state, NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow);
NYA_INTERNAL f32   _nya_ui_dom_measure_bytes(void* state, NYA_UIText role, NYA_ConstCString text, u32 bytes);
NYA_INTERNAL void  _nya_ui_dom_clip_set(void* state, NYA_Window* window, NYA_Rectf clip, b8 whole);
NYA_INTERNAL s32   _nya_ui_dom_layer_get(void* state, NYA_Window* window);
NYA_INTERNAL void  _nya_ui_dom_layer_set(void* state, NYA_Window* window, s32 layer);
NYA_INTERNAL void  _nya_ui_dom_draw(void* state, NYA_Window* window, const NYA_UIWidgetDraw* widget);

/** Codepoints in the first `bytes` of `text`. The recorder's rule: a UTF-8 continuation byte starts nothing. */
NYA_INTERNAL u32 _nya_ui_dom_cells(NYA_ConstCString text, u32 bytes) __attr_no_discard;

/** Appends `text` to the body, truncating and flagging overflow rather than overrunning the buffer. */
NYA_INTERNAL void _nya_ui_dom_put(NYA_UIDom* dom, NYA_ConstCString text);

/** Appends `text` with `<`, `>`, `&`, `"` and `'` turned into entities: a label is somebody's words. */
NYA_INTERNAL void _nya_ui_dom_escape(NYA_UIDom* dom, NYA_ConstCString text);

/** Appends a printf line, bounded, into the body. */
NYA_INTERNAL void _nya_ui_dom_putf(NYA_UIDom* dom, NYA_ConstCString format, ...) __attr_fmt_printf(2, 3);

/** The class suffix for a widget kind: `button`, `label`, … . Never null. */
NYA_INTERNAL NYA_ConstCString _nya_ui_dom_class(NYA_UIWidgetKind kind) __attr_no_discard;

/** Appends ` disabled`, ` data-focused="1"` and the like for the widget's state, common to every control. */
NYA_INTERNAL void _nya_ui_dom_state_attrs(NYA_UIDom* dom, const NYA_UIWidgetState* state);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_ui_dom_init(NYA_UIDom* dom, f32x2 cell) {
    nya_assert(dom != nullptr);
    nya_assert(cell.x >= 0.0F && cell.y >= 0.0F, "a cell has no negative side");

    if (cell.x <= 0.0F || cell.y <= 0.0F) cell = NYA_UI_DOM_CELL;

    *dom = (NYA_UIDom){
        .cell      = cell,
        .presenter = {
            .name          = "dom",
            .state         = dom,
            .look_build    = _nya_ui_dom_look_build,
            .look_use      = _nya_ui_dom_look_use,
            .measure       = _nya_ui_dom_measure,
            .measure_bytes = _nya_ui_dom_measure_bytes,
            .clip_set      = _nya_ui_dom_clip_set,
            .layer_get     = _nya_ui_dom_layer_get,
            .layer_set     = _nya_ui_dom_layer_set,
            .draw          = _nya_ui_dom_draw,
        },
    };

    dom->body[0] = '\0';
}

void nya_ui_dom_deinit(NYA_UIDom* dom) {
    nya_assert(dom != nullptr);

    *dom = (NYA_UIDom){ 0 };
}

void nya_ui_dom_reset(NYA_UIDom* dom) {
    nya_assert(dom != nullptr);

    dom->used       = 0;
    dom->sequence   = 0;
    dom->overflowed = false;
    dom->body[0]    = '\0';
}

const NYA_UIPresenter* nya_ui_dom_presenter(NYA_UIDom* dom) {
    nya_assert(dom != nullptr);
    nya_assert(dom->presenter.draw == _nya_ui_dom_draw, "a dom presenter is prepared by nya_ui_dom_init before it presents anything");

    return &dom->presenter;
}

NYA_ConstCString nya_ui_dom_body(const NYA_UIDom* dom) {
    nya_assert(dom != nullptr);

    return dom->body;
}

u32 nya_ui_dom_count(const NYA_UIDom* dom) {
    nya_assert(dom != nullptr);

    return dom->sequence;
}

b8 nya_ui_dom_overflowed(const NYA_UIDom* dom) {
    nya_assert(dom != nullptr);

    return dom->overflowed;
}

b8 nya_ui_dom_widget_kind(const NYA_UIDom* dom, u32 id, NYA_UIWidgetKind* out_kind) {
    nya_assert(dom != nullptr && out_kind != nullptr);

    *out_kind = NYA_UI_WIDGET_LABEL;

    if (id >= dom->sequence || id >= NYA_UI_DOM_MAX_WIDGETS) return false;

    *out_kind = dom->kinds[id];

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MEASUREMENT AND LOOK — the recorder's, so structure agrees across backends
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_ui_dom_look_build(void* state, u32 depth, const NYA_UIStyle* style, f32 scale, NYA_UILook* out) {
    NYA_UIDom* dom = state;

    nya_assert(dom != nullptr && style != nullptr && out != nullptr);
    nya_assert(depth <= NYA_UI_STYLE_DEPTH_MAX, "a look is built at depth %u", depth);

    nya_ui_look_scale(style, scale, out);

    for (u32 i = 0; i < NYA_UI_TEXT_COUNT; i++) out->line_heights[i] = dom->cell.y;

    dom->looks[depth] = *out;
}

void _nya_ui_dom_look_use(void* state, u32 depth) {
    NYA_UIDom* dom = state;

    nya_assert(dom != nullptr);
    nya_assert(depth <= NYA_UI_STYLE_DEPTH_MAX, "a look at depth %u was selected", depth);

    dom->depth = depth;
}

f32x2 _nya_ui_dom_measure(void* state, NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow) {
    NYA_UIDom* dom = state;

    nya_assert(dom != nullptr && text != nullptr);
    nya_assert(role < NYA_UI_TEXT_COUNT);

    f32 width = (f32)_nya_ui_dom_cells(text, (u32)strlen(text)) * dom->cell.x;

    if (overflow != NYA_UI_OVERFLOW_WRAP || room <= 0.0F || width <= room) return (f32x2){ width, dom->cell.y };

    f32 columns = floorf(room / dom->cell.x);
    if (columns < 1.0F) columns = 1.0F;

    f32 lines = ceilf(width / (columns * dom->cell.x));

    return (f32x2){ columns * dom->cell.x, lines * dom->cell.y };
}

f32 _nya_ui_dom_measure_bytes(void* state, NYA_UIText role, NYA_ConstCString text, u32 bytes) {
    NYA_UIDom* dom = state;

    nya_assert(dom != nullptr && text != nullptr);
    nya_assert(role < NYA_UI_TEXT_COUNT);

    return (f32)_nya_ui_dom_cells(text, bytes) * dom->cell.x;
}

void _nya_ui_dom_clip_set(void* state, NYA_Window* window, NYA_Rectf clip, b8 whole) {
    // The browser lays the form out and clips by its own overflow; a semantic pass sets nothing here.
    (void)state;
    (void)window;
    (void)clip;
    (void)whole;
}

s32 _nya_ui_dom_layer_get(void* state, NYA_Window* window) {
    // A semantic document has no z-order of its own — DOM order is the order — so the layer is not read back.
    (void)state;
    (void)window;

    return 0;
}

void _nya_ui_dom_layer_set(void* state, NYA_Window* window, s32 layer) {
    (void)state;
    (void)window;
    (void)layer;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DRAWING — one real control per widget
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_ui_dom_draw(void* state, NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    NYA_UIDom* dom = state;
    (void)window;

    nya_assert(dom != nullptr && widget != nullptr);

    u32 id = dom->sequence++;

    // Kept so a live server can dispatch an event on element `wN` by the widget's kind alone.
    if (id < NYA_UI_DOM_MAX_WIDGETS) dom->kinds[id] = widget->kind;

    NYA_ConstCString kind = _nya_ui_dom_class(widget->kind);

    switch (widget->kind) {
        // ── the button family: real <button>s, so they focus, click and read as buttons ──
        //
        // A plain button and the chrome/section headers are ordinary push buttons. A selectable is a button
        // that stays pressed, which ARIA spells `aria-pressed`. A section is a disclosure, which is
        // `aria-expanded` read off its mark.
        case NYA_UI_WIDGET_BUTTON:
        case NYA_UI_WIDGET_CHROME:
        case NYA_UI_WIDGET_SELECTABLE:
        case NYA_UI_WIDGET_SECTION: {
            _nya_ui_dom_putf(dom, "<button id=\"w%u\" type=\"button\" class=\"nya-%s\" data-nya=\"click\"", id, kind);

            if (widget->kind == NYA_UI_WIDGET_SELECTABLE) {
                _nya_ui_dom_putf(dom, " aria-pressed=\"%s\"", widget->as_choice.on ? "true" : "false");
            }
            if (widget->kind == NYA_UI_WIDGET_SECTION) {
                _nya_ui_dom_putf(dom, " aria-expanded=\"%s\"", widget->as_mark.mark == NYA_UI_MARK_EXPANDED ? "true" : "false");
            }

            _nya_ui_dom_state_attrs(dom, &widget->state);
            _nya_ui_dom_put(dom, ">");
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_put(dom, "</button>\n");
            break;
        }

        // ── a toggle is a checkbox, wrapped in its own <label> so the caption is its accessible name ──
        case NYA_UI_WIDGET_TOGGLE: {
            _nya_ui_dom_put(dom, "<label class=\"nya-toggle\"><input id=\"w");
            _nya_ui_dom_putf(dom, "%u\" type=\"checkbox\" data-nya=\"click\"", id);
            if (widget->as_choice.on) _nya_ui_dom_put(dom, " checked");
            _nya_ui_dom_state_attrs(dom, &widget->state);
            _nya_ui_dom_put(dom, "> ");
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_put(dom, "</label>\n");
            break;
        }

        // ── a radio is a real radio input; the shared name makes a set mutually exclusive in the browser,
        // and the server enforces it regardless since it owns the variable. See ui_present_dom.h on grouping. ──
        case NYA_UI_WIDGET_RADIO: {
            _nya_ui_dom_put(dom, "<label class=\"nya-radio\"><input id=\"w");
            _nya_ui_dom_putf(dom, "%u\" type=\"radio\" name=\"nya-radio\" data-nya=\"click\"", id);
            if (widget->as_choice.on) _nya_ui_dom_put(dom, " checked");
            _nya_ui_dom_state_attrs(dom, &widget->state);
            _nya_ui_dom_put(dom, "> ");
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_put(dom, "</label>\n");
            break;
        }

        // ── a slider is a real range input, with the caption a <label for> points at ──
        case NYA_UI_WIDGET_SLIDER: {
            _nya_ui_dom_putf(dom, "<div class=\"nya-slider\"><label for=\"w%u-i\">", id);
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_putf(dom, "</label> <input id=\"w%u-i\" data-host=\"w%u\" type=\"range\" min=\"0\" max=\"1000\" value=\"%d\" data-nya=\"input\" aria-label=\"",
                             id, id, (s32)(widget->as_slider.t * 1000.0F));
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_put(dom, "\"");
            _nya_ui_dom_state_attrs(dom, &widget->state);
            _nya_ui_dom_put(dom, "></div>\n");
            break;
        }

        // ── a field is a real text input, so a password manager, autofill and an IME all engage ──
        case NYA_UI_WIDGET_FIELD: {
            _nya_ui_dom_putf(dom, "<div class=\"nya-field\"><label for=\"w%u-i\">", id);
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_putf(dom, "</label> <input id=\"w%u-i\" data-host=\"w%u\" type=\"text\" maxlength=\"%d\" value=\"",
                             id, id, (s32)(NYA_UI_TEXT_INPUT_MAX - 1));
            _nya_ui_dom_escape(dom, widget->as_field.field.buffer != nullptr ? widget->as_field.field.buffer : "");
            _nya_ui_dom_put(dom, "\" data-nya=\"text\" aria-label=\"");
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_put(dom, "\"");
            _nya_ui_dom_state_attrs(dom, &widget->state);
            _nya_ui_dom_put(dom, "></div>\n");
            break;
        }

        // ── a dropdown is a real <select>. The widget stream carries only the shown option, not the list
        // (the open list arrives as separate SELECTABLE widgets), so the select holds the one selected
        // option; the server swaps it as the choice changes. ──
        case NYA_UI_WIDGET_DROPDOWN: {
            NYA_ConstCString shown = widget->as_dropdown.shown != nullptr ? widget->as_dropdown.shown : widget->label;

            _nya_ui_dom_putf(dom, "<div class=\"nya-dropdown\"><label for=\"w%u-i\">", id);
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_putf(dom, "</label> <select id=\"w%u-i\" data-host=\"w%u\" data-nya=\"select\" aria-label=\"", id, id);
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_put(dom, "\"");
            _nya_ui_dom_state_attrs(dom, &widget->state);
            _nya_ui_dom_put(dom, "><option selected>");
            _nya_ui_dom_escape(dom, shown);
            _nya_ui_dom_put(dom, "</option></select></div>\n");
            break;
        }

        // ── a colour picker is a native colour input, keyed by the widget's own colour as #rrggbb ──
        case NYA_UI_WIDGET_COLOR_PICKER: {
            NYA_Color c = widget->as_picker.value;

            _nya_ui_dom_putf(dom, "<div class=\"nya-picker\"><label for=\"w%u-i\">", id);
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_putf(dom, "</label> <input id=\"w%u-i\" data-host=\"w%u\" type=\"color\" value=\"#%02x%02x%02x\" data-nya=\"color\" aria-label=\"",
                             id, id, (u32)(nya_clamp(c.r, 0.0F, 1.0F) * 255.0F + 0.5F), (u32)(nya_clamp(c.g, 0.0F, 1.0F) * 255.0F + 0.5F),
                             (u32)(nya_clamp(c.b, 0.0F, 1.0F) * 255.0F + 0.5F));
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_put(dom, "\"");
            _nya_ui_dom_state_attrs(dom, &widget->state);
            _nya_ui_dom_put(dom, "></div>\n");
            break;
        }

        // ── a panel is a labelled group; its title is a heading, and its children flow after it ──
        case NYA_UI_WIDGET_PANEL: {
            _nya_ui_dom_put(dom, "<section class=\"nya-panel\" role=\"group\" aria-label=\"");
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_putf(dom, "\" id=\"w%u\">", id);
            if (widget->label[0] != '\0') {
                _nya_ui_dom_put(dom, "<h2>");
                _nya_ui_dom_escape(dom, widget->label);
                _nya_ui_dom_put(dom, "</h2>");
            }
            _nya_ui_dom_put(dom, "</section>\n");
            break;
        }

        // ── plain text ──
        case NYA_UI_WIDGET_LABEL: {
            _nya_ui_dom_putf(dom, "<p id=\"w%u\" class=\"nya-label\">", id);
            _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_put(dom, "</p>\n");
            break;
        }

        // ── the rest are chrome the layout draws but assistive tech should skip: a scrim, a rule, a
        // scrollbar, an icon, a chart. They read as decorative, keeping the accessibility tree to the
        // controls and text that carry meaning. Any words they hold still go in as escaped text. ──
        case NYA_UI_WIDGET_SCRIM:
        case NYA_UI_WIDGET_CHART:
        case NYA_UI_WIDGET_ICON:
        case NYA_UI_WIDGET_GRIP:
        case NYA_UI_WIDGET_SCROLLBAR:
        case NYA_UI_WIDGET_RULE:
        case NYA_UI_WIDGET_STRIPE:
        case NYA_UI_WIDGET_UNDERLINE: {
            _nya_ui_dom_putf(dom, "<div id=\"w%u\" class=\"nya-%s\" aria-hidden=\"true\">", id, kind);
            if (widget->label[0] != '\0') _nya_ui_dom_escape(dom, widget->label);
            _nya_ui_dom_put(dom, "</div>\n");
            break;
        }

        case NYA_UI_WIDGET_KIND_COUNT:
        default: break;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_ui_dom_state_attrs(NYA_UIDom* dom, const NYA_UIWidgetState* state) {
    // The native `disabled` attribute both greys the control and drops it out of the tab order, which is
    // exactly what a keyboard and a screen-reader user expect — no `aria-disabled` or `tabindex` needed.
    if (state->disabled) _nya_ui_dom_put(dom, " disabled");

    // The eased focus and held flags a stylesheet can read, mirroring the html presenter's data attributes.
    if (state->focused) _nya_ui_dom_put(dom, " data-focused=\"1\"");
    if (state->held) _nya_ui_dom_put(dom, " data-held=\"1\"");
}

NYA_ConstCString _nya_ui_dom_class(NYA_UIWidgetKind kind) {
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
        case NYA_UI_WIDGET_COLOR_PICKER: return "picker";
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
        default:                         break;
    }

    return "widget";
}

u32 _nya_ui_dom_cells(NYA_ConstCString text, u32 bytes) {
    u32 cells = 0;

    for (u32 i = 0; i < bytes && text[i] != '\0'; i++) {
        // Every byte that is not a UTF-8 continuation starts a codepoint, which is one cell here.
        if (((u8)text[i] & 0xC0) != 0x80) cells++;
    }

    return cells;
}

void _nya_ui_dom_put(NYA_UIDom* dom, NYA_ConstCString text) {
    u32 length = (u32)strlen(text);

    if (dom->used + length + 1 > NYA_UI_DOM_MAX) {
        dom->overflowed = true;

        // Whatever still fits, so a truncated page is still valid up to the cut rather than half an element.
        u32 room = NYA_UI_DOM_MAX - 1 - dom->used;
        if (room > 0) {
            nya_memcpy(dom->body + dom->used, text, room);
            dom->used += room;
        }

        dom->body[dom->used] = '\0';
        return;
    }

    nya_memcpy(dom->body + dom->used, text, length);
    dom->used += length;
    dom->body[dom->used] = '\0';
}

void _nya_ui_dom_putf(NYA_UIDom* dom, NYA_ConstCString format, ...) {
    char line[512] = { 0 };

    va_list args;
    va_start(args, format);
    (void)vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    _nya_ui_dom_put(dom, line);
}

void _nya_ui_dom_escape(NYA_UIDom* dom, NYA_ConstCString text) {
    for (u32 i = 0; text[i] != '\0'; i++) {
        switch (text[i]) {
            case '&':  _nya_ui_dom_put(dom, "&amp;"); break;
            case '<':  _nya_ui_dom_put(dom, "&lt;"); break;
            case '>':  _nya_ui_dom_put(dom, "&gt;"); break;
            case '"':  _nya_ui_dom_put(dom, "&quot;"); break;
            case '\'': _nya_ui_dom_put(dom, "&#39;"); break;

            default: {
                char one[2] = { text[i], '\0' };
                _nya_ui_dom_put(dom, one);
                break;
            }
        }
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE DOCUMENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The page around the controls: a doctype, a small stylesheet, the body's controls inside a `<form>` whose
 * submit is suppressed (so Enter in a field does not reload the page), and the thin client that forwards a
 * click, an input, a text change, a select and a colour pick to the server and swaps in the HTML that comes
 * back — the same live-server loop ui_present_html drives, over real form controls.
 *
 * `%s` four times: the title (escaped), the body's controls, the script's nonce attribute, and once more is
 * not needed — three substitutions.
 */
NYA_INTERNAL NYA_ConstCString _NYA_UI_DOM_PAGE =
    "<!doctype html>\n"
    "<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>%s</title>\n"
    "<link rel=\"icon\" href=\"data:,\">\n"
    "<style>\n"
    "  :root{--bg:#14161c;--panel:#1c2029;--ink:#d8dbe2;--accent:#5a7cff;--line:#2a2f3a}\n"
    "  html,body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}\n"
    "  main{max-width:640px;margin:0 auto;padding:24px}\n"
    "  .nya-panel{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:16px;margin:16px 0}\n"
    "  .nya-panel h2{margin:0 0 8px;font-size:1.1em}\n"
    "  label{display:inline-block}\n"
    "  .nya-field,.nya-slider,.nya-dropdown,.nya-picker{margin:8px 0;display:flex;gap:8px;align-items:center}\n"
    "  input[type=text],select{flex:1;background:#0f1116;color:var(--ink);border:1px solid var(--line);border-radius:5px;padding:6px 8px;font:inherit}\n"
    "  button.nya-button,button.nya-selectable{background:#262b36;color:var(--ink);border:1px solid var(--line);border-radius:5px;padding:6px 12px;cursor:pointer;font:inherit;margin:4px 4px 4px 0}\n"
    "  button[aria-pressed=\"true\"]{background:var(--accent);color:#fff;border-color:var(--accent)}\n"
    "  .nya-toggle,.nya-radio{display:flex;gap:8px;align-items:center;margin:6px 0;cursor:pointer}\n"
    "  :focus-visible{outline:2px solid var(--accent);outline-offset:2px}\n"
    "  [data-focused=\"1\"]{outline:2px solid var(--accent);outline-offset:2px}\n"
    "</style></head><body>\n"
    "<main><form id=\"nya-surface\" onsubmit=\"return false\">\n%s</form></main>\n"
    "<script%s>\n"
    "(function(){\n"
    "  var surface=document.getElementById('nya-surface');\n"
    // morph the surface to the new HTML by id, so a control that did not change keeps its focus, caret and
    // selection across a redraw. New ids are added, gone ids removed, changed ones replaced — but never the
    // element the person is in, so typing survives the round trip. The server holds the state and is
    // stateless between requests, so the diff lives here, where the live DOM already is.
    "  function morph(html){\n"
    "    var next=document.createElement('div');next.innerHTML=html;\n"
    "    var have={};for(var c=surface.firstElementChild;c;c=c.nextElementSibling)if(c.id)have[c.id]=c;\n"
    "    var seen={};var active=document.activeElement;\n"
    "    for(var n=next.firstElementChild;n;){var nx=n.nextElementSibling;if(n.id)seen[n.id]=1;var old=n.id?have[n.id]:null;\n"
    "      if(!old){surface.appendChild(n);}\n"
    "      else{ if(!old.contains(active) && old.outerHTML!==n.outerHTML) old.replaceWith(n); }\n"
    "      n=nx;}\n"
    "    for(var id in have)if(!seen[id])have[id].remove();\n"
    "  }\n"
    "  function send(id,event,value){\n"
    "    if(!id)return;\n"
    "    fetch('/event',{method:'POST',headers:{'content-type':'application/json'},\n"
    "      body:JSON.stringify({id:id,event:event,value:value})}).then(function(r){return r.text()}).then(function(html){\n"
    "        if(html)morph(html);});\n"
    "  }\n"
    // A click on a button carries its own id; a checkbox, radio, slider, field, select or colour input
    // carries its host widget's id through data-host, since the control's id is `wN-i`.
    "  function host(t){return t.dataset.host||(t.closest('[id]')||{}).id;}\n"
    "  surface.addEventListener('click',function(e){var t=e.target.closest('[data-nya=\"click\"]');if(t)send(host(t),'click',(t.type==='checkbox'||t.type==='radio')?t.checked:null);});\n"
    "  surface.addEventListener('input',function(e){var t=e.target.closest('[data-nya]');if(!t)return;var k=t.dataset.nya;if(k!=='input'&&k!=='text'&&k!=='color')return;var v=t.value;if(v!=null&&v.length>4096)v=v.slice(0,4096);send(host(t),k,v);});\n"
    "  surface.addEventListener('change',function(e){var t=e.target.closest('[data-nya=\"select\"]');if(t)send(host(t),'select',t.value);});\n"
    "})();\n"
    "</script></body></html>\n";

u32 nya_ui_dom_document(const NYA_UIDom* dom, char* out, u32 capacity, NYA_ConstCString title, NYA_ConstCString script_nonce) {
    nya_assert(dom != nullptr && out != nullptr && capacity > 0);

    // The nonce as the attribute it becomes, or nothing. It is this server's own random value, not user
    // data, so it needs no escaping; a caller that passes something else has misused it.
    char nonce_attr[96] = { 0 };
    if (script_nonce != nullptr && script_nonce[0] != '\0') (void)snprintf(nonce_attr, sizeof(nonce_attr), " nonce=\"%s\"", script_nonce);

    // The title through the same escaping a label gets, since it is dropped into the format string where a
    // raw `<` would break the page.
    char safe_title[128] = { 0 };
    {
        NYA_UIDom scratch = { 0 };
        _nya_ui_dom_escape(&scratch, title != nullptr ? title : "nyangine");

        (void)snprintf(safe_title, sizeof(safe_title), "%.*s", (s32)nya_min(scratch.used, (u32)(sizeof(safe_title) - 1)), scratch.body);
    }

    s32 written = snprintf(out, capacity, _NYA_UI_DOM_PAGE, safe_title, dom->body, nonce_attr);

    if (written <= 0) {
        out[0] = '\0';
        return 0;
    }

    return (u32)nya_min((u32)written, capacity - 1);
}

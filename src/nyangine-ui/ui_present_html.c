/**
 * @file ui_present_html.c
 *
 * The HTML presenter: one absolutely-positioned element per widget, in declaration order, into a fixed
 * buffer. The render half of a server-driven live UI; see ui_present_html.h.
 *
 * It shares its measurement and its look with the recorder on purpose — a monospace cell, so the layout
 * is exact and matches the terminal's grid — and differs only in `draw`, where the recorder keeps a
 * struct and this writes an element. Everything a browser then does to it is CSS.
 * */
#include "nyangine-core/nyangine.h"

#include "nyangine-ui/ui_internal.h"
#include "nyangine-ui/ui_present_html.h"

// PRIVATE API DECLARATION

NYA_INTERNAL void  _nya_ui_html_look_build(void* state, u32 depth, const NYA_UIStyle* style, f32 scale, NYA_UILook* out);
NYA_INTERNAL void  _nya_ui_html_look_use(void* state, u32 depth);
NYA_INTERNAL f32x2 _nya_ui_html_measure(void* state, NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow);
NYA_INTERNAL f32   _nya_ui_html_measure_bytes(void* state, NYA_UIText role, NYA_ConstCString text, u32 bytes);
NYA_INTERNAL void  _nya_ui_html_clip_set(void* state, NYA_Window* window, NYA_Rectf clip, b8 whole);
NYA_INTERNAL s32   _nya_ui_html_layer_get(void* state, NYA_Window* window);
NYA_INTERNAL void  _nya_ui_html_layer_set(void* state, NYA_Window* window, s32 layer);
NYA_INTERNAL void  _nya_ui_html_draw(void* state, NYA_Window* window, const NYA_UIWidgetDraw* widget);

/** Codepoints in the first `bytes` of `text`. Shared with the recorder's rule: a continuation byte starts nothing. */
NYA_INTERNAL u32 _nya_ui_html_cells(NYA_ConstCString text, u32 bytes) __attr_no_discard;

/** Appends `text` to the body, truncating and flagging overflow rather than overrunning the buffer. */
NYA_INTERNAL void _nya_ui_html_put(NYA_UIHtml* html, NYA_ConstCString text);

/** Appends `text` with `<`, `>`, `&`, `"` and `'` turned into entities: a label is somebody's words. */
NYA_INTERNAL void _nya_ui_html_escape(NYA_UIHtml* html, NYA_ConstCString text);

/** Appends a printf line, bounded, into the body. */
NYA_INTERNAL void _nya_ui_html_putf(NYA_UIHtml* html, NYA_ConstCString format, ...) __attr_fmt_printf(2, 3);

/** The class suffix for a widget kind: `button`, `label`, … . Never null. */
NYA_INTERNAL NYA_ConstCString _nya_ui_html_class(NYA_UIWidgetKind kind) __attr_no_discard;

/** The button-state colour a discrete backend picks: disabled, then held, then focused, else normal. */
NYA_INTERNAL NYA_Color _nya_ui_html_button_color(const NYA_UIStateColors* colors, const NYA_UIWidgetState* state) __attr_no_discard;

/** Appends `;<prop>:rgba(...)` from a colour, unless it is the zeroed "use the style default" one, which the stylesheet then answers — the same bargain the GPU backend makes with an all-zero colour. */
NYA_INTERNAL void _nya_ui_html_style_color(NYA_UIHtml* html, NYA_ConstCString prop, NYA_Color color);

/**
 * Renders the social-media embedding metadata in `meta` as the `<head>` block, into `out`, null terminated.
 * Every value is escaped through _nya_ui_html_escape and every URL is gated by nya_ui_page_meta_url_ok.
 * */
NYA_INTERNAL void _nya_ui_html_meta_head(const NYA_PageMeta* meta, char* out, u32 capacity);

/**
 * Appends one `<meta attr="key" content="value">` to `scratch`, with `value` escaped. `attr` ("name" or
 * "property") and `key` are this file's own literals; only `value` is a caller's text, so only it is escaped.
 * A null or empty `value` writes nothing, which is how a field's absence becomes a missing tag.
 * */
NYA_INTERNAL void _nya_ui_html_meta_tag(NYA_UIHtml* scratch, NYA_ConstCString attr, NYA_ConstCString key, NYA_ConstCString value);

// LIFETIME

void nya_ui_html_init(NYA_UIHtml* html, f32x2 cell) {
    nya_assert(html != nullptr);
    nya_assert(cell.x >= 0.0F && cell.y >= 0.0F, "a cell has no negative side");

    if (cell.x <= 0.0F || cell.y <= 0.0F) cell = NYA_UI_HTML_CELL;

    *html = (NYA_UIHtml){
        .cell      = cell,
        .presenter = {
            .name          = "html",
            .state         = html,
            .look_build    = _nya_ui_html_look_build,
            .look_use      = _nya_ui_html_look_use,
            .measure       = _nya_ui_html_measure,
            .measure_bytes = _nya_ui_html_measure_bytes,
            .clip_set      = _nya_ui_html_clip_set,
            .layer_get     = _nya_ui_html_layer_get,
            .layer_set     = _nya_ui_html_layer_set,
            .draw          = _nya_ui_html_draw,
        },
    };

    html->body[0] = '\0';
}

void nya_ui_html_deinit(NYA_UIHtml* html) {
    nya_assert(html != nullptr);

    *html = (NYA_UIHtml){ 0 };
}

void nya_ui_html_reset(NYA_UIHtml* html) {
    nya_assert(html != nullptr);

    html->used       = 0;
    html->sequence   = 0;
    html->layer      = 0;
    html->overflowed = false;
    html->body[0]    = '\0';
}

const NYA_UIPresenter* nya_ui_html_presenter(NYA_UIHtml* html) {
    nya_assert(html != nullptr);
    nya_assert(html->presenter.draw == _nya_ui_html_draw, "an html presenter is prepared by nya_ui_html_init before it presents anything");

    return &html->presenter;
}

NYA_ConstCString nya_ui_html_body(const NYA_UIHtml* html) {
    nya_assert(html != nullptr);

    return html->body;
}

u32 nya_ui_html_count(const NYA_UIHtml* html) {
    nya_assert(html != nullptr);

    return html->sequence;
}

b8 nya_ui_html_overflowed(const NYA_UIHtml* html) {
    nya_assert(html != nullptr);

    return html->overflowed;
}

b8 nya_ui_html_rect(const NYA_UIHtml* html, u32 id, NYA_Rectf* out_rect) {
    nya_assert(html != nullptr && out_rect != nullptr);

    *out_rect = (NYA_Rectf){ 0 };

    if (id >= html->sequence || id >= NYA_UI_HTML_MAX_WIDGETS) return false;

    *out_rect = html->rects[id];

    return true;
}

b8 nya_ui_html_widget(const NYA_UIHtml* html, u32 id, NYA_UIWidgetKind* out_kind, NYA_Rectf* out_value_rect) {
    nya_assert(html != nullptr && out_kind != nullptr && out_value_rect != nullptr);

    *out_kind       = NYA_UI_WIDGET_LABEL;
    *out_value_rect = (NYA_Rectf){ 0 };

    if (id >= html->sequence || id >= NYA_UI_HTML_MAX_WIDGETS) return false;

    *out_kind       = html->kinds[id];
    *out_value_rect = html->value_rects[id];

    return true;
}

// THE DOCUMENT

/**
 * The one stylesheet and the thin client, kept as a literal because it is the whole front-end a program
 * ships. The stylesheet colours each widget kind by class; the script forwards a click or an input on
 * anything carrying `data-nya` to the server and swaps whatever HTML comes back into the surface.
 *
 * `%s` four times: the title (escaped), the social-media embedding metadata block (each field escaped, or
 * empty), the body's elements, and the script's nonce attribute.
 */
NYA_INTERNAL NYA_ConstCString _NYA_UI_HTML_PAGE =
    "<!doctype html>\n"
    "<html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>%s</title>\n"
    "<link rel=\"icon\" href=\"data:,\">\n"
    "%s"
    "<style>\n"
    "  :root{--bg:#14161c;--panel:#1c2029;--ink:#d8dbe2;--dim:#9498a2;--accent:#5a7cff;--line:#2a2f3a}\n"
    "  html,body{margin:0;height:100%%;background:var(--bg);color:var(--ink);font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}\n"
    "  #nya-surface{position:relative;width:100%%;height:100vh;overflow:hidden}\n"
    "  #nya-surface>*{position:absolute;box-sizing:border-box;transition:background .12s,border-color .12s,color .12s,opacity .12s,transform .08s}\n"
    "  .nya-button:active,.nya-selectable:active{transform:translateY(1px)}\n"
    "  .nya-panel{background:var(--panel);border:1px solid var(--line);border-radius:10px;box-shadow:0 8px 30px rgba(0,0,0,.35)}\n"
    "  .nya-label{color:var(--ink);white-space:nowrap;display:flex;align-items:center;padding:0 2px}\n"
    "  .nya-button,.nya-selectable,.nya-dropdown{background:#262b36;color:var(--ink);border:1px solid var(--line);border-radius:5px;"
    "cursor:pointer;font:inherit;display:flex;align-items:center;justify-content:center;padding:0 8px}\n"
    "  .nya-button:hover,.nya-selectable:hover{border-color:var(--accent)}\n"
    "  .nya-selectable[data-on=\"1\"],.nya-toggle[data-on=\"1\"]{background:var(--accent);color:#fff;border-color:var(--accent)}\n"
    "  .nya-toggle,.nya-radio{cursor:pointer;border:1px solid var(--line);border-radius:5px;background:#262b36;display:flex;align-items:center;justify-content:center}\n"
    "  .nya-field input{width:100%%;height:100%%;background:var(--nya-track,#0f1116);color:var(--ink);border:1px solid var(--line);border-radius:var(--nya-radius,5px);font:inherit;padding:0 var(--nya-pad,6px);box-sizing:border-box}\n"
    "  .nya-slider input{width:100%%;accent-color:var(--nya-accent,var(--accent))}\n"
    "  .nya-scrim{background:rgba(0,0,0,.5)}\n"
    "  .nya-rule,.nya-underline{background:var(--line)}\n"
    "  .nya-underline{background:var(--accent)}\n"
    "  .nya-stripe{background:rgba(255,255,255,.03)}\n"
    "  [data-disabled=\"1\"]{opacity:.4;pointer-events:none}\n"
    "  [data-focused=\"1\"]{outline:2px solid var(--accent);outline-offset:-1px}\n"
    "</style></head><body>\n"
    "<div id=\"nya-surface\">\n%s</div>\n"
    "<script%s>\n"
    "(function(){\n"
    "  var surface=document.getElementById('nya-surface');\n"
    // morph the surface to the new HTML by id rather than replacing it whole: an element that didn't change is left alone, so its focus, caret and a mid-drag slider survive a redraw. The server is stateless (no memory of the last render), so the diff happens here where a browser keeps the live DOM anyway; new ids added, gone ids removed, changed ones patched.
    "  function morph(html){\n"
    "    var next=document.createElement('div');next.innerHTML=html;\n"
    "    var have={};for(var c=surface.firstElementChild;c;c=c.nextElementSibling)if(c.id)have[c.id]=c;\n"
    "    var seen={};var active=document.activeElement;\n"
    "    for(var n=next.firstElementChild;n;){var nx=n.nextElementSibling;seen[n.id]=1;var old=have[n.id];\n"
    "      if(!old){surface.appendChild(n);}\n"
    "      else{ if(old!==active && old.outerHTML!==n.outerHTML) old.replaceWith(n); }\n"
    "      n=nx;}\n"
    "    for(var id in have)if(!seen[id])have[id].remove();\n"
    "  }\n"
    "  function send(id,event,value){\n"
    "    fetch('/event',{method:'POST',headers:{'content-type':'application/json'},\n"
    "      body:JSON.stringify({id:id,event:event,value:value})}).then(function(r){return r.text()}).then(function(html){\n"
    "        if(html)morph(html);});\n"
    "  }\n"
    "  surface.addEventListener('click',function(e){var t=e.target.closest('[data-nya]');if(t&&t.dataset.nya==='click')send(t.id,'click',null);});\n"
    // A value change on a slider or field: the event name is the element's own data-nya ('input' for the range, 'text' for the field), the id sent is the widget's (the nearest ancestor carrying one, since the <input> has none), and the value is capped so no field can post an unbounded body.
    "  surface.addEventListener('input',function(e){var t=e.target.closest('[data-nya]');if(!t)return;var host=t.closest('[id]');if(!host)return;var v=e.target.value;if(v!=null&&v.length>4096)v=v.slice(0,4096);send(host.id,t.dataset.nya,v);});\n"
    "})();\n"
    "</script></body></html>\n";

u32 nya_ui_html_document(const NYA_UIHtml* html, char* out, u32 capacity, NYA_ConstCString title, NYA_ConstCString script_nonce) {
    return nya_ui_html_document_meta(html, out, capacity, title, script_nonce, nullptr);
}

u32 nya_ui_html_document_meta(const NYA_UIHtml* html, char* out, u32 capacity, NYA_ConstCString title, NYA_ConstCString script_nonce,
                              const NYA_PageMeta* meta) {
    nya_assert(html != nullptr && out != nullptr && capacity > 0);

    // The nonce as the attribute it becomes, or nothing: it's this server's own random value, not user data, so it needs no escaping; a caller that passes something else has misused it.
    char nonce_attr[96] = { 0 };
    if (script_nonce != nullptr && script_nonce[0] != '\0') (void)snprintf(nonce_attr, sizeof(nonce_attr), " nonce=\"%s\"", script_nonce);

    // The title through the same escaping a label gets, into a small stack buffer, since it's dropped into the format string where a raw `<` would break the page.
    char safe_title[128] = { 0 };
    {
        NYA_UIHtml scratch = { 0 };
        _nya_ui_html_escape(&scratch, title != nullptr ? title : "nyangine");

        (void)snprintf(safe_title, sizeof(safe_title), "%.*s", (s32)nya_min(scratch.used, (u32)(sizeof(safe_title) - 1)), scratch.body);
    }

    // The embedding metadata as its block of `<meta>`/`<link>` tags, each field escaped, or the empty string when there's no metadata — which keeps the default page byte-for-byte unchanged.
    char meta_head[NYA_PAGE_META_HEAD_MAX] = { 0 };
    if (meta != nullptr) _nya_ui_html_meta_head(meta, meta_head, sizeof(meta_head));

    s32 written = snprintf(out, capacity, _NYA_UI_HTML_PAGE, safe_title, meta_head, html->body, nonce_attr);

    if (written <= 0) {
        out[0] = '\0';
        return 0;
    }

    return (u32)nya_min((u32)written, capacity - 1);
}

// MEASUREMENT AND LOOK — the recorder's, so structure agrees across backends

void _nya_ui_html_look_build(void* state, u32 depth, const NYA_UIStyle* style, f32 scale, NYA_UILook* out) {
    NYA_UIHtml* html = state;

    nya_assert(html != nullptr && style != nullptr && out != nullptr);
    nya_assert(depth <= NYA_UI_STYLE_DEPTH_MAX, "a look is built at depth %u", depth);

    nya_ui_look_scale(style, scale, out);

    for (u32 i = 0; i < NYA_UI_TEXT_COUNT; i++) out->line_heights[i] = html->cell.y;

    html->looks[depth] = *out;
}

void _nya_ui_html_look_use(void* state, u32 depth) {
    NYA_UIHtml* html = state;

    nya_assert(html != nullptr);
    nya_assert(depth <= NYA_UI_STYLE_DEPTH_MAX, "a look at depth %u was selected", depth);

    html->depth = depth;
}

f32x2 _nya_ui_html_measure(void* state, NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow) {
    NYA_UIHtml* html = state;

    nya_assert(html != nullptr && text != nullptr);
    nya_assert(role < NYA_UI_TEXT_COUNT);

    f32 width = (f32)_nya_ui_html_cells(text, (u32)strlen(text)) * html->cell.x;

    if (overflow != NYA_UI_OVERFLOW_WRAP || room <= 0.0F || width <= room) return (f32x2){ width, html->cell.y };

    f32 columns = floorf(room / html->cell.x);
    if (columns < 1.0F) columns = 1.0F;

    f32 lines = ceilf(width / (columns * html->cell.x));

    return (f32x2){ columns * html->cell.x, lines * html->cell.y };
}

f32 _nya_ui_html_measure_bytes(void* state, NYA_UIText role, NYA_ConstCString text, u32 bytes) {
    NYA_UIHtml* html = state;

    nya_assert(html != nullptr && text != nullptr);
    nya_assert(role < NYA_UI_TEXT_COUNT);

    return (f32)_nya_ui_html_cells(text, bytes) * html->cell.x;
}

void _nya_ui_html_clip_set(void* state, NYA_Window* window, NYA_Rectf clip, b8 whole) {
    // The browser clips by the surface and the panels' own overflow; there is nothing to set here.
    (void)state;
    (void)window;
    (void)clip;
    (void)whole;
}

s32 _nya_ui_html_layer_get(void* state, NYA_Window* window) {
    NYA_UIHtml* html = state;
    (void)window;

    return html->layer;
}

void _nya_ui_html_layer_set(void* state, NYA_Window* window, s32 layer) {
    NYA_UIHtml* html = state;
    (void)window;

    html->layer = layer;
}

// DRAWING — one element per widget

void _nya_ui_html_draw(void* state, NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    NYA_UIHtml* html = state;
    (void)window;

    nya_assert(html != nullptr && widget != nullptr);

    u32 id = html->sequence++;

    // Kept so a live server can aim a synthetic pointer at this widget from its id alone; see the header.
    if (id < NYA_UI_HTML_MAX_WIDGETS) {
        html->rects[id]       = widget->rect;
        html->kinds[id]       = widget->kind;
        html->value_rects[id] = widget->kind == NYA_UI_WIDGET_SLIDER ? widget->as_slider.track
                              : widget->kind == NYA_UI_WIDGET_FIELD  ? widget->as_field.field.box
                                                                     : (NYA_Rectf){ 0 };
    }

    NYA_ConstCString kind = _nya_ui_html_class(widget->kind);

    // The frame every element shares: an id for a patch, a class for CSS, the computed rectangle, the z-index, and the state as data attributes a stylesheet and a client both read.
    _nya_ui_html_putf(html,
                      "<div id=\"w%u\" class=\"nya-%s\" style=\"left:%dpx;top:%dpx;width:%dpx;height:%dpx;z-index:%d",
                      id, kind, (s32)widget->rect.x, (s32)widget->rect.y, (s32)widget->rect.width, (s32)widget->rect.height, html->layer);

    if (widget->opacity < 1.0F) _nya_ui_html_putf(html, ";opacity:%.3f", (f64)widget->opacity);

    // A colour the caller set on this specific widget, as the GPU backend also honours over the style: it wins the text colour, since the style block below writes fills and borders and never `color`.
    _nya_ui_html_style_color(html, "color", widget->color);

    // The program's own style, at the depth this pass selected, as inline CSS layering over the fixed stylesheet: a custom accent, panel colour or radius set with nya_ui_style_set/_push reaches the browser here — the same NYA_UILook the GPU backend draws from, turned into properties rather than triangles; a zeroed colour is left to the stylesheet, as the GPU leaves it to the style.
    const NYA_UILook*  look  = &html->looks[html->depth];
    const NYA_UIStyle* style = &look->style;

    switch (widget->kind) {
        case NYA_UI_WIDGET_PANEL:
            _nya_ui_html_style_color(html, "background", style->panel);
            // The ink is the colour of the outline the GPU draws around a panel; here it is the border's.
            _nya_ui_html_style_color(html, "border-color", style->ink);
            if (look->radius > 0.0F) _nya_ui_html_putf(html, ";border-radius:%dpx", (s32)look->radius);
            break;

        // The dim sheet over the surface behind a modal: the style's own scrim colour, falling through to the stylesheet's default the way every other colour does.
        case NYA_UI_WIDGET_SCRIM: _nya_ui_html_style_color(html, "background", style->scrim); break;

        // The value widgets paint through their own inner <input>, which no fill on this <div> would reach; their look travels as inherited CSS custom properties instead (the stylesheet reads them off the input, with the fixed sheet as fallback), so a custom track, accent, radius or padding shows on the field and slider natively. A zeroed colour writes nothing and the default stands.
        case NYA_UI_WIDGET_FIELD:
            _nya_ui_html_style_color(html, "--nya-track", style->track);
            if (look->radius > 0.0F) _nya_ui_html_putf(html, ";--nya-radius:%dpx", (s32)look->radius);
            if (look->padding > 0.0F) _nya_ui_html_putf(html, ";--nya-pad:%dpx", (s32)roundf(look->padding * 0.5F));
            break;

        case NYA_UI_WIDGET_SLIDER:
            _nya_ui_html_style_color(html, "--nya-accent", style->accent);
            _nya_ui_html_style_color(html, "--nya-track", style->track);
            break;

        // The button family shares a body colour and a rounded border; a selectable or toggle that's on takes the accent (the colour the stylesheet's `[data-on]` rule uses), since an inline fill would otherwise sit over that rule and hide the chosen state.
        case NYA_UI_WIDGET_BUTTON:
        case NYA_UI_WIDGET_SELECTABLE:
        case NYA_UI_WIDGET_TOGGLE:
        case NYA_UI_WIDGET_RADIO:
        case NYA_UI_WIDGET_DROPDOWN: {
            b8 chosen = (widget->kind == NYA_UI_WIDGET_SELECTABLE || widget->kind == NYA_UI_WIDGET_TOGGLE)
                        && widget->as_choice.on && !widget->state.disabled;

            NYA_Color fill = chosen ? style->accent : _nya_ui_html_button_color(&style->button, &widget->state);

            _nya_ui_html_style_color(html, "background", fill);
            _nya_ui_html_style_color(html, "border-color", fill);
            if (look->radius > 0.0F) _nya_ui_html_putf(html, ";border-radius:%dpx", (s32)look->radius);
            break;
        }

        // The accent under a chosen tab is the accent colour, flat.
        case NYA_UI_WIDGET_UNDERLINE: _nya_ui_html_style_color(html, "background", style->accent); break;

        // The rest keep the stylesheet's fill: a label and text follow the per-widget colour above, the rule and stripe are fixed sheets, and the field and slider carried their look as the custom properties above rather than a fill on the div.
        case NYA_UI_WIDGET_LABEL:
        case NYA_UI_WIDGET_COLOR_PICKER:
        case NYA_UI_WIDGET_CHART:
        case NYA_UI_WIDGET_ICON:
        case NYA_UI_WIDGET_SECTION:
        case NYA_UI_WIDGET_CHROME:
        case NYA_UI_WIDGET_GRIP:
        case NYA_UI_WIDGET_SCROLLBAR:
        case NYA_UI_WIDGET_RULE:
        case NYA_UI_WIDGET_STRIPE:
        case NYA_UI_WIDGET_KIND_COUNT:
        default:                      break;
    }

    _nya_ui_html_put(html, "\"");

    if (widget->state.disabled) _nya_ui_html_put(html, " data-disabled=\"1\"");
    if (widget->state.focused) _nya_ui_html_put(html, " data-focused=\"1\"");
    if (widget->state.held) _nya_ui_html_put(html, " data-held=\"1\"");

    // The event a click on this widget stands for, so the thin client knows what to send back: only the widgets a person acts on carry one; a label or a rule is inert.
    switch (widget->kind) {
        case NYA_UI_WIDGET_BUTTON:
        case NYA_UI_WIDGET_SELECTABLE:
        case NYA_UI_WIDGET_RADIO:
        case NYA_UI_WIDGET_TOGGLE:
        case NYA_UI_WIDGET_DROPDOWN:
        case NYA_UI_WIDGET_SECTION:
        case NYA_UI_WIDGET_CHROME: _nya_ui_html_put(html, " data-nya=\"click\""); break;

        default: break;
    }

    // The on/off widgets say which they are, for the stylesheet's `[data-on="1"]`.
    switch (widget->kind) {
        case NYA_UI_WIDGET_SELECTABLE:
        case NYA_UI_WIDGET_TOGGLE:
        case NYA_UI_WIDGET_RADIO: _nya_ui_html_putf(html, " data-on=\"%d\"", widget->as_choice.on ? 1 : 0); break;

        default: break;
    }

    _nya_ui_html_put(html, ">");

    // The contents, where a kind's shape shows: every kind is answered, so a widget added later is a -Wswitch error here rather than a blank element in a browser.
    switch (widget->kind) {
        case NYA_UI_WIDGET_LABEL:
        case NYA_UI_WIDGET_BUTTON:
        case NYA_UI_WIDGET_SELECTABLE:
        case NYA_UI_WIDGET_RADIO:
        case NYA_UI_WIDGET_SECTION:
        case NYA_UI_WIDGET_CHROME: _nya_ui_html_escape(html, widget->label); break;

        case NYA_UI_WIDGET_TOGGLE: _nya_ui_html_put(html, widget->as_choice.on ? "on" : "off"); break;

        case NYA_UI_WIDGET_DROPDOWN:
            _nya_ui_html_escape(html, widget->as_dropdown.shown != nullptr ? widget->as_dropdown.shown : widget->label);
            _nya_ui_html_put(html, " \xe2\x96\xbe");
            break;

        case NYA_UI_WIDGET_SLIDER:
            // A real range input, so the browser gives the drag and keyboard for free; the value the layout computed is its position, and `data-nya="input"` is added by the frame's default.
            _nya_ui_html_putf(html, "<input type=\"range\" min=\"0\" max=\"1000\" value=\"%d\" data-nya=\"input\">", (s32)(widget->as_slider.t * 1000.0F));
            break;

        case NYA_UI_WIDGET_FIELD:
            // A real text input: `data-nya="text"` marks it as a text write-back (the event the client posts as `{ id, event: "text", value }`, distinct from the slider's `"input"`, so a server knows to set the field's text rather than aim a pointer), and `maxlength` bounds what a browser sends to the field's capacity, which the server truncates to the same so neither can overrun.
            _nya_ui_html_putf(html, "<input type=\"text\" maxlength=\"%d\" value=\"", (s32)(NYA_UI_TEXT_INPUT_MAX - 1));
            _nya_ui_html_escape(html, widget->as_field.field.buffer != nullptr ? widget->as_field.field.buffer : "");
            _nya_ui_html_put(html, "\" data-nya=\"text\">");
            break;

        // The rest are marks and fills the stylesheet draws from the class and rectangle alone: a scrim is a dim sheet, a rule and underline are lines, a panel is a frame; their label, when they have one, is a title the frame already showed nothing of, so it goes in as text.
        case NYA_UI_WIDGET_PANEL:
        case NYA_UI_WIDGET_SCRIM:
        case NYA_UI_WIDGET_COLOR_PICKER:
        case NYA_UI_WIDGET_CHART:
        case NYA_UI_WIDGET_ICON:
        case NYA_UI_WIDGET_GRIP:
        case NYA_UI_WIDGET_SCROLLBAR:
        case NYA_UI_WIDGET_RULE:
        case NYA_UI_WIDGET_STRIPE:
        case NYA_UI_WIDGET_UNDERLINE:
            if (widget->label[0] != '\0') _nya_ui_html_escape(html, widget->label);
            break;

        case NYA_UI_WIDGET_KIND_COUNT:
        default: break;
    }

    _nya_ui_html_put(html, "</div>\n");
}

// INTERNAL

NYA_ConstCString _nya_ui_html_class(NYA_UIWidgetKind kind) {
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

NYA_Color _nya_ui_html_button_color(const NYA_UIStateColors* colors, const NYA_UIWidgetState* state) {
    // The discrete pick the terminal and skin selection make, rather than the GPU's eased mix: a static render has one state, not a frame mid-transition, so it reads the same order — disabled over held over focused over the resting colour.
    if (state->disabled) return colors->disabled;
    if (state->held) return colors->pressed;
    if (state->focused) return colors->focused;

    return colors->normal;
}

void _nya_ui_html_style_color(NYA_UIHtml* html, NYA_ConstCString prop, NYA_Color color) {
    // Alpha zero is the style's "leave it to the default", so nothing is written and the stylesheet's colour stands — the GPU backend reads the same all-zero colour as "use the style" and draws nothing new either.
    if (color.a <= 0.0F) return;

    // `prop` is one of this file's own literals, never a caller's text, so it needs no escaping.
    _nya_ui_html_putf(html, ";%s:rgba(%d,%d,%d,%.3f)", prop, (s32)(color.r * 255.0F), (s32)(color.g * 255.0F),
                      (s32)(color.b * 255.0F), (f64)color.a);
}

u32 _nya_ui_html_cells(NYA_ConstCString text, u32 bytes) {
    u32 cells = 0;

    for (u32 i = 0; i < bytes && text[i] != '\0'; i++) {
        // Every byte that is not a UTF-8 continuation starts a codepoint, which is one cell here.
        if (((u8)text[i] & 0xC0) != 0x80) cells++;
    }

    return cells;
}

void _nya_ui_html_put(NYA_UIHtml* html, NYA_ConstCString text) {
    u32 length = (u32)strlen(text);

    if (html->used + length + 1 > NYA_UI_HTML_MAX) {
        html->overflowed = true;

        // Whatever still fits, so a truncated page is still valid up to the cut rather than half an element.
        u32 room = NYA_UI_HTML_MAX - 1 - html->used;
        if (room > 0) {
            nya_memcpy(html->body + html->used, text, room);
            html->used += room;
        }

        html->body[html->used] = '\0';
        return;
    }

    nya_memcpy(html->body + html->used, text, length);
    html->used += length;
    html->body[html->used] = '\0';
}

void _nya_ui_html_putf(NYA_UIHtml* html, NYA_ConstCString format, ...) {
    char line[512] = { 0 };

    va_list args;
    va_start(args, format);
    (void)vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    _nya_ui_html_put(html, line);
}

void _nya_ui_html_escape(NYA_UIHtml* html, NYA_ConstCString text) {
    for (u32 i = 0; text[i] != '\0'; i++) {
        switch (text[i]) {
            case '&':  _nya_ui_html_put(html, "&amp;"); break;
            case '<':  _nya_ui_html_put(html, "&lt;"); break;
            case '>':  _nya_ui_html_put(html, "&gt;"); break;
            case '"':  _nya_ui_html_put(html, "&quot;"); break;
            case '\'': _nya_ui_html_put(html, "&#39;"); break;

            default: {
                char one[2] = { text[i], '\0' };
                _nya_ui_html_put(html, one);
                break;
            }
        }
    }
}

// PAGE METADATA — the tags a link unfurls with

void _nya_ui_html_meta_tag(NYA_UIHtml* scratch, NYA_ConstCString attr, NYA_ConstCString key, NYA_ConstCString value) {
    if (value == nullptr || value[0] == '\0') return;

    // `attr` and `key` are literals from _nya_ui_html_meta_head; `value` is the caller's field, so only it goes through the escaper — which turns `"` into `&quot;`, so no value can close the attribute early.
    _nya_ui_html_put(scratch, "<meta ");
    _nya_ui_html_put(scratch, attr);
    _nya_ui_html_put(scratch, "=\"");
    _nya_ui_html_put(scratch, key);
    _nya_ui_html_put(scratch, "\" content=\"");
    _nya_ui_html_escape(scratch, value);
    _nya_ui_html_put(scratch, "\">\n");
}

void _nya_ui_html_meta_head(const NYA_PageMeta* meta, char* out, u32 capacity) {
    nya_assert(meta != nullptr && out != nullptr && capacity > 0);

    out[0] = '\0';

    // The same escaper the body uses, into a scratch presenter, so the metadata and a label are escaped by one rule; the block is copied out of it below, and a field left null skips its tag.
    NYA_UIHtml scratch = { 0 };

    // A URL only reaches the head once it's a well-formed http(s) URL, so a `javascript:` or `data:` URL never lands in an href; the three URL fields share the gate.
    NYA_ConstCString url   = nya_ui_page_meta_url_ok(meta->canonical_url) ? meta->canonical_url : nullptr;
    NYA_ConstCString image = nya_ui_page_meta_url_ok(meta->image_url) ? meta->image_url : nullptr;

    // The standard description, which is not OpenGraph but every crawler reads.
    _nya_ui_html_meta_tag(&scratch, "name", "description", meta->description);

    // OpenGraph: the vocabulary Facebook, LinkedIn, Slack, Discord and most everything else unfurl from.
    _nya_ui_html_meta_tag(&scratch, "property", "og:title", meta->title);
    _nya_ui_html_meta_tag(&scratch, "property", "og:description", meta->description);
    _nya_ui_html_meta_tag(&scratch, "property", "og:type", meta->type);
    _nya_ui_html_meta_tag(&scratch, "property", "og:url", url);
    _nya_ui_html_meta_tag(&scratch, "property", "og:image", image);
    // An image's alt text is meaningless without the image, so it rides along only when the image was emitted.
    if (image != nullptr) _nya_ui_html_meta_tag(&scratch, "property", "og:image:alt", meta->image_alt);
    _nya_ui_html_meta_tag(&scratch, "property", "og:site_name", meta->site_name);
    _nya_ui_html_meta_tag(&scratch, "property", "og:locale", meta->locale);

    // Twitter Card: only when a card is chosen, since the `twitter:card` tag is what turns the rest on; title, description and image mirror the OpenGraph ones, so a page fills them once.
    if (meta->twitter_card != NYA_TWITTER_CARD_NONE) {
        NYA_ConstCString card = meta->twitter_card == NYA_TWITTER_CARD_SUMMARY_LARGE_IMAGE ? "summary_large_image" : "summary";

        _nya_ui_html_meta_tag(&scratch, "name", "twitter:card", card);
        _nya_ui_html_meta_tag(&scratch, "name", "twitter:title", meta->title);
        _nya_ui_html_meta_tag(&scratch, "name", "twitter:description", meta->description);
        _nya_ui_html_meta_tag(&scratch, "name", "twitter:image", image);
    }

    // The oEmbed discovery link a consumer follows to fetch structured metadata as JSON: its href is a URL, so it passes the same gate and is escaped like every other value.
    if (nya_ui_page_meta_url_ok(meta->oembed_url)) {
        _nya_ui_html_put(&scratch, "<link rel=\"alternate\" type=\"application/json+oembed\" href=\"");
        _nya_ui_html_escape(&scratch, meta->oembed_url);
        _nya_ui_html_put(&scratch, "\"");
        if (meta->title != nullptr && meta->title[0] != '\0') {
            _nya_ui_html_put(&scratch, " title=\"");
            _nya_ui_html_escape(&scratch, meta->title);
            _nya_ui_html_put(&scratch, "\"");
        }
        _nya_ui_html_put(&scratch, ">\n");
    }

    (void)snprintf(out, capacity, "%.*s", (s32)nya_min(scratch.used, capacity - 1), scratch.body);
}

b8 nya_ui_page_meta_url_ok(NYA_ConstCString url) {
    if (url == nullptr || url[0] == '\0') return false;

    // nya_url_parse only accepts the schemes this engine speaks (http, https, ws, wss), so `javascript:` and `data:` are refused for free; here we narrow that to the two an unfurled link may point a browser at, since a `ws(s):` endpoint is a real URL but not one a preview image or canonical page is served over.
    NYA_Url        parsed  = { 0 };
    NYA_UrlFailure failure = { 0 };
    if (!nya_url_parse(url, strlen(url), &parsed, &failure).ok) return false;

    return parsed.scheme == NYA_URL_SCHEME_HTTP || parsed.scheme == NYA_URL_SCHEME_HTTPS;
}

/** A field copied into `arena`, null terminated and truncated to NYA_PAGE_META_FIELD_MAX, so the oEmbed JSON
 *  it becomes is bounded. Null or empty gives null, which the caller reads as "no such member". */
NYA_INTERNAL NYA_ConstCString _nya_ui_page_meta_field(NYA_Arena* arena, NYA_ConstCString value) {
    if (value == nullptr || value[0] == '\0') return nullptr;

    u32   length = (u32)nya_min((u64)strlen(value), (u64)NYA_PAGE_META_FIELD_MAX);
    char* copy   = nya_arena_alloc(arena, length + 1);
    if (copy == nullptr) return nullptr;

    nya_memcpy(copy, value, length);
    copy[length] = '\0';

    return copy;
}

NYA_Error nya_ui_page_meta_oembed(NYA_Arena* arena, const NYA_PageMeta* meta, NYA_Object** out_object) {
    nya_assert(arena != nullptr && meta != nullptr && out_object != nullptr);

    *out_object = nullptr;

    NYA_Object* object = nya_object_create(arena);
    if (object == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for the oEmbed document");

    // The two members every oEmbed 1.0 response carries: a generic page is a "link"; the richer types (photo, video, rich) carry a player this metadata doesn't describe.
    nya_object_add(object, "version", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "1.0" });
    nya_object_add(object, "type", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "link" });

    // The rest are the fields the value actually holds, each bounded, each skipped when unset — the same "only when set" the head follows; the serializer JSON-escapes every value on the way out.
    NYA_ConstCString title    = _nya_ui_page_meta_field(arena, meta->title);
    NYA_ConstCString provider = _nya_ui_page_meta_field(arena, meta->site_name);
    NYA_ConstCString author   = _nya_ui_page_meta_field(arena, meta->author_name);
    NYA_ConstCString thumb    = nya_ui_page_meta_url_ok(meta->image_url) ? _nya_ui_page_meta_field(arena, meta->image_url) : nullptr;

    if (title != nullptr) nya_object_add(object, "title", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)title });
    if (provider != nullptr) nya_object_add(object, "provider_name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)provider });
    if (author != nullptr) nya_object_add(object, "author_name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)author });
    if (thumb != nullptr) nya_object_add(object, "thumbnail_url", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)thumb });

    *out_object = object;

    return NYA_OK;
}

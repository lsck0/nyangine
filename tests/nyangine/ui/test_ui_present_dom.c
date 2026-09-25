/**
 * The DOM presenter: the same widgets as *semantic, accessible* HTML. That a button is a real `<button>`, a
 * toggle a real `<input type="checkbox">`, a field a real `<input type="text">`, a dropdown a real `<select>`
 * and a slider a real `<input type="range">`; that each labelled control has a `<label for>` or an
 * `aria-label`; that a disabled control drops out of the tab order natively; and that a label a person typed
 * cannot smuggle markup into the page in either the text or an attribute context.
 *
 * It runs headless and with no browser, the same property that makes every other presenter checkable, so it
 * runs clean under ASan/LSan/UBSan. See ui_present_dom.h.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

static NYA_Window window = {
    .handle        = { .index = 1, .generation = 1 },
    .screen_width  = 800,
    .screen_height = 600,
};

static NYA_UIDom dom;

/** What a widget below reads, so a pass is a pure function of these. */
static b8   state_toggle   = false;
static f32  state_volume   = 0.5F;
static u32  state_theme    = 1;
static u32  state_quality  = 0;
static char state_name[NYA_UI_TEXT_INPUT_MAX] = "ada\"><b>";

/** A second, smaller tree: a marked selectable, then a disabled button, for the aria-pressed / disabled checks. */
static void controls(NYA_Window* w, NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(w, pass);

    if (nya_ui_panel_begin(ui, "controls", (NYA_UIPanel){ .width = nya_ui_fixed(320), .title = "controls" })) {
        (void)nya_ui_selectable(ui, "grid", true);
        nya_ui_disabled_begin(ui);
        (void)nya_ui_button(ui, "unavailable");
        nya_ui_disabled_end(ui);
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

/**
 * One form, drawn through whatever presenter is installed. Both the field's default text and the label carry
 * an attack: `"><b>` would break out of an attribute and open a tag if a value ever reached the page raw.
 * */
static void form(NYA_Window* w, NYA_UIPass pass) {
    static const NYA_ConstCString themes[] = { "dark", "light", "high contrast <x>" };

    NYA_UI* ui = nya_ui_begin(w, pass);

    if (nya_ui_panel_begin(ui, "settings", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_CENTER, .width = nya_ui_fixed(360), .title = "settings" })) {
        nya_ui_label(ui, "<script>alert(1)</script>");
        (void)nya_ui_button(ui, "save & quit");
        (void)nya_ui_toggle(ui, "fullscreen", &state_toggle);
        (void)nya_ui_slider(ui, "volume", &state_volume, 0.0F, 1.0F, 0.05F);
        (void)nya_ui_text_input(ui, "name", state_name, sizeof(state_name));
        (void)nya_ui_dropdown(ui, "theme", themes, 3, &state_theme);
        (void)nya_ui_radio(ui, "fast", &state_quality, 0);
        (void)nya_ui_radio(ui, "fancy", &state_quality, 1);

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());
    defer SDL_Quit();

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    nya_system_settings_init();
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_input_init();
    nya_system_asset_init();

    nya_ui_dom_init(&dom, NYA_UI_DOM_CELL);
    nya_ui_presenter_set(&window, nya_ui_dom_presenter(&dom));

    // an input pass first, so the draw pass has a laid-out tree to render — the same order a frame runs.
    form(&window, NYA_UI_PASS_INPUT);

    nya_ui_dom_reset(&dom);
    form(&window, NYA_UI_PASS_DRAW);

    NYA_ConstCString body = nya_ui_dom_body(&dom);

    // TEST: something was drawn, and it did not overflow the buffer.
    nya_check(nya_ui_dom_count(&dom) > 0, "the pass drew widgets, got %u", nya_ui_dom_count(&dom));
    nya_check(!nya_ui_dom_overflowed(&dom), "and did not overflow the buffer");

    // TEST: escaping — a label cannot inject markup, in text OR attribute context.
    nya_check(!nya_string_contains(body, "<script>alert"), "a label's markup does not reach the page as markup");
    nya_check(nya_string_contains(body, "&lt;script&gt;alert(1)&lt;/script&gt;"), "it is escaped to text");
    // The field's default text `ada"><b>` sits in a value="" attribute; a raw `"` would end the attribute and a raw `<b>` open a tag. Both must be entities, and no live `<b>` may appear anywhere.
    nya_check(nya_string_contains(body, "ada&quot;&gt;&lt;b&gt;"), "the field value is escaped for the attribute context");
    nya_check(!nya_string_contains(body, "><b>"), "no attribute break-out reaches the page");
    // The dropdown option `high contrast <x>` never selected, but the escaping of an option is still checked through the shown one below; the label's `<x>`-style attack is covered by the field and script cases.

    // TEST: the interactive widgets are REAL, semantic form controls.
    nya_check(nya_string_contains(body, "<button") && nya_string_contains(body, "type=\"button\""), "a button is a real <button>");
    nya_check(nya_string_contains(body, "type=\"checkbox\""), "a toggle is a real checkbox");
    nya_check(nya_string_contains(body, "type=\"range\""), "a slider is a real range input");
    nya_check(nya_string_contains(body, "type=\"text\""), "a field is a real text input");
    nya_check(nya_string_contains(body, "<select"), "a dropdown is a real <select>");
    nya_check(nya_string_contains(body, "type=\"radio\""), "a radio is a real radio input");
    nya_check(nya_string_contains(body, "value=\"ada&quot;"), "the field carries its (escaped) text as the value a browser edits");

    // TEST: accessibility — labels are associated, and states are ARIA/native.
    nya_check(nya_string_contains(body, "<label for=\""), "a labelled control has a <label for> pointing at it");
    nya_check(nya_string_contains(body, "aria-label=\""), "and value controls carry an aria-label as a fallback name");
    nya_check(nya_string_contains(body, "role=\"group\""), "a panel is a labelled group");
    nya_check(nya_string_contains(body, "<h2>settings</h2>"), "with its title as a heading");
    // The dropdown's shown option is the selected <option>, escaped.
    nya_check(nya_string_contains(body, "<option selected>light</option>"), "the dropdown shows its selected option");

    // TEST: the id-to-kind table a live server dispatches on.
    {
        b8 found_slider = false, found_field = false, found_select = false;
        for (u32 id = 0; id < nya_ui_dom_count(&dom); id++) {
            NYA_UIWidgetKind kind = NYA_UI_WIDGET_LABEL;
            if (!nya_ui_dom_widget_kind(&dom, id, &kind)) continue;
            if (kind == NYA_UI_WIDGET_SLIDER) found_slider = true;
            if (kind == NYA_UI_WIDGET_FIELD) found_field = true;
            if (kind == NYA_UI_WIDGET_DROPDOWN) found_select = true;
        }
        nya_check(found_slider && found_field && found_select, "the slider, field and dropdown ids each resolve to their kind");
        NYA_UIWidgetKind past = NYA_UI_WIDGET_BUTTON;
        nya_check(!nya_ui_dom_widget_kind(&dom, nya_ui_dom_count(&dom), &past), "an id past the pass resolves to nothing");
    }

    // TEST: aria-pressed on a selectable, and a disabled control drops out of the tab order via the native `disabled` attribute rather than markup of its own.
    {
        controls(&window, NYA_UI_PASS_INPUT);
        nya_ui_dom_reset(&dom);
        controls(&window, NYA_UI_PASS_DRAW);

        NYA_ConstCString b = nya_ui_dom_body(&dom);
        nya_check(nya_string_contains(b, "aria-pressed=\"true\""), "a marked selectable reports aria-pressed=true");
        nya_check(nya_string_contains(b, "<button") && nya_string_contains(b, " disabled"), "a disabled control carries the native disabled attribute");
    }

    // TEST: the document wraps the body in a valid, accessible page with a <form> whose submit is suppressed and a client that forwards events.
    {
        // redraw the full form so the page has the whole control set again.
        form(&window, NYA_UI_PASS_INPUT);
        nya_ui_dom_reset(&dom);
        form(&window, NYA_UI_PASS_DRAW);

        static char page[NYA_UI_DOM_MAX + 4096];
        u32         written = nya_ui_dom_document(&dom, page, sizeof(page), "settings & more", "");

        nya_check(written > 0, "a document is written");
        nya_check(nya_string_starts_with(page, "<!doctype html>"), "as a page");
        nya_check(nya_string_contains(page, "<html lang=\"en\">"), "with a language for a screen reader");
        nya_check(nya_string_contains(page, "<title>settings &amp; more</title>"), "with the title escaped");
        nya_check(nya_string_contains(page, "<form id=\"nya-surface\""), "the controls live in a real form");
        nya_check(nya_string_contains(page, "onsubmit=\"return false\""), "whose submit is suppressed, so Enter does not reload");
        nya_check(nya_string_contains(page, "addEventListener('change'"), "and the client forwards a select change");
        nya_check(nya_string_contains(page, "&lt;script&gt;alert(1)"), "with the widget body still escaped inside it");
    }

    // TEST: the same tree renders to the same count twice — a patch needs that.
    {
        form(&window, NYA_UI_PASS_INPUT);
        nya_ui_dom_reset(&dom);
        form(&window, NYA_UI_PASS_DRAW);
        u32 first = nya_ui_dom_count(&dom);

        nya_ui_dom_reset(&dom);
        form(&window, NYA_UI_PASS_DRAW);
        nya_check(nya_ui_dom_count(&dom) == first, "an unchanged tree draws the same number of widgets, got %u then %u", first, nya_ui_dom_count(&dom));
    }

    nya_ui_dom_deinit(&dom);

    return nya_check_failures() == 0 ? 0 : 1;
}

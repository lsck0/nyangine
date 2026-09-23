/**
 * The HTML presenter: the same widgets as DOM elements. That every widget becomes one positioned
 * element with a stable id, that a label a person typed cannot smuggle markup into the page, that the
 * interactive kinds carry the event a client sends back, and that the whole thing wraps into a page with
 * one stylesheet and a thin client.
 *
 * It runs headless and with no browser: the presenter writes into the caller's buffer and nothing else,
 * the same property that makes the TUI backend checkable. See ui_present_html.h.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

static NYA_Window window = {
    .handle        = { .index = 1, .generation = 1 },
    .screen_width  = 800,
    .screen_height = 600,
};

static NYA_UIHtml html;

/** What a widget below reads, so a pass is a pure function of these. */
static b8   state_toggle = false;
static f32  state_volume = 0.5F;
static char state_name[NYA_UI_TEXT_INPUT_MAX] = "ada";

/**
 * One menu, drawn through whatever presenter is installed. The `<script>` label is the attack: a name a
 * person could set, which must come out as text and never as markup.
 * */
static void menu(NYA_Window* w, NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(w, pass);

    if (nya_ui_panel_begin(ui, "settings", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_CENTER, .width = nya_ui_fixed(360), .title = "settings" })) {
        nya_ui_label(ui, "<script>alert(1)</script>");
        (void)nya_ui_button(ui, "save & quit");
        (void)nya_ui_toggle(ui, "fullscreen", &state_toggle);
        (void)nya_ui_slider(ui, "volume", &state_volume, 0.0F, 1.0F, 0.05F);
        (void)nya_ui_text_input(ui, "name", state_name, sizeof(state_name));

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

    nya_ui_html_init(&html, NYA_UI_HTML_CELL);
    nya_ui_presenter_set(&window, nya_ui_html_presenter(&html));

    // an input pass first, so the draw pass has a laid-out tree to render — the same order a frame runs.
    menu(&window, NYA_UI_PASS_INPUT);

    nya_ui_html_reset(&html);
    menu(&window, NYA_UI_PASS_DRAW);

    NYA_ConstCString body = nya_ui_html_body(&html);

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: something was drawn, into positioned elements with stable ids.
    // ─────────────────────────────────────────────────────────────────────────────
    nya_check(nya_ui_html_count(&html) > 0, "the pass drew widgets, got %u", nya_ui_html_count(&html));
    nya_check(!nya_ui_html_overflowed(&html), "and did not overflow the buffer");
    nya_check(nya_string_contains(body, "id=\"w0\""), "the first widget is w0");
    nya_check(nya_string_contains(body, "position") == false, "elements are positioned by the stylesheet, not inline position");
    nya_check(nya_string_contains(body, "left:") && nya_string_contains(body, "top:"), "each element is placed from the computed rect");

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the label cannot inject markup — the whole point of escaping.
    // ─────────────────────────────────────────────────────────────────────────────
    nya_check(!nya_string_contains(body, "<script>alert"), "a label's markup does not reach the page as markup");
    nya_check(nya_string_contains(body, "&lt;script&gt;alert(1)&lt;/script&gt;"), "it is escaped to text");

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the interactive widgets carry the event a client sends back.
    // ─────────────────────────────────────────────────────────────────────────────
    nya_check(nya_string_contains(body, "class=\"nya-button\""), "the button is a button");
    nya_check(nya_string_contains(body, "data-nya=\"click\""), "and carries a click event");
    nya_check(nya_string_contains(body, "class=\"nya-toggle\""), "the toggle is a toggle");
    nya_check(nya_string_contains(body, "type=\"range\""), "the slider is a real range input");
    nya_check(nya_string_contains(body, "type=\"text\" value=\"ada\""), "the field carries its text");
    nya_check(nya_string_contains(body, "data-nya=\"input\""), "and reports input");

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the same tree renders to the same ids twice — a patch needs that.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u32 first = nya_ui_html_count(&html);

        nya_ui_html_reset(&html);
        menu(&window, NYA_UI_PASS_DRAW);

        nya_check(nya_ui_html_count(&html) == first, "an unchanged tree draws the same number of widgets, got %u then %u", first, nya_ui_html_count(&html));
        nya_check(nya_string_contains(nya_ui_html_body(&html), "id=\"w0\""), "with the same first id");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the document wraps the body in a page with the stylesheet and the client.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        static char page[NYA_UI_HTML_MAX + 4096];
        u32         written = nya_ui_html_document(&html, page, sizeof(page), "settings & more", "");

        nya_check(written > 0, "a document is written");
        nya_check(nya_string_starts_with(page, "<!doctype html>"), "as a page");
        nya_check(nya_string_contains(page, "<title>settings &amp; more</title>"), "with the title escaped");
        nya_check(nya_string_contains(page, "id=\"nya-surface\""), "and the surface the client patches");
        nya_check(nya_string_contains(page, ".nya-button"), "the stylesheet that colours a button");
        nya_check(nya_string_contains(page, "addEventListener('click'"), "and the client that forwards a click");
        nya_check(nya_string_contains(page, "&lt;script&gt;alert(1)"), "with the widget body still escaped inside it");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a program's custom style reaches the browser as inline CSS. A distinctive
    // accent and panel colour and a larger radius must show on the right elements,
    // and a colour left at alpha zero must fall through to the stylesheet, unwritten.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        // The button body colour is deliberately alpha zero on every state: opaque blue would be unmistakable
        // in the output, so its absence proves the presenter honours "alpha zero means the default".
        nya_ui_style_set(&window, (NYA_UIStyle){
                                      .panel  = { 0.0F, 1.0F, 0.0F, 1.0F },   // green, on the panel
                                      .accent = { 1.0F, 0.0F, 0.0F, 1.0F },   // red, on the chosen toggle
                                      .radius = 16.0F,
                                      .button = { .normal   = { 0.0F, 0.0F, 1.0F, 0.0F },
                                                  .focused  = { 0.0F, 0.0F, 1.0F, 0.0F },
                                                  .pressed  = { 0.0F, 0.0F, 1.0F, 0.0F },
                                                  .disabled = { 0.0F, 0.0F, 1.0F, 0.0F } },
                                  });

        state_toggle = true;   // so the toggle is "on" and wears the accent

        menu(&window, NYA_UI_PASS_INPUT);

        nya_ui_html_reset(&html);
        menu(&window, NYA_UI_PASS_DRAW);

        NYA_ConstCString styled = nya_ui_html_body(&html);

        nya_check(nya_string_contains(styled, "background:rgba(0,255,0"), "the custom panel colour reaches the panel as inline CSS");
        nya_check(nya_string_contains(styled, "rgba(255,0,0"), "and the custom accent reaches the chosen toggle");
        nya_check(nya_string_contains(styled, "border-radius:16px"), "and the larger radius rounds the styled widgets");
        nya_check(!nya_string_contains(styled, "0,0,255"), "a colour left at alpha zero is not emitted, leaving the stylesheet's default");
    }

    nya_ui_html_deinit(&html);

    return nya_check_failures() == 0 ? 0 : 1;
}

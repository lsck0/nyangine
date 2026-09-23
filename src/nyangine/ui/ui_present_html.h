/**
 * @file ui_present_html.h
 *
 * The presenter that draws a UI pass as HTML, so the same component code that runs on a GPU and in a
 * terminal renders in a browser with no client of its own.
 *
 * ```c
 * static NYA_UIHtml html;
 * nya_ui_html_init(&html, (f32x2){ 8.0F, 18.0F });
 * nya_ui_presenter_set(window, nya_ui_html_presenter(&html));
 *
 * nya_ui_html_reset(&html);
 * menu(window, NYA_UI_PASS_DRAW);         // the same pass the GPU and the terminal run
 *
 * char page[32 * 1024] = { 0 };
 * nya_ui_html_document(&html, page, sizeof(page), "my app");   // a whole page, or...
 * NYA_ConstCString fragment = nya_ui_html_body(&html);         // ...just the elements, for a patch
 * ```
 *
 * ── the model: server-rendered, live, thin client ──
 *
 * This is the render half of a Phoenix-LiveView-shaped loop, and the UI's own input pass is the other
 * half. The server holds the state and runs the immediate-mode pass; the browser holds nothing but the
 * DOM. The round trip is:
 *
 *   1. the server runs NYA_UI_PASS_DRAW through this presenter and sends the HTML
 *   2. the browser shows it; every interactive widget carries an `id` and a `data-nya` event name
 *   3. a click or a keystroke on one is sent back — `{ id, event, value }` — over a WebSocket or a POST
 *   4. the server feeds that into NYA_UI_PASS_INPUT, which is what a mouse and a keyboard feed locally,
 *      then runs the draw pass again and sends the changed HTML
 *
 * The client is a few lines of JavaScript that forward events and swap in HTML; it is emitted by
 * nya_ui_html_document so a program ships no framework and writes no front-end. That is the whole point:
 * one component, and the browser is just another presenter — the DOM where the terminal had cells and
 * the GPU had triangles.
 *
 * This file is the render half only. Feeding the events back into the input pass and diffing one HTML
 * against the last are a server's job over `http`/`http_websocket`, and are the next slice; what is here
 * is complete and testable on its own, which is why it is a presenter and not a server.
 *
 * ── positioned, not reflowed ──
 *
 * Every element is placed absolutely from the rectangle the layout already computed, the same numbers
 * the GPU draws at. The browser is not asked to lay anything out, so what it shows is pixel-for-pixel
 * what the native window shows, and a widget that fits on the GPU fits here. A later, semantic mode that
 * emits nested flexbox instead is possible — the widget stream carries the container nesting in its
 * declaration order — but exact placement is what makes "the same UI" literally true first.
 *
 * ── the ids are stable, and that is what makes a patch possible ──
 *
 * A widget's id is its position in the pass — `w0`, `w1`, … — assigned in declaration order and reset
 * each pass. As long as the tree is the same shape, a widget keeps its id across renders, so the client
 * can patch element `w7` in place rather than replacing the page. A tree that changes shape between
 * renders renumbers from the branch that changed, which is the same thing every server-rendered
 * framework does and the reason a stable outer structure matters.
 *
 * ── what a browser adds back ──
 *
 * Colour, fonts and the exact look come from the one stylesheet nya_ui_html_document emits, keyed on a
 * class per widget kind, so a program restyles its whole UI in CSS without touching a widget. The
 * presenter itself writes no colour but the one a caller set on a specific widget, exactly as the GPU
 * backend takes the style's colour unless a widget overrode it.
 *
 * ── the bounds ──
 *
 * The HTML accumulates into a fixed buffer, NYA_UI_HTML_MAX bytes, because a UI pass is bounded and so
 * is what it renders to. A pass that overflows it is truncated and nya_ui_html_overflowed says so, the
 * same bargain the recorder makes with its widget table.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_shapes.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/ui/ui_present.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Bytes of HTML one pass may render to, terminator included. A menu is a few kilobytes; this is generous. */
#ifndef NYA_UI_HTML_MAX
#define NYA_UI_HTML_MAX 65536
#endif

/** Widgets one pass may render, which bounds the id-to-rectangle table a live server reads back. */
#ifndef NYA_UI_HTML_MAX_WIDGETS
#define NYA_UI_HTML_MAX_WIDGETS 256
#endif

/** The default metric, in pixels: a monospace cell, so a measurement here matches the terminal's grid. */
#define NYA_UI_HTML_CELL ((f32x2){ 8.0F, 18.0F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_UIHtml NYA_UIHtml;

/**
 * The HTML presenter's caller-owned buffer, which outlives a pass and allocates nothing.
 *
 * Prepared by nya_ui_html_init and installed as nya_ui_html_presenter(html). Reset before each pass;
 * read after it with nya_ui_html_body or nya_ui_html_document.
 * */
struct NYA_UIHtml {
    NYA_UIPresenter presenter;

    /** One character cell in pixels, the metric measurement is a multiple of; see the header. */
    f32x2 cell;

    /** The elements this pass drew, concatenated. Not a whole page — nya_ui_html_document wraps it. */
    char body[NYA_UI_HTML_MAX];
    u32  used;

    /** Widgets drawn this pass, which is the next widget's id and the count for the ceiling audit. */
    u32 sequence;

    /**
     * The rectangle each widget was drawn at, indexed by its id, up to NYA_UI_HTML_MAX_WIDGETS.
     *
     * A live server needs this and nothing else the browser has: to turn a click on element `wN` back
     * into a pointer over that widget, it looks up rect N and aims the synthetic pointer at its centre.
     * */
    NYA_Rectf rects[NYA_UI_HTML_MAX_WIDGETS];

    /** The current back-to-front layer, written as a z-index so a raised panel covers an earlier one. */
    s32 layer;

    /** Set when the pass wanted more than NYA_UI_HTML_MAX, so a caller can tell a truncated page from a whole one. */
    b8 overflowed;

    /** The looks at each style depth, and which is in force, exactly as the recorder keeps them. */
    NYA_UILook looks[NYA_UI_STYLE_DEPTH_MAX + 1];
    u32        depth;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Prepares `html` and its presenter. `cell` is the pixel metric text is measured in; zero for the default.
 *
 * A monospace cell rather than a real face, for the reason the recorder uses one: the layout is then
 * exact arithmetic, the browser re-measures with its own font anyway, and the two agree on structure.
 * */
NYA_API void nya_ui_html_init(NYA_UIHtml* html, f32x2 cell);

/** Clears `html` entirely. The presenter it held is no longer valid; init again to reuse the memory. */
NYA_API void nya_ui_html_deinit(NYA_UIHtml* html);

/** Empties the body and the id counter before a pass. Everything a pass wrote before is gone. */
NYA_API void nya_ui_html_reset(NYA_UIHtml* html);

/** The presenter to hand nya_ui_presenter_set. Valid until nya_ui_html_deinit. */
NYA_API const NYA_UIPresenter* nya_ui_html_presenter(NYA_UIHtml* html) __attr_no_discard;

/** The elements this pass drew, as one string. For a live patch, where only the fragment travels. */
NYA_API NYA_ConstCString nya_ui_html_body(const NYA_UIHtml* html) __attr_no_discard;

/** How many widgets the last pass drew, which is also the first free id. */
NYA_API u32 nya_ui_html_count(const NYA_UIHtml* html) __attr_no_discard;

/** Whether the last pass wanted more room than NYA_UI_HTML_MAX, so its HTML is truncated. */
NYA_API b8 nya_ui_html_overflowed(const NYA_UIHtml* html) __attr_no_discard;

/**
 * The rectangle widget `id` was drawn at, for a server turning a click on `wN` into a pointer.
 *
 * False for an id the last pass did not draw or one past the table, in which case `out_rect` is zeroed.
 * */
NYA_API b8 nya_ui_html_rect(const NYA_UIHtml* html, u32 id, OUT NYA_Rectf* out_rect) __attr_no_discard;

/**
 * Writes a whole page around the body: a doctype, the one stylesheet that colours every widget kind, the
 * body's elements, and the few lines of client script that forward events and swap in patches.
 *
 * `title` is the page's, escaped. `script_nonce`, when not empty, is written as the `nonce` on the one
 * inline script, so a page served under a strict Content-Security-Policy can allow that script by nonce
 * rather than by `'unsafe-inline'`; pass "" when there is no CSP to satisfy. Returns the bytes written,
 * terminator excluded, and truncates rather than overrun `capacity`. This is the first-load response;
 * nya_ui_html_body is what every later patch sends.
 * */
NYA_API u32 nya_ui_html_document(const NYA_UIHtml* html, OUT char* out, u32 capacity, NYA_ConstCString title, NYA_ConstCString script_nonce);

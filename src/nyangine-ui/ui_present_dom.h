/**
 * @file ui_present_dom.h
 *
 * The presenter that draws a UI pass as *semantic* HTML: a real `<form>`, real `<input>`, `<button>` and
 * `<select>` elements, `<label for>` associations and ARIA roles, so a password manager, autofill, an IME
 * and a screen reader all work on the same widget tree the GPU and the terminal draw.
 *
 * ```c
 * static NYA_UIDom dom;
 * nya_ui_dom_init(&dom, (f32x2){ 8.0F, 18.0F });
 * nya_ui_presenter_set(window, nya_ui_dom_presenter(&dom));
 *
 * nya_ui_dom_reset(&dom);
 * menu(window, NYA_UI_PASS_DRAW);          // the same pass the GPU and the terminal run
 *
 * char page[32 * 1024] = { 0 };
 * nya_ui_dom_document(&dom, page, sizeof(page), "my app", "");   // a whole accessible page, or...
 * NYA_ConstCString fragment = nya_ui_dom_body(&dom);             // ...just the controls, for a patch
 * ```
 *
 * ── why a second HTML presenter ──
 *
 * ui_present_html emits one absolutely-positioned `<div>` per widget: pixel-for-pixel what the native
 * window shows, and exactly right when the point is that the page *looks* the same. But a `<div role>` is
 * not an `<input>`. A password manager will not offer to fill a positioned `<div>`, autofill has nothing
 * to key on, an IME has no editable element to compose into, and a screen reader announces a wall of
 * generic groups. This presenter trades exact placement for real controls: the browser lays the form out,
 * so it does not look pixel-identical to the GPU window, but every interactive widget is the native
 * element assistive technology and the browser's own machinery already understand.
 *
 * The two share a seam and a widget stream and differ only in `draw`. A program picks one by which
 * presenter it installs; a server can serve the accessible form to a browser and keep the positioned one
 * for a canvas view. Neither replaces the other.
 *
 * ── the element mapping ──
 *
 *   BUTTON, CHROME, SECTION   `<button type="button">`, SECTION carrying `aria-expanded`
 *   SELECTABLE                `<button type="button" aria-pressed>`
 *   TOGGLE                    `<input type="checkbox">` wrapped in its `<label>`
 *   RADIO                     `<input type="radio">` wrapped in its `<label>`
 *   SLIDER                    `<input type="range" min max value>` with a `<label for>`
 *   FIELD                     `<input type="text" value maxlength>` with a `<label for>`
 *   DROPDOWN                  `<select>` with the shown option selected, and a `<label for>`
 *   COLOR_PICKER              `<input type="color" value="#rrggbb">` with a `<label for>`
 *   LABEL                     `<p>` of text
 *   PANEL                     `<section role="group" aria-label>` with an `<h2>` title
 *   SCRIM, RULE, STRIPE, …    decorative `<div aria-hidden="true">`
 *
 * Every control is a real, natively focusable element, so tab order is DOM order — which is declaration
 * order, the same order the GPU draws in — and needs no `tabindex`. A disabled widget carries the native
 * `disabled` attribute, which both greys it and drops it out of the tab order the way a screen reader
 * user expects.
 *
 * ── security ──
 *
 * Every value a person can set — a label, a field's text, a dropdown's option, a panel title — is HTML
 * escaped for both the text and the attribute context (`< > & " '`), so a name like `<script>` can never
 * become markup or break out of an attribute. The escaping is the same rule ui_present_html uses; see
 * _nya_ui_dom_escape.
 *
 * ── the bounds ──
 *
 * The HTML accumulates into a fixed buffer, NYA_UI_DOM_MAX bytes, the same bargain ui_present_html makes:
 * a pass that overflows it is truncated and nya_ui_dom_overflowed says so.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_object.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/math/math_shapes.h"
#include "nyangine-std/math/math_vector.h"
#include "nyangine-ui/ui_present.h"

// CONSTANTS

/** Bytes of HTML one semantic pass may render to, terminator included. A form is a few kilobytes; this is generous. */
#ifndef NYA_UI_DOM_MAX
#define NYA_UI_DOM_MAX 65536
#endif

/** Widgets one pass may render, which bounds the id-to-kind table a live server reads back. */
#ifndef NYA_UI_DOM_MAX_WIDGETS
#define NYA_UI_DOM_MAX_WIDGETS 256
#endif

/** The default metric, in pixels: a monospace cell, so a measurement here matches the terminal's grid. */
#define NYA_UI_DOM_CELL ((f32x2){ 8.0F, 18.0F })

// TYPES

typedef struct NYA_UIDom NYA_UIDom;

/**
 * The semantic-HTML presenter's caller-owned buffer, which outlives a pass and allocates nothing.
 *
 * Prepared by nya_ui_dom_init and installed as nya_ui_dom_presenter(dom). Reset before each pass; read
 * after it with nya_ui_dom_body or nya_ui_dom_document.
 * */
struct NYA_UIDom {
    NYA_UIPresenter presenter;

    /** One character cell in pixels, the metric measurement is a multiple of; shared with the recorder's rule. */
    f32x2 cell;

    /** The controls this pass drew, concatenated. Not a whole page — nya_ui_dom_document wraps it. */
    char body[NYA_UI_DOM_MAX];
    u32  used;

    /** Widgets drawn this pass, which is the next widget's id and the count for the ceiling audit. */
    u32 sequence;

    /** What kind each id was, so a server knows a click is a button but a `change` is a select or a checkbox. */
    NYA_UIWidgetKind kinds[NYA_UI_DOM_MAX_WIDGETS];

    /** Set when the pass wanted more than NYA_UI_DOM_MAX, so a caller can tell a truncated page from a whole one. */
    b8 overflowed;

    /** The looks at each style depth, and which is in force, exactly as the recorder and the html presenter keep them. */
    NYA_UILook looks[NYA_UI_STYLE_DEPTH_MAX + 1];
    u32        depth;
};

// FUNCTIONS

/**
 * Prepares `dom` and its presenter. `cell` is the pixel metric text is measured in; zero for the default.
 *
 * A monospace cell rather than a real face, for the reason the recorder and the html presenter use one:
 * the layout is then exact arithmetic and the browser re-measures with its own font anyway.
 * */
NYA_API void nya_ui_dom_init(NYA_UIDom* dom, f32x2 cell);

/** Clears `dom` entirely. The presenter it held is no longer valid; init again to reuse the memory. */
NYA_API void nya_ui_dom_deinit(NYA_UIDom* dom);

/** Empties the body and the id counter before a pass. Everything a pass wrote before is gone. */
NYA_API void nya_ui_dom_reset(NYA_UIDom* dom);

/** The presenter to hand nya_ui_presenter_set. Valid until nya_ui_dom_deinit. */
NYA_API const NYA_UIPresenter* nya_ui_dom_presenter(NYA_UIDom* dom) __attr_no_discard;

/** The controls this pass drew, as one string. For a live patch, where only the fragment travels. */
NYA_API NYA_ConstCString nya_ui_dom_body(const NYA_UIDom* dom) __attr_no_discard;

/** How many widgets the last pass drew, which is also the first free id. */
NYA_API u32 nya_ui_dom_count(const NYA_UIDom* dom) __attr_no_discard;

/** Whether the last pass wanted more room than NYA_UI_DOM_MAX, so its HTML is truncated. */
NYA_API b8 nya_ui_dom_overflowed(const NYA_UIDom* dom) __attr_no_discard;

/** What widget `id` was, for a server dispatching an event by kind. False for an id the last pass did not draw. */
NYA_API b8 nya_ui_dom_widget_kind(const NYA_UIDom* dom, u32 id, OUT NYA_UIWidgetKind* out_kind) __attr_no_discard;

/**
 * Writes a whole accessible page around the body: a doctype, a small stylesheet, the body's controls inside
 * a `<form>` whose submit is suppressed, and the few lines of client script that forward events and swap in
 * patches.
 *
 * `title` is the page's, escaped. `script_nonce`, when not empty, is written as the `nonce` on the one inline
 * script, so a page served under a strict Content-Security-Policy can allow that script by nonce rather than
 * by `'unsafe-inline'`; pass "" when there is no CSP to satisfy. Returns the bytes written, terminator
 * excluded, and truncates rather than overrun `capacity`.
 * */
NYA_API u32 nya_ui_dom_document(const NYA_UIDom* dom, OUT char* out, u32 capacity, NYA_ConstCString title, NYA_ConstCString script_nonce);

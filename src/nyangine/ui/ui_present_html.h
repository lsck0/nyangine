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

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_shapes.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/ui/ui_present.h"

// CONSTANTS

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

/**
 * Bytes one page-metadata value may render to in the `<head>`, terminator included.
 *
 * A dozen short tags, each an escaped field of the value — generous for that and small enough to build on
 * the stack. A value whose escaped fields want more than this is truncated the way the body is.
 * */
#ifndef NYA_PAGE_META_HEAD_MAX
#define NYA_PAGE_META_HEAD_MAX 4096
#endif

/**
 * The longest a single metadata field is copied at when it becomes an oEmbed value.
 *
 * The head escaper already bounds what a field contributes there; this bounds the oEmbed JSON, so a caller
 * that hands over a novel for a title answers a truncated one rather than an unbounded response body.
 * */
#ifndef NYA_PAGE_META_FIELD_MAX
#define NYA_PAGE_META_FIELD_MAX 1024
#endif

// TYPES

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

    /** What kind each id was, so a server knows a click is a button but an `input` is a slider or a field. */
    NYA_UIWidgetKind kinds[NYA_UI_HTML_MAX_WIDGETS];

    /**
     * The interactive sub-rectangle of a value widget: a slider's track, a field's box, zero for the rest.
     *
     * A value event carries a number, and a slider takes the value the pointer is over its track — so a
     * server turns `input=700` into a pointer at 70% along this rectangle, the same arithmetic a real drag
     * is. See nya_ui_html_widget.
     * */
    NYA_Rectf value_rects[NYA_UI_HTML_MAX_WIDGETS];

    /** The current back-to-front layer, written as a z-index so a raised panel covers an earlier one. */
    s32 layer;

    /** Set when the pass wanted more than NYA_UI_HTML_MAX, so a caller can tell a truncated page from a whole one. */
    b8 overflowed;

    /** The looks at each style depth, and which is in force, exactly as the recorder keeps them. */
    NYA_UILook looks[NYA_UI_STYLE_DEPTH_MAX + 1];
    u32        depth;
};

typedef enum NYA_TwitterCard NYA_TwitterCard;
typedef struct NYA_PageMeta  NYA_PageMeta;

/**
 * Which Twitter Card a link unfurls as, or none.
 *
 * `_NONE` (the zero) emits no `twitter:*` tag at all, so a zeroed NYA_PageMeta stays quiet; the two real
 * cards are the ones a page without a player needs — a small thumbnail beside the text, or a large one above.
 * */
enum NYA_TwitterCard {
    NYA_TWITTER_CARD_NONE = 0,
    NYA_TWITTER_CARD_SUMMARY,
    NYA_TWITTER_CARD_SUMMARY_LARGE_IMAGE,
};

/**
 * The metadata a link unfurls with: the title, blurb and image a social network or a chat app shows when
 * someone pastes a URL this server answers. It is threaded into the SSR `<head>` as OpenGraph, Twitter Card
 * and a standard description, and it is what the oEmbed endpoint answers from.
 *
 * Every field is optional and a zeroed value emits nothing new — the default page is unchanged — so a caller
 * fills only what it has. A tag is emitted only when its field is set; see nya_ui_html_document_meta.
 *
 * ── security ──
 *
 * Every field is escaped for the attribute context of the head (the same escaper a label goes through, `"`
 * included) and JSON-escaped for oEmbed, so nothing here can break out of a tag or a string. A URL field —
 * `canonical_url`, `image_url`, `oembed_url` — is validated as an http(s) URL and dropped otherwise, so a
 * `javascript:` or `data:` URL never reaches `og:url`, `og:image` or the discovery link. See
 * nya_ui_page_meta_url_ok.
 * */
struct NYA_PageMeta {
    /** The page's title, for `og:title` and `twitter:title`. The `<title>` element is set separately. */
    NYA_ConstCString title;

    /** A sentence about the page, for `<meta name="description">`, `og:description` and `twitter:description`. */
    NYA_ConstCString description;

    /** The canonical http(s) URL of the page, for `og:url`. Dropped when it is not an http(s) URL. */
    NYA_ConstCString canonical_url;

    /** An http(s) URL of the preview image, for `og:image` and `twitter:image`. Dropped when not http(s). */
    NYA_ConstCString image_url;

    /** Alternative text for the image, for `og:image:alt`. Only emitted when the image URL is. */
    NYA_ConstCString image_alt;

    /** The site's name, for `og:site_name` and the oEmbed `provider_name`. */
    NYA_ConstCString site_name;

    /** The author's name, for the oEmbed `author_name`. */
    NYA_ConstCString author_name;

    /** The OpenGraph object type, for `og:type`: "website", "article", "video.other", … . */
    NYA_ConstCString type;

    /** Which Twitter Card to unfurl as; NYA_TWITTER_CARD_NONE emits no `twitter:*` tag. */
    NYA_TwitterCard twitter_card;

    /** The locale, for `og:locale`: "en_US", "de_DE", … . */
    NYA_ConstCString locale;

    /**
     * The http(s) URL of this page's oEmbed endpoint, for the `<link rel="alternate" type="application/json
     * +oembed">` a consumer follows to fetch structured metadata. Dropped when it is not an http(s) URL.
     * */
    NYA_ConstCString oembed_url;
};

// FUNCTIONS

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
 * What widget `id` was, and its interactive value rectangle, for a server turning an `input` event back
 * into a pointer.
 *
 * `out_kind` is the widget's kind; `out_value_rect` is its track (slider) or box (field), zero for a
 * kind that has none. False for an id the last pass did not draw, in which case both are zeroed.
 * */
NYA_API b8 nya_ui_html_widget(const NYA_UIHtml* html, u32 id, OUT NYA_UIWidgetKind* out_kind, OUT NYA_Rectf* out_value_rect) __attr_no_discard;

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

/**
 * nya_ui_html_document, plus the social-media embedding metadata in `meta` woven into the `<head>`.
 *
 * The same page, with a standard `<meta name="description">`, the OpenGraph tags (`og:title`,
 * `og:description`, `og:type`, `og:url`, `og:image`, `og:image:alt`, `og:site_name`, `og:locale`), the
 * Twitter Card tags (`twitter:card`, `twitter:title`, `twitter:description`, `twitter:image`), and — when
 * `meta->oembed_url` is set — the `<link rel="alternate" type="application/json+oembed">` discovery tag a
 * consumer follows to the oEmbed endpoint. So a link to this page unfurls with a title, blurb and image.
 *
 * A tag is emitted only when its field is set, and a null or zeroed `meta` emits nothing new, so this is a
 * drop-in for nya_ui_html_document that a page opts into by filling the value. Every field is escaped for
 * the head's attribute context and every URL field is validated as http(s); see NYA_PageMeta. The existing
 * `<title>` still comes from `title`, escaped, unchanged.
 * */
NYA_API u32 nya_ui_html_document_meta(const NYA_UIHtml* html, OUT char* out, u32 capacity, NYA_ConstCString title, NYA_ConstCString script_nonce,
                                      const NYA_PageMeta* meta);

/**
 * Whether `url` is a URL safe to place in `href`/`src`: a well-formed absolute http(s) URL and nothing else.
 *
 * False for null, empty, a `javascript:` or `data:` URL, a `ws(s):` one, or anything nya_url_parse refuses.
 * This is the gate every URL field passes before it reaches the head or the oEmbed response.
 * */
NYA_API b8 nya_ui_page_meta_url_ok(NYA_ConstCString url) __attr_no_discard;

/**
 * Builds the oEmbed response document for `meta` into `*out_object`, allocated from `arena`.
 *
 * A "link"-type oEmbed 1.0 object — `version`, `type`, `title`, `provider_name` (from `site_name`),
 * `author_name`, and `thumbnail_url` (from `image_url`, only when it is a valid http(s) URL). Each field is
 * bounded to NYA_PAGE_META_FIELD_MAX so the rendered JSON is bounded; the serializer escapes every value, so
 * this is what an `/oembed?url=…` handler renders with nya_http_response_json. Never fails on a zeroed
 * `meta`: `version` and `type` are always present, which is the minimum a valid oEmbed response carries.
 * */
NYA_API NYA_Error nya_ui_page_meta_oembed(NYA_Arena* arena, const NYA_PageMeta* meta, OUT NYA_Object** out_object) __attr_no_discard;

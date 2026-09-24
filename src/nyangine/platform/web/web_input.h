/**
 * @file web_input.h
 *
 * Canvas pointer and keyboard input, drained the way the engine drains a device queue.
 *
 * ```
 * nya_web_input_attach   listen on a canvas (by CSS selector) and its keyboard
 * nya_web_input_detach   stop listening and drop anything still queued
 * nya_web_input_poll     take the oldest event, oldest-first, one per call
 * ```
 *
 * ── where this sits ──
 *
 * The renderer half of the web platform (the SDL_GPU → GLES3 shim) puts pixels on a canvas; this is how
 * that canvas talks back. A wasm module has no SDL event pump — the browser owns the DOM events — so the
 * module cannot poll SDL for a mouse move the way core_event.c does on native. This primitive is the
 * missing seam: it attaches the four pointer listeners and the two key listeners the browser fires, buffers
 * them in a small queue, and hands them back one at a time to be turned into whatever the caller wants —
 * an engine NYA_Event, a direct scene nudge, a UI pointer.
 *
 * ── why polled, like the other seams ──
 *
 * It keeps the cooperative rhythm the socket and fetch seams keep: the listeners never call into wasm, they
 * only push onto a JS queue, and `poll` drains that queue after each tick the same way nya_net_transport_poll
 * drains a transport. So the wasm module reads input exactly where and when it reads the network — never
 * re-entrantly from inside a DOM callback. Coordinates come back in the canvas' drawing-buffer pixels (the
 * space the shim's viewport and render2d's orthographic projection agree on), not CSS pixels, so an event's
 * x/y lands where the sprite under the cursor is drawn regardless of the canvas' displayed size.
 *
 * The seam is engine-agnostic on purpose: it names no NYA_Keycode and no NYA_Event, only a flat event with
 * a kind, a position and a key code. A printable key arrives as its lowercase Unicode code point (so 'a' is
 * 0x61, matching the engine's own keycode for it); the non-printable keys this primitive forwards arrive as
 * the NYA_WEB_KEY_* sentinels below, which a caller maps to its own vocabulary.
 *
 * Off wasm there is no DOM: `attach` refuses (returns false) and `poll` never yields an event, so the seam
 * compiles and stays null-safe in the native tree the way fetch and the client socket do.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What a queued event is. The pointer kinds carry x/y (and the wheel its delta); the key kinds carry a key. */
typedef enum {
    /** The pointer moved. x/y is the new position; delta_x/delta_y is the movement since the last move. */
    NYA_WEB_INPUT_MOUSE_MOVED,

    /** A mouse button went down at x/y. `button` is 1 left, 2 middle, 3 right, matching NYA_MOUSE_BUTTON_*. */
    NYA_WEB_INPUT_MOUSE_DOWN,

    /** A mouse button came up at x/y. `button` as for MOUSE_DOWN. */
    NYA_WEB_INPUT_MOUSE_UP,

    /** The wheel turned over x/y. delta_x/delta_y is the notch amount (browser deltaX/deltaY, sign as the DOM gives it). */
    NYA_WEB_INPUT_WHEEL,

    /** A key went down. `key` is its code (a Unicode code point or a NYA_WEB_KEY_* sentinel); `repeat` is auto-repeat. */
    NYA_WEB_INPUT_KEY_DOWN,

    /** A key came up. `key` as for KEY_DOWN; `repeat` is always false. */
    NYA_WEB_INPUT_KEY_UP,
} NYA_WebInputKind;

/*
 * The non-printable keys this seam forwards, as key codes carried in NYA_WebInputEvent.key. Chosen in a
 * private range well above the Unicode code points a printable key delivers, so a caller can switch on the
 * code without a separate "is this a character" flag. Named apart from the KEY_DOWN/KEY_UP event kinds
 * above (which say a key changed state) — these say which key it was.
 */
#define NYA_WEB_KEY_LEFT  0x01000001u
#define NYA_WEB_KEY_RIGHT 0x01000002u
#define NYA_WEB_KEY_UP    0x01000003u
#define NYA_WEB_KEY_DOWN  0x01000004u

/** One drained event. A flat record: which fields carry meaning is set by `kind` (see the enum above). */
typedef struct {
    NYA_WebInputKind kind;

    /** Pointer position, in the canvas' drawing-buffer pixels. Meaningful for the mouse and wheel kinds. */
    f32 x, y;

    /** A move's movement since the previous move, or the wheel's notch amount. Zero for the key and button kinds. */
    f32 delta_x, delta_y;

    /** Which mouse button, for MOUSE_DOWN/MOUSE_UP: 1 left, 2 middle, 3 right. Zero for the other kinds. */
    s32 button;

    /** The key code, for KEY_DOWN/KEY_UP: a lowercase Unicode code point, or a NYA_WEB_KEY_* sentinel. */
    u32 key;

    /** True when a KEY_DOWN is the browser's auto-repeat rather than a fresh press. Always false otherwise. */
    b8 repeat;
} NYA_WebInputEvent;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Starts listening on the canvas `selector` names (a CSS selector, e.g. "#canvas") and on the keyboard, and
 * begins queuing events for `poll` to drain. Returns false when `selector` is null, when no element matches,
 * and off wasm where there is no DOM. Calling it again re-points the listeners at whatever `selector` now
 * names and clears the queue, so it is safe to call on a resize or a canvas swap. The pointer listeners sit
 * on the canvas; the key listeners sit on the window, and the keys this seam forwards (the arrows and space)
 * have their default page action suppressed so the page does not scroll under the module.
 * */
NYA_API b8 nya_web_input_attach(NYA_ConstCString selector) __attr_no_discard;

/** Stops listening and drops any queued events. A no-op when nothing is attached, so it is always safe to call. */
NYA_API void nya_web_input_detach(void);

/**
 * Takes the oldest queued event into `out_event` and returns true, or returns false when the queue is empty
 * (and always, off wasm). Poll it in a loop each tick until it returns false to drain the frame's input.
 * Never writes `out_event` when it returns false; null `out_event` is refused.
 * */
NYA_API b8 nya_web_input_poll(OUT NYA_WebInputEvent* out_event) __attr_no_discard;

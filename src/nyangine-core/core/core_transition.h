/**
 * @file core_transition.h
 *
 * A screen transition: a full-frame overlay that covers or reveals the frame over a fixed duration, for
 * the moment a game swaps what it draws. Fixed state, no allocation, and free when nothing is running.
 *
 * ```c
 * // swap now, then let the new screen emerge from black over half a second.
 * gny_show_next_screen();
 * nya_transition_begin(NYA_TRANSITION_FADE_IN, 0.5F, NYA_COLOR_BLACK, NYA_EASE_CUBIC_OUT);
 *
 * // or cover first and swap once covered.
 * nya_transition_begin(NYA_TRANSITION_FADE_OUT, 0.5F, NYA_COLOR_BLACK, NYA_EASE_CUBIC_IN);
 * if (!nya_transition_active()) gny_show_next_screen();
 * ```
 *
 * The overlay is drawn by the app loop with the ordinary 2D rectangle path, over the frame the layers
 * left behind: no post pass and no shader of its own. A FADE is a full-screen rect whose alpha is the
 * coverage; a WIPE is an opaque rect whose width is. An OUT goes from clear to covered, an IN from covered
 * to clear, so a swap that happens on one tick pairs with an IN and never shows the new screen uncovered.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/math/math_tween.h"

// NYA_Color is the renderer's, so it is not included from here: this header is parsed after it in the
// umbrella, the same way core_entity.h and core_scene.h name a colour without inverting the layering.
typedef struct NYA_Color  NYA_Color;
typedef struct NYA_Window NYA_Window;

/** Which shape the overlay takes. See nya_transition_begin. */
typedef enum NYA_TransitionKind {
    /** No transition; the zero value, so a zeroed state is idle. */
    NYA_TRANSITION_NONE = 0,

    /** A full-screen rectangle whose alpha eases from clear to `color`. */
    NYA_TRANSITION_FADE_OUT,

    /** A full-screen rectangle whose alpha eases from `color` to clear. */
    NYA_TRANSITION_FADE_IN,

    /** An opaque rectangle of `color` that grows from the left edge across the screen. */
    NYA_TRANSITION_WIPE_OUT,

    /** An opaque rectangle of `color` that shrinks back toward the left edge. */
    NYA_TRANSITION_WIPE_IN,

    NYA_TRANSITION_COUNT,
} NYA_TransitionKind;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Starts a transition, replacing any already running. A `duration_s` at or below zero starts nothing,
 * so a caller need not special-case an instant switch. `ease` shapes the progress; its zero is
 * NYA_EASE_LINEAR. `color` is what a FADE fades to and what a WIPE is painted in.
 * */
NYA_API void nya_transition_begin(NYA_TransitionKind kind, f32 duration_s, NYA_Color color, NYA_EaseType ease);

/** Ends whatever is running, if anything, at once. The next frame draws nothing. */
NYA_API void nya_transition_end(void);

/** Whether a transition is running. False once the duration has elapsed. */
NYA_API b8 nya_transition_active(void) __attr_no_discard;

/**
 * The eased progress in [0, 1], zero when nothing is running. The overlay's coverage (a FADE's alpha, a
 * WIPE's fraction of the width) is this for an OUT and one minus it for an IN.
 * */
NYA_API f32 nya_transition_alpha(void) __attr_no_discard;

/** The running kind, or NYA_TRANSITION_NONE when idle. */
NYA_API NYA_TransitionKind nya_transition_kind(void) __attr_no_discard;

/** The colour the running transition was begun with. */
NYA_API NYA_Color nya_transition_color(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * DRIVEN BY THE APP LOOP
 * ─────────────────────────────────────────────────────────
 */

/** Advances the running transition by `delta_time_s`, completing it when the duration is reached. */
NYA_API void nya_system_transition_update(f32 delta_time_s);

/** Draws the overlay for `window` when one is running, with the 2D rectangle path. A no-op when idle. */
NYA_API void nya_transition_draw(NYA_Window* window);

/**
 * The screen transition primitive: a bounded overlay whose progress runs 0 -> 1 over a fixed
 * duration on a clock the caller advances, and which is inactive, and so draws nothing, when idle.
 *
 * The clock is driven by hand with nya_system_transition_update, the same call the app loop makes each
 * frame, so the timeline is exercised without a window or a real frame.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    // Nothing running: idle, drawing nothing, and reading as NONE.
    {
        nya_check(!nya_transition_active(), "a fresh transition is not running");
        nya_check(nya_transition_alpha() == 0.0F, "and its alpha is zero, so it draws nothing");
        nya_check(nya_transition_kind() == NYA_TRANSITION_NONE, "and its kind is NONE");
    }

    // A linear fade runs its alpha 0 -> 1 across the duration, then completes.
    {
        nya_transition_begin(NYA_TRANSITION_FADE_OUT, 1.0F, NYA_COLOR_BLACK, NYA_EASE_LINEAR);

        nya_check(nya_transition_active(), "a begun transition is running");
        nya_check(nya_transition_kind() == NYA_TRANSITION_FADE_OUT, "and reports its kind");
        nya_check(nya_transition_alpha() == 0.0F, "and starts fully transparent");

        // A quarter of the way, linear alpha is a quarter.
        nya_system_transition_update(0.25F);
        nya_check(fabsf(nya_transition_alpha() - 0.25F) < 0.001F, "a quarter in reads 0.25, got %f", (f64)nya_transition_alpha());

        // Monotonic on the way up: another quarter is more, never less.
        f32 before = nya_transition_alpha();
        nya_system_transition_update(0.25F);
        nya_check(nya_transition_alpha() > before, "alpha only rises while running");
        nya_check(fabsf(nya_transition_alpha() - 0.5F) < 0.001F, "half in reads 0.5, got %f", (f64)nya_transition_alpha());

        // Near the end it is nearly opaque, still running.
        nya_system_transition_update(0.49F);
        nya_check(nya_transition_active(), "still running just short of the duration");
        nya_check(nya_transition_alpha() > 0.98F, "and nearly opaque, got %f", (f64)nya_transition_alpha());

        // Crossing the duration completes it: idle again, and back to drawing nothing.
        nya_system_transition_update(0.02F);
        nya_check(!nya_transition_active(), "past the duration it is done");
        nya_check(nya_transition_alpha() == 0.0F, "and draws nothing again");
        nya_check(nya_transition_kind() == NYA_TRANSITION_NONE, "and reads as NONE again");
    }

    // Advancing an idle transition does nothing and cannot revive it.
    {
        nya_system_transition_update(1.0F);
        nya_check(!nya_transition_active(), "an idle clock stays idle");
    }

    // Easing bends the curve without leaving [0, 1]: an ease-out is ahead of linear at the midpoint.
    {
        nya_transition_begin(NYA_TRANSITION_FADE_OUT, 1.0F, NYA_COLOR_BLACK, NYA_EASE_CUBIC_OUT);
        nya_system_transition_update(0.5F);
        f32 eased = nya_transition_alpha();
        nya_check(eased > 0.5F && eased <= 1.0F, "cubic-out is ahead of linear at the midpoint, got %f", (f64)eased);
        nya_transition_end();
    }

    // The colour it was begun with is what it reports, so the overlay draws in it.
    {
        nya_transition_begin(NYA_TRANSITION_WIPE_OUT, 2.0F, NYA_COLOR_WHITE, NYA_EASE_LINEAR);
        nya_check(nya_transition_active(), "the wipe is running");
        nya_check(nya_transition_kind() == NYA_TRANSITION_WIPE_OUT, "and reports the wipe kind");

        NYA_Color color = nya_transition_color();
        nya_check(color.r == 1.0F && color.g == 1.0F && color.b == 1.0F, "and hands back the colour it was begun with");

        // End stops a running transition at once.
        nya_transition_end();
        nya_check(!nya_transition_active(), "end stops it");
        nya_check(nya_transition_alpha() == 0.0F, "and it draws nothing after");
    }

    // A zero-length transition is an instant switch: it starts nothing to draw.
    {
        nya_transition_begin(NYA_TRANSITION_FADE_OUT, 0.0F, NYA_COLOR_BLACK, NYA_EASE_LINEAR);
        nya_check(!nya_transition_active(), "a zero duration starts nothing");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}

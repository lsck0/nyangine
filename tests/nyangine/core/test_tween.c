/**
 * The tween system: interpolation, delay, repeat, yoyo, sequences, cancellation and handle staleness.
 *
 * The two behaviours worth pinning hardest are that `from` is read when the tween *begins* rather than
 * when it is created — which is what makes a sequence work at all — and that a stale handle resolves to
 * nothing rather than to whatever reused its slot.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define TICK (1.0F / 60.0F)

static void step(f32 seconds) {
    u32 ticks = (u32)(seconds / TICK) + 1;
    for (u32 i = 0; i < ticks; i++) nya_system_tween_update(TICK);
}

static u32   completions   = 0;
static void* completed_for = nullptr;

/* Externally linked, not static: a callback is resolved by name through the registry. */
void tween_test_on_complete(void* address) {
    completions++;
    completed_for = address;
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    nya_system_tween_init();
    defer nya_system_tween_deinit();
    defer nya_system_callback_deinit();

    // ── A scalar tween reaches its target and finishes.
    {
        f32       value = 0.0F;
        NYA_Tween t     = nya_tween_f32(&value, 10.0F, 0.5F);

        nya_check(nya_tween_active(t), "a new tween should be active");
        nya_check(nya_tween_count() == 1, "one tween running, got %u", nya_tween_count());

        step(0.25F);
        nya_check(value > 0.0F && value < 10.0F, "halfway should be between the ends, got %f", (f64)value);

        step(0.30F);
        nya_check(fabsf(value - 10.0F) < 0.001F, "it should land exactly on the target, got %f", (f64)value);
        nya_check(!nya_tween_active(t), "and stop being active");
        nya_check(nya_tween_count() == 0, "and free its slot");
    }

    // ── A stale handle resolves to nothing rather than to whatever reused the slot.
    {
        f32       value = 0.0F;
        NYA_Tween old   = nya_tween_f32(&value, 1.0F, 0.1F);
        step(0.2F);
        nya_check(!nya_tween_active(old), "the finished tween is gone");

        f32       other = 0.0F;
        NYA_Tween fresh = nya_tween_f32(&other, 1.0F, 10.0F);
        nya_check(!nya_tween_active(old), "the old handle must not resolve to the new tween");
        nya_check(nya_tween_active(fresh), "but the new one does");

        nya_tween_cancel(fresh);
    }

    // ── Delay: nothing moves until it elapses, and `from` is read at that moment, not at creation.
    {
        f32 value = 0.0F;
        (void)nya_tween_f32(&value, 10.0F, 0.5F, .delay = 0.3F);

        step(0.2F);
        nya_check(value == 0.0F, "a delayed tween should not have moved yet, got %f", (f64)value);

        // Move the value while the tween waits. The tween must start from here, not from zero.
        value = 100.0F;
        step(0.6F);
        nya_check(fabsf(value - 10.0F) < 0.001F, "it should still land on the target, got %f", (f64)value);
    }

    // ── Vector targets write every component and leave neighbouring memory alone.
    {
        struct { f32x3 v; f32 guard; } packed = { .v = { 0.0F, 0.0F, 0.0F }, .guard = 1234.0F };

        // Parenthesised: the preprocessor would otherwise split on the commas inside the braces.
        (void)nya_tween_f32x3(&packed.v, ((f32x3){ 1.0F, 2.0F, 3.0F }), 0.2F);
        step(0.3F);

        nya_check(fabsf(packed.v.x - 1.0F) < 0.001F && fabsf(packed.v.y - 2.0F) < 0.001F
                      && fabsf(packed.v.z - 3.0F) < 0.001F,
                  "every component should reach its target, got (%f, %f, %f)", (f64)packed.v.x, (f64)packed.v.y, (f64)packed.v.z);
        nya_check(packed.guard == 1234.0F, "a f32x3 tween must not write past three floats");
    }

    // ── Completion callbacks run once, with the address, and survive through the registry.
    {
        completions   = 0;
        completed_for = nullptr;

        f32 value = 0.0F;
        (void)nya_tween_f32(&value, 1.0F, 0.1F, .on_complete = nya_callback(tween_test_on_complete));

        step(0.2F);
        nya_check(completions == 1, "the callback should run exactly once, ran %u", completions);
        nya_check(completed_for == &value, "and be handed the address it animated");

        step(0.5F);
        nya_check(completions == 1, "and not run again afterwards, ran %u", completions);
    }

    // ── Cancelling stops the tween, leaves the value alone, and does not run the callback.
    {
        completions = 0;

        f32       value = 0.0F;
        NYA_Tween t     = nya_tween_f32(&value, 10.0F, 1.0F, .on_complete = nya_callback(tween_test_on_complete));

        step(0.3F);
        f32 midway = value;

        nya_tween_cancel(t);
        nya_check(!nya_tween_active(t), "cancelling should stop it");

        step(1.0F);
        nya_check(value == midway, "the value should stay where it was, %f vs %f", (f64)value, (f64)midway);
        nya_check(completions == 0, "a cancelled tween must not run its completion callback");
    }

    // ── Cancel-by-target takes everything writing to an address. This is the teardown call.
    {
        f32 value = 0.0F;
        f32 other = 0.0F;

        (void)nya_tween_f32(&value, 1.0F, 5.0F);
        (void)nya_tween_f32(&value, 2.0F, 5.0F);
        (void)nya_tween_f32(&other, 3.0F, 5.0F);

        nya_check(nya_tween_cancel_target(&value) == 2, "both tweens on that address should be cancelled");
        nya_check(nya_tween_count() == 1, "the unrelated one should survive, got %u", nya_tween_count());
        nya_check(nya_tween_cancel_target(nullptr) == 0, "a null address cancels nothing");

        nya_tween_cancel_all();
        nya_check(nya_tween_count() == 0, "cancel_all should empty the pool");
    }

    // ── Repeat restarts from the original value rather than drifting.
    {
        f32 value = 0.0F;
        (void)nya_tween_f32(&value, 10.0F, 0.1F, .repeat = 3);

        step(0.35F);
        nya_check(fabsf(value - 10.0F) < 0.5F, "three runs should end at the target, got %f", (f64)value);
        nya_check(nya_tween_count() == 0, "and then finish");
    }

    // ── Yoyo comes back to where it started on an even number of runs.
    {
        f32 value = 0.0F;
        (void)nya_tween_f32(&value, 10.0F, 0.1F, .repeat = 2, .yoyo = true);

        step(0.25F);
        nya_check(fabsf(value) < 0.5F, "two yoyo runs should return to the start, got %f", (f64)value);
    }

    // ── Forever does not finish on its own.
    {
        f32       value = 0.0F;
        NYA_Tween t     = nya_tween_f32(&value, 10.0F, 0.05F, .repeat = NYA_TWEEN_REPEAT_FOREVER);

        step(2.0F);
        nya_check(nya_tween_active(t), "a forever tween should still be running");

        nya_tween_cancel(t);
    }

    // ── A zero duration is a set: it lands immediately rather than never.
    {
        f32 value = 0.0F;
        (void)nya_tween_f32(&value, 7.0F, 0.0F);

        step(TICK);
        nya_check(fabsf(value - 7.0F) < 0.001F, "a zero-duration tween should land at once, got %f", (f64)value);
        nya_check(nya_tween_count() == 0, "and finish");
    }

    // ── A sequence runs its steps in order, each waiting for the one before.
    {
        f32 a = 0.0F;
        f32 b = 0.0F;

        (void)nya_tween_sequence(
            (NYA_TweenSpec[]){
                nya_tween_spec_f32(&a, 1.0F, 0.2F),
                nya_tween_spec_f32(&b, 1.0F, 0.2F),
            },
            2
        );

        nya_check(nya_tween_count() == 2, "the whole sequence exists immediately, got %u", nya_tween_count());

        step(0.25F);
        nya_check(fabsf(a - 1.0F) < 0.01F, "the first step should be done, got %f", (f64)a);
        nya_check(b < 0.9F, "the second should not be, got %f", (f64)b);

        step(0.3F);
        nya_check(fabsf(b - 1.0F) < 0.01F, "and then finish, got %f", (f64)b);
        nya_check(nya_tween_count() == 0, "leaving nothing running");
    }

    /*
     * ── A delay expiring must not shorten the frame for the tweens after it in the pool.
     *
     * The update loop hands the remainder of the frame to a tween whose delay just ran out. That
     * remainder used to be written back into the loop's own delta_time_s, so every slot visited
     * afterwards was advanced by the leftover instead of by the real frame — and because slots are
     * walked in index order, whether a tween ran slow depended on which slot it happened to get.
     */
    {
        f32 delayed = 0.0F;
        f32 plain   = 0.0F;

        // The delayed one first, so it is the lower slot index and is visited first.
        (void)nya_tween_f32(&delayed, 1.0F, 1.0F, .delay = TICK / 2.0F);
        (void)nya_tween_f32(&plain, 1.0F, 1.0F);

        // One tick past the delay. The delayed tween gets half a tick, the plain one a whole tick.
        nya_system_tween_update(TICK);

        nya_check(fabsf(plain - TICK) < 0.0005F, "the second tween should have advanced a full tick, got %f", (f64)plain);
        nya_check(fabsf(delayed - (TICK / 2.0F)) < 0.0005F, "and the delayed one only the half tick left to it, got %f",
                  (f64)delayed);

        nya_tween_cancel_target(&delayed);
        nya_tween_cancel_target(&plain);
    }

    // ── nya_tween_progress reports the eased fraction, and 1 for anything that is not running.
    {
        f32 value = 0.0F;

        nya_check(nya_tween_progress(NYA_TWEEN_NONE) == 1.0F, "a handle that names nothing reads as arrived");

        NYA_Tween handle = nya_tween_f32(&value, 10.0F, 1.0F, .delay = 0.5F);
        nya_check(nya_tween_progress(handle) == 0.0F, "a tween still waiting out its delay has not started");

        step(0.5F + 0.5F);
        f32 progress = nya_tween_progress(handle);
        nya_check(fabsf(progress - (value / 10.0F)) < 0.01F,
                  "progress should be the fraction the value was built from: %f against %f", (f64)progress, (f64)(value / 10.0F));

        step(1.0F);
        nya_check(nya_tween_progress(handle) == 1.0F, "and a finished tween reads as arrived");
        nya_check(!nya_tween_active(handle), "which is the same thing as its handle no longer resolving");
    }

    // ── An empty sequence is not an error.
    {
        nya_check(nya_tween_sequence(nullptr, 0).index == 0, "a null sequence yields no handle");
        nya_check(nya_tween_sequence((NYA_TweenSpec[]){ nya_tween_spec_f32(nullptr, 0.0F, 0.0F) }, 0).index == 0,
                  "a zero-count sequence yields no handle");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}

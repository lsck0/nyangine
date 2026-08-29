/**
 * Damped springs: convergence, the damping ratio's effect, and stability under a bad timestep.
 *
 * The property that matters most is the one an ease cannot give: retargeting mid-flight bends the
 * motion instead of restarting it, because the velocity carries. The stability check is the other —
 * explicit Euler on these parameters diverges, and the point of the semi-implicit step is that it does not.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define TICK (1.0F / 60.0F)

s32 main(void) {
    // ── A critically damped spring converges and does not overshoot.
    {
        NYA_SpringF32 spring = { .value = 0.0F, .frequency = 4.0F, .damping = 1.0F };

        f32 highest = 0.0F;
        for (u32 i = 0; i < 240; i++) {
            f32 v = nya_spring_f32(&spring, 1.0F, TICK);
            highest = nya_max(highest, v);
        }

        nya_check(fabsf(spring.value - 1.0F) < 0.01F, "it should arrive, got %f", (f64)spring.value);
        nya_check(highest <= 1.02F, "critically damped should not overshoot, peaked at %f", (f64)highest);
        nya_check(nya_spring_f32_settled(&spring, 1.0F, 0.01F), "and report itself settled");
    }

    // ── Underdamped overshoots; overdamped does not and is slower.
    {
        NYA_SpringF32 bouncy = { .frequency = 4.0F, .damping = 0.3F };
        NYA_SpringF32 sludgy = { .frequency = 4.0F, .damping = 3.0F };

        f32 bouncy_peak = 0.0F;
        f32 sludgy_peak = 0.0F;
        for (u32 i = 0; i < 240; i++) {
            bouncy_peak = nya_max(bouncy_peak, nya_spring_f32(&bouncy, 1.0F, TICK));
            sludgy_peak = nya_max(sludgy_peak, nya_spring_f32(&sludgy, 1.0F, TICK));
        }

        nya_check(bouncy_peak > 1.05F, "an underdamped spring should overshoot, peaked at %f", (f64)bouncy_peak);
        nya_check(sludgy_peak <= 1.01F, "an overdamped one should not, peaked at %f", (f64)sludgy_peak);
    }

    // ── Zeroed frequency and damping fall back to usable defaults rather than dividing by nothing.
    {
        NYA_SpringF32 spring = { 0 };
        for (u32 i = 0; i < 240; i++) (void)nya_spring_f32(&spring, 5.0F, TICK);

        nya_check(fabsf(spring.value - 5.0F) < 0.05F, "a zeroed spring should still converge, got %f", (f64)spring.value);
    }

    // ── Retargeting mid-flight carries velocity instead of restarting. This is the whole point.
    {
        NYA_SpringF32 spring = { .frequency = 4.0F, .damping = 1.0F };

        for (u32 i = 0; i < 20; i++) (void)nya_spring_f32(&spring, 1.0F, TICK);
        nya_check(spring.velocity > 0.0F, "it should be moving toward the first target");

        f32 carried = spring.velocity;
        (void)nya_spring_f32(&spring, 2.0F, TICK);
        nya_check(spring.velocity > carried * 0.5F, "a new target must not throw the velocity away");

        for (u32 i = 0; i < 400; i++) (void)nya_spring_f32(&spring, 2.0F, TICK);
        nya_check(fabsf(spring.value - 2.0F) < 0.01F, "and it should arrive at the new target, got %f", (f64)spring.value);
    }

    // ── A stalled frame is clamped rather than integrated whole. Explicit Euler would diverge here.
    {
        NYA_SpringF32 spring = { .frequency = 20.0F, .damping = 1.0F };

        // Two seconds in one step: a debugger pause, not motion.
        for (u32 i = 0; i < 30; i++) (void)nya_spring_f32(&spring, 1.0F, 2.0F);

        nya_check(isfinite((f64)spring.value), "a stiff spring must survive a huge step, got %f", (f64)spring.value);
        nya_check(fabsf(spring.value) < 100.0F, "and not fly off, got %f", (f64)spring.value);
    }

    // ── A non-positive step does nothing.
    {
        NYA_SpringF32 spring = { .value = 3.0F, .frequency = 4.0F };
        nya_check(nya_spring_f32(&spring, 99.0F, 0.0F) == 3.0F, "a zero step should not move it");
        nya_check(nya_spring_f32(&spring, 99.0F, -1.0F) == 3.0F, "nor a negative one");
    }

    // ── Reset drops velocity, so the next step starts clean.
    {
        NYA_SpringF32 spring = { .frequency = 4.0F };
        for (u32 i = 0; i < 20; i++) (void)nya_spring_f32(&spring, 10.0F, TICK);
        nya_check(spring.velocity != 0.0F, "it should have picked up speed");

        nya_spring_f32_reset(&spring, 0.0F);
        nya_check(spring.value == 0.0F && spring.velocity == 0.0F, "reset should zero both");
        nya_check(!nya_spring_f32_settled(&spring, 10.0F, 0.01F), "and it is not settled at a distant target");
    }

    // ── The vector springs converge componentwise.
    {
        NYA_SpringF32x2 two   = { .frequency = 5.0F };
        NYA_SpringF32x3 three = { .frequency = 5.0F };

        for (u32 i = 0; i < 300; i++) {
            (void)nya_spring_f32x2(&two, (f32x2){ 1.0F, -2.0F }, TICK);
            (void)nya_spring_f32x3(&three, (f32x3){ 1.0F, -2.0F, 3.0F }, TICK);
        }

        nya_check(fabsf(two.value.x - 1.0F) < 0.01F && fabsf(two.value.y + 2.0F) < 0.01F,
                  "f32x2 should converge, got (%f, %f)", (f64)two.value.x, (f64)two.value.y);
        nya_check(fabsf(three.value.x - 1.0F) < 0.01F && fabsf(three.value.y + 2.0F) < 0.01F
                      && fabsf(three.value.z - 3.0F) < 0.01F,
                  "f32x3 should converge, got (%f, %f, %f)", (f64)three.value.x, (f64)three.value.y, (f64)three.value.z);

        nya_spring_f32x2_reset(&two, f32x2_zero);
        nya_spring_f32x3_reset(&three, f32x3_zero);
        nya_check(two.value.x == 0.0F && three.value.z == 0.0F, "reset should zero the vector springs");
    }

    // ── Settled needs both position and velocity: passing through at speed is not arrival.
    {
        NYA_SpringF32 spring = { .value = 1.0F, .velocity = 50.0F, .frequency = 4.0F };
        nya_check(!nya_spring_f32_settled(&spring, 1.0F, 0.01F), "moving fast through the target is not settled");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}

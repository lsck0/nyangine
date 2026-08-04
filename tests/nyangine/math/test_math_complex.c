/**
 * Complex numbers.
 *
 * The arithmetic itself is the compiler's (_Complex), and cabs/carg/conj come from <tgmath.h>, so
 * what is worth testing is the engine's own additions and the contracts math_complex.h states about
 * them: the zero cases that would otherwise divide by zero, and the shortest-arc rule in slerp.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define EPS 1.0e-5

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: construction and component access
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: construction\n");
  {
    c64 z = nya_complex_f64(3.0, 4.0);
    nya_assert(nya_complex_real(z) == 3.0);
    nya_assert(nya_complex_imag(z) == 4.0);

    // The generic form dispatches on the argument width.
    c32 z32 = nya_complex(1.0F, 2.0F);
    nya_assert(nya_complex_real(z32) == 1.0F);
    nya_assert(nya_complex_imag(z32) == 2.0F);

    c64 z64 = nya_complex(1.0, 2.0);
    nya_assert(nya_complex_real(z64) == 1.0);
    nya_assert(nya_complex_imag(z64) == 2.0);

    // The accessors are lvalues, which is why they are __real__/__imag__ rather than creal/cimag.
    nya_complex_imag(z) = 0.0;
    nya_assert(nya_complex_imag(z) == 0.0);
    nya_assert(nya_complex_real(z) == 3.0);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: polar construction, and that it inverts cabs/carg
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: polar\n");
  {
    c64 z = nya_complex_from_polar(2.0, M_PI / 3.0);

    nya_assert(fabs(cabs(z) - 2.0) < EPS);
    nya_assert(fabs(carg(z) - M_PI / 3.0) < EPS);

    // Round trip the other way: pull a number apart and rebuild it.
    c64 original = nya_complex_f64(-1.5, 0.75);
    c64 rebuilt  = nya_complex_from_polar(cabs(original), carg(original));
    nya_assert(nya_complex_approx_equals(rebuilt, original, EPS));

    // A zero magnitude is the origin whatever the angle.
    nya_assert(cabs(nya_complex_from_polar(0.0, 1.234)) < EPS);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_complex_unit is e^(iθ), and multiplying by it rotates
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: unit\n");
  {
    c64 u = nya_complex_unit(M_PI_2);

    nya_assert(fabs(cabs(u) - 1.0) < EPS);
    nya_assert(fabs(nya_complex_real(u)) < EPS);          // cos(π/2) = 0
    nya_assert(fabs(nya_complex_imag(u) - 1.0) < EPS);    // sin(π/2) = 1

    // A quarter turn takes 1 to i, and i to -1.
    c64 one = nya_complex_f64(1.0, 0.0);
    nya_assert(nya_complex_approx_equals(one * u, nya_complex_f64(0.0, 1.0), EPS));
    nya_assert(nya_complex_approx_equals(one * u * u, nya_complex_f64(-1.0, 0.0), EPS));

    // Rotation never changes magnitude.
    c64 z = nya_complex_f64(3.0, -4.0);
    nya_assert(fabs(cabs(z * u) - cabs(z)) < EPS);

    // A full turn is the identity.
    nya_assert(nya_complex_approx_equals(z * nya_complex_unit(2.0 * M_PI), z, 1.0e-4));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: magnitude_squared
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: magnitude_squared\n");
  {
    c64 z = nya_complex_f64(3.0, 4.0);
    nya_assert(fabs(nya_complex_magnitude_squared(z) - 25.0) < EPS);

    // Consistent with cabs, which is the whole point of offering it as a cheaper comparison.
    nya_assert(fabs(nya_complex_magnitude_squared(z) - cabs(z) * cabs(z)) < EPS);
    nya_assert(nya_complex_magnitude_squared(nya_complex_f64(0.0, 0.0)) == 0.0);

    c32 z32 = nya_complex_f32(3.0F, 4.0F);
    nya_assert(fabsf(nya_complex_magnitude_squared(z32) - 25.0F) < 1.0e-4F);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: normalize, including the zero case it promises not to divide by
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: normalize\n");
  {
    c64 z = nya_complex_normalize(nya_complex_f64(3.0, 4.0));
    nya_assert(fabs(cabs(z) - 1.0) < EPS);

    // Direction is preserved: 3+4i normalises to 0.6+0.8i.
    nya_assert(fabs(nya_complex_real(z) - 0.6) < EPS);
    nya_assert(fabs(nya_complex_imag(z) - 0.8) < EPS);

    // "Returns zero unchanged rather than dividing by it" — so no NaN comes out.
    c64 zero = nya_complex_normalize(nya_complex_f64(0.0, 0.0));
    nya_assert(nya_complex_real(zero) == 0.0);
    nya_assert(nya_complex_imag(zero) == 0.0);
    nya_assert(nya_complex_is_finite(zero) == true);

    // Already unit stays unit.
    nya_assert(nya_complex_approx_equals(nya_complex_normalize(nya_complex_unit(0.9)), nya_complex_unit(0.9), EPS));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: lerp is the straight line between the two
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: lerp\n");
  {
    c64 a = nya_complex_f64(0.0, 0.0);
    c64 b = nya_complex_f64(4.0, 8.0);

    nya_assert(nya_complex_approx_equals(nya_complex_lerp(a, b, 0.0), a, EPS));
    nya_assert(nya_complex_approx_equals(nya_complex_lerp(a, b, 1.0), b, EPS));
    nya_assert(nya_complex_approx_equals(nya_complex_lerp(a, b, 0.5), nya_complex_f64(2.0, 4.0), EPS));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: slerp moves along the arc and takes the short way round
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: slerp\n");
  {
    c64 a = nya_complex_unit(0.0);
    c64 b = nya_complex_unit(M_PI_2);

    nya_assert(nya_complex_approx_equals(nya_complex_slerp(a, b, 0.0), a, EPS));
    nya_assert(nya_complex_approx_equals(nya_complex_slerp(a, b, 1.0), b, EPS));

    // Unlike lerp, the magnitude is held along the way rather than cutting the chord.
    for (u32 i = 0; i <= 10; i++) {
      f64 t = (f64)i / 10.0;
      nya_assert(fabs(cabs(nya_complex_slerp(a, b, t)) - 1.0) < EPS);
    }

    // Halfway is halfway in angle.
    nya_assert(fabs(carg(nya_complex_slerp(a, b, 0.5)) - M_PI_4) < EPS);

    // Shortest way round: from just under a half turn to just over it should cross π, not sweep
    // all the way back through zero.
    c64 near_pi  = nya_complex_unit(M_PI - 0.1);
    c64 past_pi  = nya_complex_unit(-M_PI + 0.1);   // the same as π + 0.1, wrapped
    c64 midpoint = nya_complex_slerp(near_pi, past_pi, 0.5);

    // The short arc between them passes through ±π, so the midpoint is near the negative real axis.
    nya_assert(nya_complex_real(midpoint) < -0.9);
    nya_assert(fabs(nya_complex_imag(midpoint)) < 0.2);

    // Magnitudes interpolate too when the endpoints differ in length.
    c64 small = nya_complex_from_polar(1.0, 0.0);
    c64 large = nya_complex_from_polar(3.0, 0.0);
    nya_assert(fabs(cabs(nya_complex_slerp(small, large, 0.5)) - 2.0) < EPS);

    // "Falls back to a plain lerp when either endpoint is zero and has no angle."
    c64 zero = nya_complex_f64(0.0, 0.0);
    c64 half = nya_complex_slerp(zero, large, 0.5);
    nya_assert(nya_complex_is_finite(half) == true);
    nya_assert(nya_complex_approx_equals(half, nya_complex_lerp(zero, large, 0.5), EPS));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: approx_equals compares by distance
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: approx_equals\n");
  {
    c64 a = nya_complex_f64(1.0, 2.0);
    nya_assert(nya_complex_approx_equals(a, a, 0.0) == true);
    nya_assert(nya_complex_approx_equals(a, nya_complex_f64(1.0 + 1.0e-9, 2.0), EPS) == true);
    nya_assert(nya_complex_approx_equals(a, nya_complex_f64(1.5, 2.0), EPS) == false);

    // "Not fooled by the sign of a zero component": -0.0 and +0.0 are the same point.
    nya_assert(nya_complex_approx_equals(nya_complex_f64(0.0, 0.0), nya_complex_f64(-0.0, -0.0), 0.0) == true);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: is_finite catches NaN and infinity in either component
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: is_finite\n");
  {
    nya_assert(nya_complex_is_finite(nya_complex_f64(1.0, 2.0)) == true);
    nya_assert(nya_complex_is_finite(nya_complex_f64(0.0, 0.0)) == true);

    f64 inf = INFINITY;
    f64 nan = NAN;
    nya_assert(nya_complex_is_finite(nya_complex_f64(inf, 0.0)) == false);
    nya_assert(nya_complex_is_finite(nya_complex_f64(0.0, inf)) == false);
    nya_assert(nya_complex_is_finite(nya_complex_f64(nan, 0.0)) == false);
    nya_assert(nya_complex_is_finite(nya_complex_f64(0.0, nan)) == false);
    nya_assert(nya_complex_is_finite(nya_complex_f64(-inf, nan)) == false);
    printf("  PASSED\n");
  }

  printf("PASSED: test_math_complex\n");
  return 0;
}

/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_min
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(nya_min(3, 5) == 3);
  nya_assert(nya_min(5, 3) == 3);
  nya_assert(nya_min(5, 5) == 5);
  nya_assert(nya_min(0, 0) == 0);
  nya_assert(nya_min(-5, -3) == -5);
  nya_assert(nya_min(-5, 5) == -5);
  nya_assert(nya_min(0, -1) == -1);
  nya_assert(nya_min(0, 1) == 0);

  // Test with floating point
  nya_assert(nya_min(3.5, 5.5) == 3.5);
  nya_assert(nya_min(-1.5, 1.5) == -1.5);
  nya_assert(nya_min(0.0, 0.0) == 0.0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_max
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(nya_max(3, 5) == 5);
  nya_assert(nya_max(5, 3) == 5);
  nya_assert(nya_max(5, 5) == 5);
  nya_assert(nya_max(0, 0) == 0);
  nya_assert(nya_max(-5, -3) == -3);
  nya_assert(nya_max(-5, 5) == 5);
  nya_assert(nya_max(0, -1) == 0);
  nya_assert(nya_max(0, 1) == 1);

  // Test with floating point
  nya_assert(nya_max(3.5, 5.5) == 5.5);
  nya_assert(nya_max(-1.5, 1.5) == 1.5);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_clamp
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(nya_clamp(7, 0, 5) == 5);  // above max
  nya_assert(nya_clamp(-2, 0, 5) == 0); // below min
  nya_assert(nya_clamp(3, 0, 5) == 3);  // in range
  nya_assert(nya_clamp(0, 0, 5) == 0);  // at min
  nya_assert(nya_clamp(5, 0, 5) == 5);  // at max
  nya_assert(nya_clamp(100, 10, 20) == 20);
  nya_assert(nya_clamp(5, 10, 20) == 10);
  nya_assert(nya_clamp(15, 10, 20) == 15);

  // Negative range
  nya_assert(nya_clamp(-5, -10, -1) == -5);
  nya_assert(nya_clamp(-15, -10, -1) == -10);
  nya_assert(nya_clamp(0, -10, -1) == -1);

  // Floating point clamp
  nya_assert(nya_clamp(1.5, 0.0, 1.0) == 1.0);
  nya_assert(nya_clamp(-0.5, 0.0, 1.0) == 0.0);
  nya_assert(nya_clamp(0.5, 0.0, 1.0) == 0.5);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_lerp
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(nya_lerp(0.0, 10.0, 0.0) == 0.0);
  nya_assert(nya_lerp(0.0, 10.0, 0.5) == 5.0);
  nya_assert(nya_lerp(0.0, 10.0, 1.0) == 10.0);
  nya_assert(nya_lerp(0.0, 10.0, 0.25) == 2.5);
  nya_assert(nya_lerp(0.0, 10.0, 0.75) == 7.5);

  // Negative ranges
  nya_assert(nya_lerp(-10.0, 10.0, 0.5) == 0.0);
  nya_assert(nya_lerp(-10.0, 10.0, 0.0) == -10.0);
  nya_assert(nya_lerp(-10.0, 10.0, 1.0) == 10.0);

  // Same start and end
  nya_assert(nya_lerp(5.0, 5.0, 0.0) == 5.0);
  nya_assert(nya_lerp(5.0, 5.0, 0.5) == 5.0);
  nya_assert(nya_lerp(5.0, 5.0, 1.0) == 5.0);

  // Reversed order (b < a)
  nya_assert(nya_lerp(10.0, 0.0, 0.0) == 10.0);
  nya_assert(nya_lerp(10.0, 0.0, 0.5) == 5.0);
  nya_assert(nya_lerp(10.0, 0.0, 1.0) == 0.0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_lerp extrapolates outside [0, 1] rather than rejecting it
  //
  // This used to assert that t outside the unit range panicked. It is now documented as deliberate:
  // overshoot is exactly what a spring or a back ease is built out of, so clamping or panicking
  // would make those impossible to express.
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(nya_lerp(0.0, 10.0, -0.1) == -1.0);
  nya_assert(nya_lerp(0.0, 10.0, 1.1) == 11.0);
  nya_assert(nya_lerp(0.0, 10.0, -1.0) == -10.0);
  nya_assert(nya_lerp(0.0, 10.0, 2.0) == 20.0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: chained min/max operations
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(nya_min(nya_max(3, 1), 5) == 3);
  nya_assert(nya_max(nya_min(7, 5), 3) == 5);

  return 0;
}

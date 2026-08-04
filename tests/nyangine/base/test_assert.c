/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_assert - basic assertion passes
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(1);
  nya_assert(true);
  nya_assert(42 == 42);
  nya_assert(1, "This should pass");
  nya_assert(1, "This should pass with %d args", 1);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_assert - failing assertion panics
  // ─────────────────────────────────────────────────────────────────────────────
  nya_expect_crash(nya_assert(0));
  nya_expect_crash(nya_assert(false));
  nya_expect_crash(nya_assert(1 == 2));
  nya_expect_crash(nya_assert(0, "Fail message"));
  nya_expect_crash(nya_assert(0, "Fail with %d", 42));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_expect_crash - catches panics
  // ─────────────────────────────────────────────────────────────────────────────
  nya_expect_crash(nya_panic("Test panic"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_expect_crash - fails when no panic occurs
  // ─────────────────────────────────────────────────────────────────────────────
  // We can't test this directly without causing a test failure,
  // but we verify the mechanism works with a known panic above.

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_todo, nya_unimplemented, nya_unreachable all panic
  // ─────────────────────────────────────────────────────────────────────────────
  nya_expect_crash(nya_todo());
  nya_expect_crash(nya_unimplemented());
  nya_expect_crash(nya_unreachable());

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_unused - compiles without warning
  // ─────────────────────────────────────────────────────────────────────────────
  s32 unused_var = 42;
  nya_unused(unused_var);

  s32 a = 1, b = 2;
  nya_unused(a, b);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_assert_type_match - compiles for matching types
  // ─────────────────────────────────────────────────────────────────────────────
  u32 x = 0;
  u32 y = 0;
  nya_assert_type_match(x, y);

  s64 m = 0;
  s64 n = 0;
  nya_assert_type_match(m, n);

  return 0;
}

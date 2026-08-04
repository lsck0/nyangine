/**
 * The nya_cast_to_* range checks.
 *
 * test_types.c checks that in-range values survive a cast. This checks the part that matters: that
 * out-of-range values are caught, that the widest types are not a hole in the check, and that a
 * macro taking an expression evaluates it once.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: in-range casts, at the boundaries
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: boundaries\n");
  {
    nya_assert(nya_cast_to_u8(0) == 0);
    nya_assert(nya_cast_to_u8(U8_MAX) == U8_MAX);
    nya_assert(nya_cast_to_u16(U16_MAX) == U16_MAX);
    nya_assert(nya_cast_to_u32(U32_MAX) == U32_MAX);

    nya_assert(nya_cast_to_s8(S8_MIN) == S8_MIN);
    nya_assert(nya_cast_to_s8(S8_MAX) == S8_MAX);
    nya_assert(nya_cast_to_s32(S32_MIN) == S32_MIN);
    nya_assert(nya_cast_to_s32(S32_MAX) == S32_MAX);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: values past the top of the range are rejected
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: too large is caught\n");
  {
    s32 over_u8  = (s32)U8_MAX + 1;
    s32 over_u16 = (s32)U16_MAX + 1;
    s64 over_u32 = (s64)U32_MAX + 1;
    s32 over_s8  = (s32)S8_MAX + 1;

    nya_expect_crash((void)nya_cast_to_u8(over_u8));
    nya_expect_crash((void)nya_cast_to_u16(over_u16));
    nya_expect_crash((void)nya_cast_to_u32(over_u32));
    nya_expect_crash((void)nya_cast_to_s8(over_s8));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: negatives are rejected by the unsigned casts
  //
  // The check is `val >= MIN && val <= MAX`. When MIN and MAX are unsigned and val is signed, the
  // usual arithmetic conversions turn a negative val into a very large unsigned one before either
  // comparison happens. For the narrow types the upper bound still catches it. For u64 there is no
  // value large enough to exceed U64_MAX, so both halves are satisfied and -1 casts silently.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: negative is caught\n");
  {
    s32 negative = -1;

    nya_expect_crash((void)nya_cast_to_u8(negative));
    nya_expect_crash((void)nya_cast_to_u16(negative));
    nya_expect_crash((void)nya_cast_to_u32(negative));
    nya_expect_crash((void)nya_cast_to_u64(negative));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the argument is evaluated exactly once
  //
  // These are statement-expression macros that name `val` more than once. If it is not bound to a
  // temporary first, an argument with a side effect happens as many times as it is written, which
  // is the classic macro trap and silently corrupts a counter at the call site.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: single evaluation\n");
  {
    s32 counter = 0;
    u8  result  = nya_cast_to_u8(++counter);

    nya_assert(result == 1, "expected the cast of the first increment, got %u", (u32)result);
    nya_assert(counter == 1, "the argument was evaluated %d times, expected once", counter);
    printf("  PASSED\n");
  }

  printf("PASSED: test_types_casts\n");
  return 0;
}

/**
 * Regression test for unchecked accumulation in _nya_type_try_parse_u128 (base_types.c).
 *
 * The decimal and hex loops accumulate with no overflow check:
 *
 *     *out_value = (*out_value * 10) + digit;
 *
 * so a literal with more digits than u128 can hold wraps. Two consequences, and the second is the
 * worse one:
 *
 *   - Under FLAGS_SANITIZE the build names unsigned-integer-overflow together with
 *     -fno-sanitize-recover=all, so this aborts the process rather than returning false.
 *   - Without sanitizers it wraps silently. The range check the integer cases apply afterwards
 *     ("if (value < S64_MIN || value > S64_MAX) return false") runs on the *wrapped* value, so a
 *     wrapped result that happens to land inside the target's range is accepted as a completely
 *     different number.
 *
 * nya_type_parse is the parse primitive behind command line arguments (base_args.c:774), JSON
 * numbers (serde_json.c:499) and the .nya save format (serde_nya.c:678), so the input is attacker
 * or user supplied on all three paths.
 *
 * The contract the callers already assume is the one asserted below: an unrepresentable literal is
 * a parse failure, not a wrap.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** 40 digits. u128 tops out at 39, so this is the smallest comfortable overflow. */
#define TOO_BIG_U128 "9999999999999999999999999999999999999999"

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a literal past u128 is rejected rather than wrapped
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u128 value = 0;
    b8   ok    = nya_type_parse(NYA_TYPE_U128, (const u8*)TOO_BIG_U128, strlen(TOO_BIG_U128), &value);
    nya_assert(!ok, "a 40 digit literal does not fit u128 and must not parse");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the same through the s64 path, which range checks only after the wrap
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s64 value = 0;
    b8  ok    = nya_type_parse(NYA_TYPE_S64, (const u8*)TOO_BIG_U128, strlen(TOO_BIG_U128), &value);
    nya_assert(!ok, "a 40 digit literal does not fit s64 and must not parse, got " FMTs64, value);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a wrap that lands back inside the target's range
  // ─────────────────────────────────────────────────────────────────────────────
  //
  // This is the case the range check cannot catch. U128_MAX + 44 wraps to 43, which is a perfectly
  // ordinary s64, so the parse reports success and hands back a number the document never
  // contained. Verified against a build without sanitizers, which is what ships:
  //
  //     s64 parse("340282366920938463463374607431768211499") -> ok=1 value=43
  {
    NYA_ConstCString text = "340282366920938463463374607431768211499"; // U128_MAX + 44

    s64 value = 0;
    b8  ok    = nya_type_parse(NYA_TYPE_S64, (const u8*)text, strlen(text), &value);
    nya_assert(!ok, "an unrepresentable literal was accepted as " FMTs64, value);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the hex loop has the same hole
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u128 value = 0;
    NYA_ConstCString text = "0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"; // 33 hex digits, u128 holds 32
    b8   ok               = nya_type_parse(NYA_TYPE_U128, (const u8*)text, strlen(text), &value);
    nya_assert(!ok, "a 33 digit hex literal does not fit u128 and must not parse");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the boundary values still parse
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u64 value = 0;
    b8  ok    = nya_type_parse(NYA_TYPE_U64, (const u8*)"18446744073709551615", 20, &value);
    nya_assert(ok, "U64_MAX must still parse");
    nya_assert(value == U64_MAX);
  }

  nya_info("PASSED: nya_type_parse rejects unrepresentable literals instead of wrapping");
  return 0;
}

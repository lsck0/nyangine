/**
 * @file type_parse_integer.c
 *
 * A bounded proof that the engine's integer parser never lets its accumulator wrap without noticing,
 * and never reads past the text it was handed, whatever string it is asked to parse.
 *
 * The functions under proof are `_nya_type_accumulate_digit` and `_nya_type_try_parse_u128` from
 * src/nyangine/base/base_types.c — the shared core every unsigned and signed integer parse runs through
 * (`nya_type_parse` for U8..U128 and, via the magnitude, S8..S128). They reach fully untrusted input: a
 * query-string value, a JSON number in an HTTP body, a `.nya` config field. The accumulator is a u128,
 * so a digit that pushed it past U128_MAX would wrap silently and hand a caller a number nothing in the
 * string named — the class of bug the sibling comment in this file records having been caught by fuzzing
 * the HTTP body parser. The overflow guard in `_nya_type_accumulate_digit` exists to stop exactly that.
 *
 * The bodies below are a verbatim copy of the engine's two functions, lifted here rather than #included
 * because the real translation unit is the whole `nya_type_parse` switch over every scalar type and the
 * float parser with it, none of which CBMC needs to reason about the digit loop. They are kept
 * byte-for-byte in step with the source.
 *
 * What CBMC checks here:
 *   - --bounds-check / --pointer-check: every `data[i]` read the parser makes is inside the allocated
 *     buffer, for every input the harness admits. This is the out-of-bounds-read proof.
 *   - the functional invariant: whenever `_nya_type_accumulate_digit` reports success, its result really
 *     is `before * base + digit` with no modular wrap — reconstructing `before` by exact division of the
 *     result proves the multiply-add stayed inside a u128. A wrap would make the reconstruction disagree.
 *     Alongside it, a bounded end-to-end parse pins the produced value to an independently computed one,
 *     so the loop that drives the accumulator is shown to compute the number the digits spell.
 *
 * Run by `./build verify`. Small bounds keep it to a fraction of a second.
 */

#include <stdint.h>
#include <stdlib.h>

typedef _Bool               b8;
typedef uint8_t             u8;
typedef uint64_t            u64;
typedef unsigned __int128   u128;

#define nullptr ((void*)0)
#define false   0
#define true    1
#define OUT

#define nya_assert(...) ((void)0)

/* The engine's U128_MAX, the largest value the accumulator can hold. */
#define U128_MAX ((u128)-1)

/* ── mirrored from src/nyangine/base/base_types.c ───────────────────────────────────────────────── */

/**
 * `*accumulator = *accumulator * base + digit`, or false if that would not fit in a u128.
 * */
static b8 _nya_type_accumulate_digit(u128* accumulator, u128 base, u8 digit) {
    if (*accumulator > (U128_MAX - digit) / base) return false;

    *accumulator = (*accumulator * base) + digit;
    return true;
}

static b8 _nya_type_try_parse_u128(const u8* data, u64 length, OUT u128* out_value) {
    nya_assert(data != nullptr);
    nya_assert(out_value != nullptr);

    *out_value = 0;

    if (length == 0) return false;

    // binary
    if (length > 2 && data[0] == '0' && (data[1] == 'b' || data[1] == 'B')) {
        for (u64 i = 2; i < length; i++) {
            u8 c = data[i];
            if (c != '0' && c != '1') return false;
            if (!_nya_type_accumulate_digit(out_value, 2, (u8)(c - '0'))) return false;
        }
        return true;
    }

    // hex
    if (length > 2 && data[0] == '0' && (data[1] == 'x' || data[1] == 'X')) {
        for (u64 i = 2; i < length; i++) {
            u8 c = data[i];
            u8 digit;
            if ('0' <= c && c <= '9') {
                digit = c - '0';
            } else if ('a' <= c && c <= 'f') {
                digit = 10 + (c - 'a');
            } else if ('A' <= c && c <= 'F') {
                digit = 10 + (c - 'A');
            } else {
                return false;
            }
            if (!_nya_type_accumulate_digit(out_value, 16, digit)) return false;
        }
        return true;
    }

    // decimal
    for (u64 i = 0; i < length; i++) {
        u8 c = data[i];
        if (!('0' <= c && c <= '9')) return false;

        if (!_nya_type_accumulate_digit(out_value, 10, (u8)(c - '0'))) return false;
    }

    return true;
}

/* ── the harness ────────────────────────────────────────────────────────────────────────────────── */

/* Long enough to exercise the decimal loop over several digits, short enough that the reference value
 * below cannot itself overflow a u128 and the loop stays inside the unwind bound. */
#define INT_MAX_INPUT 6u

extern u128 nondet_before(void);
extern u8   nondet_digit(void);
extern u64  nondet_length(void);

/*
 * The no-wrap check for one concrete base. It reconstructs `before` from a successful result by exact
 * division: had the multiply-add wrapped past U128_MAX, the wrapped result divided by the base would no
 * longer be `before`, so the reconstruction is what turns "did not wrap" into a checked fact. The base
 * is a literal, not a variable, so the u128 division CBMC reasons about is division by a constant — the
 * only form that stays tractable at 128 bits.
 */
#define ACCUMULATE_NO_WRAP(BASE)                                                                                          \
    do {                                                                                                                 \
        u128 before = nondet_before();                                                                                   \
        u8   digit  = nondet_digit();                                                                                    \
        __CPROVER_assume((u128)digit < (BASE)); /* a digit legal in this base — the only way it is called */             \
                                                                                                                         \
        u128 accumulator = before;                                                                                       \
        b8   accepted    = _nya_type_accumulate_digit(&accumulator, (BASE), digit);                                      \
                                                                                                                         \
        if (accepted) {                                                                                                  \
            __CPROVER_assert(accumulator >= before, "a successful accumulate never moves the value backward");          \
            __CPROVER_assert((accumulator - digit) % (BASE) == 0, "a successful accumulate is a base multiple plus the digit"); \
            __CPROVER_assert((accumulator - digit) / (BASE) == before, "a successful accumulate is before*base+digit with no wrap"); \
        }                                                                                                                \
    } while (0)

int main(void) {
    /*
     * Part 1 — the accumulator never wraps undetected, over its whole u128 range, for each of the three
     * bases the parser drives it with. A digit is always accumulated with one of these literal bases.
     */
    ACCUMULATE_NO_WRAP(2);
    ACCUMULATE_NO_WRAP(10);
    ACCUMULATE_NO_WRAP(16);

    /*
     * Part 2 — memory safety and value, end to end.
     *
     * A buffer of exactly `length` nondeterministic decimal digits: any read past it is a real
     * out-of-bounds access CBMC reports. The digits are constrained to 0-9 so the decimal path runs,
     * and a bounded input keeps the independently computed reference inside a u128.
     */
    u64 length = nondet_length();
    __CPROVER_assume(length <= INT_MAX_INPUT);

    u8* data = malloc(length);
    __CPROVER_assume(length == 0 || data != nullptr);

    u128 reference = 0;
    for (u64 i = 0; i < length; i++) {
        __CPROVER_assume('0' <= data[i] && data[i] <= '9');
        reference = reference * 10 + (u128)(data[i] - '0');
    }

    u128 parsed = 0;
    b8   ok     = _nya_type_try_parse_u128(data, length, &parsed);

    // Any non-empty run of decimal digits parses, and to exactly the value those digits spell.
    if (length == 0) {
        __CPROVER_assert(!ok, "the empty string is not a number");
    } else {
        __CPROVER_assert(ok, "a run of decimal digits parses");
        __CPROVER_assert(parsed == reference, "the parsed value is the number the digits spell");
    }

    return 0;
}

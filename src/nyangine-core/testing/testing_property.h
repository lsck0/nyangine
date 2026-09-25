/**
 * @file testing_property.h
 *
 * Property testing: state the law, not the example. A law is a function that draws its own inputs and
 * returns whether the law held; the harness runs it over many generated cases and, when one fails,
 * shrinks it to the smallest input that still fails before reporting.
 *
 * Overview:
 *   nya_property_check          runs a law over N cases and shrinks the first failure
 *   nya_property_draw_*         where a law gets its inputs
 *   nya_property_note           what a failing case should print about itself
 *
 * ```c
 * static b8 base64_round_trips(NYA_Property* property) {
 *     u8  bytes[64];
 *     u32 count = (u32)nya_property_draw_below(property, sizeof(bytes) + 1);
 *     nya_property_draw_bytes(property, bytes, count);
 *
 *     NYA_String* encoded = nya_string_create(arena);
 *     nya_base64_encode(encoded, bytes, count);
 *     ...
 *     return decoded_matches_the_input;
 * }
 *
 * failures += nya_property_check("base64 round trips", 2000, seed, base64_round_trips);
 * ```
 *
 * ## Internal shrinking
 *
 * A case is a buffer of bytes, and every draw reads from it in order. Shrinking works on that buffer,
 * not on the values: cut it shorter, zero a byte, halve a byte, and replay the law. Since every draw
 * is monotone in its bytes, a smaller buffer means smaller values, shorter arrays and fewer branches
 * taken, which is exactly what a smaller counterexample is.
 *
 * That gives one shrinker for every law, including laws over types nobody wrote a shrinker for.
 *
 * Rejected: a shrinker per generator, the way the classic QuickCheck does it. It needs a shrink
 * function beside every generator, they get written for the easy types and skipped for the hard ones,
 * and a composite type shrinks only as well as its worst member.
 *
 * ## Determinism
 *
 * The bytes of case `n` come from hashing (seed, n), so a case reproduces from its seed and index
 * alone, and a reported counterexample is replayable without storing anything but the seed.
 * */
#pragma once

#ifdef NYA_TESTING

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Bytes of entropy one case may draw. A law that wants more than this is generating a structure too
 * large to shrink usefully; 1024 bytes is a few hundred draws, which covers the laws here with room.
 * */
#ifndef NYA_PROPERTY_ENTROPY_MAX
#define NYA_PROPERTY_ENTROPY_MAX 1024
#endif

/**
 * Bytes the smallest case gets.
 *
 * Case `n` is `MIN + (n % (MAX - MIN))` bytes long, so the early cases are small and the later ones
 * large: starting every case at the full buffer means the first failure found is always a large one
 * and the shrinker does all the work. Sixty-four rather than eight, because a law that draws a length
 * and then that many items exhausts eight bytes immediately and then draws nothing but zeroes, which
 * tests one shape very thoroughly and no others.
 * */
#ifndef NYA_PROPERTY_ENTROPY_MIN
#define NYA_PROPERTY_ENTROPY_MIN 64
#endif

static_assert(NYA_PROPERTY_ENTROPY_MIN < NYA_PROPERTY_ENTROPY_MAX, "a case cannot be smaller than the smallest and larger than the largest");

/**
 * Shrink attempts before the smallest found so far is reported.
 *
 * Every accepted shrink restarts the budget, so this bounds a plateau rather than the whole search. A
 * thousand replays of a law that runs in microseconds is imperceptible, and a law slow enough for it to
 * matter is one that should have been a simulation.
 * */
#ifndef NYA_PROPERTY_SHRINK_MAX
#define NYA_PROPERTY_SHRINK_MAX 1000
#endif

/** Bytes a law's note may take. */
#define NYA_PROPERTY_NOTE_MAX 256

/**
 * The siphash key cases are drawn under. Nothing up my sleeve: the ASCII of "nyangine" and "property".
 * */
#define NYA_PROPERTY_HASH_KEY_LOW  0x6E79616E67696E65ULL
#define NYA_PROPERTY_HASH_KEY_HIGH 0x70726F7065727479ULL

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Property NYA_Property;

/** A law. Draws its inputs from `property` and returns whether the law held for them. */
typedef b8 (*NYA_PropertyFn)(NYA_Property* property);

struct NYA_Property {
    NYA_ConstCString name;

    /** The bytes this case draws from, and how many of them are real. */
    u8  entropy[NYA_PROPERTY_ENTROPY_MAX];
    u32 entropy_length;

    /** How far into `entropy` the draws have got. Past the end every draw reads zero. */
    u32 cursor;

    /**
     * An arena reset before every case, so a law can allocate without leaking across a thousand of
     * them. Owned by the harness; a law must not destroy it.
     * */
    NYA_Arena* allocator;

    /** What the failing case says about itself. See nya_property_note. */
    char note[NYA_PROPERTY_NOTE_MAX];

    /** Cases that drew past the end of their entropy. Reported, since it means the law wants more. */
    u32 exhausted;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Runs `law` over `case_count` generated cases. Returns 0 when the law held for all of them and 1 when
 * it did not, having printed the shrunk counterexample.
 *
 * One rather than a count of failing cases: a law that fails does so for a reason, and the hundred
 * other cases that also fail are the same finding reported a hundred times.
 * */
NYA_API u32 nya_property_check(NYA_ConstCString name, u32 case_count, u64 seed, NYA_PropertyFn law);

/**
 * What the failing case should say about itself, printed with the counterexample. Overwrites whatever
 * a previous call set, so a law states its note right before it returns false.
 * */
NYA_API void nya_property_note(NYA_Property* property, NYA_ConstCString format, ...) __attr_fmt_printf(2, 3);

/*
 * ─────────────────────────────────────────────────────────
 * DRAWS
 * ─────────────────────────────────────────────────────────
 */

/** One byte. Zero once the entropy is used up, which is what makes a truncated buffer a smaller case. */
NYA_API u8 nya_property_draw_u8(NYA_Property* property);

/** Eight bytes, low first. */
NYA_API u64 nya_property_draw_u64(NYA_Property* property);

/**
 * A draw below `limit`, biased small: most draws read one byte, so a shorter buffer produces smaller
 * values and shrinking a length shrinks everything downstream of it.
 * */
NYA_API u64 nya_property_draw_below(NYA_Property* property, u64 limit);

/** True `percent` of the time, out of a hundred. */
NYA_API b8 nya_property_draw_bool(NYA_Property* property, u32 percent);

/** A float within [low, high], with the edges drawn more often than uniform noise would. */
NYA_API f32 nya_property_draw_f32(NYA_Property* property, f32 low, f32 high);

/**
 * A float from the whole range, infinities and NaN included. For a law that claims to hold for every
 * float rather than for the well behaved ones.
 * */
NYA_API f32 nya_property_draw_f32_any(NYA_Property* property);

/** `count` bytes. Reads zeroes past the end of the entropy. */
NYA_API void nya_property_draw_bytes(NYA_Property* property, OUT u8* out, u32 count);

/**
 * A printable string of up to `length_max` characters, allocated from the property's arena. Never
 * null, and always terminated.
 * */
NYA_API NYA_CString nya_property_draw_text(NYA_Property* property, u32 length_max) __attr_no_discard;

#endif // NYA_TESTING

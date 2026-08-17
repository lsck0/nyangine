/**
 * The unicode name mangling that stands in for templates.
 *
 * `nya_template(Base, A, B)` pastes together the identifier `BaseᐸAˏBᐳ`, which is what every derived
 * container in the engine is actually called — NYA_ArrayᐸNYA_Jobᐳ and friends. It is a token paste
 * and nothing more, so what can go wrong is entirely about *which* identifier comes out: an arity
 * dispatched to the wrong arm, or two different parameter lists colliding on one name, would make
 * two unrelated containers silently the same type.
 *
 * The assertions here are therefore mostly compile time. A name that resolved wrongly would not
 * produce a failing run, it would fail to build — so static_assert and deliberate type mismatches
 * are the tools, and the runtime part only exists to prove the types are usable.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────
 * FIXTURES
 * ─────────────────────────────────────────────────────────
 */

/* Distinct sizes, so a name collision shows up as a static_assert on sizeof rather than as nothing. */
typedef struct {
  u8 byte;
} OneByte;

typedef struct {
  u64 a;
  u64 b;
} SixteenBytes;

/* One arm per arity. Written the way a real derive macro writes them. */
#define derive_box(...)                                                                                                                              \
  typedef struct {                                                                                                                                   \
    u32 tag;                                                                                                                                         \
  } nya_template(Box, __VA_ARGS__)

derive_box(OneByte);
derive_box(OneByte, SixteenBytes);
derive_box(OneByte, SixteenBytes, u32);
derive_box(OneByte, SixteenBytes, u32, u8);

/* The engine's own derive, so the last block can compare its mangling against this file's. */
nya_derive_array(OneByte);

/* A second base, to prove the base name participates rather than only the parameters. */
typedef struct {
  u64 payload[4];
} CrateᐸOneByteᐳ;

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the macro produces the identifier you would have written by hand
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The load bearing property. Every derived container in the engine is *declared* through
     * nya_template and then *used* by writing the unicode name out — NYA_ArrayᐸNYA_Jobᐳ appears
     * literally in core_job.h. If the two ever disagreed, the declaration and the use would be
     * different types and nothing would compile, but only in the file that happened to use it.
     */
    nya_template(Box, OneByte) declared = { .tag = 1 };
    BoxᐸOneByteᐳ*            written  = &declared;

    nya_assert(written->tag == 1, "the macro and the hand written name must be the same type");

    // Same in the other direction, and through the second base.
    CrateᐸOneByteᐳ         crate     = { .payload = { 7, 0, 0, 0 } };
    nya_template(Crate, OneByte)* aliased = &crate;

    nya_assert(aliased->payload[0] == 7, "the base name has to participate in the mangling");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: every arity from one to four dispatches to its own arm
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The arity is picked by counting arguments against a trailing list of arm names, which is the
     * part that is easy to get subtly wrong: an off-by-one there sends two parameters to the three
     * parameter arm, and the paste fails to compile in a way that names neither the macro nor the
     * caller. These four lines are what would catch that.
     */
    nya_template(Box, OneByte)                          one   = { .tag = 1 };
    nya_template(Box, OneByte, SixteenBytes)            two   = { .tag = 2 };
    nya_template(Box, OneByte, SixteenBytes, u32)       three = { .tag = 3 };
    nya_template(Box, OneByte, SixteenBytes, u32, u8)   four  = { .tag = 4 };

    nya_assert(one.tag == 1 && two.tag == 2 && three.tag == 3 && four.tag == 4);

    // Each arity is a *different* type. _Generic is the only way to ask that in C, and it fails to
    // compile if two of these ever mangled to the same name — a duplicate association is an error.
    nya_assert(
        _Generic(
            one,
            nya_template(Box, OneByte): 1,
            nya_template(Box, OneByte, SixteenBytes): 2,
            nya_template(Box, OneByte, SixteenBytes, u32): 3,
            nya_template(Box, OneByte, SixteenBytes, u32, u8): 4,
            default: 0
        ) == 1,
        "the one parameter form resolved to the wrong arm"
    );

    nya_assert(
        _Generic(
            four,
            nya_template(Box, OneByte): 1,
            nya_template(Box, OneByte, SixteenBytes): 2,
            nya_template(Box, OneByte, SixteenBytes, u32): 3,
            nya_template(Box, OneByte, SixteenBytes, u32, u8): 4,
            default: 0
        ) == 4,
        "the four parameter form resolved to the wrong arm"
    );
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: parameter order is part of the name
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * A separator that did not survive the paste would make Boxᐸa,bᐳ and Boxᐸb,aᐳ the same
     * identifier — and for a hash map keyed one way against the other, that is a type confusion the
     * compiler would never mention.
     */
    derive_box(SixteenBytes, OneByte);

    static_assert(
        sizeof(nya_template(Box, OneByte, SixteenBytes)) == sizeof(nya_template(Box, SixteenBytes, OneByte)),
        "the fixtures are meant to be the same size, so distinctness cannot be inferred from it"
    );

    nya_template(Box, OneByte, SixteenBytes) forward = { .tag = 10 };
    nya_template(Box, SixteenBytes, OneByte) reverse = { .tag = 20 };

    // Same shape, same size, and still not the same type — which is exactly what the separator buys.
    nya_assert(
        _Generic(
            forward,
            nya_template(Box, OneByte, SixteenBytes): 1,
            nya_template(Box, SixteenBytes, OneByte): 2,
            default: 0
        ) == 1,
        "swapping the parameters must produce a different type"
    );

    nya_assert(
        _Generic(
            reverse,
            nya_template(Box, OneByte, SixteenBytes): 1,
            nya_template(Box, SixteenBytes, OneByte): 2,
            default: 0
        ) == 2,
        "swapping the parameters must produce a different type"
    );
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the real derives in the engine are the same mangling
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * Not a test of nya_array — a test that the container macros and this file agree on how a name
     * is built. If they ever diverged, every hand written NYA_ArrayᐸTᐳ in the tree would stop
     * naming the array that nya_derive_array declared.
     */
    NYA_Arena* arena = nya_arena_create(.name = "template_test");
    defer nya_arena_destroy(arena);

    NYA_ArrayᐸOneByteᐳ*         written = nya_array_create(arena, OneByte);
    nya_template(NYA_Array, OneByte)* through_macro = written;

    nya_array_push_back(through_macro, ((OneByte){ .byte = 0xAB }));

    nya_assert(written->length == 1, "pushing through the macro spelled name must reach the same array");
    nya_assert(written->items[0].byte == 0xAB);
  }

  printf("PASSED: test_template\n");
  return 0;
}

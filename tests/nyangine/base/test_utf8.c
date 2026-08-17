/**
 * UTF-8 decoding, which is what stands between the text renderer and every language but English.
 *
 * The decoder is tested rather than the atlas because the atlas needs a GPU and this does not. What
 * it has to get right is not the happy path — it is the malformed input, because a decoder that
 * consumes zero bytes on a bad sequence spins forever, and one that consumes the length a truncated
 * lead byte *claimed* reads off the end of the buffer.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Decodes a whole string, so a test can state what it should come out as. */
static u32 decode_all(NYA_ConstCString text, u32* out, u32 capacity) {
  u32 count = 0;

  for (NYA_ConstCString cursor = text; *cursor != '\0' && count < capacity;) {
    u32 codepoint = 0;
    cursor       += nya_utf8_next(cursor, &codepoint);
    out[count++]  = codepoint;
  }

  return count;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  u32 out[32];

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the four lengths
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u32 count = decode_all("aé€𝄞", out, nya_carray_length(out));

    // One string, four characters, ten bytes — which is the whole reason a byte-indexed renderer
    // could not draw it: it saw ten things and drew nine gaps.
    nya_assert(count == 4, "four codepoints, got " FMTu32, count);
    nya_assert(out[0] == 0x61, "ASCII 'a'");
    nya_assert(out[1] == 0xE9, "two bytes: e acute");
    nya_assert(out[2] == 0x20AC, "three bytes: euro sign");
    nya_assert(out[3] == 0x1D11E, "four bytes: a G clef, past the basic plane");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: malformed input makes progress and stays in bounds
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // A lead byte claiming three bytes with nothing after it. Consuming the three it claimed would
    // read past the terminator; consuming zero would spin forever. One byte is the only safe answer.
    u32 codepoint = 0;
    u32 length    = nya_utf8_next("\xE2", &codepoint);

    nya_assert(length == 1, "a truncated sequence consumes exactly one byte, got " FMTu32, length);
    nya_assert(codepoint == 0xFFFD, "and decodes as the replacement character");

    // A continuation byte where a lead was expected.
    length = nya_utf8_next("\x80", &codepoint);
    nya_assert(length == 1 && codepoint == 0xFFFD, "a stray continuation byte is replaced");

    // Every byte of a garbage string is consumed, so the loop terminates. That is the property that
    // actually matters: a decoder that stalls hangs the frame rather than drawing badly.
    u32 count = decode_all("\xFF\xFE\x80\xC0", out, nya_carray_length(out));
    nya_assert(count == 4, "four bad bytes are four replacements, got " FMTu32, count);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: overlong encodings and surrogates are rejected
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // C0 80 is a two byte spelling of NUL. Accepting it is the classic way a decoder becomes a
    // security problem: a filter that checked for a literal 0x00 never sees this one.
    u32 codepoint = 0;
    (void)nya_utf8_next("\xC0\x80", &codepoint);
    nya_assert(codepoint == 0xFFFD, "an overlong NUL is rejected");

    // E0 80 80 is a three byte spelling of the same.
    (void)nya_utf8_next("\xE0\x80\x80", &codepoint);
    nya_assert(codepoint == 0xFFFD, "and so is the three byte version");

    // ED A0 80 is U+D800, half of a surrogate pair. Surrogates exist only inside UTF-16 and have no
    // legal UTF-8 encoding at all.
    (void)nya_utf8_next("\xED\xA0\x80", &codepoint);
    nya_assert(codepoint == 0xFFFD, "a surrogate has no UTF-8 encoding");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: real text in several languages round trips
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The strings an i18n file actually holds. Each is checked by its codepoint count, because the
    // byte count and the character count differ for every one of them — which is exactly the gap a
    // byte-indexed renderer fell into.
    nya_assert(decode_all("Grüße", out, nya_carray_length(out)) == 5, "German");
    nya_assert(decode_all("l'été", out, nya_carray_length(out)) == 5, "French");
    nya_assert(decode_all("años", out, nya_carray_length(out)) == 4, "Spanish");
    nya_assert(decode_all("Ελλάδα", out, nya_carray_length(out)) == 6, "Greek");
    nya_assert(decode_all("Привет", out, nya_carray_length(out)) == 6, "Russian");

    // And the empty string decodes to nothing rather than to one replacement.
    nya_assert(decode_all("", out, nya_carray_length(out)) == 0, "an empty string is empty");

    printf("  PASSED\n");
  }

  printf("PASSED: test_render2d_utf8\n");
  return 0;
}

/**
 * base32: crypto_encoding.h. RFC 4648 section 10's vectors in both directions; then every spelling the
 * decoder must refuse, since a secret has exactly one; then the round trips as laws.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** Cases per law. */
#define CASES 4000

/** Fixed, so the suite is the same run every time. */
#define SEED 0x6261736533322121ULL

/** Longest input a law draws, and the text it can encode to. */
#define BYTES_MAX 96
#define TEXT_MAX  (NYA_CRYPTO_BASE32_LENGTH(BYTES_MAX) + 1)

/* HELPERS */

static void check_encode(NYA_ConstCString data, NYA_ConstCString expected) {
    char text[64] = { 0 };
    u64  length   = 0;

    nya_assert(nya_crypto_base32_encode((const u8*)data, strlen(data), text, sizeof(text), &length).ok);
    nya_assert(
        length == strlen(expected) && nya_string_equals((NYA_ConstCString)text, expected),
        "base32(\"%s\"): got %s, want %s",
        data,
        text,
        expected
    );
}

static void check_decode(NYA_ConstCString text, NYA_ConstCString expected) {
    u8  data[64] = { 0 };
    u64 size     = 0;

    NYA_Error decoded = nya_crypto_base32_decode(text, strlen(text), data, sizeof(data), &size);
    nya_assert(decoded.ok, "'%s' was refused: %s", text, (const char*)decoded.message);
    nya_assert(size == strlen(expected) && nya_memcmp(data, expected, size) == 0, "'%s' decoded to the wrong bytes", text);
}

static void check_refused(NYA_ConstCString text, NYA_ErrorKind kind) {
    u8  data[64];
    u64 size = 99;
    nya_memset(data, 0xA5, sizeof(data));

    NYA_Error decoded = nya_crypto_base32_decode(text, strlen(text), data, sizeof(data), &size);
    nya_assert(decoded.kind == kind, "'%s' was not refused as expected", text);
    nya_assert(size == 0, "a refusal reports a size");

    // nothing written: every byte is still the fill it started as.
    for (u32 i = 0; i < sizeof(data); i++) nya_assert(data[i] == 0xA5, "'%s' was refused after writing", text);
}

/* LAWS */

/** Any bytes encode to text that decodes to the same bytes. */
static b8 law_bytes_round_trip(NYA_Property* property) {
    u8  data[BYTES_MAX];
    u32 size = (u32)nya_property_draw_below(property, sizeof(data) + 1);
    nya_property_draw_bytes(property, data, size);

    char text[TEXT_MAX];
    u64  length = 0;
    if (!nya_crypto_base32_encode(data, size, text, sizeof(text), &length).ok) return false;

    u8  back[BYTES_MAX];
    u64 back_size = 0;
    if (!nya_crypto_base32_decode(text, length, back, sizeof(back), &back_size).ok) return false;

    nya_property_note(property, "%u bytes, '%s'", size, text);
    return length == NYA_CRYPTO_BASE32_LENGTH(size) && back_size == size && nya_memcmp(data, back, size) == 0;
}

/**
 * Any text the decoder accepts is exactly what the encoder writes for those bytes. With the round trip
 * above, that is the one spelling per secret the decoder promises.
 * */
static b8 law_accepted_text_is_canonical(NYA_Property* property) {
    // mostly alphabet, sometimes padding, occasionally anything, so both sides of the decoder get cases.
    char text[TEXT_MAX] = { 0 };
    u32  groups         = (u32)nya_property_draw_below(property, 4);
    u32  length         = groups * NYA_CRYPTO_BASE32_GROUP_CHARACTERS;

    for (u32 i = 0; i < length; i++) {
        u32 pick = (u32)nya_property_draw_below(property, 100);
        if (pick < 85)
            text[i] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"[nya_property_draw_below(property, 32)];
        else if (pick < 97)
            text[i] = '=';
        else
            text[i] = (char)(1 + nya_property_draw_below(property, 126));
    }

    u8  data[BYTES_MAX];
    u64 size = 0;
    if (!nya_crypto_base32_decode(text, length, data, sizeof(data), &size).ok) return true;

    char again[TEXT_MAX];
    u64  again_length = 0;
    if (!nya_crypto_base32_encode(data, size, again, sizeof(again), &again_length).ok) return false;

    nya_property_note(property, "'%.*s' decoded and re-encoded as '%s'", (int)length, text, again);
    return again_length == length && nya_memcmp(again, text, length) == 0;
}

/* TESTS */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    u32 failures = 0;

    // RFC 4648 section 10.
    NYA_ConstCString vectors[][2] = {
        { "",       ""                 },
        { "f",      "MY======"         },
        { "fo",     "MZXQ===="         },
        { "foo",    "MZXW6==="         },
        { "foob",   "MZXW6YQ="         },
        { "fooba",  "MZXW6YTB"         },
        { "foobar", "MZXW6YTBOI======" },
    };

    printf("TEST: the vectors RFC 4648 section 10 prints, both ways\n");
    {
        for (u32 i = 0; i < nya_carray_length(vectors); i++) {
            check_encode(vectors[i][0], vectors[i][1]);
            check_decode(vectors[i][1], vectors[i][0]);
        }

        printf("  all %zu, every padding length included\n", nya_carray_length(vectors));
    }

    printf("TEST: every other spelling is refused, and nothing is written\n");
    {
        check_refused("my======", NYA_ERROR_PARSE);         // lower case
        check_refused("MY=====", NYA_ERROR_PARSE);          // a padding short
        check_refused("MY", NYA_ERROR_PARSE);               // no padding
        check_refused("MY=======", NYA_ERROR_PARSE);        // a padding too many
        check_refused("M=======", NYA_ERROR_PARSE);         // one character is no whole byte
        check_refused("MZX=====", NYA_ERROR_PARSE);         // nor are three
        check_refused("MZXW6Y==", NYA_ERROR_PARSE);         // nor six
        check_refused("========", NYA_ERROR_PARSE);         // padding alone
        check_refused("MY======MZXW6YTB", NYA_ERROR_PARSE); // padding in the middle
        check_refused("MZXW=6YQ", NYA_ERROR_PARSE);         // data after padding
        check_refused("MZXW6YT1", NYA_ERROR_PARSE);         // 0, 1, 8 and 9 are not in the alphabet
        check_refused("MZXW6YT8", NYA_ERROR_PARSE);
        check_refused("MZXW 6YT", NYA_ERROR_PARSE); // whitespace
        check_refused("MZ======", NYA_ERROR_PARSE); // spare bits set: 'f' is only ever MY
        check_refused("MZXR====", NYA_ERROR_PARSE); // spare bits set: 'fo' is only ever MZXQ
        check_refused("MZXW6YR=", NYA_ERROR_PARSE); // spare bits set: 'foob' is only ever MZXW6YQ

        // a NUL inside the length is a character like any other, and not one of the alphabet.
        u8  data[8];
        u64 size = 0;
        nya_assert(nya_crypto_base32_decode("MZXW\0YTB", 8, data, sizeof(data), &size).kind == NYA_ERROR_PARSE);

        // a buffer one byte short is refused, not overrun.
        nya_assert(nya_crypto_base32_decode("MZXW6YTB", 8, data, 4, &size).kind == NYA_ERROR_OUT_OF_MEMORY);

        char text[8];
        u64  length = 0;
        nya_assert(
            nya_crypto_base32_encode((const u8*)"fooba", 5, text, sizeof(text), &length).kind == NYA_ERROR_OUT_OF_MEMORY,
            "the terminator needs a byte"
        );
        nya_assert(length == 0 && text[0] == '\0');

        printf("  case, padding, alphabet, whitespace, spare bits, NUL and short buffers\n");
    }

    printf("TEST: a TOTP secret, the size RFC 6238 asks for\n");
    {
        u8 secret[20];
        nya_assert(nya_os_random_bytes(secret, sizeof(secret)));

        char text[NYA_CRYPTO_BASE32_LENGTH(20) + 1] = { 0 };
        u64  length                                 = 0;
        nya_assert(nya_crypto_base32_encode(secret, sizeof(secret), text, sizeof(text), &length).ok);

        // 160 bits is exactly 32 characters, so an authenticator app's secret carries no padding at all.
        nya_assert(length == 32 && strchr(text, '=') == nullptr);

        printf("  160 bits are 32 characters with no padding\n");
    }

    printf("TEST: the laws of base32\n");
    {
        failures += nya_property_check("bytes round trip through base32", CASES, SEED, law_bytes_round_trip);
        failures += nya_property_check("accepted text is the encoder's own", CASES, SEED, law_accepted_text_is_canonical);

        printf("  both laws held over %d cases\n", CASES);
    }

    printf("%s: test_encoding (%u failures)\n", failures == 0 ? "PASSED" : "FAILED", failures);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

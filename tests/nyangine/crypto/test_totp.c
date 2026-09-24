/**
 * RFC 6238 and RFC 4226, against the codes the standards print.
 *
 * Every case here is a published vector. RFC 4226 appendix D is the truncation itself: ten counters
 * under one key, with the six digit answers written out, which is the only part of HOTP anybody gets
 * wrong. RFC 6238 appendix B is the time half: six instants, including one past 2^31 seconds, each
 * with the step it falls in and the code for it.
 *
 * Appendix B prints eight digits where this engine computes six. Those are the same number: RFC 4226
 * section 5.3 takes the truncated value modulo 10^Digit, so the six digit code is the eight digit one
 * modulo a million, which is its last six characters. The expectations below are written as the RFC
 * prints them and cut at the end, so a reader can check them against the table without arithmetic.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** Cases per law. Each is one HMAC over eight bytes, so this costs nothing even sanitized. */
#define CASES 2000

/** Fixed, so the suite is the same run every time. */
#define SEED 0x746F74705F766563ULL

/** RFC 4226 appendix D and RFC 6238 appendix B share this seed: ASCII "12345678901234567890". */
#define VECTOR_SECRET "12345678901234567890"

/* HELPERS */

/** One counter under the vector key, against the answer the RFC prints for it. */
static void check_code(NYA_ConstCString what, u64 counter, NYA_ConstCString printed) {
    char code[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
    nya_crypto_totp_code((const u8*)VECTOR_SECRET, strlen(VECTOR_SECRET), counter, code);

    // the last six characters of what the RFC printed, which is that value modulo a million.
    NYA_ConstCString expected = printed + strlen(printed) - NYA_CRYPTO_TOTP_DIGITS;

    nya_assert(strcmp(code, expected) == 0, "%s: counter " FMTu64 " gave %s, want %s", what, counter, code, expected);
}

/** One instant from appendix B: the step it falls in, and then the code for that step. */
static void check_time(u64 unix_s, u64 expected_counter, NYA_ConstCString printed) {
    u64 counter = nya_crypto_totp_counter(unix_s);
    nya_assert(counter == expected_counter, "unix " FMTu64 " is step " FMTu64 ", want " FMTu64, unix_s, counter, expected_counter);

    check_code("rfc 6238 appendix b", counter, printed);
}

/* LAWS */

/** A code is always exactly six decimal digits, leading zeros kept rather than trimmed. */
static b8 law_code_is_six_digits(NYA_Property* property) {
    NYA_CryptoTotpSecret secret = { 0 };
    nya_property_draw_bytes(property, secret.bytes, sizeof(secret.bytes));

    u64 counter = nya_property_draw_below(property, U32_MAX);

    char code[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
    nya_crypto_totp_code(secret.bytes, sizeof(secret.bytes), counter, code);

    nya_property_note(property, "step " FMTu64 " gave %s", counter, code);

    if (strlen(code) != NYA_CRYPTO_TOTP_DIGITS) return false;

    for (u32 i = 0; i < NYA_CRYPTO_TOTP_DIGITS; i++) {
        if (code[i] < '0' || code[i] > '9') return false;
    }

    return true;
}

/** Two adjacent steps give different codes, which is what makes a step worth having. */
static b8 law_steps_differ(NYA_Property* property) {
    NYA_CryptoTotpSecret secret = { 0 };
    nya_property_draw_bytes(property, secret.bytes, sizeof(secret.bytes));

    u64 counter = nya_property_draw_below(property, U32_MAX);

    char here[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
    char next[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
    nya_crypto_totp_code(secret.bytes, sizeof(secret.bytes), counter, here);
    nya_crypto_totp_code(secret.bytes, sizeof(secret.bytes), counter + 1, next);

    nya_property_note(property, "step " FMTu64 " gave %s and the next %s", counter, here, next);

    return !nya_crypto_totp_code_equals(here, next);
}

/* TESTS */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    u32 failures = 0;

    printf("TEST: the HOTP codes RFC 4226 appendix D prints\n");
    {
        // the whole table, so every truncation offset the last nibble can name is exercised.
        check_code("rfc 4226 appendix d", 0, "755224");
        check_code("rfc 4226 appendix d", 1, "287082");
        check_code("rfc 4226 appendix d", 2, "359152");
        check_code("rfc 4226 appendix d", 3, "969429");
        check_code("rfc 4226 appendix d", 4, "338314");
        check_code("rfc 4226 appendix d", 5, "254676");
        check_code("rfc 4226 appendix d", 6, "287922");
        check_code("rfc 4226 appendix d", 7, "162583");
        check_code("rfc 4226 appendix d", 8, "399871");
        check_code("rfc 4226 appendix d", 9, "520489");

        printf("  all ten published codes matched\n");
    }

    printf("TEST: the TOTP codes RFC 6238 appendix B prints\n");
    {
        // the SHA-1 rows; the SHA-256 and SHA-512 ones use their own longer seeds and other algorithms.
        check_time(59, 0x0000000000000001ULL, "94287082");
        check_time(1111111109, 0x00000000023523ECULL, "07081804");

        // one second later and one step later, which is the boundary a wrong division lands on.
        check_time(1111111111, 0x00000000023523EDULL, "14050471");
        check_time(1234567890, 0x000000000273EF07ULL, "89005924");
        check_time(2000000000, 0x0000000003F940AAULL, "69279037");

        // past 2^31 seconds: the row that catches a time or a counter held in 32 bits.
        check_time(20000000000ULL, 0x0000000027BC86AAULL, "65353130");

        printf("  all six published instants matched, step and code\n");
    }

    printf("TEST: the step a time falls in\n");
    {
        nya_assert(nya_crypto_totp_counter(0) == 0, "the epoch is step zero");
        nya_assert(nya_crypto_totp_counter(NYA_CRYPTO_TOTP_STEP_S - 1) == 0, "the last second of step zero is still step zero");
        nya_assert(nya_crypto_totp_counter(NYA_CRYPTO_TOTP_STEP_S) == 1, "the step boundary is the next step");

        printf("  the boundary falls where RFC 6238 section 4.2 puts it\n");
    }

    printf("TEST: comparing codes\n");
    {
        char code[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
        nya_crypto_totp_code((const u8*)VECTOR_SECRET, strlen(VECTOR_SECRET), 1, code);

        char same[NYA_CRYPTO_TOTP_CODE_BYTES] = "287082";
        nya_assert(nya_crypto_totp_code_equals(code, same), "the same code compares equal");

        // one digit out at each end, since a comparison that stopped early would miss one of them.
        char first[NYA_CRYPTO_TOTP_CODE_BYTES] = "387082";
        char last[NYA_CRYPTO_TOTP_CODE_BYTES]  = "287083";
        nya_assert(!nya_crypto_totp_code_equals(code, first), "a different first digit is not equal");
        nya_assert(!nya_crypto_totp_code_equals(code, last), "a different last digit is not equal");

        // a short submission zero filled into a buffer of this size is not a prefix match.
        char shorter[NYA_CRYPTO_TOTP_CODE_BYTES] = "28708";
        nya_assert(!nya_crypto_totp_code_equals(code, shorter), "a five digit submission is not equal");

        printf("  equal, differing at either end, and short all answered correctly\n");
    }

    printf("TEST: a secret from the random source\n");
    {
        NYA_CryptoTotpSecret secret = { 0 };
        NYA_EXPECT(nya_crypto_totp_secret_create(&secret), "while creating a TOTP secret");

        NYA_CryptoTotpSecret zero = { 0 };
        nya_assert(!nya_crypto_equals(secret.bytes, zero.bytes, sizeof(zero.bytes)), "a fresh secret is not all zero");

        // two in a row differ, which is the cheap check that the source is not a constant.
        NYA_CryptoTotpSecret other = { 0 };
        NYA_EXPECT(nya_crypto_totp_secret_create(&other), "while creating a second TOTP secret");
        nya_assert(!nya_crypto_equals(secret.bytes, other.bytes, sizeof(other.bytes)), "two fresh secrets differ");

        nya_crypto_totp_secret_destroy(&secret);
        nya_assert(nya_crypto_equals(secret.bytes, zero.bytes, sizeof(zero.bytes)), "destroy wipes the secret");

        nya_crypto_totp_secret_destroy(&other);

        printf("  created, distinct, and wiped\n");
    }

    printf("TEST: the laws of a code\n");
    {
        failures += nya_property_check("a code is six digits", CASES, SEED, law_code_is_six_digits);
        failures += nya_property_check("adjacent steps differ", CASES, SEED, law_steps_differ);

        printf("  both laws held over %d cases\n", CASES);
    }

    printf("%s: test_totp (%u failures)\n", failures == 0 ? "PASSED" : "FAILED", failures);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

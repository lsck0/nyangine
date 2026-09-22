/**
 * The second factor's policy: enrolment, the skew window, the replay guard, the limit, and refusals.
 *
 * The arithmetic is proven against RFC 6238's published vectors in tests/nyangine/crypto/test_totp.c.
 * Nothing here recomputes a code by hand; every case drives the same primitive the server does and
 * checks what the policy around it decides. The refusals are cases of their own, because a second
 * factor that accepts is only half of one: what it refuses, and what its refusal says, is the rest.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** A plausible wall clock, far enough from the epoch that a step either side is an ordinary number. */
#define NOW 1700000000ULL

/** A code as it is printed, so a wrong one can be built next to a right one. */
#define CODE_CHARACTERS NYA_CRYPTO_TOTP_DIGITS

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HELPERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The code this secret's authenticator would show at `unix_s`. */
static void code_at(const NYA_CryptoTotpSecret* secret, u64 unix_s, OUT char out_code[NYA_CRYPTO_TOTP_CODE_BYTES]) {
    nya_crypto_totp_code(secret->bytes, sizeof(secret->bytes), nya_crypto_totp_counter(unix_s), out_code);
}

/**
 * A six digit code that is none of the three this secret accepts around `unix_s`.
 *
 * Searched rather than made up: a literal like "000000" is a real code for one secret in a million,
 * and a test that fails that rarely is worse than no test.
 * */
static void wrong_code_at(const NYA_CryptoTotpSecret* secret, u64 unix_s, OUT char out_code[NYA_CRYPTO_TOTP_CODE_BYTES]) {
    char before[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
    char here[NYA_CRYPTO_TOTP_CODE_BYTES]   = { 0 };
    char after[NYA_CRYPTO_TOTP_CODE_BYTES]  = { 0 };

    code_at(secret, unix_s - NYA_CRYPTO_TOTP_STEP_S, before);
    code_at(secret, unix_s, here);
    code_at(secret, unix_s + NYA_CRYPTO_TOTP_STEP_S, after);

    for (u32 candidate = 0; candidate < 10; candidate++) {
        for (u32 i = 0; i < CODE_CHARACTERS; i++) out_code[i] = (char)('0' + candidate);
        out_code[CODE_CHARACTERS] = '\0';

        if (strcmp(out_code, before) != 0 && strcmp(out_code, here) != 0 && strcmp(out_code, after) != 0) return;
    }

    nya_assert(false, "ten repdigit codes and all three steps matched one of them, which cannot happen");
}

/** Spends `count` wrong codes, asserting each one is refused rather than limited. */
static void spend_wrong(NYA_HttpTotpGuard* guard, const NYA_CryptoTotpSecret* secret, u32 count, u64 now_s) {
    char wrong[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
    wrong_code_at(secret, now_s, wrong);

    for (u32 i = 0; i < count; i++) {
        NYA_HttpTotpVerdict verdict = nya_http_totp_verify(guard, secret, wrong, now_s);
        nya_assert(verdict == NYA_HTTP_TOTP_REFUSED, "attempt %u of %u was verdict %d, want a refusal", i + 1, count, (int)verdict);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TESTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_HttpTotpEnrolment enrolment = { 0 };
    NYA_EXPECT(nya_http_totp_enrol_create("nyangine", "luca@example.test", &enrolment), "while enrolling a second factor");

    printf("TEST: what an enrolment hands out\n");
    {
        // the base32 an app takes typed in is the same twenty bytes the server keeps.
        u8  decoded[NYA_CRYPTO_TOTP_SECRET_BYTES] = { 0 };
        u64 decoded_size                          = 0;
        NYA_EXPECT(
            nya_crypto_base32_decode(enrolment.secret_base32, strlen(enrolment.secret_base32), decoded, sizeof(decoded), &decoded_size),
            "while decoding the printed secret"
        );

        nya_assert(decoded_size == sizeof(enrolment.secret.bytes), "the printed secret is " FMTu64 " bytes", decoded_size);
        nya_assert(nya_crypto_equals(decoded, enrolment.secret.bytes, sizeof(decoded)), "the printed secret is the stored one");

        // a twenty byte secret is four whole base32 groups, so an authenticator never sees padding.
        nya_assert(strchr(enrolment.secret_base32, '=') == nullptr, "the printed secret carries no padding");

        /*
         * The URI an app scans. The account is percent encoded because an address has an '@' in it,
         * and the issuer appears twice: once in the label for an old app and once as the parameter a
         * current one reads.
         */
        char expected[NYA_HTTP_TOTP_URI_BYTES] = { 0 };
        (void)snprintf(
            expected,
            sizeof(expected),
            "otpauth://totp/nyangine:luca%%40example.test?secret=%s&issuer=nyangine&algorithm=SHA1&digits=6&period=30",
            enrolment.secret_base32
        );

        nya_assert(strcmp(enrolment.uri, expected) == 0, "the URI is\n  %s\nwant\n  %s", enrolment.uri, expected);

        printf("  the secret round trips and the URI names every parameter\n");
    }

    printf("TEST: the recovery codes\n");
    {
        NYA_HttpTotpRecoveryHash zero = { 0 };

        for (u32 i = 0; i < NYA_HTTP_TOTP_RECOVERY_CODES; i++) {
            nya_assert(strlen(enrolment.recovery[i]) == NYA_HTTP_TOTP_RECOVERY_TEXT_BYTES - 1, "recovery code %u is the printed length", i);
            nya_assert(enrolment.recovery[i][NYA_HTTP_TOTP_RECOVERY_GROUP] == '-', "recovery code %u is grouped for reading", i);
            nya_assert(!nya_crypto_equals(enrolment.recovery_hash[i].bytes, zero.bytes, sizeof(zero.bytes)), "recovery code %u is hashed", i);

            // distinct, since ten codes drawn from one source that repeats are one code.
            for (u32 j = 0; j < i; j++) {
                nya_assert(strcmp(enrolment.recovery[i], enrolment.recovery[j]) != 0, "recovery codes %u and %u differ", i, j);
            }
        }

        printf("  ten distinct codes, grouped, and stored only as hashes\n");
    }

    printf("TEST: enrolment refuses a label it cannot write\n");
    {
        NYA_HttpTotpEnrolment refused = { 0 };
        NYA_CryptoTotpSecret  zero    = { 0 };

        char long_name[NYA_HTTP_TOTP_ACCOUNT_MAX + 8] = { 0 };
        nya_memset(long_name, 'a', sizeof(long_name) - 1);

        nya_assert(!nya_http_totp_enrol_create("", "luca", &refused).ok, "an empty issuer is refused");
        nya_assert(!nya_http_totp_enrol_create("nyangine", "", &refused).ok, "an empty account is refused");

        // a colon splits the label, so one inside a half would let an issuer be smuggled past an app.
        nya_assert(!nya_http_totp_enrol_create("nya:ngine", "luca", &refused).ok, "a colon in the issuer is refused");
        nya_assert(!nya_http_totp_enrol_create("nyangine", "lu:ca", &refused).ok, "a colon in the account is refused");
        nya_assert(!nya_http_totp_enrol_create("nyangine", "lu\nca", &refused).ok, "a control character in the account is refused");
        nya_assert(!nya_http_totp_enrol_create("nyangine", long_name, &refused).ok, "an account past its bound is refused");

        nya_assert(nya_crypto_equals(refused.secret.bytes, zero.bytes, sizeof(zero.bytes)), "a refused enrolment holds no secret");
        nya_assert(refused.uri[0] == '\0', "a refused enrolment holds no URI");

        printf("  six labels refused, and nothing left behind\n");
    }

    printf("TEST: a current code is accepted once\n");
    {
        NYA_HttpTotpGuard guard                 = { 0 };
        char              code[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
        code_at(&enrolment.secret, NOW, code);

        nya_assert(nya_http_totp_verify(&guard, &enrolment.secret, code, NOW) == NYA_HTTP_TOTP_ACCEPTED, "the current code is accepted");
        nya_assert(guard.last_counter == nya_crypto_totp_counter(NOW), "the guard remembers which step was spent");

        // the replay: the same digits, still inside the same thirty seconds.
        nya_assert(nya_http_totp_verify(&guard, &enrolment.secret, code, NOW + 1) == NYA_HTTP_TOTP_REFUSED, "the same code is refused the second time");

        // and the step before the one just used, which a window check alone would still accept.
        char previous[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
        code_at(&enrolment.secret, NOW - NYA_CRYPTO_TOTP_STEP_S, previous);
        nya_assert(nya_http_totp_verify(&guard, &enrolment.secret, previous, NOW) == NYA_HTTP_TOTP_REFUSED, "an earlier step is refused after a later one");

        printf("  accepted once, refused on replay and refused going backwards\n");
    }

    printf("TEST: the skew window is one step either side\n");
    {
        // a fresh guard per case: a guard that has accepted something is testing the replay rule instead.
        const s64 offsets[] = { -(s64)NYA_CRYPTO_TOTP_STEP_S, 0, (s64)NYA_CRYPTO_TOTP_STEP_S };

        for (u32 i = 0; i < nya_carray_length(offsets); i++) {
            NYA_HttpTotpGuard guard                          = { 0 };
            char              code[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
            code_at(&enrolment.secret, (u64)((s64)NOW + offsets[i]), code);

            nya_assert(
                nya_http_totp_verify(&guard, &enrolment.secret, code, NOW) == NYA_HTTP_TOTP_ACCEPTED,
                "a code %lld seconds out is inside the window",
                (long long)offsets[i]
            );
        }

        // two steps out is a clock nobody should be trusted with, and three live codes is enough.
        const s64 outside[] = { -2 * (s64)NYA_CRYPTO_TOTP_STEP_S, 2 * (s64)NYA_CRYPTO_TOTP_STEP_S };

        for (u32 i = 0; i < nya_carray_length(outside); i++) {
            NYA_HttpTotpGuard guard                          = { 0 };
            char              code[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
            code_at(&enrolment.secret, (u64)((s64)NOW + outside[i]), code);

            nya_assert(
                nya_http_totp_verify(&guard, &enrolment.secret, code, NOW) == NYA_HTTP_TOTP_REFUSED,
                "a code %lld seconds out is outside the window",
                (long long)outside[i]
            );
        }

        printf("  three steps accepted, the fourth and fifth refused\n");
    }

    printf("TEST: a malformed submission is the same refusal as a wrong one\n");
    {
        // none of these says what was wrong with it, which is the point: one verdict, one message.
        const NYA_ConstCString malformed[] = { "", "1", "12345", "1234567", "12a456", " 12345", "123456 ", "abcdef", "12345\n" };

        for (u32 i = 0; i < nya_carray_length(malformed); i++) {
            NYA_HttpTotpGuard guard = { 0 };

            nya_assert(
                nya_http_totp_verify(&guard, &enrolment.secret, malformed[i], NOW) == NYA_HTTP_TOTP_REFUSED,
                "'%s' is refused",
                malformed[i]
            );

            // and it cost an attempt: a submission that never reaches the comparison is still a guess.
            nya_assert(guard.attempts == 1, "'%s' spent an attempt", malformed[i]);
        }

        printf("  nine malformed submissions refused, each at the cost of an attempt\n");
    }

    printf("TEST: the limit on guessing\n");
    {
        NYA_HttpTotpGuard guard                 = { 0 };
        char              code[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
        code_at(&enrolment.secret, NOW, code);

        spend_wrong(&guard, &enrolment.secret, NYA_HTTP_TOTP_ATTEMPTS_MAX, NOW);

        // the right code, and it is still not looked at: the limit comes before the comparison.
        nya_assert(nya_http_totp_verify(&guard, &enrolment.secret, code, NOW) == NYA_HTTP_TOTP_RATE_LIMITED, "a sixth attempt is limited");
        nya_assert(guard.last_counter == 0, "a limited attempt does not spend a step");

        // still limited on the last second of the window, and free again once it has passed.
        u64 late = NOW + NYA_HTTP_TOTP_ATTEMPT_WINDOW_S - 1;
        nya_assert(nya_http_totp_verify(&guard, &enrolment.secret, code, late) == NYA_HTTP_TOTP_RATE_LIMITED, "the window has not elapsed yet");

        u64  over               = NOW + NYA_HTTP_TOTP_ATTEMPT_WINDOW_S;
        char later[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
        code_at(&enrolment.secret, over, later);

        nya_assert(nya_http_totp_verify(&guard, &enrolment.secret, later, over) == NYA_HTTP_TOTP_ACCEPTED, "the window elapsed and the code is read again");

        printf("  five tries a minute, and the sixth is not even compared\n");
    }

    printf("TEST: answering clears the limit\n");
    {
        NYA_HttpTotpGuard guard                 = { 0 };
        char              code[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
        code_at(&enrolment.secret, NOW, code);

        spend_wrong(&guard, &enrolment.secret, NYA_HTTP_TOTP_ATTEMPTS_MAX - 1, NOW);
        nya_assert(nya_http_totp_verify(&guard, &enrolment.secret, code, NOW) == NYA_HTTP_TOTP_ACCEPTED, "the last attempt in the window still works");
        nya_assert(guard.attempts == 0, "whoever just answered is not who the limit is for");

        printf("  a success resets the window it was spent in\n");
    }

    printf("TEST: a recovery code is spent when it is used\n");
    {
        NYA_HttpTotpGuard        guard                                     = { 0 };
        NYA_HttpTotpRecoveryHash hashes[NYA_HTTP_TOTP_RECOVERY_CODES]      = { 0 };
        char                     typed[NYA_HTTP_TOTP_RECOVERY_TEXT_BYTES]  = { 0 };

        for (u32 i = 0; i < NYA_HTTP_TOTP_RECOVERY_CODES; i++) hashes[i] = enrolment.recovery_hash[i];

        nya_assert(
            nya_http_totp_recovery_redeem(&guard, hashes, nya_carray_length(hashes), enrolment.recovery[0], NOW) == NYA_HTTP_TOTP_ACCEPTED,
            "a code as it was printed is accepted"
        );

        NYA_HttpTotpRecoveryHash zero = { 0 };
        nya_assert(nya_crypto_equals(hashes[0].bytes, zero.bytes, sizeof(zero.bytes)), "the code used is gone from the store");

        nya_assert(
            nya_http_totp_recovery_redeem(&guard, hashes, nya_carray_length(hashes), enrolment.recovery[0], NOW) == NYA_HTTP_TOTP_REFUSED,
            "the same code is refused the second time"
        );

        // typed off paper: lower case and no dash, which is how it comes back in practice.
        u32 at = 0;
        for (u32 i = 0; enrolment.recovery[1][i] != '\0'; i++) {
            char character = enrolment.recovery[1][i];
            if (character == '-') continue;

            typed[at] = (character >= 'A' && character <= 'Z') ? (char)(character + ('a' - 'A')) : character;
            at++;
        }

        nya_assert(
            nya_http_totp_recovery_redeem(&guard, hashes, nya_carray_length(hashes), typed, NOW) == NYA_HTTP_TOTP_ACCEPTED,
            "a code typed in lower case without its dash is accepted"
        );

        nya_assert(
            nya_http_totp_recovery_redeem(&guard, hashes, nya_carray_length(hashes), "TOOSHORT", NOW) == NYA_HTTP_TOTP_REFUSED,
            "a code of the wrong length is refused"
        );

        // the rest of this window spent on codes that were never issued, from a clean window.
        guard.attempts = 0;
        for (u32 i = 0; i < NYA_HTTP_TOTP_ATTEMPTS_MAX; i++) {
            nya_assert(
                nya_http_totp_recovery_redeem(&guard, hashes, nya_carray_length(hashes), "AAAAAAAA-AAAAAAAA", NOW) == NYA_HTTP_TOTP_REFUSED,
                "guess %u at a code that was never issued is refused",
                i + 1
            );
        }

        // a real code, unspent, and it is not read: recovery is guessed against the same limit.
        nya_assert(
            nya_http_totp_recovery_redeem(&guard, hashes, nya_carray_length(hashes), enrolment.recovery[2], NOW) == NYA_HTTP_TOTP_RATE_LIMITED,
            "recovery codes are limited like codes"
        );

        printf("  redeemed once, forgiving about typing, and limited like a code\n");
    }

    nya_http_totp_enrol_destroy(&enrolment);

    NYA_CryptoTotpSecret zero = { 0 };
    nya_assert(nya_crypto_equals(enrolment.secret.bytes, zero.bytes, sizeof(zero.bytes)), "destroying an enrolment wipes its secret");
    nya_assert(enrolment.uri[0] == '\0', "destroying an enrolment wipes its URI");
    nya_assert(enrolment.recovery[0][0] == '\0', "destroying an enrolment wipes its recovery codes");

    printf("PASSED: test_http_totp (0 failures)\n");

    return EXIT_SUCCESS;
}

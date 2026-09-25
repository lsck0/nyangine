/**
 * crypto_secret.h: the constant time comparison, the wipe, and a key's life from the random source to
 * zero. Timing cannot be asserted in a unit test, so what is checked is the answer at every position a
 * short circuit would stop at.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("TEST: a comparison that does not say where it stopped\n");
    {
        u8 left[64];
        u8 right[64];
        for (u32 i = 0; i < sizeof(left); i++) left[i] = right[i] = (u8)(i * 7);

        nya_assert(nya_crypto_equals(left, right, sizeof(left)));

        // one differing bit at every position, first to last, answers false, which is all a caller may learn.
        for (u32 i = 0; i < sizeof(right); i++) {
            for (u32 bit = 0; bit < 8; bit++) {
                right[i] ^= (u8)(1U << bit);
                nya_assert(!nya_crypto_equals(left, right, sizeof(left)), "a difference at byte %u bit %u was missed", i, bit);
                right[i] ^= (u8)(1U << bit);
            }
        }

        // a difference past the compared length is not a difference, and nothing to compare is equal.
        right[sizeof(right) - 1] ^= 0xFFU;
        nya_assert(nya_crypto_equals(left, right, sizeof(left) - 1));
        nya_assert(nya_crypto_equals(left, right, 0));
        nya_assert(nya_crypto_equals(nullptr, nullptr, 0));

        printf("  every single bit difference in 64 bytes, the length bound, and empty\n");
    }

    printf("TEST: a wipe zeroes exactly what it is given\n");
    {
        u8 buffer[48];
        nya_memset(buffer, 0xC3, sizeof(buffer));

        nya_crypto_wipe(buffer + 8, 32);

        for (u32 i = 0; i < sizeof(buffer); i++) {
            b8 inside = i >= 8 && i < 40;
            nya_assert(buffer[i] == (inside ? 0x00 : 0xC3), "byte %u after a wipe of 8..40", i);
        }

        // nothing to wipe is a no-op, pointer or not.
        nya_crypto_wipe(nullptr, 0);

        printf("  the range and nothing either side of it\n");
    }

    printf("TEST: a key from the random source, and its end\n");
    {
        NYA_CryptoKey32 first  = { 0 };
        NYA_CryptoKey32 second = { 0 };
        nya_assert(nya_crypto_key_create(&first).ok);
        nya_assert(nya_crypto_key_create(&second).ok);

        // 256 bits drawn twice agree with probability 2^-256, and an all zero draw with the same.
        nya_assert(!nya_is_zeroed(first) && !nya_is_zeroed(second));
        nya_assert(!nya_crypto_equals(first.bytes, second.bytes, sizeof(first.bytes)), "two fresh keys were equal");

        nya_crypto_key_destroy(&first);
        nya_assert(nya_is_zeroed(first), "a destroyed key is wiped");
        nya_assert(!nya_is_zeroed(second), "destroying one key touched another");

        // destroying twice, and destroying nothing, are both no-ops.
        nya_crypto_key_destroy(&first);
        nya_crypto_key_destroy(nullptr);
        nya_crypto_key_destroy(&second);

        printf("  two keys differ, a destroyed one is zero, and destroy is idempotent\n");
    }

    printf("PASSED: test_secret (0 failures)\n");

    return EXIT_SUCCESS;
}

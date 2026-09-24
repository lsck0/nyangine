/**
 * ECDSA over P-256: a real signature from a real key, the refusals, and the point checks that keep a
 * forged key off the curve.
 *
 * The key and the signature are a fixture, made once with `openssl ecparam -name prime256v1 -genkey`
 * and `openssl dgst -sha256 -sign`, with the DER signature unwrapped into the `r || s` form a JWS
 * carries. Verifying somebody else's signature is the point: a bug that made this agree with itself
 * would be invisible to a round trip.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────
 * THE FIXTURE
 * ─────────────────────────────────────────────────────────
 */

static const u8 FIXTURE_X[] = {
  0xE4, 0x38, 0x5A, 0x9C, 0x4B, 0xB9, 0x37, 0xBB, 0xF5, 0xCC, 0x7D, 0xCB,
  0xE5, 0x97, 0x86, 0x3A, 0x1B, 0xD4, 0x6D, 0x9A, 0x16, 0xF8, 0x18, 0x66,
  0xFB, 0x15, 0x5E, 0xF8, 0x4A, 0x64, 0x8D, 0x56,
};

static const u8 FIXTURE_Y[] = {
  0xF2, 0xBC, 0xFF, 0x5D, 0xC8, 0x48, 0xC0, 0xD0, 0x4A, 0xB2, 0x92, 0x49,
  0x0A, 0xFE, 0xBB, 0xE9, 0x50, 0xB5, 0x57, 0xB9, 0x0C, 0xBA, 0x96, 0xF5,
  0x9A, 0xEB, 0x12, 0x13, 0x30, 0xD9, 0x7D, 0xA5,
};

static const u8 FIXTURE_SIGNATURE[] = {
  0xA6, 0x7F, 0x94, 0xDE, 0x57, 0x9B, 0x34, 0x68, 0x59, 0x19, 0x7F, 0x5D,
  0xE8, 0x68, 0xAB, 0xBD, 0x60, 0x32, 0x45, 0x43, 0xBE, 0x0E, 0x38, 0xA0,
  0x2F, 0x9C, 0xE1, 0x24, 0xD6, 0x67, 0xF6, 0x3F, 0xF5, 0x05, 0x63, 0xD0,
  0xF4, 0x39, 0x7B, 0x38, 0x8D, 0x9B, 0xA9, 0x81, 0x3E, 0x5D, 0x70, 0x00,
  0x9F, 0x21, 0xD9, 0xD7, 0x2D, 0x08, 0x57, 0xC7, 0xA5, 0xF8, 0xFD, 0xC5,
  0x6C, 0xAF, 0x28, 0xF2,
};

static const char FIXTURE_MESSAGE[] = "the message this fixture signs";

s32 main(void) {
  u64 message_size = sizeof(FIXTURE_MESSAGE) - 1;

  // TEST: a signature openssl made verifies, and nothing else does.
  {
    NYA_CryptoEcdsaPublicKey key = { 0 };

    nya_check(nya_crypto_ecdsa_public_key_from_xy(FIXTURE_X, sizeof(FIXTURE_X), FIXTURE_Y, sizeof(FIXTURE_Y), &key).ok,
              "the key is read from its two coordinates");

    nya_check(nya_crypto_ecdsa_verify_sha256(&key, (const u8*)FIXTURE_MESSAGE, message_size, FIXTURE_SIGNATURE, sizeof(FIXTURE_SIGNATURE)),
              "the signature verifies against the message it was made over");

    nya_check(!nya_crypto_ecdsa_verify_sha256(&key, (const u8*)"the message this fixture signs.", message_size + 1, FIXTURE_SIGNATURE,
                                              sizeof(FIXTURE_SIGNATURE)),
              "and not against a message with one character more");

    for (u64 index = 0; index < sizeof(FIXTURE_SIGNATURE); index += 17) {
      u8 tampered[sizeof(FIXTURE_SIGNATURE)];
      nya_memcpy(tampered, FIXTURE_SIGNATURE, sizeof(tampered));

      tampered[index] ^= 0x01;

      nya_check(!nya_crypto_ecdsa_verify_sha256(&key, (const u8*)FIXTURE_MESSAGE, message_size, tampered, sizeof(tampered)),
                "a signature with byte " FMTu64 " flipped is refused", index);
    }

    // the JWS form and only it: a DER signature is a different encoding, and its first byte is 0x30.
    nya_check(!nya_crypto_ecdsa_verify_sha256(&key, (const u8*)FIXTURE_MESSAGE, message_size, FIXTURE_SIGNATURE, sizeof(FIXTURE_SIGNATURE) - 2),
              "a signature of the wrong length is refused");
  }

  // TEST: r and s must be in 1..n-1, which is where the cheap forgeries live.
  {
    NYA_CryptoEcdsaPublicKey key = { 0 };
    nya_check(nya_crypto_ecdsa_public_key_from_xy(FIXTURE_X, sizeof(FIXTURE_X), FIXTURE_Y, sizeof(FIXTURE_Y), &key).ok, "the key is read");

    u8 zeroed[sizeof(FIXTURE_SIGNATURE)] = { 0 };
    nya_check(!nya_crypto_ecdsa_verify_sha256(&key, (const u8*)FIXTURE_MESSAGE, message_size, zeroed, sizeof(zeroed)),
              "a signature of all zeroes is refused");

    // r = 0 with a real s, and s = 0 with a real r: both are zero in a half that may not be.
    u8 half[sizeof(FIXTURE_SIGNATURE)];
    nya_memcpy(half, FIXTURE_SIGNATURE, sizeof(half));
    nya_memset(half, 0, 32);

    nya_check(!nya_crypto_ecdsa_verify_sha256(&key, (const u8*)FIXTURE_MESSAGE, message_size, half, sizeof(half)), "r of zero is refused");

    nya_memcpy(half, FIXTURE_SIGNATURE, sizeof(half));
    nya_memset(half + 32, 0, 32);

    nya_check(!nya_crypto_ecdsa_verify_sha256(&key, (const u8*)FIXTURE_MESSAGE, message_size, half, sizeof(half)), "and so is s of zero");

    // n itself, which is congruent to zero and is the other spelling of it.
    static const u8 ORDER[32] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                  0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51 };

    nya_memcpy(half, FIXTURE_SIGNATURE, sizeof(half));
    nya_memcpy(half, ORDER, sizeof(ORDER));

    nya_check(!nya_crypto_ecdsa_verify_sha256(&key, (const u8*)FIXTURE_MESSAGE, message_size, half, sizeof(half)), "r equal to the order is refused");
  }

  // TEST: what a key may not be, which is the invalid-curve check.
  {
    NYA_CryptoEcdsaPublicKey key = { 0 };

    // one coordinate moved: a point that satisfies some other curve's equation, which is the attack
    // this refuses rather than doing arithmetic in whatever group it lands in.
    u8 off_curve[sizeof(FIXTURE_X)];
    nya_memcpy(off_curve, FIXTURE_X, sizeof(off_curve));
    off_curve[31] ^= 0x01;

    nya_check(!nya_crypto_ecdsa_public_key_from_xy(off_curve, sizeof(off_curve), FIXTURE_Y, sizeof(FIXTURE_Y), &key).ok,
              "a point that is not on P-256 is refused");

    u8 zeroes[32] = { 0 };
    nya_check(!nya_crypto_ecdsa_public_key_from_xy(zeroes, sizeof(zeroes), zeroes, sizeof(zeroes), &key).ok, "and so is the point at infinity");

    // a coordinate at or above the field prime is not a field element.
    static const u8 PRIME[32] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    nya_check(!nya_crypto_ecdsa_public_key_from_xy(PRIME, sizeof(PRIME), FIXTURE_Y, sizeof(FIXTURE_Y), &key).ok,
              "a coordinate equal to the field prime is refused");

    u8 too_long[33] = { 0 };
    nya_check(!nya_crypto_ecdsa_public_key_from_xy(too_long, sizeof(too_long), FIXTURE_Y, sizeof(FIXTURE_Y), &key).ok,
              "and one longer than the field is not a coordinate");

    // a provider that stripped a leading zero is still describing the same key.
    u8  shortened[31] = { 0 };
    u64 leading       = FIXTURE_X[0];

    if (leading == 0) {
      nya_memcpy(shortened, FIXTURE_X + 1, sizeof(shortened));

      NYA_CryptoEcdsaPublicKey padded = { 0 };
      nya_check(nya_crypto_ecdsa_public_key_from_xy(shortened, sizeof(shortened), FIXTURE_Y, sizeof(FIXTURE_Y), &padded).ok,
                "a coordinate with its leading zero stripped is the same coordinate");
    }

    // and a key nobody read verifies nothing.
    NYA_CryptoEcdsaPublicKey empty = { 0 };
    nya_check(!nya_crypto_ecdsa_verify_sha256(&empty, (const u8*)FIXTURE_MESSAGE, message_size, FIXTURE_SIGNATURE, sizeof(FIXTURE_SIGNATURE)),
              "a zeroed key verifies nothing");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}

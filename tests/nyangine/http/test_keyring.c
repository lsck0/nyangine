/**
 * The keyring: a rotation nobody notices. A token sealed before the key rolled still opens after it,
 * an expired key opens nothing, and sealing always uses the newest key.
 *
 * The rotation window is a day, which a test cannot wait out, so the dates on the keys are moved by
 * hand where a rotation or an expiry has to be provoked. Everything else runs on the real clock.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an empty ring is filled once and then left alone.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_HttpKeyring ring = { 0 };

    nya_check(nya_http_keyring_count(&ring) == 0, "a zeroed ring is empty");
    nya_check(nya_http_keyring_rotate(&ring), "the first rotate mints a key");
    nya_check(nya_http_keyring_count(&ring) == 1, "so there is one, got %u", nya_http_keyring_count(&ring));

    // called again straight away, nothing is due, so nothing changes.
    nya_check(!nya_http_keyring_rotate(&ring), "a second rotate in the same window changes nothing");
    nya_check(nya_http_keyring_count(&ring) == 1, "and still one key, got %u", nya_http_keyring_count(&ring));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a round trip through the ring, and the empty-ring error.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_HttpKeyring ring = { 0 };

    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    nya_check(!nya_http_keyring_seal(&ring, "session", (const u8*)"x", 1, 900, token, sizeof(token)).ok, "an empty ring will not seal");

    NYA_EXPECT(nya_http_keyring_rotate(&ring) ? NYA_OK : nya_error(NYA_ERROR_NOT_OK, "rotate"));

    u64 who = 4200;
    NYA_EXPECT(nya_http_keyring_seal(&ring, "session", (const u8*)&who, sizeof(who), 900, token, sizeof(token)));

    u64 back = 0;
    u64 size = 0;
    nya_check(nya_http_keyring_unseal(&ring, "session", token, strlen(token), (u8*)&back, sizeof(back), &size), "the ring opens what it sealed");
    nya_check(back == 4200 && size == sizeof(who), "to the same bytes, got %llu", (unsigned long long)back);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a token sealed before a rotation still opens after it. The whole point.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_HttpKeyring ring = { 0 };
    NYA_EXPECT(nya_http_keyring_rotate(&ring) ? NYA_OK : nya_error(NYA_ERROR_NOT_OK, "rotate"));

    // seal with the first key.
    u64  who                            = 77;
    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    NYA_EXPECT(nya_http_keyring_seal(&ring, "session", (const u8*)&who, sizeof(who), 3600, token, sizeof(token)));

    // make the current key look a day old so a rotation is due, then rotate: a new key goes in front and
    // the one that sealed the token stays as a previous key.
    ring.keys[0].created_at_s -= NYA_HTTP_KEYRING_ROTATE_S + 1;

    nya_check(nya_http_keyring_rotate(&ring), "a due key is rolled");
    nya_check(nya_http_keyring_count(&ring) == 2, "leaving two keys, got %u", nya_http_keyring_count(&ring));

    u64 back = 0;
    u64 size = 0;
    nya_check(nya_http_keyring_unseal(&ring, "session", token, strlen(token), (u8*)&back, sizeof(back), &size),
              "the token from before the rotation still opens");
    nya_check(back == 77, "as the same state, got %llu", (unsigned long long)back);

    // and a new token uses the new key, which is index zero.
    u64  after                             = 88;
    char token2[NYA_HTTP_SEAL_MAX_TOKEN]   = { 0 };
    NYA_EXPECT(nya_http_keyring_seal(&ring, "session", (const u8*)&after, sizeof(after), 3600, token2, sizeof(token2)));
    nya_check(!nya_string_equals(token, token2), "a new token is sealed with the new key");

    nya_check(nya_http_keyring_unseal(&ring, "session", token2, strlen(token2), (u8*)&back, sizeof(back), &size), "and opens");
    nya_check(back == 88, "to its own state, got %llu", (unsigned long long)back);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an expired key is dropped, and its tokens stop opening.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_HttpKeyring ring = { 0 };
    NYA_EXPECT(nya_http_keyring_rotate(&ring) ? NYA_OK : nya_error(NYA_ERROR_NOT_OK, "rotate"));

    u64  who                            = 5;
    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    NYA_EXPECT(nya_http_keyring_seal(&ring, "session", (const u8*)&who, sizeof(who), 3600, token, sizeof(token)));

    // roll a second key in so the ring is not emptied, then age the first past its whole verify life.
    ring.keys[0].created_at_s -= NYA_HTTP_KEYRING_ROTATE_S + 1;
    nya_check(nya_http_keyring_rotate(&ring), "a second key is rolled in");

    // find the key that sealed the token — it is now index one — and expire it.
    ring.keys[1].expires_at_s = nya_clock_get_timestamp_s();

    nya_check(nya_http_keyring_rotate(&ring), "and the expired one is pruned");
    nya_check(nya_http_keyring_count(&ring) == 1, "leaving only the current key, got %u", nya_http_keyring_count(&ring));

    u64 back = 0;
    u64 size = 0;
    nya_check(!nya_http_keyring_unseal(&ring, "session", token, strlen(token), (u8*)&back, sizeof(back), &size),
              "a token whose key aged off the ring no longer opens");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the ring never grows past its bound, dropping the oldest.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_HttpKeyring ring = { 0 };

    // force a rotation every time by aging the newest key before each call.
    for (u32 index = 0; index < NYA_HTTP_KEYRING_MAX_KEYS + 3; index++) {
      if (ring.key_count > 0) ring.keys[0].created_at_s -= NYA_HTTP_KEYRING_ROTATE_S + 1;
      nya_check(nya_http_keyring_rotate(&ring), "each forced rotation mints a key, round %u", index);
    }

    nya_check(nya_http_keyring_count(&ring) == NYA_HTTP_KEYRING_MAX_KEYS, "the ring holds no more than its bound, got %u",
              nya_http_keyring_count(&ring));

    nya_http_keyring_wipe(&ring);
    nya_check(nya_http_keyring_count(&ring) == 0, "and wipes to empty");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}

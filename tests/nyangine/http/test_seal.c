/**
 * The sealed cookie: state put in a client's hands, trusted only because a single altered byte, a
 * wrong label, a wrong secret or a passed expiry all make it not open.
 *
 * No clock is pinned here, so the expiry tests use a one-second ttl and the real wall clock; what is
 * checked is that a token in the future opens and a token already past does not, not the exact second.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** A secret long enough to be accepted, and nothing anybody would ship. */
static const u8 SECRET[] = "0123456789abcdef0123456789abcdef";
#define SECRET_SIZE (sizeof(SECRET) - 1)

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a round trip, which is the whole point.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    typedef struct {
      u64 user_id;
      u32 flags;
    } State;

    State out = { .user_id = 4200, .flags = 0b101 };

    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    nya_check(nya_http_seal(SECRET, SECRET_SIZE, "session", (const u8*)&out, sizeof(out), 900, token, sizeof(token)).ok, "state seals");
    nya_check(token[0] != '\0', "into a token");
    nya_check(strlen(token) < NYA_HTTP_MAX_COOKIE_VALUE, "that fits a cookie, got %zu bytes", strlen(token));

    // the token is not the state: the id is nowhere in the printable token.
    nya_check(!nya_string_contains(token, "4200"), "and the state is not readable in it");

    State back = { 0 };
    u64   size = 0;

    nya_check(nya_http_unseal(SECRET, SECRET_SIZE, "session", token, strlen(token), (u8*)&back, sizeof(back), &size), "and unseals");
    nya_check(size == sizeof(State), "to the same size, got %llu", (unsigned long long)size);
    nya_check(back.user_id == 4200 && back.flags == 0b101, "as the same state, got %llu / %u", (unsigned long long)back.user_id, back.flags);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: everything that must make a token not open.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    NYA_EXPECT(nya_http_seal(SECRET, SECRET_SIZE, "session", (const u8*)"hello", 5, 900, token, sizeof(token)));

    u8  out[64] = { 0 };
    u64 size    = 0;

    nya_check(nya_http_unseal(SECRET, SECRET_SIZE, "session", token, strlen(token), out, sizeof(out), &size), "the real token opens");

    // a different label: a theme cookie pasted into the session slot does not open.
    nya_check(!nya_http_unseal(SECRET, SECRET_SIZE, "theme", token, strlen(token), out, sizeof(out), &size), "a wrong label does not");

    // a different secret: another server's key does not open this one's token.
    static const u8 OTHER[] = "ffffffffffffffffffffffffffffffff";
    nya_check(!nya_http_unseal(OTHER, sizeof(OTHER) - 1, "session", token, strlen(token), out, sizeof(out), &size), "a wrong secret does not");

    // one byte flipped anywhere in the token: the tag no longer matches.
    char tampered[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    (void)snprintf(tampered, sizeof(tampered), "%s", token);
    tampered[3] = tampered[3] == 'A' ? 'B' : 'A';

    nya_check(!nya_http_unseal(SECRET, SECRET_SIZE, "session", tampered, strlen(tampered), out, sizeof(out), &size), "a single altered byte does not");

    // something that is not a token at all.
    nya_check(!nya_http_unseal(SECRET, SECRET_SIZE, "session", "not a token", 11, out, sizeof(out), &size), "and neither does nonsense");
    nya_check(!nya_http_unseal(SECRET, SECRET_SIZE, "session", "", 0, out, sizeof(out), &size), "nor nothing at all");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: it expires, and the buffer bounds are honoured.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    NYA_EXPECT(nya_http_seal(SECRET, SECRET_SIZE, "s", (const u8*)"x", 1, 1, token, sizeof(token)));

    u8  out[8] = { 0 };
    u64 size   = 0;

    nya_check(nya_http_unseal(SECRET, SECRET_SIZE, "s", token, strlen(token), out, sizeof(out), &size), "a one-second token opens now");

    // wait it out. A one-second ttl is expired two seconds later whatever the sub-second timing.
    nya_os_time_sleep_ms(2100);

    nya_check(!nya_http_unseal(SECRET, SECRET_SIZE, "s", token, strlen(token), out, sizeof(out), &size), "and not after it expires");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: what seal refuses to make.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };

    nya_check(!nya_http_seal((const u8*)"short", 5, "s", (const u8*)"x", 1, 900, token, sizeof(token)).ok, "a short secret is refused");
    nya_check(!nya_http_seal(SECRET, SECRET_SIZE, "", (const u8*)"x", 1, 900, token, sizeof(token)).ok, "an empty label is refused");
    nya_check(!nya_http_seal(SECRET, SECRET_SIZE, "s", (const u8*)"x", 1, 0, token, sizeof(token)).ok, "a zero ttl is refused");

    u8 enormous[NYA_HTTP_SEAL_MAX_PLAINTEXT + 1] = { 0 };
    nya_check(!nya_http_seal(SECRET, SECRET_SIZE, "s", enormous, sizeof(enormous), 900, token, sizeof(token)).ok, "and a plaintext past the bound");

    // an empty plaintext is a fine thing to seal: a token that says only "this is a valid session".
    NYA_EXPECT(nya_http_seal(SECRET, SECRET_SIZE, "s", nullptr, 0, 900, token, sizeof(token)));

    u8  out[8] = { 0 };
    u64 size   = 0;
    nya_check(nya_http_unseal(SECRET, SECRET_SIZE, "s", token, strlen(token), out, sizeof(out), &size), "and it opens");
    nya_check(size == 0, "to nothing, got %llu", (unsigned long long)size);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a max-size seal fits a cookie, which is the reason for the bound.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u8 full[NYA_HTTP_SEAL_MAX_PLAINTEXT] = { 0 };
    nya_memset(full, 'z', sizeof(full));

    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    nya_check(nya_http_seal(SECRET, SECRET_SIZE, "session", full, sizeof(full), 900, token, sizeof(token)).ok, "the biggest seal seals");
    nya_check(strlen(token) < NYA_HTTP_MAX_COOKIE_VALUE, "and still fits a cookie value, got %zu", strlen(token));

    u8  back[NYA_HTTP_SEAL_MAX_PLAINTEXT] = { 0 };
    u64 size                             = 0;

    nya_check(nya_http_unseal(SECRET, SECRET_SIZE, "session", token, strlen(token), back, sizeof(back), &size), "and opens");
    nya_check(size == sizeof(full) && memcmp(full, back, sizeof(full)) == 0, "to the same bytes");

    // one byte short of room to hold it is a refusal, not a truncation.
    u8 too_small[NYA_HTTP_SEAL_MAX_PLAINTEXT - 1] = { 0 };
    nya_check(!nya_http_unseal(SECRET, SECRET_SIZE, "session", token, strlen(token), too_small, sizeof(too_small), &size),
              "a buffer too small for the plaintext is refused");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}

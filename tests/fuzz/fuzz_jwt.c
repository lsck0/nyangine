/**
 * The JWT decoder, fed whatever arrives in `Authorization: Bearer`.
 *
 * A token is a stranger's bytes: it comes off a header the client writes, so `nya_http_jwt_decode` is
 * asked to verify and parse a string chosen to break it. The order the decoder promises is the security
 * property — a length bound, the three-part shape, the signature in constant time, the `alg`, and only
 * then the JSON payload — and none of that matters if a crafted token reads out of bounds or asserts.
 *
 * The oracle is not "it verifies": under a fixed secret almost nothing an attacker types will. It is
 * that a refusal produces no identity a caller could act on, that an identity that did verify is
 * well formed and was actually valid at `now_s`, and that verifying the same bytes twice gives the same
 * answer. A token that does not verify is the expected result and not a finding; a crash on one is.
 *
 * The corpus seeds a real HS256 token minted with the same secret so a fuzzer reaches past the shape
 * checks into the signature and the payload; a genuine round trip is test_auth.c's.
 **/

// clang-format off
// the engine defines the feature test macros this build needs, so it comes before any libc header.
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"
// clang-format on

#define FUZZ_TARGET "jwt"

/** A fixed key of the minimum length, so the shape of the secret is never what makes a case interesting. */
static const u8 SECRET[] = "0123456789abcdef0123456789abcdef";
#define SECRET_SIZE (sizeof(SECRET) - 1)

/** A fixed clock the token is checked against, so expiry is decided by the token's bytes and not the wall. */
#define NOW_S 1'700'000'000ULL

/** Everything a verified identity promises, and the calls that take one. */
static void fuzz_check_identity(const NYA_HttpIdentity* identity) {
    // a verified subject is never empty and is terminated inside its buffer: a handler indexes it as a string.
    u64 subject_length = strnlen(identity->subject, sizeof(identity->subject));
    nya_assert(subject_length > 0, "a verified identity carried an empty subject");
    nya_assert(subject_length < sizeof(identity->subject), "a verified subject was not terminated inside its buffer");

    // the scope is a mask of the bits the enum defines and nothing else.
    NYA_HttpScope known = NYA_HTTP_SCOPE_READ | NYA_HTTP_SCOPE_WRITE | NYA_HTTP_SCOPE_ADMIN | NYA_HTTP_SCOPE_SECOND_FACTOR;
    nya_assert(((u32)identity->scope & ~(u32)known) == 0, "a verified identity carried a scope bit the enum does not define");

    // the decoder refuses an expiry that is not after the issue time, and refuses a token outside its window.
    nya_assert(identity->expires_at_s > identity->issued_at_s, "a verified identity expires before it was issued");
    nya_assert(NOW_S < identity->expires_at_s, "an expired token verified");
}

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_jwt");
    defer      nya_arena_destroy(arena);

    NYA_HttpIdentity identity = { 0 };

    b8 verified = nya_http_jwt_decode(arena, (const char*)data, size, SECRET, SECRET_SIZE, NOW_S, &identity).ok;

    if (!verified) {
        // a refusal leaves nothing a caller could mistake for a subject: the decoder zeroes the identity first.
        nya_assert(identity.subject[0] == '\0', "a refused token left bytes in the subject");
        nya_assert(identity.scope == NYA_HTTP_SCOPE_NONE, "a refused token left a scope set");
        return;
    }

    fuzz_check_identity(&identity);

    // verifying the identical bytes a second time gives the identical answer: nothing here depends on hidden state.
    NYA_HttpIdentity again = { 0 };
    b8 reverified = nya_http_jwt_decode(arena, (const char*)data, size, SECRET, SECRET_SIZE, NOW_S, &again).ok;

    nya_assert(reverified, "a token that verified once was refused the second time");
    nya_assert(again.scope == identity.scope && again.issued_at_s == identity.issued_at_s && again.expires_at_s == identity.expires_at_s
                   && strcmp(again.subject, identity.subject) == 0,
               "the decoder was not deterministic for one token");

    // a token that verified under the right secret must not verify under a wrong one of the same length: the
    // signature is what carries the trust, so flipping the key it is checked against has to refuse it.
    static const u8 OTHER_SECRET[] = "fedcba9876543210fedcba9876543210";

    NYA_HttpIdentity wrong = { 0 };
    b8 wrong_key = nya_http_jwt_decode(arena, (const char*)data, size, OTHER_SECRET, sizeof(OTHER_SECRET) - 1, NOW_S, &wrong).ok;

    nya_assert(!wrong_key && wrong.subject[0] == '\0', "a token verified under a secret it was not signed with");
}

#include "tests/fuzz/fuzz.h"

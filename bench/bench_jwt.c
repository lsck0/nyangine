/**
 * The JWT paths a token-guarded server pays: minting a token once at login, and verifying one on every
 * request that carries it.
 *
 * nya_http_jwt_encode signs an identity into a compact JWS with one HMAC-SHA256 and allocates nothing;
 * nya_http_jwt_decode is the per-request cost — a base64url decode of two segments, a constant-time
 * signature check, and a small JSON parse of the claims into an arena. Verification is the hot one: it
 * runs before a guarded handler on every request, so it has to clear many per second. Both run under a
 * fixed secret against a token minted the same way a real login would.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

static const u8 SECRET[]  = "0123456789abcdef0123456789abcdef";
#define SECRET_SIZE (sizeof(SECRET) - 1)
#define NOW_S 1'700'000'000ULL

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_jwt");
    defer      nya_arena_destroy(arena);

    NYA_Arena* scratch = nya_arena_create(.name = "bench_jwt_scratch");
    defer      nya_arena_destroy(scratch);

    NYA_HttpIdentity identity = {
        .scope        = NYA_HTTP_SCOPE_READ | NYA_HTTP_SCOPE_WRITE,
        .issued_at_s  = NOW_S,
        .expires_at_s = NOW_S + 3600,
    };
    nya_memcpy(identity.subject, "bench-subject", sizeof("bench-subject"));

    char token[NYA_HTTP_MAX_TOKEN_BYTES] = { 0 };
    NYA_EXPECT(nya_http_jwt_encode(&identity, SECRET, SECRET_SIZE, token, sizeof(token)), "the bench token is minted");
    u64 token_size = strlen(token);

    nya_bench_begin("JWT (the two paths a token-guarded request pays)");

    // The per-request check: verify the signature and parse the claims. The hot path, so it must clear
    // many per second; a slow number here is felt on every guarded route.
    nya_bench("verify and parse (per request)", 1, {
        nya_arena_free_all(scratch);
        NYA_HttpIdentity found = { 0 };
        NYA_Error valid = nya_http_jwt_decode(scratch, token, token_size, SECRET, SECRET_SIZE, NOW_S, &found);
        nya_bench_keep(valid.ok ? (u64)found.scope : 0);
    });

    // The login check: sign an identity into a token. Allocates nothing, so it is the cheap one.
    nya_bench("mint a token (per login)", 1, {
        char minted[NYA_HTTP_MAX_TOKEN_BYTES] = { 0 };
        NYA_Error signed_ = nya_http_jwt_encode(&identity, SECRET, SECRET_SIZE, minted, sizeof(minted));
        nya_bench_keep(signed_.ok ? minted[0] : 0);
    });

    return nya_bench_end();
}

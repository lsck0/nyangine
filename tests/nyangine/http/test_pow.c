/**
 * The proof-of-work layer: a request with no solution is handed a challenge and its handler does not
 * run; a correct solution passes exactly once; an under-difficulty solution, a replayed one, and a
 * forged or tampered token are all refused with a fresh challenge; a malformed solution header is 400.
 *
 * Driven through nya_http_router_dispatch with the layer installed, like test_idempotency.c: a route
 * table and an exchange are data, and what is under test is the wall rather than the socket. A
 * side-effect counter stands in for the work a real handler does, so "the handler ran" is an assertion
 * on a number. The clock is the exchange's own `now_s`, stepped by the test, so the spent-nonce window
 * is exercised without sleeping.
 *
 * The difficulty is deliberately tiny — eight leading zero bits, a few hundred hashes — so nya_http_pow_solve
 * returns at once and the test is deterministic. Expiry of the sealed token itself is http_seal's own
 * test; here a "forged" token (sealed under a different secret) and a "tampered" one (a byte flipped)
 * stand for every token that does not open, which is the case the layer has to refuse.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#define GUARDED_PATH "/api/guarded"
#define NOW_S        1700000000ULL
#define TTL_S        30ULL
#define DIFFICULTY   8

/** A fixed secret, so the seal is reproducible across a run. Past NYA_HTTP_SEAL_MIN_SECRET_BYTES. */
static const u8 SECRET[32] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01,
                               0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x20 };

/** A different secret, for the forged-token case: a token sealed under it must not open under SECRET. */
static const u8 OTHER_SECRET[32] = { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0,
                                     0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0 };

/** How many times the guarded handler has run. The whole point of the wall is that a refusal never moves it. */
static u32 SIDE_EFFECTS = 0;

static NYA_HttpStatus guarded(NYA_HttpExchange* exchange) {
    SIDE_EFFECTS++;

    return nya_http_response_text(exchange->response, "{\"ok\":true}", NYA_HTTP_MEDIA_JSON).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

static const NYA_HttpRoute ROUTES[] = {
    {
     .method   = NYA_HTTP_METHOD_POST,
     .path     = GUARDED_PATH,
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = guarded,
     .summary  = "A route behind the proof-of-work wall",
     // OK is the handler's; BAD_REQUEST and UNAUTHORIZED are the layer's, and a route's statuses cover
     // its whole chain (see http_router.h). SERVICE_UNAVAILABLE and INTERNAL_ERROR are 5xx and need no declaration.
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED },
     },
};

static const NYA_HttpRouter ROUTER = { .name = "guarded", .routes = ROUTES, .route_count = nya_carray_length(ROUTES) };

/* ONE EXCHANGE */

/** Builds a POST request by hand, optionally carrying the token and solution headers (lowercased, as parsed). */
static void make_request(OUT NYA_HttpRequest* request, NYA_ConstCString token, NYA_ConstCString solution) {
    *request = (NYA_HttpRequest){ .method = NYA_HTTP_METHOD_POST, .keep_alive = true };

    NYA_UrlFailure failure = { 0 };
    NYA_EXPECT(nya_url_parse_target(GUARDED_PATH, strlen(GUARDED_PATH), &request->target, &failure), "while building a request");

    (void)snprintf(request->path, sizeof(request->path), "%.*s", (int)request->target.path.length, request->target.text + request->target.path.offset);

    if (token != nullptr) {
        (void)snprintf(request->headers[request->header_count].name, sizeof(request->headers[0].name), "x-pow-token");
        (void)snprintf(request->headers[request->header_count].value, sizeof(request->headers[0].value), "%s", token);
        request->header_count++;
    }

    if (solution != nullptr) {
        (void)snprintf(request->headers[request->header_count].name, sizeof(request->headers[0].name), "x-pow-solution");
        (void)snprintf(request->headers[request->header_count].value, sizeof(request->headers[0].value), "%s", solution);
        request->header_count++;
    }

    request->media_type = NYA_HTTP_MEDIA_JSON;
    request->body_size  = (u64)snprintf((char*)request->body, sizeof(request->body), "{}");
}

/** One dispatch through the PoW layer, at time `now_s`, with the given secret on the exchange. */
static NYA_HttpStatus dispatch(NYA_Arena* arena, const u8* secret, NYA_ConstCString token, NYA_ConstCString solution, u64 now_s, OUT NYA_HttpResponse* out_response) {
    NYA_HttpRequest request = { 0 };
    make_request(&request, token, solution);

    u8* buffer = nya_arena_alloc(arena, NYA_HTTP_MAX_BODY_BYTES + 1);
    nya_http_response_create(out_response, buffer, NYA_HTTP_MAX_BODY_BYTES + 1);

    const NYA_HttpRouter* routers[] = { &ROUTER };
    const NYA_HttpLayerFn layers[]  = { nya_http_layer_pow };

    NYA_HttpExchange exchange = {
        .request     = &request,
        .response    = out_response,
        .arena       = arena,
        .now_s       = now_s,
        .secret      = secret,
        .secret_size = 32,
        .address     = "127.0.0.1",
    };

    return nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), layers, nya_carray_length(layers));
}

/** Two null-terminated names equal, ignoring ASCII case. */
static b8 name_equals(NYA_ConstCString a, NYA_ConstCString b) {
    u64 index = 0;
    for (; a[index] != '\0' && b[index] != '\0'; index++) {
        char la = (a[index] >= 'A' && a[index] <= 'Z') ? (char)(a[index] + 32) : a[index];
        char lb = (b[index] >= 'A' && b[index] <= 'Z') ? (char)(b[index] + 32) : b[index];
        if (la != lb) return false;
    }

    return a[index] == '\0' && b[index] == '\0';
}

/** The value of a response header by name, or null. */
static NYA_ConstCString response_header(const NYA_HttpResponse* response, NYA_ConstCString name) {
    for (u32 index = 0; index < response->header_count; index++) {
        if (name_equals(response->headers[index].name, name)) return response->headers[index].value;
    }

    return nullptr;
}

/** Leading zero bits of SHA-256(nonce || suffix), the measure the layer uses, for the deterministic tests. */
static u32 zero_bits(const u8* nonce, const u8* suffix, u64 suffix_size) {
    u8 material[NYA_HTTP_POW_NONCE_BYTES + NYA_HTTP_POW_MAX_SOLUTION] = { 0 };
    nya_memcpy(material, nonce, NYA_HTTP_POW_NONCE_BYTES);
    nya_memcpy(material + NYA_HTTP_POW_NONCE_BYTES, suffix, suffix_size);

    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256(material, NYA_HTTP_POW_NONCE_BYTES + suffix_size, &digest);

    return nya_http_pow_leading_zero_bits(digest.bytes, sizeof(digest.bytes));
}

/** An eight-byte counter, little-endian, whose digest with `nonce` is guaranteed under `difficulty` bits. */
static void find_under(const u8* nonce, u8 difficulty, OUT u8* out_suffix, OUT u64* out_size) {
    for (u64 counter = 0;; counter++) {
        u8 suffix[8] = { 0 };
        for (u64 byte = 0; byte < sizeof(suffix); byte++) suffix[byte] = (u8)(counter >> (byte * 8));

        if (zero_bits(nonce, suffix, sizeof(suffix)) < difficulty) {
            nya_memcpy(out_suffix, suffix, sizeof(suffix));
            *out_size = sizeof(suffix);
            return;
        }
    }
}

/* THE TESTS */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_pow");
    defer      nya_arena_destroy(arena);

    NYA_EXPECT(nya_http_pow_init(arena, .difficulty = DIFFICULTY, .ttl_s = TTL_S), "while readying the store");

    // TEST: a request with no proof is handed a challenge, and its handler does not run.
    char challenge_token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    u8   nonce[NYA_HTTP_POW_NONCE_BYTES]          = { 0 };

    {
        nya_http_pow_reset();
        SIDE_EFFECTS = 0;

        NYA_HttpResponse response = { 0 };
        NYA_HttpStatus   status   = dispatch(arena, SECRET, nullptr, nullptr, NOW_S, &response);

        nya_check(status == NYA_HTTP_STATUS_UNAUTHORIZED, "a request with no proof is 401");
        nya_check(SIDE_EFFECTS == 0, "and the guarded handler did not run");

        NYA_ConstCString challenge   = response_header(&response, NYA_HTTP_POW_CHALLENGE_HEADER);
        NYA_ConstCString difficulty  = response_header(&response, NYA_HTTP_POW_DIFFICULTY_HEADER);
        NYA_ConstCString token       = response_header(&response, NYA_HTTP_POW_TOKEN_HEADER);

        nya_check(challenge != nullptr, "the challenge carries a nonce");
        nya_check(difficulty != nullptr && strcmp(difficulty, "8") == 0, "the challenge carries the difficulty in force");
        nya_check(token != nullptr, "the challenge carries a sealed token");

        // Keep the token and the decoded nonce for the accept, replay and tamper tests below.
        if (token != nullptr) (void)snprintf(challenge_token, sizeof(challenge_token), "%s", token);

        if (challenge != nullptr) {
            u64 nonce_size = 0;
            nya_check(nya_crypto_base64url_decode(challenge, strlen(challenge), nonce, sizeof(nonce), &nonce_size), "the nonce is base64url");
            nya_check(nonce_size == NYA_HTTP_POW_NONCE_BYTES, "the nonce is the expected size");
        }

        nya_http_response_destroy(&response);
    }

    // TEST: a correct solution passes, the handler runs exactly once, and the nonce is then spent.
    u8   solution[NYA_HTTP_POW_MAX_SOLUTION] = { 0 };
    u64  solution_size                       = 0;
    char solution_b64[128]                   = { 0 };

    {
        SIDE_EFFECTS = 0;

        nya_check(nya_http_pow_solve(nonce, sizeof(nonce), DIFFICULTY, solution, sizeof(solution), &solution_size), "a solution is found");

        u64 encoded = 0;
        nya_check(nya_crypto_base64url_encode(solution, solution_size, solution_b64, sizeof(solution_b64), &encoded), "the solution encodes");

        NYA_HttpResponse response = { 0 };
        NYA_HttpStatus   status   = dispatch(arena, SECRET, challenge_token, solution_b64, NOW_S, &response);

        nya_check(status == NYA_HTTP_STATUS_OK, "a correct solution passes the wall");
        nya_check(SIDE_EFFECTS == 1, "the guarded handler ran exactly once");
        nya_check(nya_http_pow_count() == 1, "the nonce is now spent");

        nya_http_response_destroy(&response);
    }

    // TEST: the same token and solution again is a replay, refused with a fresh 401, handler not re-run.
    {
        SIDE_EFFECTS = 0;

        NYA_HttpResponse response = { 0 };
        NYA_HttpStatus   status   = dispatch(arena, SECRET, challenge_token, solution_b64, NOW_S, &response);

        nya_check(status == NYA_HTTP_STATUS_UNAUTHORIZED, "a replayed proof is refused 401");
        nya_check(SIDE_EFFECTS == 0, "and the handler did not run again");
        nya_check(response_header(&response, NYA_HTTP_POW_TOKEN_HEADER) != nullptr, "the refusal hands out a fresh challenge");

        nya_http_response_destroy(&response);
    }

    // TEST: a solution under the difficulty is refused, and the handler does not run.
    {
        nya_http_pow_reset();
        SIDE_EFFECTS = 0;

        // A fresh challenge for a clean nonce, so the refusal is difficulty and not replay.
        NYA_HttpResponse issued = { 0 };
        (void)dispatch(arena, SECRET, nullptr, nullptr, NOW_S, &issued);
        NYA_ConstCString token     = response_header(&issued, NYA_HTTP_POW_TOKEN_HEADER);
        NYA_ConstCString challenge = response_header(&issued, NYA_HTTP_POW_CHALLENGE_HEADER);

        char fresh_token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
        if (token != nullptr) (void)snprintf(fresh_token, sizeof(fresh_token), "%s", token);

        // The fresh challenge's own nonce, and a suffix found to be under the bar against it: an honest
        // under-difficulty answer rather than a solved one, chosen deterministically so the test never flakes.
        u8  fresh_nonce[NYA_HTTP_POW_NONCE_BYTES] = { 0 };
        u64 fresh_nonce_size                      = 0;
        nya_check(nya_crypto_base64url_decode(challenge, strlen(challenge), fresh_nonce, sizeof(fresh_nonce), &fresh_nonce_size), "the fresh nonce decodes");

        u8   weak[8]     = { 0 };
        u64  weak_size   = 0;
        char weak_b64[128] = { 0 };
        u64  weak_encoded  = 0;
        find_under(fresh_nonce, DIFFICULTY, weak, &weak_size);
        (void)nya_crypto_base64url_encode(weak, weak_size, weak_b64, sizeof(weak_b64), &weak_encoded);

        NYA_HttpResponse response = { 0 };
        NYA_HttpStatus   status   = dispatch(arena, SECRET, fresh_token, weak_b64, NOW_S, &response);

        nya_check(status == NYA_HTTP_STATUS_UNAUTHORIZED, "an under-difficulty solution is refused 401");
        nya_check(SIDE_EFFECTS == 0, "and the handler did not run");

        nya_http_response_destroy(&issued);
        nya_http_response_destroy(&response);
    }

    // TEST: a token forged under another secret does not open, and is refused.
    {
        nya_http_pow_reset();
        SIDE_EFFECTS = 0;

        // Mint a well-formed token under the wrong secret: right layout, right difficulty, wrong key.
        u8 sealed[NYA_HTTP_POW_SEALED] = { 0 };
        sealed[0]                      = NYA_HTTP_POW_VERSION;
        sealed[1]                      = DIFFICULTY;
        nya_memcpy(sealed + 2, nonce, sizeof(nonce));

        char forged[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
        NYA_EXPECT(nya_http_seal(OTHER_SECRET, sizeof(OTHER_SECRET), NYA_HTTP_POW_LABEL, sealed, sizeof(sealed), TTL_S, forged, sizeof(forged)),
                   "while sealing a forged token");

        NYA_HttpResponse response = { 0 };
        NYA_HttpStatus   status   = dispatch(arena, SECRET, forged, solution_b64, NOW_S, &response);

        nya_check(status == NYA_HTTP_STATUS_UNAUTHORIZED, "a token forged under another secret is refused 401");
        nya_check(SIDE_EFFECTS == 0, "and the handler did not run");

        nya_http_response_destroy(&response);
    }

    // TEST: a valid token with one byte flipped does not open, and is refused.
    {
        nya_http_pow_reset();
        SIDE_EFFECTS = 0;

        NYA_HttpResponse issued = { 0 };
        (void)dispatch(arena, SECRET, nullptr, nullptr, NOW_S, &issued);
        NYA_ConstCString token = response_header(&issued, NYA_HTTP_POW_TOKEN_HEADER);

        char tampered[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
        if (token != nullptr) (void)snprintf(tampered, sizeof(tampered), "%s", token);

        // Flip a character in the middle of the token — still base64url, but no longer the sealed bytes.
        u64 length = strlen(tampered);
        if (length > 0) tampered[length / 2] = tampered[length / 2] == 'A' ? 'B' : 'A';

        NYA_HttpResponse response = { 0 };
        NYA_HttpStatus   status   = dispatch(arena, SECRET, tampered, solution_b64, NOW_S, &response);

        nya_check(status == NYA_HTTP_STATUS_UNAUTHORIZED, "a tampered token is refused 401");
        nya_check(SIDE_EFFECTS == 0, "and the handler did not run");

        nya_http_response_destroy(&issued);
        nya_http_response_destroy(&response);
    }

    // TEST: a solution header that is not base64url is a malformed request, 400 rather than a challenge.
    {
        nya_http_pow_reset();
        SIDE_EFFECTS = 0;

        NYA_HttpResponse issued = { 0 };
        (void)dispatch(arena, SECRET, nullptr, nullptr, NOW_S, &issued);
        NYA_ConstCString token = response_header(&issued, NYA_HTTP_POW_TOKEN_HEADER);

        char fresh_token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
        if (token != nullptr) (void)snprintf(fresh_token, sizeof(fresh_token), "%s", token);

        // A '+' is not in the base64url alphabet, so this decodes to nothing and is a malformed header.
        NYA_HttpResponse response = { 0 };
        NYA_HttpStatus   status   = dispatch(arena, SECRET, fresh_token, "not+valid+base64url==", NOW_S, &response);

        nya_check(status == NYA_HTTP_STATUS_BAD_REQUEST, "a non-base64url solution is 400");
        nya_check(SIDE_EFFECTS == 0, "and the handler did not run");

        nya_http_response_destroy(&issued);
        nya_http_response_destroy(&response);
    }

    // TEST: with no secret on the exchange the wall fails closed with 503 rather than waving requests through.
    {
        nya_http_pow_reset();
        SIDE_EFFECTS = 0;

        NYA_HttpResponse response = { 0 };
        NYA_HttpStatus   status   = dispatch(arena, nullptr, "anything", "anything", NOW_S, &response);

        nya_check(status == NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "no secret is a fail-closed 503");
        nya_check(SIDE_EFFECTS == 0, "and the handler did not run");

        nya_http_response_destroy(&response);
    }

    // gives the lock back to the arena before it is destroyed, and exercises deinit's own path.
    nya_http_pow_deinit();

    printf("test_pow: %u assertion(s) failed.\n", nya_check_failures());

    return nya_check_failures() == 0 ? 0 : 1;
}

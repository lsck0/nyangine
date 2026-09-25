#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_ceiling.h"
#include "nyangine-std/base/base_thread.h"
#include "nyangine-core/crypto/crypto_encoding.h"
#include "nyangine-core/crypto/crypto_hash.h"
#include "nyangine-core/http/http_message.h"
#include "nyangine-core/http/http_pow.h"
#include "nyangine-core/http/http_seal.h"
#include "nyangine-std/os/os_random.h"

// PRIVATE TYPES

/** One nonce that has already bought a request, kept until its challenge would have expired. */
typedef struct {
    /** Zero is a slot nobody has spent, so a zeroed table is an empty one. */
    b8 used;

    /** The nonce the challenge carried, matched whole against a request's token on the way in. */
    u8 nonce[NYA_HTTP_POW_NONCE_BYTES];

    /** Seconds since the epoch after which this slot is free again — the same window the token had. */
    u64 expires_at_s;
} _NYA_HttpPowSpent;

typedef struct {
    _NYA_HttpPowSpent entries[NYA_HTTP_POW_MAX_SPENT];

    /** Occupied slots, which is the ceiling's live count. An expired slot is swept before it is reused. */
    u32 count;

    /** Leading zero bits a solution must have. Zero here means the default; always read through _difficulty. */
    u8 difficulty;

    /** How long a challenge lives, in seconds. Zero here means the default; always read through _ttl_s. */
    u64 ttl_s;

    /** Made at init from the caller's arena. Null when init was never called, which nya_mutex_lock allows. */
    NYA_Mutex* mutex;

    b8 registered;
} _NYA_HttpPowStore;

/** One server per process, so one store. Zeroed, so the layer works before init at the cost of no lock. */
NYA_INTERNAL _NYA_HttpPowStore _NYA_HTTP_POW = { 0 };

// PRIVATE API DECLARATION

/** The difficulty in force, resolving the zero default and the clamp, so the policy lives in one place. */
NYA_INTERNAL u8 _nya_http_pow_difficulty(void) __attr_no_discard;

/** The challenge TTL in force, resolving the zero default. */
NYA_INTERNAL u64 _nya_http_pow_ttl_s(void) __attr_no_discard;

/** Whether `entry` has lived past its window as of `now_s`. An empty entry is never expired. */
NYA_INTERNAL b8 _nya_http_pow_expired(const _NYA_HttpPowSpent* entry, u64 now_s) __attr_no_discard;

/** Clears every expired slot, decrementing the count. Called before a spend needs a slot. */
NYA_INTERNAL void _nya_http_pow_sweep(u64 now_s);

/** Whether `nonce` is already spent and still live as of `now_s`. An expired match reads as absent. */
NYA_INTERNAL b8 _nya_http_pow_seen(const u8* nonce, u64 now_s) __attr_no_discard;

/** Remembers `nonce` as spent until `expires_at_s`, sweeping as of `now_s`. Reuses an expired slot, then evicts the soonest to expire. */
NYA_INTERNAL void _nya_http_pow_spend(const u8* nonce, u64 now_s, u64 expires_at_s);

/** The digest of `nonce || solution`, the material a difficulty is measured against. */
NYA_INTERNAL void _nya_http_pow_digest(const u8* nonce, const u8* solution, u64 solution_size, OUT NYA_CryptoSha256Digest* out_digest);

/** Issues a fresh challenge onto `exchange` and answers 401 without running the chain. */
NYA_INTERNAL NYA_HttpStatus _nya_http_pow_issue(NYA_HttpExchange* exchange) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error _nya_http_pow_init(NYA_Arena* arena, NYA_HttpPowOptions options) {
    nya_assert(arena != nullptr);

    _NYA_HTTP_POW.difficulty = options.difficulty;
    _NYA_HTTP_POW.ttl_s      = options.ttl_s;

    // The lock is made once and kept: a second init reloads the policy, not the store; a second mutex would leak the first and split threads across two.
    if (_NYA_HTTP_POW.mutex == nullptr) {
        NYA_TRY(nya_mutex_create(arena, &_NYA_HTTP_POW.mutex));
    }

    if (!_NYA_HTTP_POW.registered) {
        nya_ceiling_register("http_pow", NYA_HTTP_POW_MAX_SPENT, &_NYA_HTTP_POW.count);
        _NYA_HTTP_POW.registered = true;
    }

    return NYA_OK;
}

void nya_http_pow_deinit(void) {
    // The lock goes back to its arena, so the store is again the zeroed table it was before init; registered stays true, since nya_ceiling_register holds the count's address, which doesn't move.
    nya_mutex_destroy(_NYA_HTTP_POW.mutex);
    _NYA_HTTP_POW.mutex = nullptr;

    nya_memset(_NYA_HTTP_POW.entries, 0, sizeof(_NYA_HTTP_POW.entries));
    _NYA_HTTP_POW.count      = 0;
    _NYA_HTTP_POW.difficulty = 0;
    _NYA_HTTP_POW.ttl_s      = 0;
}

void nya_http_pow_reset(void) {
    nya_mutex_lock(_NYA_HTTP_POW.mutex);

    nya_memset(_NYA_HTTP_POW.entries, 0, sizeof(_NYA_HTTP_POW.entries));
    _NYA_HTTP_POW.count = 0;

    nya_mutex_unlock(_NYA_HTTP_POW.mutex);
}

u32 nya_http_pow_count(void) {
    return _NYA_HTTP_POW.count;
}

u32 nya_http_pow_leading_zero_bits(const u8* digest, u64 size) {
    nya_assert(digest != nullptr);

    u32 bits = 0;

    for (u64 index = 0; index < size; index++) {
        u8 byte = digest[index];

        if (byte == 0) {
            bits += 8;
            continue;
        }

        // The first non-zero byte ends the run; count the zeros above its top set bit and stop.
        for (u8 mask = 0x80; mask != 0; mask >>= 1) {
            if ((byte & mask) != 0) return bits;
            bits++;
        }
    }

    return bits;
}

b8 nya_http_pow_solve(const u8* nonce, u64 nonce_size, u8 difficulty, OUT u8* out_solution, u64 capacity, OUT u64* out_size) {
    nya_assert(nonce != nullptr && out_solution != nullptr && out_size != nullptr);

    // A difficulty past the clamp is one the layer would never mint, so refuse rather than spin forever on an unhittable target; eight bytes is the counter's width, and a buffer below it can't hold one.
    if (difficulty > NYA_HTTP_POW_MAX_DIFFICULTY || capacity < sizeof(u64)) return false;

    // The nonce is bound to the challenge by the caller, not its length: the digest reads a fixed NYA_HTTP_POW_NONCE_BYTES, so a size argument is here only for symmetry with the layer.
    nya_unused(nonce_size);

    // A counter, little-endian, from zero: deterministic (same challenge, same suffix) and monotone (every candidate tried once); the first whose digest clears the bar wins.
    for (u64 counter = 0;; counter++) {
        u8 suffix[sizeof(u64)] = { 0 };
        for (u64 byte = 0; byte < sizeof(u64); byte++) suffix[byte] = (u8)(counter >> (byte * 8));

        NYA_CryptoSha256Digest digest = { 0 };
        _nya_http_pow_digest(nonce, suffix, sizeof(suffix), &digest);

        if (nya_http_pow_leading_zero_bits(digest.bytes, sizeof(digest.bytes)) >= difficulty) {
            nya_memcpy(out_solution, suffix, sizeof(suffix));
            *out_size = sizeof(suffix);
            return true;
        }
    }
}

NYA_HttpStatus nya_http_layer_pow(NYA_HttpExchange* exchange, NYA_HttpChain* next) {
    nya_assert(exchange != nullptr && exchange->request != nullptr && exchange->response != nullptr);

    // No secret is nothing to seal a challenge with. Fail closed: a wall that can't be raised must refuse, not wave everyone through — the shape http_router.h takes for a missing permission table.
    if (exchange->secret == nullptr || exchange->secret_size == 0) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "the proof-of-work wall has no secret to seal a challenge with");
    }

    NYA_ConstCString token    = nya_http_request_header(exchange->request, NYA_HTTP_POW_TOKEN_HEADER);
    NYA_ConstCString solution = nya_http_request_header(exchange->request, NYA_HTTP_POW_SOLUTION_HEADER);

    // A request that carries neither is a first contact: hand it a challenge and let it come back.
    if (token == nullptr || token[0] == '\0' || solution == nullptr || solution[0] == '\0') return _nya_http_pow_issue(exchange);

    // The found suffix, decoded from the header; a body that isn't base64url or is longer than any solution should be is a malformed request (400), not a wrong answer.
    u8  solution_bytes[NYA_HTTP_POW_MAX_SOLUTION] = { 0 };
    u64 solution_size                             = 0;
    u64 solution_text_size                        = strnlen(solution, NYA_HTTP_SEAL_MAX_TOKEN);

    if (!nya_crypto_base64url_decode(solution, solution_text_size, solution_bytes, sizeof(solution_bytes), &solution_size)) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_BAD_REQUEST, "the " NYA_HTTP_POW_SOLUTION_HEADER " is not valid base64url, or is too long");
    }

    // Open the token with this server's own secret; one that doesn't open (tampered, forged under another key, or expired) is refused with a fresh challenge the client can retry.
    u8  sealed[NYA_HTTP_POW_SEALED] = { 0 };
    u64 sealed_size                 = 0;
    u64 token_size                  = strnlen(token, NYA_HTTP_SEAL_MAX_TOKEN);

    if (!nya_http_unseal(exchange->secret, exchange->secret_size, NYA_HTTP_POW_LABEL, token, token_size, sealed, sizeof(sealed), &sealed_size)) {
        return _nya_http_pow_issue(exchange);
    }

    // The layout http_pow.h fixes: a version byte, a difficulty byte, then the nonce; anything else is a token this layer didn't mint, refused rather than read past.
    if (sealed_size != NYA_HTTP_POW_SEALED || sealed[0] != NYA_HTTP_POW_VERSION) return _nya_http_pow_issue(exchange);

    // The difficulty is the sealed one, never the X-Pow-Difficulty header: a client can't lower the bar it was set, since the byte lives inside a token it can't open.
    u8        difficulty = sealed[1];
    const u8* nonce      = sealed + 2;

    NYA_CryptoSha256Digest digest = { 0 };
    _nya_http_pow_digest(nonce, solution_bytes, solution_size, &digest);

    // Below the bar is not a wrong answer worth a 400: it is an unsolved challenge, refused with a fresh one.
    if (nya_http_pow_leading_zero_bits(digest.bytes, sizeof(digest.bytes)) < difficulty) return _nya_http_pow_issue(exchange);

    // The work is proven. One request per proof: a spent nonce is a replay, refused with a fresh challenge; otherwise it's remembered as long as the token would live and the chain runs.
    nya_mutex_lock(_NYA_HTTP_POW.mutex);

    if (_nya_http_pow_seen(nonce, exchange->now_s)) {
        nya_mutex_unlock(_NYA_HTTP_POW.mutex);
        return _nya_http_pow_issue(exchange);
    }

    _nya_http_pow_spend(nonce, exchange->now_s, exchange->now_s + _nya_http_pow_ttl_s());

    nya_mutex_unlock(_NYA_HTTP_POW.mutex);

    return nya_http_chain_next(exchange, next);
}

// PRIVATE API IMPLEMENTATION

u8 _nya_http_pow_difficulty(void) {
    u8 difficulty = _NYA_HTTP_POW.difficulty != 0 ? _NYA_HTTP_POW.difficulty : NYA_HTTP_POW_DEFAULT_DIFFICULTY;

    return difficulty <= NYA_HTTP_POW_MAX_DIFFICULTY ? difficulty : NYA_HTTP_POW_MAX_DIFFICULTY;
}

u64 _nya_http_pow_ttl_s(void) {
    return _NYA_HTTP_POW.ttl_s != 0 ? _NYA_HTTP_POW.ttl_s : NYA_HTTP_POW_DEFAULT_TTL_S;
}

b8 _nya_http_pow_expired(const _NYA_HttpPowSpent* entry, u64 now_s) {
    return entry->used && now_s >= entry->expires_at_s;
}

void _nya_http_pow_sweep(u64 now_s) {
    for (u32 index = 0; index < NYA_HTTP_POW_MAX_SPENT; index++) {
        _NYA_HttpPowSpent* entry = &_NYA_HTTP_POW.entries[index];

        if (_nya_http_pow_expired(entry, now_s)) {
            nya_memset(entry, 0, sizeof(*entry));
            if (_NYA_HTTP_POW.count > 0) _NYA_HTTP_POW.count--;
        }
    }
}

b8 _nya_http_pow_seen(const u8* nonce, u64 now_s) {
    for (u32 index = 0; index < NYA_HTTP_POW_MAX_SPENT; index++) {
        const _NYA_HttpPowSpent* entry = &_NYA_HTTP_POW.entries[index];

        if (!entry->used) continue;

        // An expired match reads as absent: its window is over, so a resent nonce is a stale token refused for expiry not replay, and its slot is the sweep's to reclaim.
        if (_nya_http_pow_expired(entry, now_s)) continue;

        if (nya_memcmp(entry->nonce, nonce, NYA_HTTP_POW_NONCE_BYTES) == 0) return true;
    }

    return false;
}

void _nya_http_pow_spend(const u8* nonce, u64 now_s, u64 expires_at_s) {
    // Reclaim expired slots first, as of now — not the future new expiry, which would sweep still-live entries — so the count is honest and a table full of stale nonces takes a new one.
    _nya_http_pow_sweep(now_s);

    _NYA_HttpPowSpent* slot = nullptr;

    if (_NYA_HTTP_POW.count < NYA_HTTP_POW_MAX_SPENT) {
        for (u32 index = 0; index < NYA_HTTP_POW_MAX_SPENT; index++) {
            if (!_NYA_HTTP_POW.entries[index].used) {
                slot = &_NYA_HTTP_POW.entries[index];
                break;
            }
        }

        _NYA_HTTP_POW.count++;
    } else {
        // Every slot is live. Give up the one that expires soonest: closest to being reclaimed anyway, so its token has the least life left to replay. A full table is a burst, not a leak.
        for (u32 index = 0; index < NYA_HTTP_POW_MAX_SPENT; index++) {
            _NYA_HttpPowSpent* candidate = &_NYA_HTTP_POW.entries[index];
            if (slot == nullptr || candidate->expires_at_s < slot->expires_at_s) slot = candidate;
        }
    }

    nya_memset(slot, 0, sizeof(*slot));

    slot->used = true;
    nya_memcpy(slot->nonce, nonce, NYA_HTTP_POW_NONCE_BYTES);
    slot->expires_at_s = expires_at_s;
}

void _nya_http_pow_digest(const u8* nonce, const u8* solution, u64 solution_size, OUT NYA_CryptoSha256Digest* out_digest) {
    // A fixed buffer: the nonce is fixed and the solution bounded, so the material never allocates and the concatenation is unambiguous — the nonce is always the first NYA_HTTP_POW_NONCE_BYTES.
    nya_assert(solution_size <= NYA_HTTP_POW_MAX_SOLUTION);

    u8 material[NYA_HTTP_POW_NONCE_BYTES + NYA_HTTP_POW_MAX_SOLUTION] = { 0 };

    nya_memcpy(material, nonce, NYA_HTTP_POW_NONCE_BYTES);
    nya_memcpy(material + NYA_HTTP_POW_NONCE_BYTES, solution, solution_size);

    nya_crypto_sha256(material, NYA_HTTP_POW_NONCE_BYTES + solution_size, out_digest);
}

NYA_HttpStatus _nya_http_pow_issue(NYA_HttpExchange* exchange) {
    u8 nonce[NYA_HTTP_POW_NONCE_BYTES] = { 0 };

    if (!nya_os_random_bytes(nonce, sizeof(nonce))) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_INTERNAL_ERROR, "the system random source failed, so no challenge could be minted");
    }

    u8 difficulty = _nya_http_pow_difficulty();

    // The sealed plaintext in the fixed layout (version, difficulty, nonce), sealed with the exchange's own secret so the client holds it, returns it untouched, and can't read the nonce or lower the difficulty.
    u8 sealed[NYA_HTTP_POW_SEALED] = { 0 };
    sealed[0]                      = NYA_HTTP_POW_VERSION;
    sealed[1]                      = difficulty;
    nya_memcpy(sealed + 2, nonce, sizeof(nonce));

    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };

    if (!nya_http_seal(exchange->secret, exchange->secret_size, NYA_HTTP_POW_LABEL, sealed, sizeof(sealed), _nya_http_pow_ttl_s(), token, sizeof(token)).ok) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_INTERNAL_ERROR, "the proof-of-work challenge could not be sealed");
    }

    char nonce_b64[64] = { 0 };
    u64  nonce_b64_size = 0;

    if (!nya_crypto_base64url_encode(nonce, sizeof(nonce), nonce_b64, sizeof(nonce_b64), &nonce_b64_size)) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_INTERNAL_ERROR, "the proof-of-work nonce could not be encoded");
    }

    char difficulty_text[8] = { 0 };
    (void)snprintf(difficulty_text, sizeof(difficulty_text), "%u", (unsigned)difficulty);

    // The problem body first (it resets the response and writes a refusal's shape), then the three headers; a losable header is never why a refusal fails, so they're added after the status — a client reading them retries, one that doesn't sees a plain 401.
    NYA_HttpStatus status = nya_http_response_problem(
        exchange, NYA_HTTP_STATUS_UNAUTHORIZED,
        "solve the proof-of-work challenge: find a suffix whose SHA-256 with " NYA_HTTP_POW_CHALLENGE_HEADER " has "
        NYA_HTTP_POW_DIFFICULTY_HEADER " leading zero bits, then resend with " NYA_HTTP_POW_SOLUTION_HEADER " and " NYA_HTTP_POW_TOKEN_HEADER
    );

    (void)nya_http_response_header(exchange->response, NYA_HTTP_POW_CHALLENGE_HEADER, nonce_b64);
    (void)nya_http_response_header(exchange->response, NYA_HTTP_POW_DIFFICULTY_HEADER, difficulty_text);
    (void)nya_http_response_header(exchange->response, NYA_HTTP_POW_TOKEN_HEADER, token);

    return status;
}

#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_ceiling.h"
#include "nyangine-std/base/base_thread.h"
#include "nyangine-core/crypto/crypto_hash.h"
#include "nyangine-core/http/http_idempotency.h"
#include "nyangine-core/http/http_message.h"

// PRIVATE TYPES

/** Where an entry is in its life. Zero is a slot nobody has used, so a zeroed table is an empty one. */
typedef enum {
    _NYA_HTTP_IDEMPOTENCY_EMPTY = 0,

    /** The chain is running for this key. A duplicate that arrives now is a 409, and this is never evicted. */
    _NYA_HTTP_IDEMPOTENCY_IN_FLIGHT,

    /** The answer is stored and replayable. This is what a retry with the same key gets back. */
    _NYA_HTTP_IDEMPOTENCY_COMPLETED,
} _NYA_HttpIdempotencyState;

/** One key and the answer stored under it. The body is a fixed buffer, so an entry is plain storage. */
typedef struct {
    char key[NYA_HTTP_IDEMPOTENCY_MAX_KEY];

    _NYA_HttpIdempotencyState state;

    /** BLAKE2b over the method, path and body of the request that reserved the key; see _nya_http_idempotency_fingerprint. */
    u8 fingerprint[NYA_HTTP_IDEMPOTENCY_FINGERPRINT_BYTES];

    /** The captured answer, meaningful only in _COMPLETED. */
    NYA_HttpStatus    status;
    NYA_HttpMediaType media;
    u8                body[NYA_HTTP_IDEMPOTENCY_MAX_BODY_BYTES];
    u64               body_size;

    /** Seconds since the epoch of the last touch: reserve, then completion. TTL is measured from here. */
    u64 updated_at_s;
} _NYA_HttpIdempotencyEntry;

typedef struct {
    _NYA_HttpIdempotencyEntry entries[NYA_HTTP_IDEMPOTENCY_MAX_ENTRIES];

    /** Occupied slots, which is the ceiling's live count. An expired slot is swept before it is reused. */
    u32 count;

    u64 ttl_s;

    /** Made at init from the caller's arena. Null when init was never called, which nya_mutex_lock allows. */
    NYA_Mutex* mutex;

    b8 registered;
} _NYA_HttpIdempotencyStore;

/** One server per process, so one store. Zeroed, so the layer works before init at the cost of no lock. */
NYA_INTERNAL _NYA_HttpIdempotencyStore _NYA_HTTP_IDEMPOTENCY = { 0 };

// PRIVATE API DECLARATION

/** The store's TTL, resolving the zero default. Read rather than the field, so the default lives in one place. */
NYA_INTERNAL u64 _nya_http_idempotency_ttl_s(void) __attr_no_discard;

/** Whether `entry` has lived past the TTL as of `now_s`. An empty entry is never expired. */
NYA_INTERNAL b8 _nya_http_idempotency_expired(const _NYA_HttpIdempotencyEntry* entry, u64 now_s) __attr_no_discard;

/** Whether `key` is a header this layer will store: printable, non-empty, and inside the bound. */
NYA_INTERNAL b8 _nya_http_idempotency_key_valid(NYA_ConstCString key, u64 size) __attr_no_discard;

/** The 128-bit fingerprint of a request, so a key reused for a different payload is caught rather than served. */
NYA_INTERNAL void _nya_http_idempotency_fingerprint(const NYA_HttpRequest* request, OUT u8* out_fingerprint);

/** Clears every expired slot to _EMPTY, decrementing the count. Called before a reservation needs a slot. */
NYA_INTERNAL void _nya_http_idempotency_sweep(u64 now_s);

/** The live (non-expired) entry for `key`, or null. Asking makes nothing; an expired match reads as absent. */
NYA_INTERNAL _NYA_HttpIdempotencyEntry* _nya_http_idempotency_find(NYA_ConstCString key, u64 now_s) __attr_no_discard;

/** A slot for `key`, marked in-flight. Null only when every slot is a live in-flight request. */
NYA_INTERNAL _NYA_HttpIdempotencyEntry* _nya_http_idempotency_reserve(NYA_ConstCString key, const u8* fingerprint, u64 now_s) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error _nya_http_idempotency_init(NYA_Arena* arena, NYA_HttpIdempotencyOptions options) {
    nya_assert(arena != nullptr);

    _NYA_HTTP_IDEMPOTENCY.ttl_s = options.ttl_s;

    // The lock is made once and kept: a second init reloads the TTL, not the store; a second mutex would leak the first and split threads across two.
    if (_NYA_HTTP_IDEMPOTENCY.mutex == nullptr) {
        NYA_TRY(nya_mutex_create(arena, &_NYA_HTTP_IDEMPOTENCY.mutex));
    }

    if (!_NYA_HTTP_IDEMPOTENCY.registered) {
        nya_ceiling_register("http_idempotency", NYA_HTTP_IDEMPOTENCY_MAX_ENTRIES, &_NYA_HTTP_IDEMPOTENCY.count);
        _NYA_HTTP_IDEMPOTENCY.registered = true;
    }

    return NYA_OK;
}

NYA_HttpStatus nya_http_layer_idempotency(NYA_HttpExchange* exchange, NYA_HttpChain* next) {
    nya_assert(exchange != nullptr && exchange->request != nullptr && exchange->response != nullptr);

    // A safe method is already a repeat of itself, so there is nothing to dedupe: straight through.
    if (nya_http_method_is_safe(exchange->request->method)) return nya_http_chain_next(exchange, next);

    NYA_ConstCString key = nya_http_request_header(exchange->request, "idempotency-key");

    // No key, or an empty one, is a client that did not ask for this: straight through, like a safe method.
    if (key == nullptr || key[0] == '\0') return nya_http_chain_next(exchange, next);

    u64 key_size = strnlen(key, NYA_HTTP_IDEMPOTENCY_MAX_KEY);

    // A garbage or oversized key is a malformed header, refused rather than stored under a truncated name.
    if (!_nya_http_idempotency_key_valid(key, key_size)) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_BAD_REQUEST, "the Idempotency-Key is empty, too long, or not printable");
    }

    u64 now_s = exchange->now_s;

    u8 fingerprint[NYA_HTTP_IDEMPOTENCY_FINGERPRINT_BYTES] = { 0 };
    _nya_http_idempotency_fingerprint(exchange->request, fingerprint);

    nya_mutex_lock(_NYA_HTTP_IDEMPOTENCY.mutex);

    _NYA_HttpIdempotencyEntry* entry = _nya_http_idempotency_find(key, now_s);

    if (entry != nullptr) {
        // A duplicate that arrived while the first is still running. Refused, not run: that is the point.
        if (entry->state == _NYA_HTTP_IDEMPOTENCY_IN_FLIGHT) {
            nya_mutex_unlock(_NYA_HTTP_IDEMPOTENCY.mutex);
            return nya_http_response_problem(exchange, NYA_HTTP_STATUS_CONFLICT, "a request with this Idempotency-Key is already in flight");
        }

        // Same key, different request. Serving the first answer would answer a question this did not ask.
        if (nya_memcmp(entry->fingerprint, fingerprint, sizeof(fingerprint)) != 0) {
            nya_mutex_unlock(_NYA_HTTP_IDEMPOTENCY.mutex);
            return nya_http_response_problem(exchange, NYA_HTTP_STATUS_UNPROCESSABLE, "this Idempotency-Key was already used for a different request");
        }

        // A completed match: replayed. Copied out under the lock and written after it, so the store isn't held across response calls and the entry can't move under the copy.
        NYA_HttpStatus    status = entry->status;
        NYA_HttpMediaType media  = entry->media;
        u8                body[NYA_HTTP_IDEMPOTENCY_MAX_BODY_BYTES];
        u64               body_size = entry->body_size;

        nya_memcpy(body, entry->body, body_size);

        nya_mutex_unlock(_NYA_HTTP_IDEMPOTENCY.mutex);

        nya_http_response_reset(exchange->response);

        // A body that no longer fits is a bug in this file, not the caller's; 500 is the honest answer.
        if (!nya_http_response_bytes(exchange->response, body, body_size, media).ok) {
            return nya_http_response_problem(exchange, NYA_HTTP_STATUS_INTERNAL_ERROR, "the stored answer could not be replayed");
        }

        // A losable header: a client ignoring it still gets the right answer, one reading it learns the write didn't happen twice; never the reason a replay fails.
        (void)nya_http_response_header(exchange->response, "Idempotency-Replayed", "true");

        return status;
    }

    // First time for this key: reserve it in-flight so a concurrent duplicate sees the 409 above.
    entry = _nya_http_idempotency_reserve(key, fingerprint, now_s);

    // Every slot is a live in-flight request. Rather than refuse a new key (making the layer the outage it prevents), this one is untracked and runs like any un-keyed request.
    if (entry == nullptr) {
        nya_mutex_unlock(_NYA_HTTP_IDEMPOTENCY.mutex);
        return nya_http_chain_next(exchange, next);
    }

    nya_mutex_unlock(_NYA_HTTP_IDEMPOTENCY.mutex);

    // The chain runs with no lock held, so handlers are as concurrent as they were without this layer.
    NYA_HttpStatus status = nya_http_chain_next(exchange, next);

    nya_mutex_lock(_NYA_HTTP_IDEMPOTENCY.mutex);

    // Re-find under the lock: an in-flight entry is never evicted, so it's still ours unless it expired while the handler ran, in which case there's nothing to complete.
    entry = _nya_http_idempotency_find(key, now_s);

    if (entry != nullptr && entry->state == _NYA_HTTP_IDEMPOTENCY_IN_FLIGHT) {
        if (exchange->response->body_size <= sizeof(entry->body)) {
            entry->state     = _NYA_HTTP_IDEMPOTENCY_COMPLETED;
            entry->status    = status;
            entry->media     = exchange->response->media_type;
            entry->body_size = exchange->response->body_size;
            nya_memcpy(entry->body, exchange->response->body, exchange->response->body_size);
            entry->updated_at_s = now_s;
        } else {
            // Too large to store, so the reservation is dropped and a retry re-runs rather than replaying a body cut where nobody decided to. See the header note.
            entry->state = _NYA_HTTP_IDEMPOTENCY_EMPTY;
            if (_NYA_HTTP_IDEMPOTENCY.count > 0) _NYA_HTTP_IDEMPOTENCY.count--;
        }
    }

    nya_mutex_unlock(_NYA_HTTP_IDEMPOTENCY.mutex);

    return status;
}

void nya_http_idempotency_deinit(void) {
    // The lock goes back to its arena, so the store is again the zeroed table it was before init; registered stays true, since nya_ceiling_register holds the count's address, which doesn't move.
    nya_mutex_destroy(_NYA_HTTP_IDEMPOTENCY.mutex);
    _NYA_HTTP_IDEMPOTENCY.mutex = nullptr;

    nya_memset(_NYA_HTTP_IDEMPOTENCY.entries, 0, sizeof(_NYA_HTTP_IDEMPOTENCY.entries));
    _NYA_HTTP_IDEMPOTENCY.count = 0;
    _NYA_HTTP_IDEMPOTENCY.ttl_s = 0;
}

void nya_http_idempotency_reset(void) {
    nya_mutex_lock(_NYA_HTTP_IDEMPOTENCY.mutex);

    nya_memset(_NYA_HTTP_IDEMPOTENCY.entries, 0, sizeof(_NYA_HTTP_IDEMPOTENCY.entries));
    _NYA_HTTP_IDEMPOTENCY.count = 0;

    nya_mutex_unlock(_NYA_HTTP_IDEMPOTENCY.mutex);
}

u32 nya_http_idempotency_count(void) {
    nya_mutex_lock(_NYA_HTTP_IDEMPOTENCY.mutex);
    u32 count = _NYA_HTTP_IDEMPOTENCY.count;
    nya_mutex_unlock(_NYA_HTTP_IDEMPOTENCY.mutex);

    return count;
}

// PRIVATE API IMPLEMENTATION

u64 _nya_http_idempotency_ttl_s(void) {
    return _NYA_HTTP_IDEMPOTENCY.ttl_s > 0 ? _NYA_HTTP_IDEMPOTENCY.ttl_s : NYA_HTTP_IDEMPOTENCY_TTL_S;
}

b8 _nya_http_idempotency_expired(const _NYA_HttpIdempotencyEntry* entry, u64 now_s) {
    if (entry->state == _NYA_HTTP_IDEMPOTENCY_EMPTY) return false;

    // now_s before the stamp is a clock that went backwards, which is not expiry; treated as still live.
    return now_s > entry->updated_at_s && now_s - entry->updated_at_s >= _nya_http_idempotency_ttl_s();
}

b8 _nya_http_idempotency_key_valid(NYA_ConstCString key, u64 size) {
    // strnlen stopped at the bound, so a value that filled it whole has no terminator inside range.
    if (size == 0 || size >= NYA_HTTP_IDEMPOTENCY_MAX_KEY) return false;

    // Printable ASCII, no spaces: a key is a token a client echoes back, not free text; a space or control byte is a header this server didn't send and won't store.
    for (u64 index = 0; index < size; index++) {
        if (key[index] < 0x21 || key[index] > 0x7E) return false;
    }

    return true;
}

void _nya_http_idempotency_fingerprint(const NYA_HttpRequest* request, OUT u8* out_fingerprint) {
    // The body first, into its own digest, so a body of any size folds down to a fixed field below.
    u8 body_digest[NYA_HTTP_IDEMPOTENCY_FINGERPRINT_BYTES] = { 0 };
    nya_crypto_blake2b(request->body, request->body_size, body_digest, sizeof(body_digest));

    // Then method and path over that digest, so two requests differing in any of the three differ here; path is bounded by NYA_HTTP_MAX_PATH and method is one byte, so this buffer is fixed.
    u8  material[1 + NYA_HTTP_MAX_PATH + NYA_HTTP_IDEMPOTENCY_FINGERPRINT_BYTES] = { 0 };
    u64 used                                                                     = 0;

    material[used++] = (u8)request->method;

    u64 path_size = strnlen(request->path, NYA_HTTP_MAX_PATH);
    nya_memcpy(material + used, request->path, path_size);
    used += path_size;

    nya_memcpy(material + used, body_digest, sizeof(body_digest));
    used += sizeof(body_digest);

    nya_crypto_blake2b(material, used, out_fingerprint, NYA_HTTP_IDEMPOTENCY_FINGERPRINT_BYTES);
}

void _nya_http_idempotency_sweep(u64 now_s) {
    for (u32 index = 0; index < NYA_HTTP_IDEMPOTENCY_MAX_ENTRIES; index++) {
        _NYA_HttpIdempotencyEntry* entry = &_NYA_HTTP_IDEMPOTENCY.entries[index];

        if (_nya_http_idempotency_expired(entry, now_s)) {
            nya_memset(entry, 0, sizeof(*entry));
            if (_NYA_HTTP_IDEMPOTENCY.count > 0) _NYA_HTTP_IDEMPOTENCY.count--;
        }
    }
}

_NYA_HttpIdempotencyEntry* _nya_http_idempotency_find(NYA_ConstCString key, u64 now_s) {
    for (u32 index = 0; index < NYA_HTTP_IDEMPOTENCY_MAX_ENTRIES; index++) {
        _NYA_HttpIdempotencyEntry* entry = &_NYA_HTTP_IDEMPOTENCY.entries[index];

        if (entry->state == _NYA_HTTP_IDEMPOTENCY_EMPTY) continue;

        // An expired match reads as absent: the key is free to reserve again and a stale answer is never replayed; the slot is reclaimed by the sweep in _reserve, not here.
        if (_nya_http_idempotency_expired(entry, now_s)) continue;

        if (strcmp(entry->key, key) == 0) return entry;
    }

    return nullptr;
}

_NYA_HttpIdempotencyEntry* _nya_http_idempotency_reserve(NYA_ConstCString key, const u8* fingerprint, u64 now_s) {
    // Reclaim expired slots first, so the count is honest and a full-of-stale table takes a new key.
    _nya_http_idempotency_sweep(now_s);

    _NYA_HttpIdempotencyEntry* slot = nullptr;

    if (_NYA_HTTP_IDEMPOTENCY.count < NYA_HTTP_IDEMPOTENCY_MAX_ENTRIES) {
        // A free slot exists; take the first empty one.
        for (u32 index = 0; index < NYA_HTTP_IDEMPOTENCY_MAX_ENTRIES; index++) {
            if (_NYA_HTTP_IDEMPOTENCY.entries[index].state == _NYA_HTTP_IDEMPOTENCY_EMPTY) {
                slot = &_NYA_HTTP_IDEMPOTENCY.entries[index];
                break;
            }
        }

        _NYA_HTTP_IDEMPOTENCY.count++;
    } else {
        // Full of live entries. Only a completed one may be given up (an in-flight entry is the request this layer guards), and among those the oldest, as base_circuit.h chooses.
        for (u32 index = 0; index < NYA_HTTP_IDEMPOTENCY_MAX_ENTRIES; index++) {
            _NYA_HttpIdempotencyEntry* candidate = &_NYA_HTTP_IDEMPOTENCY.entries[index];

            if (candidate->state != _NYA_HTTP_IDEMPOTENCY_COMPLETED) continue;
            if (slot == nullptr || candidate->updated_at_s < slot->updated_at_s) slot = candidate;
        }

        // Every slot is a live in-flight request: nothing safe to reuse, so the caller passes through.
        if (slot == nullptr) return nullptr;
    }

    nya_memset(slot, 0, sizeof(*slot));

    (void)snprintf(slot->key, sizeof(slot->key), "%s", key);
    slot->state = _NYA_HTTP_IDEMPOTENCY_IN_FLIGHT;
    nya_memcpy(slot->fingerprint, fingerprint, NYA_HTTP_IDEMPOTENCY_FINGERPRINT_BYTES);
    slot->updated_at_s = now_s;

    return slot;
}

/**
 * @file http_keyring.h
 *
 * The keys a server seals with, rotated on their own so nobody has to think about them.
 *
 * ```c
 * // once, at start: load whatever was saved, or start empty
 * NYA_HttpKeyring ring = saved_ring_or_zero();
 *
 * // on a timer, and at start: mint a key when one is due, drop the expired ones
 * if (nya_http_keyring_rotate(&ring)) save_ring(&ring);   // true means it changed; persist it
 *
 * // sealing always uses the newest key
 * char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
 * NYA_TRY(nya_http_keyring_seal(&ring, "session", (const u8*)&who, sizeof(who), 900, token, sizeof(token)));
 *
 * // opening tries the newest, then the ones it replaced, so a token from before a rotation still opens
 * Who who = { 0 };
 * u64 size = 0;
 * if (nya_http_keyring_unseal(&ring, "session", cookie.text, cookie.size, (u8*)&who, sizeof(who), &size)) serve(&who);
 * ```
 *
 * ── why a ring and not a key ──
 *
 * A single sealing key has a bad choice built into it: rotate it and every token in the wild stops
 * opening at once — everybody logged out — or never rotate it and a key that leaks is a key that opens
 * tokens forever. A ring removes the choice. New tokens are sealed with the newest key; an old token is
 * opened by whichever key sealed it, for as long as that key has not expired. Rotation is then just
 * minting a new newest key, and it costs nobody their session.
 *
 * This is the one part of a login the user is promised never to notice: a key rotates under them, their
 * cookie keeps working, and the day it finally stops the worst that happens is one more sign-in.
 *
 * ── how long a key lives ──
 *
 * A key signs for NYA_HTTP_KEYRING_ROTATE_S and then verifies for as long again, so it outlives every
 * token it ever sealed — a token's own expiry is shorter than the tail. Concretely: mint at day zero,
 * stop sealing with it at day one, keep opening its tokens until day two, forget it. There is always at
 * least one key sealing and usually two that verify, which is the overlap that makes a rotation seamless.
 *
 * ── the keys are secret, and persisting them is the program's call ──
 *
 * A NYA_HttpKeyring is plain bytes a program may write wherever it keeps secrets — an encrypted
 * database, a file only the service can read. It is not written for the program, because where a secret
 * is safe to keep is a decision the engine cannot make: a ring saved to a world-readable file is a ring
 * that leaks. A program with nowhere safe keeps it in memory and accepts that a restart logs everyone
 * out, which is a real and sometimes correct choice.
 *
 * Thread safety: none. One thread rotates and seals, as with the rest of the module; a server that
 * seals on workers copies the ring to them and rotates on the thread that owns it.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/http/http_seal.h"

// CONSTANTS

/** Bytes of one key. Thirty two, which is what the seal derives its AEAD key from. */
#define NYA_HTTP_KEYRING_KEY_BYTES 32

/**
 * Keys a ring holds at once.
 *
 * Four is more than the two a steady rotation needs, so a burst of rotations — a clock that jumped, a
 * restart loop — cannot push a still-valid key out before its tokens have expired. The oldest past this
 * is dropped, which is only ever a key that was about to expire anyway.
 * */
#define NYA_HTTP_KEYRING_MAX_KEYS 4

/**
 * How long a key seals before the next one takes over, in seconds.
 *
 * A day. Paired with the verify tail below it means a key is useful for two days and sealed with for
 * one, which is a rotation a leaked key survives for at most a day and a user never feels.
 * */
#define NYA_HTTP_KEYRING_ROTATE_S (24ULL * 60 * 60)

/**
 * How long a key keeps verifying after it stops sealing, in seconds.
 *
 * As long again as it sealed, so it outlives every token it made — a stateless token's own life is
 * shorter than this by construction, which is what lets a token be trusted with no row behind it.
 * */
#define NYA_HTTP_KEYRING_VERIFY_TAIL_S NYA_HTTP_KEYRING_ROTATE_S

// TYPES

typedef struct NYA_HttpKey     NYA_HttpKey;
typedef struct NYA_HttpKeyring NYA_HttpKeyring;

/** One key and the two dates that decide what it may still do. */
struct NYA_HttpKey {
    u8 material[NYA_HTTP_KEYRING_KEY_BYTES]; // @redact

    /** Seconds since the epoch: when it was minted, and after which it verifies nothing. */
    u64 created_at_s;
    u64 expires_at_s;
};

/**
 * A ring of keys, newest first.
 *
 * Plain bytes on purpose: a program saves and loads it as it sees fit; see the header. A zeroed ring is
 * a valid empty one, which nya_http_keyring_rotate fills with a first key.
 * */
struct NYA_HttpKeyring {
    NYA_HttpKey keys[NYA_HTTP_KEYRING_MAX_KEYS];
    u32         key_count;
};

// FUNCTIONS

/**
 * Drops every expired key and mints a new newest one when the current key is due, or when there is none.
 *
 * Call it at start and on a timer — hourly is plenty, since the rotation is daily. Returns whether the
 * ring changed, which is exactly when a program that persists the ring has to save it again; a call
 * that changed nothing costs a comparison and no write.
 *
 * Idempotent within a rotation window: called twice a minute apart with no key due, the second does
 * nothing.
 * */
NYA_API b8 nya_http_keyring_rotate(NYA_HttpKeyring* ring) __attr_no_discard;

/**
 * Seals `plaintext` with the ring's newest key, exactly as nya_http_seal does with a bare secret.
 *
 * NYA_ERROR_NOT_OK on an empty ring, which is a ring nya_http_keyring_rotate has not been called on
 * yet — a program's start-up bug rather than a runtime state, so it is loud.
 * */
NYA_API NYA_Error nya_http_keyring_seal(
    const NYA_HttpKeyring* ring, NYA_ConstCString label, const u8* plaintext, u64 plaintext_size, u64 ttl_s, OUT char* out_token, u64 capacity
) __attr_no_discard;

/**
 * Opens `token` with whichever of the ring's keys sealed it, newest first.
 *
 * The whole point: a token sealed before the last rotation still opens, because the key that sealed it
 * is still on the ring. False when no key opens it — tampered, expired, sealed under another label, or
 * sealed by a key that has since aged off the ring.
 * */
NYA_API b8 nya_http_keyring_unseal(
    const NYA_HttpKeyring* ring, NYA_ConstCString label, const char* token, u64 token_size, OUT u8* out_plaintext, u64 capacity, OUT u64* out_size
) __attr_no_discard;

/** How many keys the ring holds, for a status page and for the ceiling audit. */
NYA_API u32 nya_http_keyring_count(const NYA_HttpKeyring* ring) __attr_no_discard;

/** Wipes every key. What a program calls on shutdown so no key is left in a freed page. */
NYA_API void nya_http_keyring_wipe(NYA_HttpKeyring* ring);

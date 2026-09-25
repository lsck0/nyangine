/**
 * The keyring as laws: unseal tries every key that has not expired and only those, so a token opens for
 * exactly as long as the key that sealed it is on the ring; prune keeps exactly the non-expired keys and
 * leaves them newest-first; and rotate is idempotent inside a window and mints one key when a key is due.
 *
 * The example walk — a single rotation, a single expiry — is test_keyring.c. This is the part examples
 * cannot reach: the same guarantees over rings of every size, with keys expiring in every combination.
 *
 * The rotation window is a day, which no test can wait out, so the dates on the keys are set by hand and
 * prune is called with the `now_s` a law chose. A token's own ttl is always well in the future here, so
 * whether it opens turns only on which key is present — which is the thing under test.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#define CASES 3000

/** "keyring." in ASCII. Fixed, so every run is the same run. */
#define SEED 0x6B657972696E672EULL

/* HELPERS */

/** A clock value far from the epoch, so a key can be dated before or after it without underflow. */
#define NOW_BASE_S (1000000000ULL)

/** Fills a ring with `count` keys of drawn material, newest-first, each valid well past `now_s`. */
static void draw_ring(NYA_Property* property, OUT NYA_HttpKeyring* ring, u32 count, u64 now_s) {
    nya_memset(ring, 0, sizeof(*ring));

    for (u32 index = 0; index < count; index++) {
        nya_property_draw_bytes(property, ring->keys[index].material, NYA_HTTP_KEYRING_KEY_BYTES);

        // Newest first: index 0 is the youngest. The exact dates only have to keep every key valid; the laws that care about expiry set the dates they need themselves.
        ring->keys[index].created_at_s = now_s - (u64)index * NYA_HTTP_KEYRING_ROTATE_S;
        ring->keys[index].expires_at_s = now_s + NYA_HTTP_KEYRING_VERIFY_TAIL_S;
    }

    ring->key_count = count;
}

/* LAWS */

/**
 * A token sealed by any key on the ring opens through the ring, whatever that key's position; and once
 * that one key is expired and pruned away, the very same token no longer opens — the key is the whole of
 * the trust, and unseal tries all of them and only the live ones.
 * */
static b8 law_opens_until_its_key_is_gone(NYA_Property* property) {
    u64 now_s = NOW_BASE_S;

    u32 count = 1 + (u32)nya_property_draw_below(property, NYA_HTTP_KEYRING_MAX_KEYS);

    NYA_HttpKeyring ring = { 0 };
    draw_ring(property, &ring, count, now_s);

    // Seal under the key at a drawn index, using that key's own material directly. The token's ttl is far in the future, so it never expires on its own during the test.
    u32 sealer = (u32)nya_property_draw_below(property, count);

    u64  who                            = 0xC0FFEEULL + sealer;
    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    if (!nya_http_seal(ring.keys[sealer].material, NYA_HTTP_KEYRING_KEY_BYTES, "session", (const u8*)&who, sizeof(who), 3600, token, sizeof(token)).ok) {
        return false;
    }

    u64 back = 0;
    u64 size = 0;

    // The whole point of a ring: the sealing key is somewhere on it, so unseal finds it.
    if (!nya_http_keyring_unseal(&ring, "session", token, strlen(token), (u8*)&back, sizeof(back), &size)) {
        nya_property_note(property, "a token sealed by key %u of %u did not open through the ring", sealer, count);
        return false;
    }
    if (back != who || size != sizeof(who)) {
        nya_property_note(property, "the ring opened the token to the wrong bytes");
        return false;
    }

    // Now expire exactly the key that sealed it and prune. Every other key stays; this one goes.
    ring.keys[sealer].expires_at_s = now_s;

    u32 removed = _nya_http_keyring_prune(&ring, now_s);
    if (removed != 1) {
        nya_property_note(property, "expiring one key pruned %u", removed);
        return false;
    }

    b8 still_opens = nya_http_keyring_unseal(&ring, "session", token, strlen(token), (u8*)&back, sizeof(back), &size);

    nya_property_note(property, "a token whose key (index %u of %u) aged off the ring still opened", sealer, count);
    return !still_opens && size == 0;
}

/**
 * Prune keeps exactly the keys whose verify life has not ended, and keeps them in the order they were
 * in: a survivor is younger than another survivor after the prune iff it was before.
 * */
static b8 law_prune_keeps_exactly_the_live(NYA_Property* property) {
    u64 now_s = NOW_BASE_S;

    u32 count = 1 + (u32)nya_property_draw_below(property, NYA_HTTP_KEYRING_MAX_KEYS);

    NYA_HttpKeyring ring = { 0 };
    draw_ring(property, &ring, count, now_s);

    // Decide per key whether it has expired, and remember the material of the ones that should survive, in order, so the order can be checked after.
    u8  expected[NYA_HTTP_KEYRING_MAX_KEYS][NYA_HTTP_KEYRING_KEY_BYTES] = { 0 };
    u32 expected_count                                                 = 0;

    for (u32 index = 0; index < count; index++) {
        b8 expired = nya_property_draw_bool(property, 40);

        if (expired) {
            // At or before now is expired, since prune drops on `now >= expires`.
            ring.keys[index].expires_at_s = now_s - (u64)nya_property_draw_below(property, 1000);
        } else {
            ring.keys[index].expires_at_s = now_s + 1 + (u64)nya_property_draw_below(property, 1000);
            nya_memcpy(expected[expected_count], ring.keys[index].material, NYA_HTTP_KEYRING_KEY_BYTES);
            expected_count++;
        }
    }

    u32 removed = _nya_http_keyring_prune(&ring, now_s);

    if (ring.key_count != expected_count) {
        nya_property_note(property, "prune left %u keys, expected %u", ring.key_count, expected_count);
        return false;
    }
    if (removed != count - expected_count) {
        nya_property_note(property, "prune reported %u removed, expected %u", removed, count - expected_count);
        return false;
    }

    for (u32 index = 0; index < expected_count; index++) {
        if (ring.keys[index].expires_at_s <= now_s) {
            nya_property_note(property, "a pruned ring kept an expired key at %u", index);
            return false;
        }
        if (memcmp(ring.keys[index].material, expected[index], NYA_HTTP_KEYRING_KEY_BYTES) != 0) {
            nya_property_note(property, "prune reordered the survivors at %u", index);
            return false;
        }
    }

    return true;
}

/**
 * Rotate does nothing a second time inside a window, and mints exactly one key when the newest is due —
 * never growing the ring past its bound.
 * */
static b8 law_rotate_is_idempotent_then_due(NYA_Property* property) {
    // A ring the law owns; rotate reads the real clock and real randomness, which changes the key bytes but never whether these counts hold.
    NYA_HttpKeyring ring = { 0 };

    if (!nya_http_keyring_rotate(&ring)) {
        nya_property_note(property, "the first rotate of an empty ring minted nothing");
        return false;
    }

    u32 after_first = nya_http_keyring_count(&ring);
    if (after_first != 1) {
        nya_property_note(property, "the first rotate left %u keys", after_first);
        return false;
    }

    // Idempotent inside the window: nothing is due, so a second call changes nothing. Drawn repeats, so the property is that it holds however many times it is asked.
    u32 repeats = 1 + (u32)nya_property_draw_below(property, 5);
    for (u32 index = 0; index < repeats; index++) {
        if (nya_http_keyring_rotate(&ring)) {
            nya_property_note(property, "a rotate inside the window reported a change on repeat %u", index);
            return false;
        }
    }
    if (nya_http_keyring_count(&ring) != 1) {
        nya_property_note(property, "idempotent rotates changed the count to %u", nya_http_keyring_count(&ring));
        return false;
    }

    // Now force a run of due rotations by aging the newest key before each, and check the ring fills to its bound and never past it.
    u32 rolls = 1 + (u32)nya_property_draw_below(property, NYA_HTTP_KEYRING_MAX_KEYS + 4);
    for (u32 index = 0; index < rolls; index++) {
        u32 before = nya_http_keyring_count(&ring);

        ring.keys[0].created_at_s -= NYA_HTTP_KEYRING_ROTATE_S + 1;

        if (!nya_http_keyring_rotate(&ring)) {
            nya_property_note(property, "a due rotate on roll %u minted nothing", index);
            return false;
        }

        u32 now = nya_http_keyring_count(&ring);
        u32 want = before < NYA_HTTP_KEYRING_MAX_KEYS ? before + 1 : NYA_HTTP_KEYRING_MAX_KEYS;

        if (now != want) {
            nya_property_note(property, "roll %u left %u keys, expected %u", index, now, want);
            return false;
        }
    }

    return nya_http_keyring_count(&ring) <= NYA_HTTP_KEYRING_MAX_KEYS;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    u32 failures = 0;

    failures += nya_property_check("a token opens until its key ages off the ring, never after", CASES, SEED, law_opens_until_its_key_is_gone);
    failures += nya_property_check("prune keeps exactly the live keys, newest-first", CASES, SEED, law_prune_keeps_exactly_the_live);
    failures += nya_property_check("rotate is idempotent in a window and bounded when due", CASES, SEED, law_rotate_is_idempotent_then_due);

    return failures == 0 ? 0 : 1;
}

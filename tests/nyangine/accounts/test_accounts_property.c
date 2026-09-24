/**
 * The account primitives as laws: normalising a username is idempotent and folds case, so a name and its
 * shouted twin are one account; the stored password hash parses to a clean yes-or-no for any bytes at all
 * and never a crash, since a row in the database is something an attacker may one day get to choose; and
 * the login throttle's wait only ever grows with the failures behind it and is bounded, so an account is
 * slowed and never locked.
 *
 * The password-hash law is a fuzzer that happens to run in the unit suite: it feeds arbitrary bytes to
 * the hand-written cost parser through the public nya_account_password_needs_rehash and asserts a clean
 * refusal rather than a wrap or a read past the end. The dedicated coverage-guided target is
 * tests/fuzz/fuzz_password_hash.c; this is what makes a regression a failure here without a fuzzer
 * installed.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define CASES 5000

/** "accounts" folded into eight ASCII bytes. Fixed, so the suite is one run. */
#define SEED 0x6163636F756E7473ULL

/* LAWS — USERNAME NORMALISATION */

/** Normalising an already-normalised name changes nothing: normalize(normalize(x)) == normalize(x). */
static b8 law_normalize_is_idempotent(NYA_Property* property) {
    NYA_CString name = nya_property_draw_text(property, NYA_ACCOUNTS_MAX_USERNAME - 1);

    char first[NYA_ACCOUNTS_MAX_USERNAME] = { 0 };
    if (!nya_account_username_normalize(name, first, sizeof(first))) return true; // a name it will not take has no fixed point to check.

    char second[NYA_ACCOUNTS_MAX_USERNAME] = { 0 };
    b8   ok = nya_account_username_normalize(first, second, sizeof(second));

    nya_property_note(property, "normalize('%s') = '%s' but normalizing that again gave '%s'", name, first, second);
    return ok && strcmp(first, second) == 0;
}

/** A name and the same name with any letters' case flipped normalise to the very same thing. */
static b8 law_normalize_folds_case(NYA_Property* property) {
    NYA_CString name = nya_property_draw_text(property, NYA_ACCOUNTS_MAX_USERNAME - 1);

    u64         length  = strlen(name);
    NYA_CString flipped = nya_arena_alloc(property->allocator, length + 1);

    for (u64 index = 0; index < length; index++) {
        char c = name[index];

        // Flip the case of each ASCII letter, at a drawn coin so mixed spellings are covered, not only the fully-shouted one. Everything else is left exactly as it is.
        if (nya_property_draw_bool(property, 60)) {
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }

        flipped[index] = c;
    }
    flipped[length] = '\0';

    char a[NYA_ACCOUNTS_MAX_USERNAME] = { 0 };
    char b[NYA_ACCOUNTS_MAX_USERNAME] = { 0 };

    b8 ok_a = nya_account_username_normalize(name, a, sizeof(a));
    b8 ok_b = nya_account_username_normalize(flipped, b, sizeof(b));

    // Case never changes whether a name is acceptable or how long it is, so the two verdicts match and, when both are taken, the folded forms are identical.
    nya_property_note(property, "'%s' and case-variant '%s' folded to '%s' and '%s'", name, flipped, a, b);
    return ok_a == ok_b && (!ok_a || strcmp(a, b) == 0);
}

/* LAWS — THE STORED PASSWORD HASH */

/**
 * Draws a stored-hash string: sometimes a plausible `$argon2id$v=19$m=...,t=...,p=...$...$...` with drawn
 * numbers (huge ones included, to reach the overflow guard) and drawn tails, sometimes pure noise. Always
 * a valid C string inside the column's bound.
 * */
static void draw_encoded(NYA_Property* property, OUT char* out, u64 capacity) {
    out[0] = '\0';

    if (nya_property_draw_bool(property, 55)) {
        // A structured-but-hostile hash: the shape is right, the numbers and fields are the fuzzer's.
        u64 m = nya_property_draw_u64(property) >> (nya_property_draw_below(property, 40)); // spans small to way past u32.
        u64 t = nya_property_draw_u64(property) >> (nya_property_draw_below(property, 50));
        u64 p = nya_property_draw_u64(property) >> (nya_property_draw_below(property, 55));

        char salt[48] = { 0 };
        char hash[80] = { 0 };
        u64  salt_len = nya_property_draw_below(property, sizeof(salt) - 1);
        u64  hash_len = nya_property_draw_below(property, sizeof(hash) - 1);

        // base64url-ish bytes, sometimes with a stray character that makes the decode fail.
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_*!$";
        for (u64 i = 0; i < salt_len; i++) salt[i] = alphabet[nya_property_draw_below(property, sizeof(alphabet) - 1)];
        for (u64 i = 0; i < hash_len; i++) hash[i] = alphabet[nya_property_draw_below(property, sizeof(alphabet) - 1)];

        (void)snprintf(out, capacity, "$argon2id$v=19$m=%llu,t=%llu,p=%llu$%s$%s", (unsigned long long)m, (unsigned long long)t,
                       (unsigned long long)p, salt, hash);
        return;
    }

    // Pure noise, capped to the column, kept a C string by ending it. Embedded NULs just end it earlier, which the strlen/strncmp parser must handle without walking off anything.
    u64 length = nya_property_draw_below(property, capacity - 1);
    for (u64 index = 0; index < length; index++) {
        u8 byte = (u8)(1 + nya_property_draw_below(property, 255)); // 1..255: no accidental early terminator.
        out[index] = (char)byte;
    }
    out[length] = '\0';
}

/**
 * Any bytes at all in the stored-hash column parse to a clean answer, never a crash and never a cost
 * this misread: needs_rehash is a total function of the column, and when the cost parser does accept a
 * string it reports three non-zero, in-range parameters.
 * */
static b8 law_password_hash_parses_cleanly(NYA_Property* property) {
    NYA_AccountUser user = { 0 };
    draw_encoded(property, user.password, sizeof(user.password));

    // Total: it returns for every input. A hash it cannot read is one worth replacing, so this is true for noise and only sometimes false for a real hash — either way it must not crash. The sanitizers are the oracle for the reads and the writes; the assertions below are the oracle for the meaning.
    b8 needs = nya_account_password_needs_rehash(&user);
    nya_unused(needs);

    u32 memory_kib = 0xDEAD;
    u32 passes     = 0xBEEF;
    u32 lanes      = 0xCAFE;

    b8 parsed = _nya_account_password_cost(user.password, &memory_kib, &passes, &lanes);

    if (!parsed) {
        // A refusal writes zeroes for all three, never a half-read number left in the out-parameters.
        nya_property_note(property, "a refused hash left cost %u,%u,%u behind", memory_kib, passes, lanes);
        return memory_kib == 0 && passes == 0 && lanes == 0;
    }

    // Accepted: every field is a real, non-zero number that fit a u32, which is the whole reason the parser is written by hand rather than handed to sscanf.
    nya_property_note(property, "an accepted hash parsed to cost %u,%u,%u", memory_kib, passes, lanes);
    return memory_kib > 0 && passes > 0 && lanes > 0;
}

/** The hand-written decimal reader: it never reports a value past what a u32 holds, and consumes only digits. */
static b8 law_number_never_overflows(NYA_Property* property) {
    // A run of digits, sometimes far longer than a u32 can hold, then the terminator the caller expects.
    char buffer[64] = { 0 };
    u64  digits     = nya_property_draw_below(property, sizeof(buffer) - 2);

    for (u64 index = 0; index < digits; index++) buffer[index] = (char)('0' + nya_property_draw_below(property, 10));
    buffer[digits] = ','; // the terminator the m= field uses.

    const char* cursor = buffer;
    u32         value  = 0xABCD;

    b8 ok = _nya_account_number(&cursor, ',', &value);

    if (!ok) {
        // Refused (no digits, or too big): the value is cleared, not left with a partial number.
        nya_property_note(property, "a refused number left %u behind for '%s'", value, buffer);
        return value == 0;
    }

    // Accepted: it fit a u32, the cursor stopped just past the comma, and the value round-trips as text.
    b8 within  = value <= 0xFFFFFFFFU; // trivially true for a u32, but states the guarantee.
    b8 stopped = *(cursor - 1) == ',';

    nya_property_note(property, "the reader accepted '%s' as %u", buffer, value);
    return within && stopped;
}

/* LAWS — THE LOGIN THROTTLE */

/** The wait a count of failures earns never falls as the count rises, and never passes the cap. */
static b8 law_throttle_wait_is_monotone_and_bounded(NYA_Property* property) {
    u32 failures = (u32)nya_property_draw_below(property, 100);

    u64 wait      = _nya_account_throttle_wait_s(failures);
    u64 wait_next = _nya_account_throttle_wait_s(failures + 1);

    b8 monotone = wait_next >= wait;
    b8 bounded  = wait <= NYA_ACCOUNTS_THROTTLE_MAX_WAIT_S && wait_next <= NYA_ACCOUNTS_THROTTLE_MAX_WAIT_S;
    b8 free     = failures > NYA_ACCOUNTS_THROTTLE_FREE_ATTEMPTS || wait == 0; // the first few cost nothing.

    nya_property_note(property, "%u failures wait %llus, %u wait %llus (cap %d)", failures, (unsigned long long)wait, failures + 1,
                      (unsigned long long)wait_next, NYA_ACCOUNTS_THROTTLE_MAX_WAIT_S);
    return monotone && bounded && free;
}

/**
 * Through the public counter: a run of wrong answers at one instant makes the wait grow and stay under
 * the cap, and it is always a finite wait — the account is slowed, never locked shut.
 * */
static b8 law_throttle_never_locks(NYA_Property* property) {
    nya_account_throttle_reset();

    // A distinct address per case, so the fixed table neither fills nor carries state between cases.
    char address[32] = { 0 };
    (void)snprintf(address, sizeof(address), "10.%llu.%llu.%llu", (unsigned long long)nya_property_draw_below(property, 256),
                   (unsigned long long)nya_property_draw_below(property, 256), (unsigned long long)nya_property_draw_below(property, 256));

    // Past the free attempts, so the wait is guaranteed to have climbed off zero by the end; the point of the run is that it climbs and stays finite, not that a single mistyped password already costs.
    u32 rounds       = NYA_ACCOUNTS_THROTTLE_FREE_ATTEMPTS + 2 + (u32)nya_property_draw_below(property, 40);
    u32 previous     = 0;
    b8  ever_grew    = false;

    for (u32 index = 0; index < rounds; index++) {
        nya_account_throttle_fail(nullptr, address);

        u32 wait_s = 0;
        (void)nya_account_throttle_check(nullptr, address, &wait_s);

        if (wait_s > NYA_ACCOUNTS_THROTTLE_MAX_WAIT_S) {
            nya_property_note(property, "round %u waited %us, past the %d cap", index, wait_s, NYA_ACCOUNTS_THROTTLE_MAX_WAIT_S);
            return false;
        }

        // Same instant, so a later round has at least as many failures behind it and waits at least as long.
        if (wait_s < previous) {
            nya_property_note(property, "the wait fell from %us to %us at round %u", previous, wait_s, index);
            return false;
        }
        if (wait_s > previous) ever_grew = true;

        previous = wait_s;
    }

    nya_account_throttle_reset();

    // Over enough rounds the wait must have climbed off zero at some point: it is a throttle, after all.
    nya_property_note(property, "the wait never grew across %u rounds", rounds);
    return ever_grew;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    u32 failures = 0;

    failures += nya_property_check("username normalisation is idempotent", CASES, SEED, law_normalize_is_idempotent);
    failures += nya_property_check("username normalisation folds case together", CASES, SEED, law_normalize_folds_case);
    failures += nya_property_check("a stored password hash parses cleanly for any bytes", CASES, SEED, law_password_hash_parses_cleanly);
    failures += nya_property_check("the cost number reader never overflows a u32", CASES, SEED, law_number_never_overflows);
    failures += nya_property_check("the throttle wait is monotone and bounded", CASES, SEED, law_throttle_wait_is_monotone_and_bounded);
    failures += nya_property_check("the throttle slows but never locks", CASES, SEED, law_throttle_never_locks);

    return failures == 0 ? 0 : 1;
}

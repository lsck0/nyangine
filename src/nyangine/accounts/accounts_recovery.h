/**
 * @file accounts_recovery.h
 *
 * The codes that get somebody back in when the password is gone and there is no email to send one to.
 *
 * ```c
 * // once, when the account is set up or the person asks for new ones
 * char codes[NYA_ACCOUNTS_RECOVERY_CODE_COUNT][NYA_ACCOUNTS_RECOVERY_CODE_TEXT] = { 0 };
 * u32  made = 0;
 * NYA_TRY(nya_account_recovery_generate(arena, user.id, codes, &made));
 * // show them once, tell the person to keep them; they are never shown again
 *
 * // later, on the "I lost my password" screen: a code proves the account is theirs
 * NYA_AccountUser who = { 0 };
 * if (nya_account_recovery_consume(arena, "ada", typed_code, &who).ok) {
 *     nya_account_password_reset(arena, who.id, new_password);   // which ends every session too
 * }
 * ```
 *
 * ── why codes and not email ──
 *
 * Email recovery is a second account's security standing in for this one: whoever holds the inbox holds
 * the account, and a forwarded or breached inbox is a silent takeover. Recovery codes move that trust to
 * the person — a short list they wrote down when they signed up — so this service depends on no mail
 * server, no third party, and no address that can be re-assigned. The roadmap says email is not planned;
 * this is what stands in its place.
 *
 * ── stored the way a session token is, not the way a password is ──
 *
 * What is kept is a SHA-256 of each code, not the code. A recovery code is a secret this program issued
 * from the system random source, so it is long and unguessable, and the reason a token is hashed rather
 * than slowed applies unchanged: there is nothing to brute force, and a database that leaks must not
 * hand over a way back into every account. The plaintext exists for exactly as long as the one call that
 * makes it.
 *
 * ── single use, and all or nothing ──
 *
 * A consumed code is deleted, so the same slip of paper cannot be replayed. Generating a new set
 * replaces the old one entirely: a person who thinks their codes are compromised gets a clean list and
 * every old code stops working at once, which is the whole point of asking for new ones.
 *
 * ── this authenticates, it does not authorize ──
 *
 * A consumed code proves the account is the caller's. What happens next — a forced password reset, a
 * second factor being turned off — is the program's, behind whatever it does for a recovered account.
 * The code is the proof, not the permission.
 *
 * ── the throttle covers it ──
 *
 * A recovery attempt goes through accounts_throttle.h exactly as a password does, keyed on the account
 * and the address, so guessing codes is slowed the same way guessing passwords is. A code is 20 random
 * bits of base32 per group and there are two groups, so guessing is already hopeless; the throttle is
 * there so trying is not free either.
 * */
#pragma once

#include "nyangine/accounts/accounts_user.h"
#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

/** Codes made in one set. Ten, which is what every service that does this settles on. */
#define NYA_ACCOUNTS_RECOVERY_CODE_COUNT 10

/** Bytes of one code as text, terminator included: `XXXXX-XXXXX` and its NUL. */
#define NYA_ACCOUNTS_RECOVERY_CODE_TEXT 12

/** Bytes of entropy behind one code. Five, as two groups of base32: enough that guessing is hopeless. */
#define NYA_ACCOUNTS_RECOVERY_CODE_BYTES 5

// TYPES

typedef struct NYA_AccountRecoveryCode NYA_AccountRecoveryCode;

/**
 * One recovery code, as it is stored: a hash, never the code.
 *
 * Public because the ORM derives its table from the reflection, which the generator only makes for a
 * type it can see in a header; nothing outside the module has a reason to read one.
 * */
// @reflect
struct NYA_AccountRecoveryCode {
    u64 id; // @key

    u64 account_id;

    /** SHA-256 of the canonical code, as hex. What a consume looks up. */
    char code_hash[72]; // @redact

    u64 created_at_s;
};

// FUNCTIONS

/**
 * Makes a fresh set of recovery codes for an account, and answers them once.
 *
 * Replaces any set the account already had, so every old code stops working. `out_codes` is filled with
 * NYA_ACCOUNTS_RECOVERY_CODE_COUNT strings and `out_count` says how many — always the full count on
 * success, the parameter being there so a caller loops without naming the constant.
 *
 * This is the only time the codes exist as text. What is stored is a hash of each; there is no call that
 * reads them back, because there is nothing to read back.
 * */
NYA_API NYA_Error nya_account_recovery_generate(
    NYA_Arena* arena, u64 account_id, OUT char out_codes[NYA_ACCOUNTS_RECOVERY_CODE_COUNT][NYA_ACCOUNTS_RECOVERY_CODE_TEXT], OUT u32* out_count
) __attr_no_discard;

/**
 * Spends a recovery code for the account with this username, and answers whose it was.
 *
 * A code matches at most once: a match deletes the stored hash, so the same code cannot be used twice.
 * The username is folded like a login's, and the attempt is throttled on the account and `address` the
 * way a password attempt is; `address` may be null when there is nobody to name.
 *
 * NYA_ERROR_PERMISSION_DENIED for a code that does not match, an account that does not exist, and a
 * disabled account, all in the same words for the reason accounts.h gives — a recovery screen is as
 * reachable by somebody guessing as a login is.
 * */
NYA_API NYA_Error nya_account_recovery_consume(
    NYA_Arena* arena, NYA_ConstCString username, NYA_ConstCString code, NYA_ConstCString address, OUT NYA_AccountUser* out_user
) __attr_no_discard;

/** How many unused codes the account has left, for the "you have 3 codes remaining" a person is shown. */
NYA_API NYA_Error nya_account_recovery_remaining(NYA_Arena* arena, u64 account_id, OUT u32* out_remaining) __attr_no_discard;

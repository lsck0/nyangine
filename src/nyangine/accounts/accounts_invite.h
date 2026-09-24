/**
 * @file accounts_invite.h
 *
 * Who may make an account: everyone, only whoever holds a code, or nobody but an admin.
 *
 * ```c
 * // a program decides its policy once, from its own config
 * NYA_AccountRegistration policy = NYA_ACCOUNT_REGISTRATION_INVITE;
 *
 * // an admin hands out a code
 * char code[NYA_ACCOUNTS_INVITE_CODE_TEXT] = { 0 };
 * NYA_TRY(nya_account_invite_issue(arena, admin.id, 7 * 24 * 3600, code, sizeof(code)));
 *
 * // registration goes through the policy, and the code is spent only if an account is really made
 * NYA_AccountUser fresh = { 0 };
 * NYA_TRY(nya_account_register(arena, policy, "ada", password, typed_code, &fresh));
 * ```
 *
 * ── the three policies ──
 *
 * `OPEN` is a sign-up form anybody may use. `INVITE` needs a code an existing member made, which is how
 * a private community grows by vouching rather than by advertising. `CLOSED` is no self-registration at
 * all — an admin makes every account, which is what an internal tool wants. A program picks one from its
 * config; this module does not decide, it enforces.
 *
 * A fresh install has no accounts and no admin to invite anybody, so the first account is the CLI's to
 * make and becomes the owner, whatever the policy says — that bootstrap is the program's, and it calls
 * nya_account_create directly rather than coming through here. There is no default password anywhere.
 *
 * ── an invite is a recovery code that makes an account instead of opening one ──
 *
 * The same shape, for the same reasons: single use, stored as a hash and never the code, high entropy
 * from the system random source, and canonicalised so it matches however it is read off a message. A
 * consumed invite is marked used rather than deleted, because who invited whom is worth keeping — a
 * spam wave is traced back through it — where a spent recovery code is only noise.
 *
 * ── spent only on success ──
 *
 * nya_account_register spends the invite in the same step that makes the account, and if making the
 * account fails — a taken username, a short password — the invite is left unused. A code that a failed
 * attempt burned would be a way to grief somebody by registering their chosen name badly.
 *
 * ── it does not grant anything ──
 *
 * A registered account holds no roles, invite or no invite. What a new member may do is `permission`'s,
 * and an invite that carried a role would be a way to mint an admin from a leaked code.
 * */
#pragma once

#include "nyangine/accounts/accounts_user.h"
#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

/** Bytes of an invite code as text, terminator included: `XXXXX-XXXXX-XXXXX-XXXXX` and its NUL. */
#define NYA_ACCOUNTS_INVITE_CODE_TEXT 24

/** Bytes of entropy behind an invite. Ten, wider than a recovery code: an invite is pasted, not typed. */
#define NYA_ACCOUNTS_INVITE_CODE_BYTES 10

// TYPES

/** What a program allows. One of these, from its own config; see the header. */
typedef enum {
    /** Anybody may register. The invite argument is ignored. */
    NYA_ACCOUNT_REGISTRATION_OPEN = 0,

    /** Only with a valid, unused, unexpired invite code. */
    NYA_ACCOUNT_REGISTRATION_INVITE,

    /** Nobody self-registers; an admin makes every account. nya_account_register always refuses. */
    NYA_ACCOUNT_REGISTRATION_CLOSED,
} NYA_AccountRegistration;

typedef struct NYA_AccountInvite NYA_AccountInvite;

/**
 * One invite, as it is stored: a hash, and who made it and who spent it.
 *
 * Public so the ORM can derive its table from the reflection; who invited whom is a thing an admin
 * screen reads, so unlike a recovery code this one is meant to be looked at.
 * */
// @reflect
struct NYA_AccountInvite {
    u64 id; // @key

    /** SHA-256 of the canonical code, as hex. What a consume looks up. */
    char code_hash[72]; // @redact

    /** The account that made it, and the one that spent it. `used_by` is zero until it is spent. */
    u64 created_by;
    u64 used_by;

    /** Seconds since the epoch: when it was made, when it stops being valid, and when it was spent. */
    u64 created_at_s;
    u64 expires_at_s;
    u64 used_at_s;
};

// FUNCTIONS

/**
 * Makes an invite code good for `ttl_s` seconds, attributed to `created_by`, and answers it once.
 *
 * This is the only time the code exists as text. `created_by` is the account handing it out, which a
 * program has already checked holds whatever permission it requires; a `ttl_s` of zero is refused,
 * because an invite that never expires is a standing way in that outlives whatever it was for.
 * */
NYA_API NYA_Error nya_account_invite_issue(NYA_Arena* arena, u64 created_by, u64 ttl_s, OUT char* out_code, u64 capacity) __attr_no_discard;

/**
 * Makes an account under `policy`, spending `invite` when the policy calls for one.
 *
 * OPEN ignores `invite` and makes the account. INVITE requires `invite` to be a valid, unused,
 * unexpired code, and spends it in the same step — and only if the account is really made, so a taken
 * username leaves the code unused. CLOSED refuses every call, since an admin makes those accounts with
 * nya_account_create.
 *
 * The refusals name the reason a would-be member is allowed to know — a bad invite, a taken username, a
 * short password — because registration is not a login and the person is not yet an attacker to hide
 * things from.
 * */
NYA_API NYA_Error nya_account_register(
    NYA_Arena* arena, NYA_AccountRegistration policy, NYA_ConstCString username, NYA_ConstCString password, NYA_ConstCString invite,
    OUT NYA_AccountUser* out_user
) __attr_no_discard;

/** Revokes an unused invite so it can no longer be spent. NYA_ERROR_NOT_FOUND when it is not one, or already spent. */
NYA_API NYA_Error nya_account_invite_revoke(NYA_Arena* arena, NYA_ConstCString invite) __attr_no_discard;

/** The invites `created_by` made, newest first, for the admin screen that shows who let whom in. */
NYA_API NYA_Error nya_account_invite_list(NYA_Arena* arena, u64 created_by, OUT NYA_AccountInvite** out_invites, OUT u32* out_count) __attr_no_discard;

/** Deletes the spent and expired invites older than `keep_for_s`, and answers how many. What a sweep calls. */
NYA_API NYA_Error nya_account_invite_prune(NYA_Arena* arena, u64 keep_for_s, OUT u32* out_removed) __attr_no_discard;

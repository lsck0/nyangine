/**
 * @file accounts_identity.h
 *
 * The same person, arriving through Steam, through Discord, or through a password.
 *
 * ```c
 * // "sign in with Steam", after the ticket a client sent has actually been checked with Steam
 * NYA_AccountUser player  = { 0 };
 * b8              created = false;
 *
 * NYA_TRY(nya_account_from_identity(arena, "steam", "76561198000000000", "ada", &player, &created));
 * ```
 *
 * ── an account is a person, an identity is a way in ──
 *
 * One account holds as many identities as the person has: a Steam id, a Discord id, a Google subject,
 * and a password beside them. That is the whole reason this is a second table rather than three
 * columns — somebody who signed up with a password and later links Steam is one account, not two, and
 * the row that says so is the one thing that makes "log in with Steam" land on the account they
 * already had.
 *
 * `provider` is a short name this module folds to lower case — `steam`, `discord`, `google` — and not
 * an enum, because the engine has no business deciding which providers a program may have. `subject`
 * is the provider's own permanent id for that person.
 *
 * ── the subject is never an email address ──
 *
 * Refused outright, with the `@` as the test. An email is re-assigned: a company hands a leaver's
 * address to their replacement, a provider lets somebody change theirs, and either one turns "the same
 * subject" into "a different person with the same key". Every provider worth using has a stable id;
 * that id is what goes here, and the email is a display detail if it is anything.
 *
 * ── what this does NOT do, and it matters ──
 *
 * **It does not check that the person is who they say they are.** By the time anything here is called,
 * that is already settled: an OIDC id token whose signature was verified against the provider's JWKS
 * (oidc.h), or a Steam session ticket the *server* validated with Steam. This module records a fact it
 * is told.
 *
 * Which is worth saying plainly because of the way this is usually got wrong: `nya_steam_user_id()`
 * runs on the client and returns whatever that machine says. A client that sends its own Steam id to a
 * server is a client claiming an identity, and a server that writes that claim in here has built an
 * impersonate-anybody button. The engine cannot validate a ticket today — steam.h has no
 * `GetAuthSessionTicket` — so a Steam login needs that half written before it is a login at all.
 *
 * ── no throttle here ──
 *
 * accounts_throttle.h counts wrong passwords, because a password is short and guessable. A provider
 * subject is not a secret and is not guessed: it is arrived at with a signed token. Throttling this
 * would slow down nothing an attacker does and would let one person's failed logins block another's.
 *
 * ── an account made this way has no password ──
 *
 * The column is empty, and nya_account_authenticate refuses an empty hash, so the account exists and
 * simply cannot be logged into with a password until somebody sets one. That is the correct answer to
 * "what is the password of my Steam account": there is not one.
 * */
#pragma once

#include "nyangine/accounts/accounts_user.h"
#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

/** Bytes a provider name takes, terminator included. `steam`, `discord`, `accounts.google.com`. */
#define NYA_ACCOUNTS_MAX_PROVIDER 40

/**
 * Bytes a provider's id for a person takes, terminator included.
 *
 * A Steam id is 17 digits, a Discord snowflake is 19, a Google subject is 21, and an OIDC `sub` is
 * whatever the issuer says up to 255 by the spec. This holds the ones that exist with room to spare.
 * */
#define NYA_ACCOUNTS_MAX_SUBJECT 128

/** Identities one account may hold. Enough for every provider a program is likely to offer, twice. */
#define NYA_ACCOUNTS_MAX_IDENTITIES_PER_USER 8

// TYPES

typedef struct NYA_AccountIdentity NYA_AccountIdentity;

/** One way into one account. */
// @reflect
struct NYA_AccountIdentity {
    u64 id; // @key

    /** Whose it is. */
    u64 account_id;

    /** Folded to lower case, because `Steam` and `steam` are not two providers. */
    char provider[NYA_ACCOUNTS_MAX_PROVIDER];

    /** The provider's permanent id for this person, and never their email; see the header. */
    char subject[NYA_ACCOUNTS_MAX_SUBJECT];

    /**
     * What they are called over there, for the list a person is shown of their own linked accounts.
     *
     * Never trusted for anything: a display name is chosen by the person and changed whenever they
     * like, so nothing is ever looked up by it.
     * */
    char display[NYA_ACCOUNTS_MAX_DISPLAY];

    /** Seconds since the epoch: when it was linked, and when it was last signed in with. */
    u64 linked_at_s;
    u64 used_at_s;
};

// FUNCTIONS

/**
 * Records that this provider's `subject` is this account.
 *
 * Refuses a subject already linked to a *different* account, because one Steam id being two accounts
 * is the thing this table exists to prevent; linking it to the same account again only refreshes the
 * display name. Refuses a provider or subject that is empty, has a control character in it, or looks
 * like an email address.
 *
 * Whoever calls this has already proved the person owns that subject. See the header.
 * */
NYA_API NYA_Error nya_account_identity_link(
    NYA_Arena* arena, u64 account_id, NYA_ConstCString provider, NYA_ConstCString subject, NYA_ConstCString display
) __attr_no_discard;

/**
 * Removes one provider from an account.
 *
 * Refuses to remove the last way in: an account with no password and one identity would, without this,
 * become an account nobody can ever reach again, holding whatever that person owns. Set a password
 * first, or link something else.
 * */
NYA_API NYA_Error nya_account_identity_unlink(NYA_Arena* arena, u64 account_id, NYA_ConstCString provider) __attr_no_discard;

/**
 * The account this provider's `subject` belongs to.
 *
 * NYA_ERROR_NOT_FOUND when nothing is linked, which is the ordinary answer the first time somebody
 * signs in with a provider. Unlike a password login this says plainly what happened, because there is
 * nothing here to guess at: whoever is asking already holds a signed token for that subject.
 * */
NYA_API NYA_Error nya_account_find_by_identity(NYA_Arena* arena, NYA_ConstCString provider, NYA_ConstCString subject, OUT NYA_AccountUser* out_user)
    __attr_no_discard;

/**
 * What "sign in with Steam" actually is: the account for this subject, made if there is not one yet.
 *
 * `out_created` says which happened, so a program can send somebody new through whatever it does for
 * new people — a welcome, a username they pick, a tutorial — without asking a second question.
 *
 * A new account gets a username derived from the provider and the subject, holds no roles and has no
 * password. It is theirs to rename; this only has to produce something unique and legal.
 *
 * Refuses a disabled account with the same refusal a password login gives, so a ban is a ban however
 * somebody arrives.
 * */
NYA_API NYA_Error nya_account_from_identity(
    NYA_Arena* arena, NYA_ConstCString provider, NYA_ConstCString subject, NYA_ConstCString display, OUT NYA_AccountUser* out_user,
    OUT b8* out_created
) __attr_no_discard;

/** The identities an account holds, oldest first, for the list a person is shown of their own. */
NYA_API NYA_Error nya_account_identity_list(NYA_Arena* arena, u64 account_id, OUT NYA_AccountIdentity** out_identities, OUT u32* out_count)
    __attr_no_discard;

/** How many ways into this account there are, a password counted as one. What unlink checks. */
NYA_API NYA_Error nya_account_identity_count(NYA_Arena* arena, u64 account_id, OUT u32* out_count) __attr_no_discard;

/**
 * The folded form of a provider name: what uniqueness is decided on.
 *
 * Lower case, and letters, digits, `.`, `-` and `_` only — a host name or a short word, which is what
 * every provider name here is. False for anything else, so a provider named with a space or a slash is
 * refused at the edge rather than stored and never matched again.
 * */
NYA_API b8 nya_account_provider_normalize(NYA_ConstCString provider, OUT char* out_normalized, u64 capacity) __attr_no_discard;

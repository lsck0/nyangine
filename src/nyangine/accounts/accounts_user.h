/**
 * @file accounts_user.h
 *
 * A user: the row, the password, and the things that may be done to an account.
 *
 * ```c
 * NYA_AccountUser ada = { 0 };
 * NYA_TRY(nya_account_create(arena, "ada", "a long passphrase nobody else knows", &ada));
 *
 * NYA_AccountUser found = { 0 };
 * if (nya_account_authenticate(arena, "ada", attempt, &found).ok) let_them_in(&found);
 * ```
 *
 * ── the password, and why it is stored the way it is ──
 *
 * Argon2id, with the cost written into every hash rather than into this file: a stored password says
 * which parameters made it, so raising them later leaves every old password verifiable and every new
 * one stronger. The encoded form is the one everybody else writes too —
 * `$argon2id$v=19$m=...,t=...,p=...$salt$hash`, base64 without padding — so a database this wrote can
 * be read by something that is not this, which is the property that matters when somebody migrates
 * away.
 *
 * The parameters are NYA_ACCOUNTS_ARGON2ID_*, and they are what crypto_kdf.h already measured on this
 * machine. A server that wants more can raise them; a password hashed under the old ones keeps
 * working, and nya_account_password_needs_rehash says when one is worth replacing.
 *
 * ── what a login answers ──
 *
 * One refusal for every way a login can fail, at the same cost: see accounts.h. A caller that wants
 * to know *why* for its own logs has nya_account_find, which answers honestly because it is not the
 * thing an attacker can reach.
 *
 * ── the username ──
 *
 * Two fields: what the person typed, and the form uniqueness is decided on. The normalised form is
 * lower case with the characters that look like other characters folded together, so `Ada`, `ada` and
 * `ＡＤＡ` cannot be three accounts — the trick every phishing account inside a service uses. What is
 * displayed is what they typed.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/db/db_sql.h"
#include "nyangine/permission/permission.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Bytes a username may take, terminator included.
 *
 * Long enough for a name somebody chose and short enough to show in a list without a tooltip. A
 * longer one is refused at creation rather than truncated, because a truncated username is a
 * different account than the person asked for.
 * */
#define NYA_ACCOUNTS_MAX_USERNAME 64

/** Bytes a display name may take, terminator included. Wider than a username: this one is prose. */
#define NYA_ACCOUNTS_MAX_DISPLAY 128

/**
 * Bytes one encoded password hash takes, terminator included.
 *
 * The encoded form is the parameters, a 16 byte salt and a 32 byte hash, all in base64 without
 * padding: about 100 characters today. This is twice that, so raising the parameters — which lengthens
 * only the numbers — cannot overflow a column that already holds rows.
 * */
#define NYA_ACCOUNTS_MAX_HASH 256

/** The shortest password this accepts, in bytes. */
#define NYA_ACCOUNTS_MIN_PASSWORD 12

/**
 * The longest, in bytes.
 *
 * Not a security bound — Argon2id takes any length — but a bound on what one request may make this
 * process hash, since hashing is deliberately expensive and the length is the attacker's to choose.
 * */
#define NYA_ACCOUNTS_MAX_PASSWORD 1024

/*
 * The cost of one password hash. crypto_kdf.h measured these; they are repeated here rather than
 * taken from there because *this* is where raising them is a decision with a migration attached, and
 * because every hash stores the parameters it was made with, so the two can differ without anything
 * breaking.
 *
 * Nineteen mebibytes and two passes is RFC 9106's second recommended setting, the one for a server
 * that also has other work to do. It is about 30 ms on a 2020 desktop core, which is slow enough to
 * make a stolen database expensive and fast enough that a login does not feel like one.
 */
#define NYA_ACCOUNTS_ARGON2ID_MEMORY_KIB (19 * 1024)
#define NYA_ACCOUNTS_ARGON2ID_PASSES     2
#define NYA_ACCOUNTS_ARGON2ID_LANES      1

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_AccountUser NYA_AccountUser;

/**
 * One account, as it is stored.
 *
 * Flat on purpose: every field is a column the ORM derives, so a query can ask about any of them and
 * a migration can add one. See db_orm.h for what a described field may be.
 * */
// @reflect
struct NYA_AccountUser {
    /** The row id sqlite assigns. Zero on a struct that has not been inserted yet. */
    u64 id; // @key

    /** What the person typed, and what is shown back to them. */
    char username[NYA_ACCOUNTS_MAX_USERNAME];

    /**
     * The form uniqueness is decided on: see the header. Never shown to anybody, and the column a
     * lookup by name actually searches.
     * */
    char normalized[NYA_ACCOUNTS_MAX_USERNAME];

    /** What they would like to be called. Their username until they say otherwise. */
    char display[NYA_ACCOUNTS_MAX_DISPLAY];

    /**
     * The encoded Argon2id hash, parameters and salt included. Never a password, never reversible,
     * and never logged: `@redact` is what keeps it out of a request log at any depth.
     * */
    char password[NYA_ACCOUNTS_MAX_HASH]; // @redact

    /**
     * The roles this user holds, as `permission`'s bitmask. Zero is a user with no role at all, which
     * is what a fresh account is until something grants it one.
     * */
    u64 roles;

    /** Seconds since the epoch when the account was made, and when its password last changed. */
    u64 created_at_s;

    /**
     * Named like a secret and is not one: a date, which a session list and an audit line both show.
     * `@loggable` is how that is said to the lint rule rather than to a reader only.
     * */
    u64 password_changed_at_s; // @loggable

    /**
     * Refused at login, with every session ended.
     *
     * Apart from deletion because the two mean different things: a disabled account still owns what
     * it wrote and can be let back in, and a deleted one is gone. An account is never deleted by
     * being disabled, and the audit entries of a deleted one keep a tombstone rather than a name.
     * */
    b8 disabled;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Opens the accounts tables in `database`, creating or migrating them.
 *
 * `arena` owns everything this module allocates until nya_accounts_close, including the prepared
 * statements behind the tables. The database is the caller's and is not closed here.
 * */
NYA_API NYA_Error nya_accounts_open(NYA_Arena* arena, NYA_Database* database) __attr_no_discard;

/** Closes the tables. The database itself is the caller's. Calling it twice is a no-op. */
NYA_API void nya_accounts_close(void);

/** Whether the tables are open, which is what every call below needs and what a facade would ask. */
NYA_API b8 nya_accounts_is_open(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * ACCOUNTS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Creates an account and answers it, with its id filled in.
 *
 * Refuses a username that is taken in its normalised form, one that is empty or longer than
 * NYA_ACCOUNTS_MAX_USERNAME, one with a control character in it, and a password outside the length
 * bounds. Says exactly which: whoever is registering is allowed to know why they were refused.
 *
 * The new account holds no roles. Granting one is `permission`'s, and the first account of a fresh
 * install becoming the owner is the program's decision rather than this module's.
 * */
NYA_API NYA_Error nya_account_create(NYA_Arena* arena, NYA_ConstCString username, NYA_ConstCString password, OUT NYA_AccountUser* out_user)
    __attr_no_discard;

/**
 * The account with this username, in its normalised form.
 *
 * NYA_ERROR_NOT_FOUND when there is none. Honest about that, unlike a login: this is the call an
 * administrator's screen and a CLI use, and neither is reachable by somebody guessing names.
 * */
NYA_API NYA_Error nya_account_find(NYA_Arena* arena, NYA_ConstCString username, OUT NYA_AccountUser* out_user) __attr_no_discard;

/** The account with this id. NYA_ERROR_NOT_FOUND when there is none. */
NYA_API NYA_Error nya_account_find_by_id(NYA_Arena* arena, u64 id, OUT NYA_AccountUser* out_user) __attr_no_discard;

/**
 * Whether this username and password are an account, and which one.
 *
 * One refusal for every way it can fail, at the same cost whether the account exists or not; see
 * accounts.h. NYA_ERROR_PERMISSION_DENIED, always, with a message that says nothing useful to
 * somebody guessing.
 *
 * `address` is who is asking, and may be null when there is nobody to name — a CLI, a test. It is
 * what accounts_throttle.h counts against, and counting is not optional: a wrong password recorded
 * here is what buys the next attempt its wait, so no caller can forget to slow an attacker down.
 * */
NYA_API NYA_Error nya_account_authenticate(
    NYA_Arena* arena, NYA_ConstCString username, NYA_ConstCString password, NYA_ConstCString address, OUT NYA_AccountUser* out_user
) __attr_no_discard;

/**
 * Replaces a password, and ends every session the user has.
 *
 * Ending them is not optional and not a flag: changing a password is what somebody does when they
 * think a session is not theirs any more, and a change that left the old ones alive would be a
 * change that did nothing about the thing they were worried about.
 *
 * `current` is checked first. An administrator resetting a password for somebody who has lost it
 * uses nya_account_password_reset instead, which takes no current password and is behind
 * NYA_PERMISSION_MANAGE_SUBJECTS in whatever calls it.
 * */
NYA_API NYA_Error nya_account_password_change(NYA_Arena* arena, u64 id, NYA_ConstCString current, NYA_ConstCString replacement) __attr_no_discard;

/**
 * Sets a password without asking for the current one, and ends every session.
 *
 * What an administrator does for somebody who cannot log in. This module does not decide who may:
 * the permission check belongs to the route or the command that calls it, where the actor is known.
 * */
NYA_API NYA_Error nya_account_password_reset(NYA_Arena* arena, u64 id, NYA_ConstCString replacement) __attr_no_discard;

/**
 * Whether this stored hash was made with weaker parameters than this build now uses.
 *
 * True means: the password is still right, and the next time it is verified — which is the only
 * moment the plaintext exists — it is worth storing again under the current cost. That is how a
 * server raises its parameters without asking anybody to change anything.
 * */
NYA_API b8 nya_account_password_needs_rehash(const NYA_AccountUser* user) __attr_no_discard;

/**
 * Removes an account and every session it holds.
 *
 * The other half of nya_account_create, and the thing nya_account_disabled_set is deliberately not: a
 * disabled account can be let back in and still owns what it wrote, and this one is gone. What a
 * program does about the rows elsewhere that name this id is the program's, because only it knows
 * whether they are a comment to keep under a tombstone or a thing to delete.
 * */
NYA_API NYA_Error nya_account_destroy(NYA_Arena* arena, u64 id) __attr_no_discard;

/** Refuses this account at login and ends every session it has. The account and its rows stay. */
NYA_API NYA_Error nya_account_disabled_set(NYA_Arena* arena, u64 id, b8 disabled) __attr_no_discard;

/** Replaces the roles a user holds. What `permission` then answers with; see permission.h. */
NYA_API NYA_Error nya_account_roles_set(NYA_Arena* arena, u64 id, u64 roles) __attr_no_discard;

/** How many accounts exist, which is what tells a fresh install it has none. */
NYA_API NYA_Error nya_account_count(OUT u64* out_count) __attr_no_discard;

/**
 * The normalised form of `username`: what uniqueness is decided on.
 *
 * Exposed because a program that wants to say "that name is taken" before a form is submitted has to
 * ask the same question this does, and asking it a second way is how two answers come to differ.
 * */
NYA_API b8 nya_account_username_normalize(NYA_ConstCString username, OUT char* out_normalized, u64 capacity) __attr_no_discard;

/**
 * @file accounts_audit.h
 *
 * An append-only record of who did what to whom, so an account change can always be accounted for.
 *
 * ```c
 * // after an admin disables somebody
 * nya_account_audit_record(arena, admin.id, target.id, NYA_ACCOUNT_ACTION_DISABLED, "spam");
 *
 * // the admin screen for one account
 * NYA_AccountAudit* rows = nullptr;
 * u32 count = 0;
 * NYA_TRY(nya_account_audit_list(arena, target.id, 50, &rows, &count));
 * ```
 *
 * ── append only, and why ──
 *
 * Rows are written and read and never changed. An audit trail that can be edited is not one: the whole
 * value of it is that when a question is asked later — who disabled this account, who gave this person
 * their role — the answer is a fact nobody could quietly rewrite. There is no update call here, and the
 * only deletion is a retention sweep that drops rows older than a program's chosen horizon, wholesale,
 * never one at a time.
 *
 * ── it survives the account ──
 *
 * Deleting a user removes their rows and their sessions, but not the audit entries that name them: those
 * keep the id, which now resolves to no account. That dangling id is the tombstone — "this happened to a
 * user who has since been deleted" — and it is the point, because the reason an account was banned is
 * exactly what somebody needs after the account is gone. An audit that vanished with its subject would
 * be an audit that erased the evidence of its own most important entries.
 *
 * ── actor, subject, action, reason ──
 *
 * `actor` did it, `subject` had it done to them, and they are often different — an admin disabling a
 * member — but not always: somebody changing their own password is both. An `actor` of zero is the
 * system itself, a sweep or a bootstrap with no person behind it. `action` is one of a fixed set, so a
 * screen can group and translate them; `reason` is free text a person typed, the "why" a ban or a reset
 * carries.
 *
 * ── what it is not ──
 *
 * Not a request log — http_log.h is that, and it holds bytes and statuses, not decisions. Not a
 * permission check: recording that something happened is not deciding whether it may. A caller checks
 * the permission, does the thing, and records it; this module only remembers.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/** Bytes of the free-text reason an entry carries, terminator included. */
#define NYA_ACCOUNTS_AUDIT_REASON_MAX 200

// TYPES

/**
 * What was done. A fixed set so a screen can translate and group them, stored as its number.
 *
 * New actions are added at the end and never renumbered, because the number is what is written in a row
 * that outlives this enum's source: a value reused for a different action would rewrite history.
 * */
typedef enum {
    NYA_ACCOUNT_ACTION_NONE = 0,

    /** An account was made — by registration, by an admin, or by a provider login. */
    NYA_ACCOUNT_ACTION_CREATED,

    /** A password was changed by its owner, or reset by an admin or a recovery code. */
    NYA_ACCOUNT_ACTION_PASSWORD_CHANGED,

    /** An account was disabled or enabled. */
    NYA_ACCOUNT_ACTION_DISABLED,
    NYA_ACCOUNT_ACTION_ENABLED,

    /** The roles an account holds were replaced. */
    NYA_ACCOUNT_ACTION_ROLES_SET,

    /** A provider login was linked or unlinked. */
    NYA_ACCOUNT_ACTION_IDENTITY_LINKED,
    NYA_ACCOUNT_ACTION_IDENTITY_UNLINKED,

    /** Sessions were ended — one, or all of them. */
    NYA_ACCOUNT_ACTION_SESSION_REVOKED,

    /** An account was deleted. Its subject id is a tombstone from here on. */
    NYA_ACCOUNT_ACTION_DELETED,

    NYA_ACCOUNT_ACTION_COUNT,
} NYA_AccountAction;

typedef struct NYA_AccountAudit NYA_AccountAudit;

/** One entry, as it is stored. Public so the ORM derives its table; an admin screen reads these. */
// @reflect
struct NYA_AccountAudit {
    u64 id; // @key

    /** Seconds since the epoch when it happened. */
    u64 at_s;

    /** Who did it (zero is the system) and whom it was done to. Either may be a deleted account's id. */
    u64 actor_id;
    u64 subject_id;

    /** What was done, as NYA_AccountAction's number. */
    u32 action;

    /** The free-text why, or empty. Never a secret: an audit line is read by more people than a password is. */
    char reason[NYA_ACCOUNTS_AUDIT_REASON_MAX];
};

// FUNCTIONS

/**
 * Writes one entry. Best-effort by design: it logs and swallows its own failure rather than returning one.
 *
 * An audit write that failed the operation it records would be a system where turning off the audit
 * turns off enforcement — so a full disk stops the recording, never the disabling. The failure is logged
 * loudly; nothing a caller does hangs on it.
 * */
NYA_API void nya_account_audit_record(NYA_Arena* arena, u64 actor_id, u64 subject_id, NYA_AccountAction action, NYA_ConstCString reason);

/** The entries about one account, newest first, up to `limit`. What an admin screen shows for a user. */
NYA_API NYA_Error nya_account_audit_list(NYA_Arena* arena, u64 subject_id, u32 limit, OUT NYA_AccountAudit** out_entries, OUT u32* out_count)
    __attr_no_discard;

/** How many entries name this account as subject, for the screen's "N events" and the ceiling audit. */
NYA_API NYA_Error nya_account_audit_count(NYA_Arena* arena, u64 subject_id, OUT u32* out_count) __attr_no_discard;

/** Deletes entries older than `keep_for_s`, wholesale, and answers how many. The only deletion there is. */
NYA_API NYA_Error nya_account_audit_prune(NYA_Arena* arena, u64 keep_for_s, OUT u32* out_removed) __attr_no_discard;

/** The name of an action, for a screen and a log line. "unknown" for a number this build does not know. */
NYA_API NYA_ConstCString nya_account_action_name(NYA_AccountAction action) __attr_no_discard;

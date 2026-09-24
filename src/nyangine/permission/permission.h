/**
 * @file permission.h
 *
 * ── the permission module ──
 *
 * Who may do what to which thing, as a pure function of roles and overwrites.
 *
 * ```c
 * NYA_Permissions* guild = nya_permissions_create(arena);
 *
 * u32 officer = 0;
 * NYA_TRY(nya_permission_role_add(guild, "officer", 10, GNY_PERMISSION_KICK | GNY_PERMISSION_INVITE, &officer));
 * NYA_TRY(nya_permission_role_grant(guild, NYA_PERMISSION_OWNER, player_id, officer));
 *
 * if (nya_permission_has(guild, player_id, raid_id, GNY_PERMISSION_KICK)) { ... }
 * ```
 *
 * ── why this is not part of the HTTP server ──
 *
 * A guild in a game asks the same question a route does: may this actor do this thing to that resource.
 * So this module sits below `net` and below `http`, knows nothing about a request, a token or a socket,
 * and takes ids rather than any of those. A game asks whether a player may demote another; the HTTP
 * extractor asks whether a caller may reach a route. One resolver, one hierarchy rule, one audit trail.
 *
 * An id here is whatever the program uses to name a subject and a resource — an entity handle, a user
 * row's key, a channel id. This module never interprets one, which is what lets both callers keep their
 * own.
 *
 * ── the model, which is Discord's ──
 *
 * A **permission** is one bit of a u64. The engine reserves the top three (see below); every other bit
 * is the program's to name, usually as a `@flags` enum so the cheatsheet and the schema can list them.
 *
 * A **role** has a name, a position and a set of permissions it allows. A subject holds any number of
 * roles, and every subject holds `@everyone` (role 0) whether it was granted or not. Base permissions
 * are the union of what the held roles allow.
 *
 * An **overwrite** belongs to one resource and names one role or one subject, with a set to deny and a
 * set to allow. Resolution applies them in Discord's order, which is the one thing here that cannot be
 * changed without changing what an existing configuration means:
 *
 *   1. the union of the held roles' allows
 *   2. `ADMINISTRATOR` short circuits: it is every permission, on every resource
 *   3. `@everyone`'s overwrite on the resource: deny, then allow
 *   4. every other held role's denies together, then their allows together
 *   5. the subject's own overwrite on the resource: deny, then allow
 *   6. everything forbidden — by a held role or on the subject directly — is removed, absolutely
 *   7. the owner short circuits, above everything, including a forbid
 *
 * A **forbid** (step 6) is the strong deny an overwrite's deny is not: an overwrite is per resource and
 * an allow can win it back, where a forbid is global and nothing below the owner puts it back — not
 * another role's allow, not ADMINISTRATOR, not a per-resource allow overwrite. It is `permissions_forbidden`,
 * set on a role or on a subject directly.
 *
 * ── hierarchy, which is what stops escalation ──
 *
 * A subject's rank is the highest position among the roles it holds; the owner outranks everyone. An
 * actor may only act on a subject it outranks, may only grant or revoke a role below its own rank, and
 * may only hand out permissions it holds itself. `nya_permission_role_grant` and its partners enforce
 * all three and refuse rather than silently doing less, so a caller cannot forget the check by calling
 * the convenient function instead.
 *
 * ── what is not here ──
 *
 * No storage and no serialization: this is a table a program fills from wherever it keeps things, and
 * `db` does not exist yet. No cache and no invalidation counter, because resolution is a handful of
 * ORs over a fixed table rather than a query. Nothing about authentication: who the subject is has been
 * decided before anything here is called.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/**
 * Roles one table may hold, `@everyone` included.
 *
 * Sixty-four because a subject's roles are a u64 of bits, one per role, which makes "the roles this
 * subject holds" one word to copy and one AND to test. Discord ships servers with fewer than this and a
 * program that needs more is describing something other than roles.
 * */
#define NYA_PERMISSION_MAX_ROLES 64

/** A role name, terminator included. Long enough for "raid leader" and short enough to sit in the table. */
#define NYA_PERMISSION_MAX_ROLE_NAME 32

/**
 * Subjects one table may hold.
 *
 * A guild, a project or a channel's membership, not a whole game's player list: a program with more
 * subjects than this has more than one table, one per guild, which is also how it keeps them apart.
 * */
#define NYA_PERMISSION_MAX_SUBJECTS 256

/**
 * Overwrites one table may hold across every resource.
 *
 * An overwrite is the exception rather than the rule — most resources inherit and carry none — so this
 * is sized for the handful of rooms that differ rather than for the product of resources and roles.
 * */
#define NYA_PERMISSION_MAX_OVERWRITES 256

/** A permission's label, terminator included. A name an editor shows beside a checkbox, not a sentence. */
#define NYA_PERMISSION_MAX_LABEL 24

/**
 * Audit entries kept, oldest dropped.
 *
 * What changed has to outlive the change long enough to be read after an argument, and this is a ring
 * rather than a log because a table that grows without bound is a table that eventually stops the
 * program to say so. A program that needs the whole history writes entries out as they are added.
 * */
#define NYA_PERMISSION_MAX_AUDIT 128

// ───────────────────────────────────── RESERVED PERMISSIONS ─────────────────────────────────────

/** Every permission, on every resource, deny or no deny. The one bit that skips resolution. */
#define NYA_PERMISSION_ADMINISTRATOR (1ULL << 63)

/** May add, edit, reorder and delete roles, and grant those below its own rank. */
#define NYA_PERMISSION_MANAGE_ROLES (1ULL << 62)

/** May act on other subjects it outranks: add them, remove them, change what they hold. */
#define NYA_PERMISSION_MANAGE_SUBJECTS (1ULL << 61)

/** The three above. A program's own permissions take the other sixty-one bits. */
#define NYA_PERMISSION_RESERVED (NYA_PERMISSION_ADMINISTRATOR | NYA_PERMISSION_MANAGE_ROLES | NYA_PERMISSION_MANAGE_SUBJECTS)

/** No permissions at all, which is what an unknown subject resolves to. */
#define NYA_PERMISSION_NONE 0ULL

/** The role every subject holds, whether or not it was granted. Its position is always zero. */
#define NYA_PERMISSION_ROLE_EVERYONE 0U

/**
 * The actor that answers yes to every check: the owner, a migration, a console command.
 *
 * It is a subject id rather than a flag so that an audit entry says who did it, and it is this value
 * rather than "the owner" so that a program with no owner still has a way in. Never accept it from
 * outside the program.
 * */
#define NYA_PERMISSION_SYSTEM 0ULL

// ───────────────────────────────────── TYPES ─────────────────────────────────────

/** A set of permissions: one bit each, the program's to name below NYA_PERMISSION_RESERVED. */
typedef u64 NYA_Permission;

/** One table: the roles, who holds them, the overwrites and what was changed. Opaque. */
typedef struct NYA_Permissions NYA_Permissions;

/** What an overwrite is attached to. */
typedef enum {
    NYA_PERMISSION_TARGET_ROLE = 0,
    NYA_PERMISSION_TARGET_SUBJECT,

    NYA_PERMISSION_TARGET_COUNT,
} NYA_PermissionTarget;

/** What an audit entry records. */
typedef enum {
    NYA_PERMISSION_CHANGE_ROLE_ADDED = 0,
    NYA_PERMISSION_CHANGE_ROLE_EDITED,
    NYA_PERMISSION_CHANGE_ROLE_REMOVED,
    NYA_PERMISSION_CHANGE_ROLE_GRANTED,
    NYA_PERMISSION_CHANGE_ROLE_REVOKED,
    NYA_PERMISSION_CHANGE_OVERWRITE_SET,
    NYA_PERMISSION_CHANGE_OVERWRITE_CLEARED,
    NYA_PERMISSION_CHANGE_OWNER_SET,

    NYA_PERMISSION_CHANGE_COUNT,
} NYA_PermissionChange;

/** One line of the audit trail: who did what to which, and what it was before and after. */
typedef struct {
    NYA_PermissionChange change;

    /** The subject that made the change, or NYA_PERMISSION_SYSTEM. */
    u64 actor;

    /** The subject or the resource the change was about, by the change's own meaning. */
    u64 subject;
    u64 resource;

    /** The role the change was about, or NYA_PERMISSION_MAX_ROLES for a change about no role. */
    u32 role;

    NYA_Permission before_allow;
    NYA_Permission before_deny;
    NYA_Permission after_allow;
    NYA_Permission after_deny;

    /** Whatever the caller passed as `now_s`; this module reads no clock. */
    u64 at_s;
} NYA_PermissionAudit;

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

/**
 * A table with `@everyone` in it and nothing else. Lives on `arena` and is freed with it.
 *
 * `@everyone` allows nothing to start with, which is the deny-by-default this module is built on: a
 * subject that holds no role and has no overwrite may do nothing at all.
 * */
NYA_API NYA_Permissions* nya_permissions_create(NYA_Arena* arena) __attr_no_discard;

/**
 * Empties the table back to what `nya_permissions_create` answers: `@everyone` allowing nothing, no
 * subjects, no overwrites, no owner, and the audit trail cleared with them.
 *
 * The memory belongs to the arena and is not given back here — this is "forget everything", which is
 * what a session ending means, and a caller that wants the memory back destroys the arena.
 * */
NYA_API void nya_permissions_destroy(NYA_Permissions* permissions);

// ───────────────────────────────────── ROLES ─────────────────────────────────────

/**
 * Adds a role and answers its index, which is also its bit in a subject's role set.
 *
 * `position` is the rank: higher outranks lower, and `@everyone` sits at zero. Two roles may share a
 * position, and then neither outranks the other, which is what makes "equal rank cannot act on equal
 * rank" hold without a tie break nobody wrote down.
 *
 * NYA_ERROR_INVALID_ARGUMENT for an empty or oversized name, a position of zero (reserved for
 * `@everyone`), or an allow set naming a bit this actor does not hold. NYA_ERROR_OUT_OF_MEMORY past
 * NYA_PERMISSION_MAX_ROLES. NYA_ERROR_PERMISSION_DENIED when `actor` lacks MANAGE_ROLES, or when the role would
 * sit at or above the actor's own rank.
 * */
NYA_API NYA_Error nya_permission_role_add(NYA_Permissions* permissions, u64 actor, NYA_ConstCString name, u16 position, NYA_Permission allow,
                                          u64 now_s, OUT u32* out_role) __attr_no_discard;

/**
 * Replaces a role's position and allow set. The same rules as adding one, plus: the role being edited
 * has to sit below the actor's rank, so nobody edits the role that outranks them into something else.
 * */
NYA_API NYA_Error nya_permission_role_edit(NYA_Permissions* permissions, u64 actor, u32 role, u16 position, NYA_Permission allow, u64 now_s)
    __attr_no_discard;

/**
 * Removes a role: takes it from everyone holding it, drops every overwrite naming it, and frees its
 * index for the next one added.
 *
 * `@everyone` cannot be removed. Otherwise the rules are the ones for editing: MANAGE_ROLES, and the
 * role has to sit below the actor's own rank.
 * */
NYA_API NYA_Error nya_permission_role_remove(NYA_Permissions* permissions, u64 actor, u32 role, u64 now_s) __attr_no_discard;

/** The role's name, or null for an index no role has. */
NYA_API NYA_ConstCString nya_permission_role_name(const NYA_Permissions* permissions, u32 role) __attr_no_discard;

/** The role's position, or zero for an index no role has. */
NYA_API u16 nya_permission_role_position(const NYA_Permissions* permissions, u32 role) __attr_no_discard;

/** What the role allows by itself, before any overwrite. Zero for an index no role has. */
NYA_API NYA_Permission nya_permission_role_allows(const NYA_Permissions* permissions, u32 role) __attr_no_discard;

/**
 * Sets what holding this role forbids, absolutely.
 *
 * A forbidden permission is removed from the result even when another held role allows it, even when
 * the subject holds ADMINISTRATOR, and even when a per-resource overwrite tries to allow it — the one
 * thing above it is the owner, who short circuits everything. That is the difference between this and a
 * deny overwrite: a deny is per resource and an allow can win it back, a forbid is global and nothing
 * below the owner does.
 *
 * The same rules as editing a role — MANAGE_ROLES and the role below the actor's rank — but not the
 * "only what you hold" rule an allow has, because forbidding takes a permission away and taking away is
 * not escalation.
 * */
NYA_API NYA_Error nya_permission_role_forbid_set(NYA_Permissions* permissions, u64 actor, u32 role, NYA_Permission forbid, u64 now_s) __attr_no_discard;

/** What the role forbids. Zero for an index no role has. */
NYA_API NYA_Permission nya_permission_role_forbids(const NYA_Permissions* permissions, u32 role) __attr_no_discard;

/** How many roles the table holds, `@everyone` included. */
NYA_API u32 nya_permission_role_count(const NYA_Permissions* permissions) __attr_no_discard;

// ───────────────────────────────────── SUBJECTS ─────────────────────────────────────

/**
 * Gives `subject` the role, adding the subject to the table if it is new.
 *
 * NYA_ERROR_PERMISSION_DENIED when the actor lacks MANAGE_ROLES, when the role is at or above the actor's rank,
 * when the actor does not itself hold every permission the role allows, or when the actor does not
 * outrank the subject. That last one is what stops an officer from arming a rival.
 * */
NYA_API NYA_Error nya_permission_role_grant(NYA_Permissions* permissions, u64 actor, u64 subject, u32 role, u64 now_s) __attr_no_discard;

/** Takes the role away again, under the same rules. Revoking `@everyone` is refused: everyone holds it. */
NYA_API NYA_Error nya_permission_role_revoke(NYA_Permissions* permissions, u64 actor, u64 subject, u32 role, u64 now_s) __attr_no_discard;

/** The roles the subject holds as a bit per role index, `@everyone` included. */
NYA_API u64 nya_permission_subject_roles(const NYA_Permissions* permissions, u64 subject) __attr_no_discard;

/** The highest position among the subject's roles. The owner answers U16_MAX. */
NYA_API u16 nya_permission_subject_rank(const NYA_Permissions* permissions, u64 subject) __attr_no_discard;

/**
 * Sets what this subject is forbidden directly, absolutely, whatever their roles allow.
 *
 * The subject-level twin of nya_permission_role_forbid_set: this is "ada may never post here", set on
 * the person rather than on a role, and it wins over every allow below the owner the same way. Needs
 * MANAGE_ROLES and the actor to outrank the subject, since forbidding somebody who outranks you would
 * be reaching up the hierarchy to disarm them.
 * */
NYA_API NYA_Error nya_permission_subject_forbid_set(NYA_Permissions* permissions, u64 actor, u64 subject, NYA_Permission forbid, u64 now_s)
    __attr_no_discard;

/** What this subject is forbidden directly. Zero for a subject with nothing forbidden. */
NYA_API NYA_Permission nya_permission_subject_forbids(const NYA_Permissions* permissions, u64 subject) __attr_no_discard;

/** How many subjects the table knows about. A subject it has never seen still resolves, to nothing. */
NYA_API u32 nya_permission_subject_count(const NYA_Permissions* permissions) __attr_no_discard;

/**
 * Makes `subject` the owner: above every role, every deny and every check. Only the current owner or
 * NYA_PERMISSION_SYSTEM may do this, which is what stops the only unbounded permission from spreading.
 * */
NYA_API NYA_Error nya_permissions_owner_set(NYA_Permissions* permissions, u64 actor, u64 subject, u64 now_s) __attr_no_discard;

/** The owner, or NYA_PERMISSION_SYSTEM when the table has none. */
NYA_API u64 nya_permissions_owner(const NYA_Permissions* permissions) __attr_no_discard;

// ───────────────────────────────────── OVERWRITES ─────────────────────────────────────

/**
 * Sets one resource's overwrite for a role or a subject, replacing whatever was there.
 *
 * A bit in both `allow` and `deny` is NYA_ERROR_INVALID_ARGUMENT rather than a precedence rule nobody
 * remembers. An actor may only write bits it holds on that resource, which is what keeps an overwrite
 * from being the way around the hierarchy.
 * */
NYA_API NYA_Error nya_permission_overwrite_set(NYA_Permissions* permissions, u64 actor, u64 resource, NYA_PermissionTarget target, u64 id,
                                               NYA_Permission allow, NYA_Permission deny, u64 now_s) __attr_no_discard;

/** Removes it again, under the same rule. Absent is not an error: the end state is what was asked for. */
NYA_API NYA_Error nya_permission_overwrite_clear(NYA_Permissions* permissions, u64 actor, u64 resource, NYA_PermissionTarget target, u64 id, u64 now_s)
    __attr_no_discard;

/** The overwrite as stored, or false when the resource carries none for that target. */
NYA_API b8 nya_permission_overwrite_get(const NYA_Permissions* permissions, u64 resource, NYA_PermissionTarget target, u64 id,
                                        OUT NYA_Permission* out_allow, OUT NYA_Permission* out_deny) __attr_no_discard;

/** How many overwrites the table holds, across every resource. */
NYA_API u32 nya_permission_overwrite_count(const NYA_Permissions* permissions) __attr_no_discard;

// ───────────────────────────────────── LABELS ─────────────────────────────────────

// What each bit is called, so a role editor is written once and works over any program's permissions; the engine labels its own three at creation.

/**
 * Names one bit. NYA_ERROR_INVALID_ARGUMENT when `bit` is not exactly one bit or the name does not fit,
 * which is what stops a label from being attached to a set nobody can toggle.
 * */
NYA_API NYA_Error nya_permission_label_set(NYA_Permissions* permissions, NYA_Permission bit, NYA_ConstCString name) __attr_no_discard;

/** The name of one bit, or "" for a bit nobody has named. */
NYA_API NYA_ConstCString nya_permission_label(const NYA_Permissions* permissions, NYA_Permission bit) __attr_no_discard;

/** Every bit that has a name, as a set, which is what an editor walks to draw its rows. */
NYA_API NYA_Permission nya_permission_labelled(const NYA_Permissions* permissions) __attr_no_discard;

// ───────────────────────────────────── RESOLUTION ─────────────────────────────────────

/**
 * Everything `subject` may do to `resource`, by the six steps in the file note.
 *
 * A pure function of the table: the same table and the same two ids give the same answer every time,
 * which is what lets a test compare it against a naive oracle and what makes it safe to call per
 * request, per tick, or per rendered row.
 *
 * `resource` names whatever a program calls a resource; zero means "no particular resource", which
 * resolves the roles alone and is what a check about the guild rather than about a room asks.
 * */
NYA_API NYA_Permission nya_permission_resolve(const NYA_Permissions* permissions, u64 subject, u64 resource) __attr_no_discard;

/** Whether resolving gives every bit of `required`. The call sites read better than the AND does. */
NYA_API b8 nya_permission_has(const NYA_Permissions* permissions, u64 subject, u64 resource, NYA_Permission required) __attr_no_discard;

/**
 * Whether `subject` may act on a thing `owner` owns, holding either the any-permission or the
 * own-permission-plus-ownership.
 *
 * The `CanEditOwnPost` beside `CanEditAnyPost` split: a program gives moderators `edit_any` and every
 * member `edit_own`, and this is the one check a route makes rather than writing the OR by hand each
 * time and forgetting the ownership half in one of them. True when the subject holds `any` on the
 * resource, or holds `own` on it and is the owner. `owner` is whatever a program stored as the thing's
 * owner; a subject acting on their own thing passes `subject == owner`.
 *
 * `resource` is the same resource id the rest of resolution uses, so a per-room overwrite still applies
 * — `edit_own` denied in one room is denied there even to the owner.
 * */
NYA_API b8 nya_permission_may_act(
    const NYA_Permissions* permissions, u64 subject, u64 resource, u64 owner, NYA_Permission any, NYA_Permission own
) __attr_no_discard;

/**
 * Whether `actor` outranks `subject`, which is what every change here is gated on.
 *
 * The owner outranks everyone, including another owner-less table's idea of one. Equal ranks do not
 * outrank each other, and nobody outranks themselves, so an officer cannot demote a fellow officer and
 * cannot demote themselves by accident either.
 * */
NYA_API b8 nya_permission_outranks(const NYA_Permissions* permissions, u64 actor, u64 subject) __attr_no_discard;

// ───────────────────────────────────── AUDIT ─────────────────────────────────────

/** How many entries the ring holds right now, at most NYA_PERMISSION_MAX_AUDIT. */
NYA_API u32 nya_permission_audit_count(const NYA_Permissions* permissions) __attr_no_discard;

/** Entry `index`, oldest first, or false past the count. */
NYA_API b8 nya_permission_audit_at(const NYA_Permissions* permissions, u32 index, OUT NYA_PermissionAudit* out_entry) __attr_no_discard;

/** How many entries the ring has dropped since the table was made, which a report says out loud. */
NYA_API u64 nya_permission_audit_dropped(const NYA_Permissions* permissions) __attr_no_discard;

#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_ceiling.h"
#include "nyangine/permission/permission.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    char           name[NYA_PERMISSION_MAX_ROLE_NAME];
    u16            position;
    NYA_Permission allow;
    b8             used;
} _NYA_PermissionRole;

typedef struct {
    u64 id;

    /** One bit per role index, so "which roles" is one word and "does it hold this one" is one AND. */
    u64 roles;

    b8 used;
} _NYA_PermissionSubject;

typedef struct {
    u64                  resource;
    NYA_PermissionTarget target;

    /** A role index or a subject id, by `target`. */
    u64 id;

    NYA_Permission allow;
    NYA_Permission deny;

    b8 used;
} _NYA_PermissionOverwrite;

struct NYA_Permissions {
    _NYA_PermissionRole      roles[NYA_PERMISSION_MAX_ROLES];
    _NYA_PermissionSubject   subjects[NYA_PERMISSION_MAX_SUBJECTS];
    _NYA_PermissionOverwrite overwrites[NYA_PERMISSION_MAX_OVERWRITES];

    /** What nya_ceiling_register publishes, so an overlay shows how full each table is. */
    u32 role_count;
    u32 subject_count;
    u32 overwrite_count;

    u64 owner;

    char labels[64][NYA_PERMISSION_MAX_LABEL];

    NYA_PermissionAudit audit[NYA_PERMISSION_MAX_AUDIT];
    u32                 audit_count;
    u32                 audit_first;
    u64                 audit_dropped;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The subject's row, or null when the table has never seen it. */
NYA_INTERNAL const _NYA_PermissionSubject* _nya_permission_subject_find(const NYA_Permissions* permissions, u64 subject) __attr_no_discard;

/** The subject's row, made if it is new. Null only when the table is full. */
NYA_INTERNAL _NYA_PermissionSubject* _nya_permission_subject_get(NYA_Permissions* permissions, u64 subject) __attr_no_discard;

/** The overwrite for one resource and one target, or null. */
NYA_INTERNAL _NYA_PermissionOverwrite* _nya_permission_overwrite_find(const NYA_Permissions* permissions, u64 resource, NYA_PermissionTarget target,
                                                                      u64 id) __attr_no_discard;

/** Appends one audit entry, dropping the oldest when the ring is full. */
NYA_INTERNAL void _nya_permission_audit(NYA_Permissions* permissions, const NYA_PermissionAudit* entry);

/** Whether the actor may make a change at all: the owner, the system, or a holder of `required`. */
NYA_INTERNAL b8 _nya_permission_may(const NYA_Permissions* permissions, u64 actor, NYA_Permission required) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Permissions* nya_permissions_create(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    NYA_Permissions* permissions = nya_arena_alloc(arena, sizeof(NYA_Permissions));
    if (permissions == nullptr) return nullptr;

    // field by field after a memset: a compound literal of the whole table is a stack temporary of its size.
    nya_memset(permissions, 0, sizeof(*permissions));

    // @everyone exists from the start and allows nothing, which is the deny by default.
    permissions->roles[NYA_PERMISSION_ROLE_EVERYONE] = (_NYA_PermissionRole){ .position = 0, .allow = NYA_PERMISSION_NONE, .used = true };
    (void)snprintf(permissions->roles[NYA_PERMISSION_ROLE_EVERYONE].name, NYA_PERMISSION_MAX_ROLE_NAME, "%s", "@everyone");

    permissions->role_count = 1;
    permissions->owner      = NYA_PERMISSION_SYSTEM;

    // the engine's own three, so an editor shows them without every program labelling them again.
    NYA_EXPECT(nya_permission_label_set(permissions, NYA_PERMISSION_ADMINISTRATOR, "administrator"), "while labelling ADMINISTRATOR");
    NYA_EXPECT(nya_permission_label_set(permissions, NYA_PERMISSION_MANAGE_ROLES, "manage roles"), "while labelling MANAGE_ROLES");
    NYA_EXPECT(nya_permission_label_set(permissions, NYA_PERMISSION_MANAGE_SUBJECTS, "manage members"), "while labelling MANAGE_SUBJECTS");

    nya_ceiling_register("permission_roles", NYA_PERMISSION_MAX_ROLES, &permissions->role_count);
    nya_ceiling_register("permission_subjects", NYA_PERMISSION_MAX_SUBJECTS, &permissions->subject_count);
    nya_ceiling_register("permission_overwrites", NYA_PERMISSION_MAX_OVERWRITES, &permissions->overwrite_count);

    return permissions;
}

void nya_permissions_destroy(NYA_Permissions* permissions) {
    nya_assert(permissions != nullptr);

    // the ceilings keep pointing at the same counters, which is why this clears rather than reallocates.
    nya_memset(permissions->roles, 0, sizeof(permissions->roles));
    nya_memset(permissions->subjects, 0, sizeof(permissions->subjects));
    nya_memset(permissions->overwrites, 0, sizeof(permissions->overwrites));
    nya_memset(permissions->audit, 0, sizeof(permissions->audit));

    permissions->subject_count   = 0;
    permissions->overwrite_count = 0;
    permissions->audit_count     = 0;
    permissions->audit_first     = 0;
    permissions->audit_dropped   = 0;
    permissions->owner           = NYA_PERMISSION_SYSTEM;

    permissions->roles[NYA_PERMISSION_ROLE_EVERYONE] = (_NYA_PermissionRole){ .position = 0, .allow = NYA_PERMISSION_NONE, .used = true };
    (void)snprintf(permissions->roles[NYA_PERMISSION_ROLE_EVERYONE].name, NYA_PERMISSION_MAX_ROLE_NAME, "%s", "@everyone");

    permissions->role_count = 1;
}

NYA_Error nya_permission_role_add(NYA_Permissions* permissions, u64 actor, NYA_ConstCString name, u16 position, NYA_Permission allow, u64 now_s,
                                  u32* out_role) {
    nya_assert(permissions != nullptr);
    nya_assert(out_role != nullptr);

    *out_role = NYA_PERMISSION_MAX_ROLES;

    if (name == nullptr || name[0] == '\0' || strlen(name) >= NYA_PERMISSION_MAX_ROLE_NAME) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a role has a name of 1 to %d bytes", NYA_PERMISSION_MAX_ROLE_NAME - 1);
    }

    // position zero belongs to @everyone, and a second role there would outrank nothing and be outranked
    // by nothing, which is a role that cannot be managed by anyone but the owner.
    if (position == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "position 0 is @everyone's");

    if (!_nya_permission_may(permissions, actor, NYA_PERMISSION_MANAGE_ROLES)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "'%s' needs MANAGE_ROLES", name);
    }

    u16 rank = nya_permission_subject_rank(permissions, actor);

    // a role at or above the actor's own rank is a role the actor could then not be removed by.
    if (actor != NYA_PERMISSION_SYSTEM && actor != permissions->owner && position >= rank) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "'%s' would sit at or above its author", name);
    }

    // and nobody hands out what they do not hold, which is the whole escalation story in one check.
    NYA_Permission held = nya_permission_resolve(permissions, actor, 0);
    if (actor != NYA_PERMISSION_SYSTEM && (allow & ~held) != 0) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "'%s' would allow what its author does not hold", name);
    }

    if (permissions->role_count >= NYA_PERMISSION_MAX_ROLES) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a table holds %d roles", NYA_PERMISSION_MAX_ROLES);
    }

    u32 role = permissions->role_count++;

    permissions->roles[role] = (_NYA_PermissionRole){ .position = position, .allow = allow, .used = true };
    (void)snprintf(permissions->roles[role].name, NYA_PERMISSION_MAX_ROLE_NAME, "%s", name);

    _nya_permission_audit(permissions, &(NYA_PermissionAudit){
                                           .change      = NYA_PERMISSION_CHANGE_ROLE_ADDED,
                                           .actor       = actor,
                                           .role        = role,
                                           .after_allow = allow,
                                           .at_s        = now_s,
                                       });

    *out_role = role;

    return NYA_OK;
}

NYA_Error nya_permission_role_edit(NYA_Permissions* permissions, u64 actor, u32 role, u16 position, NYA_Permission allow, u64 now_s) {
    nya_assert(permissions != nullptr);

    if (role >= permissions->role_count || !permissions->roles[role].used) {
        return nya_error(NYA_ERROR_NOT_FOUND, "there is no role %u", role);
    }

    if (role == NYA_PERMISSION_ROLE_EVERYONE && position != 0) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "@everyone stays at position 0");
    }

    if (position == 0 && role != NYA_PERMISSION_ROLE_EVERYONE) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "position 0 is @everyone's");

    if (!_nya_permission_may(permissions, actor, NYA_PERMISSION_MANAGE_ROLES)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "editing a role needs MANAGE_ROLES");
    }

    b8  unbounded = actor == NYA_PERMISSION_SYSTEM || actor == permissions->owner;
    u16 rank      = nya_permission_subject_rank(permissions, actor);

    if (!unbounded && (permissions->roles[role].position >= rank || position >= rank)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "a role at or above the actor's rank is not theirs to edit");
    }

    NYA_Permission held = nya_permission_resolve(permissions, actor, 0);
    if (!unbounded && (allow & ~held) != 0) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "a role cannot be given what its editor does not hold");
    }

    NYA_Permission before = permissions->roles[role].allow;

    permissions->roles[role].position = position;
    permissions->roles[role].allow    = allow;

    _nya_permission_audit(permissions, &(NYA_PermissionAudit){
                                           .change       = NYA_PERMISSION_CHANGE_ROLE_EDITED,
                                           .actor        = actor,
                                           .role         = role,
                                           .before_allow = before,
                                           .after_allow  = allow,
                                           .at_s         = now_s,
                                       });

    return NYA_OK;
}

NYA_Error nya_permission_role_remove(NYA_Permissions* permissions, u64 actor, u32 role, u64 now_s) {
    nya_assert(permissions != nullptr);

    if (role >= permissions->role_count || !permissions->roles[role].used) return nya_error(NYA_ERROR_NOT_FOUND, "there is no role %u", role);
    if (role == NYA_PERMISSION_ROLE_EVERYONE) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "@everyone cannot be removed");

    if (!_nya_permission_may(permissions, actor, NYA_PERMISSION_MANAGE_ROLES)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "removing a role needs MANAGE_ROLES");
    }

    b8 unbounded = actor == NYA_PERMISSION_SYSTEM || actor == permissions->owner;

    if (!unbounded && permissions->roles[role].position >= nya_permission_subject_rank(permissions, actor)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "a role at or above the actor's rank is not theirs to remove");
    }

    NYA_Permission before = permissions->roles[role].allow;

    // taken from everyone holding it, and every overwrite about it dropped: an index that is reused by
    // the next role added must not inherit a single bit of what this one meant.
    for (u32 index = 0; index < NYA_PERMISSION_MAX_SUBJECTS; index++) {
        if (permissions->subjects[index].used) permissions->subjects[index].roles &= ~(1ULL << role);
    }

    for (u32 index = 0; index < NYA_PERMISSION_MAX_OVERWRITES; index++) {
        _NYA_PermissionOverwrite* overwrite = &permissions->overwrites[index];

        if (!overwrite->used || overwrite->target != NYA_PERMISSION_TARGET_ROLE || overwrite->id != role) continue;

        *overwrite = (_NYA_PermissionOverwrite){ 0 };
        permissions->overwrite_count--;
    }

    permissions->roles[role] = (_NYA_PermissionRole){ 0 };

    // the count is the high water mark rather than a population, since an index is a bit position and
    // reusing one for a different role would change what a subject's role word means.
    _nya_permission_audit(permissions, &(NYA_PermissionAudit){
                                           .change       = NYA_PERMISSION_CHANGE_ROLE_REMOVED,
                                           .actor        = actor,
                                           .role         = role,
                                           .before_allow = before,
                                           .at_s         = now_s,
                                       });

    return NYA_OK;
}

NYA_ConstCString nya_permission_role_name(const NYA_Permissions* permissions, u32 role) {
    nya_assert(permissions != nullptr);

    if (role >= permissions->role_count || !permissions->roles[role].used) return nullptr;

    return permissions->roles[role].name;
}

u16 nya_permission_role_position(const NYA_Permissions* permissions, u32 role) {
    nya_assert(permissions != nullptr);

    if (role >= permissions->role_count || !permissions->roles[role].used) return 0;

    return permissions->roles[role].position;
}

NYA_Permission nya_permission_role_allows(const NYA_Permissions* permissions, u32 role) {
    nya_assert(permissions != nullptr);

    if (role >= permissions->role_count || !permissions->roles[role].used) return NYA_PERMISSION_NONE;

    return permissions->roles[role].allow;
}

u32 nya_permission_role_count(const NYA_Permissions* permissions) {
    nya_assert(permissions != nullptr);

    return permissions->role_count;
}

NYA_Error nya_permission_role_grant(NYA_Permissions* permissions, u64 actor, u64 subject, u32 role, u64 now_s) {
    nya_assert(permissions != nullptr);

    if (role >= permissions->role_count || !permissions->roles[role].used) return nya_error(NYA_ERROR_NOT_FOUND, "there is no role %u", role);
    if (role == NYA_PERMISSION_ROLE_EVERYONE) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "everyone already holds @everyone");

    if (!_nya_permission_may(permissions, actor, NYA_PERMISSION_MANAGE_ROLES)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "granting a role needs MANAGE_ROLES");
    }

    b8 unbounded = actor == NYA_PERMISSION_SYSTEM || actor == permissions->owner;

    if (!unbounded) {
        u16 rank = nya_permission_subject_rank(permissions, actor);

        if (permissions->roles[role].position >= rank) return nya_error(NYA_ERROR_PERMISSION_DENIED, "that role is not below the actor's rank");

        // the subject too: an officer arming a rival of equal rank is the escalation this refuses.
        if (!nya_permission_outranks(permissions, actor, subject)) {
            return nya_error(NYA_ERROR_PERMISSION_DENIED, "the actor does not outrank that subject");
        }

        NYA_Permission held = nya_permission_resolve(permissions, actor, 0);
        if ((permissions->roles[role].allow & ~held) != 0) {
            return nya_error(NYA_ERROR_PERMISSION_DENIED, "that role allows what the actor does not hold");
        }
    }

    _NYA_PermissionSubject* row = _nya_permission_subject_get(permissions, subject);
    if (row == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a table holds %d subjects", NYA_PERMISSION_MAX_SUBJECTS);

    row->roles |= 1ULL << role;

    _nya_permission_audit(permissions, &(NYA_PermissionAudit){
                                           .change  = NYA_PERMISSION_CHANGE_ROLE_GRANTED,
                                           .actor   = actor,
                                           .subject = subject,
                                           .role    = role,
                                           .at_s    = now_s,
                                       });

    return NYA_OK;
}

NYA_Error nya_permission_role_revoke(NYA_Permissions* permissions, u64 actor, u64 subject, u32 role, u64 now_s) {
    nya_assert(permissions != nullptr);

    if (role >= permissions->role_count || !permissions->roles[role].used) return nya_error(NYA_ERROR_NOT_FOUND, "there is no role %u", role);
    if (role == NYA_PERMISSION_ROLE_EVERYONE) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "@everyone cannot be revoked");

    if (!_nya_permission_may(permissions, actor, NYA_PERMISSION_MANAGE_ROLES)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "revoking a role needs MANAGE_ROLES");
    }

    b8 unbounded = actor == NYA_PERMISSION_SYSTEM || actor == permissions->owner;

    if (!unbounded) {
        u16 rank = nya_permission_subject_rank(permissions, actor);

        if (permissions->roles[role].position >= rank) return nya_error(NYA_ERROR_PERMISSION_DENIED, "that role is not below the actor's rank");
        if (!nya_permission_outranks(permissions, actor, subject)) {
            return nya_error(NYA_ERROR_PERMISSION_DENIED, "the actor does not outrank that subject");
        }
    }

    _NYA_PermissionSubject* row = _nya_permission_subject_get(permissions, subject);
    if (row == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a table holds %d subjects", NYA_PERMISSION_MAX_SUBJECTS);

    row->roles &= ~(1ULL << role);

    _nya_permission_audit(permissions, &(NYA_PermissionAudit){
                                           .change  = NYA_PERMISSION_CHANGE_ROLE_REVOKED,
                                           .actor   = actor,
                                           .subject = subject,
                                           .role    = role,
                                           .at_s    = now_s,
                                       });

    return NYA_OK;
}

u64 nya_permission_subject_roles(const NYA_Permissions* permissions, u64 subject) {
    nya_assert(permissions != nullptr);

    const _NYA_PermissionSubject* row = _nya_permission_subject_find(permissions, subject);

    // @everyone whether the table has seen this subject or not, which is what makes an unknown subject
    // resolve to the same thing as a known one holding nothing.
    return (row != nullptr ? row->roles : 0ULL) | (1ULL << NYA_PERMISSION_ROLE_EVERYONE);
}

u16 nya_permission_subject_rank(const NYA_Permissions* permissions, u64 subject) {
    nya_assert(permissions != nullptr);

    if (subject == NYA_PERMISSION_SYSTEM || (permissions->owner != NYA_PERMISSION_SYSTEM && subject == permissions->owner)) return U16_MAX;

    u64 roles = nya_permission_subject_roles(permissions, subject);
    u16 rank  = 0;

    for (u32 role = 0; role < permissions->role_count; role++) {
        if ((roles & (1ULL << role)) == 0 || !permissions->roles[role].used) continue;

        rank = nya_max(rank, permissions->roles[role].position);
    }

    return rank;
}

u32 nya_permission_subject_count(const NYA_Permissions* permissions) {
    nya_assert(permissions != nullptr);

    return permissions->subject_count;
}

NYA_Error nya_permissions_owner_set(NYA_Permissions* permissions, u64 actor, u64 subject, u64 now_s) {
    nya_assert(permissions != nullptr);

    // the only unbounded permission there is, so the only ones who may hand it over are the program
    // itself and whoever already holds it.
    if (actor != NYA_PERMISSION_SYSTEM && actor != permissions->owner) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "only the owner or the program itself sets the owner");
    }

    u64 before = permissions->owner;

    permissions->owner = subject;

    _nya_permission_audit(permissions, &(NYA_PermissionAudit){
                                           .change   = NYA_PERMISSION_CHANGE_OWNER_SET,
                                           .actor    = actor,
                                           .subject  = subject,
                                           .resource = before,
                                           .role     = NYA_PERMISSION_MAX_ROLES,
                                           .at_s     = now_s,
                                       });

    return NYA_OK;
}

u64 nya_permissions_owner(const NYA_Permissions* permissions) {
    nya_assert(permissions != nullptr);

    return permissions->owner;
}

NYA_Error nya_permission_overwrite_set(NYA_Permissions* permissions, u64 actor, u64 resource, NYA_PermissionTarget target, u64 id,
                                       NYA_Permission allow, NYA_Permission deny, u64 now_s) {
    nya_assert(permissions != nullptr);

    if (target >= NYA_PERMISSION_TARGET_COUNT) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an overwrite names a role or a subject");

    // both at once is a precedence rule nobody remembers correctly; the caller says which one it meant.
    if ((allow & deny) != 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a permission is allowed or denied, not both");

    if (target == NYA_PERMISSION_TARGET_ROLE && (id >= permissions->role_count || !permissions->roles[id].used)) {
        return nya_error(NYA_ERROR_NOT_FOUND, "there is no role %llu", (unsigned long long)id);
    }

    if (!_nya_permission_may(permissions, actor, NYA_PERMISSION_MANAGE_ROLES)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "an overwrite needs MANAGE_ROLES");
    }

    b8 unbounded = actor == NYA_PERMISSION_SYSTEM || actor == permissions->owner;

    if (!unbounded) {
        // on that resource, not in general: an actor denied a permission here cannot hand it out here.
        NYA_Permission held = nya_permission_resolve(permissions, actor, resource);

        if (((allow | deny) & ~held) != 0) return nya_error(NYA_ERROR_PERMISSION_DENIED, "an overwrite cannot name what the actor does not hold here");

        if (target == NYA_PERMISSION_TARGET_ROLE) {
            u16 rank = nya_permission_subject_rank(permissions, actor);
            if (permissions->roles[id].position >= rank) return nya_error(NYA_ERROR_PERMISSION_DENIED, "that role is not below the actor's rank");
        } else if (!nya_permission_outranks(permissions, actor, id)) {
            return nya_error(NYA_ERROR_PERMISSION_DENIED, "the actor does not outrank that subject");
        }
    }

    _NYA_PermissionOverwrite* overwrite = _nya_permission_overwrite_find(permissions, resource, target, id);

    if (overwrite == nullptr) {
        for (u32 index = 0; index < NYA_PERMISSION_MAX_OVERWRITES && overwrite == nullptr; index++) {
            if (!permissions->overwrites[index].used) overwrite = &permissions->overwrites[index];
        }

        if (overwrite == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a table holds %d overwrites", NYA_PERMISSION_MAX_OVERWRITES);

        *overwrite = (_NYA_PermissionOverwrite){ .resource = resource, .target = target, .id = id, .used = true };
        permissions->overwrite_count++;
    }

    NYA_Permission before_allow = overwrite->allow;
    NYA_Permission before_deny  = overwrite->deny;

    overwrite->allow = allow;
    overwrite->deny  = deny;

    _nya_permission_audit(permissions, &(NYA_PermissionAudit){
                                           .change       = NYA_PERMISSION_CHANGE_OVERWRITE_SET,
                                           .actor        = actor,
                                           .subject      = target == NYA_PERMISSION_TARGET_SUBJECT ? id : 0,
                                           .resource     = resource,
                                           .role         = target == NYA_PERMISSION_TARGET_ROLE ? (u32)id : NYA_PERMISSION_MAX_ROLES,
                                           .before_allow = before_allow,
                                           .before_deny  = before_deny,
                                           .after_allow  = allow,
                                           .after_deny   = deny,
                                           .at_s         = now_s,
                                       });

    return NYA_OK;
}

NYA_Error nya_permission_overwrite_clear(NYA_Permissions* permissions, u64 actor, u64 resource, NYA_PermissionTarget target, u64 id, u64 now_s) {
    nya_assert(permissions != nullptr);

    if (target >= NYA_PERMISSION_TARGET_COUNT) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an overwrite names a role or a subject");

    if (!_nya_permission_may(permissions, actor, NYA_PERMISSION_MANAGE_ROLES)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "an overwrite needs MANAGE_ROLES");
    }

    _NYA_PermissionOverwrite* overwrite = _nya_permission_overwrite_find(permissions, resource, target, id);

    // absent is not an error: what the caller asked for is the end state, and it already holds.
    if (overwrite == nullptr) return NYA_OK;

    b8 unbounded = actor == NYA_PERMISSION_SYSTEM || actor == permissions->owner;

    if (!unbounded) {
        NYA_Permission held = nya_permission_resolve(permissions, actor, resource);

        // removing a deny is granting, so removal is held to the same rule as writing it.
        if (((overwrite->allow | overwrite->deny) & ~held) != 0) {
            return nya_error(NYA_ERROR_PERMISSION_DENIED, "that overwrite names what the actor does not hold here");
        }
    }

    NYA_PermissionAudit entry = {
        .change       = NYA_PERMISSION_CHANGE_OVERWRITE_CLEARED,
        .actor        = actor,
        .subject      = target == NYA_PERMISSION_TARGET_SUBJECT ? id : 0,
        .resource     = resource,
        .role         = target == NYA_PERMISSION_TARGET_ROLE ? (u32)id : NYA_PERMISSION_MAX_ROLES,
        .before_allow = overwrite->allow,
        .before_deny  = overwrite->deny,
        .at_s         = now_s,
    };

    *overwrite = (_NYA_PermissionOverwrite){ 0 };
    permissions->overwrite_count--;

    _nya_permission_audit(permissions, &entry);

    return NYA_OK;
}

b8 nya_permission_overwrite_get(const NYA_Permissions* permissions, u64 resource, NYA_PermissionTarget target, u64 id, NYA_Permission* out_allow,
                                NYA_Permission* out_deny) {
    nya_assert(permissions != nullptr);
    nya_assert(out_allow != nullptr && out_deny != nullptr);

    *out_allow = NYA_PERMISSION_NONE;
    *out_deny  = NYA_PERMISSION_NONE;

    const _NYA_PermissionOverwrite* overwrite = _nya_permission_overwrite_find(permissions, resource, target, id);
    if (overwrite == nullptr) return false;

    *out_allow = overwrite->allow;
    *out_deny  = overwrite->deny;

    return true;
}

u32 nya_permission_overwrite_count(const NYA_Permissions* permissions) {
    nya_assert(permissions != nullptr);

    return permissions->overwrite_count;
}

NYA_Error nya_permission_label_set(NYA_Permissions* permissions, NYA_Permission bit, NYA_ConstCString name) {
    nya_assert(permissions != nullptr);

    // exactly one bit: a label on a set would be a row an editor cannot toggle without touching others.
    if (bit == 0 || (bit & (bit - 1)) != 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a label names one permission, not a set");

    if (name == nullptr || name[0] == '\0' || strlen(name) >= NYA_PERMISSION_MAX_LABEL) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a label is 1 to %d bytes", NYA_PERMISSION_MAX_LABEL - 1);
    }

    u32 index = 0;
    while ((bit >> index) != 1ULL) index++;

    (void)snprintf(permissions->labels[index], NYA_PERMISSION_MAX_LABEL, "%s", name);

    return NYA_OK;
}

NYA_ConstCString nya_permission_label(const NYA_Permissions* permissions, NYA_Permission bit) {
    nya_assert(permissions != nullptr);

    if (bit == 0 || (bit & (bit - 1)) != 0) return "";

    u32 index = 0;
    while ((bit >> index) != 1ULL) index++;

    return permissions->labels[index];
}

NYA_Permission nya_permission_labelled(const NYA_Permissions* permissions) {
    nya_assert(permissions != nullptr);

    NYA_Permission named = NYA_PERMISSION_NONE;

    for (u32 index = 0; index < 64; index++) {
        if (permissions->labels[index][0] != '\0') named |= 1ULL << index;
    }

    return named;
}

NYA_Permission nya_permission_resolve(const NYA_Permissions* permissions, u64 subject, u64 resource) {
    nya_assert(permissions != nullptr);

    // the program itself, and the owner: above every role and every deny. Checked first so that a table
    // whose owner has been denied something on a resource still answers "the owner may".
    if (subject == NYA_PERMISSION_SYSTEM || (permissions->owner != NYA_PERMISSION_SYSTEM && subject == permissions->owner)) return ~0ULL;

    u64            roles = nya_permission_subject_roles(permissions, subject);
    NYA_Permission base  = NYA_PERMISSION_NONE;

    for (u32 role = 0; role < permissions->role_count; role++) {
        if ((roles & (1ULL << role)) == 0 || !permissions->roles[role].used) continue;

        base |= permissions->roles[role].allow;
    }

    // ADMINISTRATOR is every permission on every resource, so it does not look at an overwrite at all.
    if ((base & NYA_PERMISSION_ADMINISTRATOR) != 0) return ~0ULL;

    const _NYA_PermissionOverwrite* everyone =
        _nya_permission_overwrite_find(permissions, resource, NYA_PERMISSION_TARGET_ROLE, NYA_PERMISSION_ROLE_EVERYONE);

    if (everyone != nullptr) {
        base &= ~everyone->deny;
        base |= everyone->allow;
    }

    /*
     * Every other held role's denies together, and then their allows together — not role by role. The
     * difference shows when one role denies what another allows: taken together the allow wins, which is
     * what makes adding a role never take a permission away.
     */
    NYA_Permission role_deny  = NYA_PERMISSION_NONE;
    NYA_Permission role_allow = NYA_PERMISSION_NONE;

    for (u32 index = 0; index < NYA_PERMISSION_MAX_OVERWRITES; index++) {
        const _NYA_PermissionOverwrite* overwrite = &permissions->overwrites[index];

        if (!overwrite->used || overwrite->resource != resource || overwrite->target != NYA_PERMISSION_TARGET_ROLE) continue;
        if (overwrite->id == NYA_PERMISSION_ROLE_EVERYONE || (roles & (1ULL << overwrite->id)) == 0) continue;

        role_deny  |= overwrite->deny;
        role_allow |= overwrite->allow;
    }

    base &= ~role_deny;
    base |= role_allow;

    const _NYA_PermissionOverwrite* own = _nya_permission_overwrite_find(permissions, resource, NYA_PERMISSION_TARGET_SUBJECT, subject);

    if (own != nullptr) {
        base &= ~own->deny;
        base |= own->allow;
    }

    return base;
}

b8 nya_permission_has(const NYA_Permissions* permissions, u64 subject, u64 resource, NYA_Permission required) {
    return (nya_permission_resolve(permissions, subject, resource) & required) == required;
}

b8 nya_permission_outranks(const NYA_Permissions* permissions, u64 actor, u64 subject) {
    nya_assert(permissions != nullptr);

    // nobody outranks themselves, so an officer cannot demote themselves by accident either.
    if (actor == subject) return false;

    if (actor == NYA_PERMISSION_SYSTEM) return true;
    if (permissions->owner != NYA_PERMISSION_SYSTEM && subject == permissions->owner) return false;
    if (permissions->owner != NYA_PERMISSION_SYSTEM && actor == permissions->owner) return true;

    return nya_permission_subject_rank(permissions, actor) > nya_permission_subject_rank(permissions, subject);
}

u32 nya_permission_audit_count(const NYA_Permissions* permissions) {
    nya_assert(permissions != nullptr);

    return permissions->audit_count;
}

b8 nya_permission_audit_at(const NYA_Permissions* permissions, u32 index, NYA_PermissionAudit* out_entry) {
    nya_assert(permissions != nullptr);
    nya_assert(out_entry != nullptr);

    *out_entry = (NYA_PermissionAudit){ 0 };

    if (index >= permissions->audit_count) return false;

    *out_entry = permissions->audit[(permissions->audit_first + index) % NYA_PERMISSION_MAX_AUDIT];

    return true;
}

u64 nya_permission_audit_dropped(const NYA_Permissions* permissions) {
    nya_assert(permissions != nullptr);

    return permissions->audit_dropped;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

const _NYA_PermissionSubject* _nya_permission_subject_find(const NYA_Permissions* permissions, u64 subject) {
    for (u32 index = 0; index < NYA_PERMISSION_MAX_SUBJECTS; index++) {
        if (permissions->subjects[index].used && permissions->subjects[index].id == subject) return &permissions->subjects[index];
    }

    return nullptr;
}

_NYA_PermissionSubject* _nya_permission_subject_get(NYA_Permissions* permissions, u64 subject) {
    for (u32 index = 0; index < NYA_PERMISSION_MAX_SUBJECTS; index++) {
        if (permissions->subjects[index].used && permissions->subjects[index].id == subject) return &permissions->subjects[index];
    }

    for (u32 index = 0; index < NYA_PERMISSION_MAX_SUBJECTS; index++) {
        if (permissions->subjects[index].used) continue;

        permissions->subjects[index] = (_NYA_PermissionSubject){ .id = subject, .roles = 0, .used = true };
        permissions->subject_count++;

        return &permissions->subjects[index];
    }

    return nullptr;
}

_NYA_PermissionOverwrite* _nya_permission_overwrite_find(const NYA_Permissions* permissions, u64 resource, NYA_PermissionTarget target, u64 id) {
    for (u32 index = 0; index < NYA_PERMISSION_MAX_OVERWRITES; index++) {
        const _NYA_PermissionOverwrite* overwrite = &permissions->overwrites[index];

        if (!overwrite->used || overwrite->resource != resource || overwrite->target != target || overwrite->id != id) continue;

        // const in, mutable out: the table is the caller's and the lookup is the same walk either way.
        return (_NYA_PermissionOverwrite*)overwrite;
    }

    return nullptr;
}

void _nya_permission_audit(NYA_Permissions* permissions, const NYA_PermissionAudit* entry) {
    if (permissions->audit_count == NYA_PERMISSION_MAX_AUDIT) {
        permissions->audit[permissions->audit_first] = *entry;
        permissions->audit_first                     = (permissions->audit_first + 1) % NYA_PERMISSION_MAX_AUDIT;
        permissions->audit_dropped++;

        return;
    }

    permissions->audit[(permissions->audit_first + permissions->audit_count) % NYA_PERMISSION_MAX_AUDIT] = *entry;
    permissions->audit_count++;
}

b8 _nya_permission_may(const NYA_Permissions* permissions, u64 actor, NYA_Permission required) {
    if (actor == NYA_PERMISSION_SYSTEM) return true;
    if (permissions->owner != NYA_PERMISSION_SYSTEM && actor == permissions->owner) return true;

    return (nya_permission_resolve(permissions, actor, 0) & required) == required;
}

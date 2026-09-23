#include <stdio.h>
#include <string.h>

#include "nyangine/accounts/accounts_audit.h"
#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/db/db_orm.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_ConstCString _NYA_ACCOUNT_ACTION_NAMES[NYA_ACCOUNT_ACTION_COUNT] = {
    [NYA_ACCOUNT_ACTION_NONE]              = "none",
    [NYA_ACCOUNT_ACTION_CREATED]           = "created",
    [NYA_ACCOUNT_ACTION_PASSWORD_CHANGED]  = "password_changed",
    [NYA_ACCOUNT_ACTION_DISABLED]          = "disabled",
    [NYA_ACCOUNT_ACTION_ENABLED]           = "enabled",
    [NYA_ACCOUNT_ACTION_ROLES_SET]         = "roles_set",
    [NYA_ACCOUNT_ACTION_IDENTITY_LINKED]   = "identity_linked",
    [NYA_ACCOUNT_ACTION_IDENTITY_UNLINKED] = "identity_unlinked",
    [NYA_ACCOUNT_ACTION_SESSION_REVOKED]   = "session_revoked",
    [NYA_ACCOUNT_ACTION_DELETED]           = "deleted",
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_account_audit_record(NYA_Arena* arena, u64 actor_id, u64 subject_id, NYA_AccountAction action, NYA_ConstCString reason) {
    nya_assert(arena != nullptr);

    if (!nya_accounts_is_open()) return;

    NYA_AccountAudit entry = { 0 };

    entry.at_s       = nya_clock_get_timestamp_s();
    entry.actor_id   = actor_id;
    entry.subject_id = subject_id;
    entry.action     = (u32)action;

    (void)snprintf(entry.reason, sizeof(entry.reason), "%s", reason != nullptr ? reason : "");

    NYA_Error written = nya_orm_insert(_NYA_ACCOUNTS.audit, &entry);

    // Logged and swallowed, for the reason the header gives: recording that a thing happened must never
    // be able to stop the thing from happening, or the audit becomes the way to disable enforcement.
    if (!written.ok) {
        nya_log_error(
            "An account audit entry could not be written (actor %llu, subject %llu, action %s): %s",
            (unsigned long long)actor_id, (unsigned long long)subject_id, nya_account_action_name(action), (NYA_ConstCString)written.message
        );
    }
}

NYA_Error nya_account_audit_list(NYA_Arena* arena, u64 subject_id, u32 limit, NYA_AccountAudit** out_entries, u32* out_count) {
    nya_assert(arena != nullptr && out_entries != nullptr && out_count != nullptr);

    *out_entries = nullptr;
    *out_count   = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    if (limit == 0) limit = 100;

    void* rows  = nullptr;
    u32   count = 0;

    NYA_TRY(nya_orm_select(
        _NYA_ACCOUNTS.audit, arena, "WHERE subject_id = ? ORDER BY at_s DESC, id DESC LIMIT ?",
        (NYA_SqlValue[]){ nya_sql_s64((s64)subject_id), nya_sql_s64((s64)limit) }, 2, &rows, &count
    ));

    *out_entries = (NYA_AccountAudit*)rows;
    *out_count   = count;

    return NYA_OK;
}

NYA_Error nya_account_audit_count(NYA_Arena* arena, u64 subject_id, u32* out_count) {
    nya_assert(arena != nullptr && out_count != nullptr);

    *out_count = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    void* rows  = nullptr;
    u32   count = 0;

    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.audit, arena, "WHERE subject_id = ?", (NYA_SqlValue[]){ nya_sql_s64((s64)subject_id) }, 1, &rows, &count));

    *out_count = count;

    return NYA_OK;
}

NYA_Error nya_account_audit_prune(NYA_Arena* arena, u64 keep_for_s, u32* out_removed) {
    nya_assert(arena != nullptr && out_removed != nullptr);

    *out_removed = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    u64 now_s  = nya_clock_get_timestamp_s();
    u64 cutoff = now_s > keep_for_s ? now_s - keep_for_s : 0;

    void* rows  = nullptr;
    u32   count = 0;

    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.audit, arena, "WHERE at_s <= ?", (NYA_SqlValue[]){ nya_sql_s64((s64)cutoff) }, 1, &rows, &count));

    for (u32 index = 0; index < count; index++) {
        const NYA_AccountAudit* entry = nya_orm_at(_NYA_ACCOUNTS.audit, rows, index);

        NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.audit, nya_sql_s64((s64)entry->id)));

        *out_removed += 1;
    }

    return NYA_OK;
}

NYA_ConstCString nya_account_action_name(NYA_AccountAction action) {
    if (action <= NYA_ACCOUNT_ACTION_NONE || action >= NYA_ACCOUNT_ACTION_COUNT) return "unknown";

    return _NYA_ACCOUNT_ACTION_NAMES[action];
}

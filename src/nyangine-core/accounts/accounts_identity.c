#include <stdio.h>
#include <string.h>

#include "nyangine-core/accounts/accounts_identity.h"
#include "nyangine-core/accounts/accounts_session.h"
#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_clock.h"
#include "nyangine-core/db/db_orm.h"

// PRIVATE API DECLARATION

/** Whether a subject is one this will store: present, short enough, printable, and not an email. */
NYA_INTERNAL b8 _nya_account_subject_is_valid(NYA_ConstCString subject) __attr_no_discard;

/** The row for this provider and subject, or false when there is none. */
NYA_INTERNAL b8 _nya_account_identity_find(
    NYA_Arena* arena, NYA_ConstCString provider, NYA_ConstCString subject, OUT NYA_AccountIdentity* out_identity
) __attr_no_discard;

/** Makes a username for somebody arriving through a provider, unique against what is already there. */
NYA_INTERNAL NYA_Error _nya_account_identity_username(
    NYA_Arena* arena, NYA_ConstCString provider, NYA_ConstCString subject, OUT char* out_username, u64 capacity
) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error nya_account_identity_link(NYA_Arena* arena, u64 account_id, NYA_ConstCString provider, NYA_ConstCString subject, NYA_ConstCString display) {
    nya_assert(arena != nullptr);

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    char folded[NYA_ACCOUNTS_MAX_PROVIDER] = { 0 };
    if (!nya_account_provider_normalize(provider, folded, sizeof(folded))) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a provider name", provider != nullptr ? provider : "");
    }

    if (!_nya_account_subject_is_valid(subject)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is not a provider subject; see accounts_identity.h on why an email is refused");
    }

    // The account must exist: a row pointing at nothing is a way into an account that later becomes somebody else's.
    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_find_by_id(arena, account_id, &user));

    NYA_AccountIdentity existing = { 0 };

    if (_nya_account_identity_find(arena, folded, subject, &existing)) {
        // Somebody else's: refused, not moved — quietly re-pointing it would be an account takeover with no evidence.
        if (existing.account_id != account_id) return nya_error(NYA_ERROR_ALREADY_EXISTS, "that %s account is linked to somebody else", folded);

        // Theirs already: the display name is the only thing that can have changed.
        (void)snprintf(existing.display, sizeof(existing.display), "%s", display != nullptr ? display : "");
        existing.used_at_s = nya_clock_get_timestamp_s();

        return nya_orm_update(_NYA_ACCOUNTS.identities, &existing);
    }

    u32 held = 0;
    NYA_TRY(nya_account_identity_count(arena, account_id, &held));

    if (held >= NYA_ACCOUNTS_MAX_IDENTITIES_PER_USER) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "an account holds at most %d identities", NYA_ACCOUNTS_MAX_IDENTITIES_PER_USER);
    }

    // One provider once per account: a second id for the same provider makes "which one are you" ambiguous.
    NYA_AccountIdentity* rows  = nullptr;
    u32                  count = 0;

    NYA_TRY(nya_account_identity_list(arena, account_id, &rows, &count));

    for (u32 index = 0; index < count; index++) {
        if (nya_string_equals(rows[index].provider, folded)) return nya_error(NYA_ERROR_ALREADY_EXISTS, "that account already has a %s login", folded);
    }

    NYA_AccountIdentity identity = { 0 };

    identity.account_id  = account_id;
    identity.linked_at_s = nya_clock_get_timestamp_s();
    identity.used_at_s   = identity.linked_at_s;

    (void)snprintf(identity.provider, sizeof(identity.provider), "%s", folded);
    (void)snprintf(identity.subject, sizeof(identity.subject), "%s", subject);
    (void)snprintf(identity.display, sizeof(identity.display), "%s", display != nullptr ? display : "");

    return nya_orm_insert(_NYA_ACCOUNTS.identities, &identity);
}

NYA_Error nya_account_identity_unlink(NYA_Arena* arena, u64 account_id, NYA_ConstCString provider) {
    nya_assert(arena != nullptr);

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    char folded[NYA_ACCOUNTS_MAX_PROVIDER] = { 0 };
    if (!nya_account_provider_normalize(provider, folded, sizeof(folded))) return nya_error(NYA_ERROR_NOT_FOUND, "no such login");

    // The last way in stays: unlinking an account's only identity (with no password) would strand it forever.
    u32 ways_in = 0;
    NYA_TRY(nya_account_identity_count(arena, account_id, &ways_in));

    if (ways_in <= 1) return nya_error(NYA_ERROR_PERMISSION_DENIED, "that is the only way into this account; set a password or link another first");

    NYA_AccountIdentity* rows  = nullptr;
    u32                  count = 0;

    NYA_TRY(nya_account_identity_list(arena, account_id, &rows, &count));

    for (u32 index = 0; index < count; index++) {
        if (!nya_string_equals(rows[index].provider, folded)) continue;

        return nya_orm_delete(_NYA_ACCOUNTS.identities, nya_sql_s64((s64)rows[index].id));
    }

    return nya_error(NYA_ERROR_NOT_FOUND, "that account has no %s login", folded);
}

NYA_Error nya_account_find_by_identity(NYA_Arena* arena, NYA_ConstCString provider, NYA_ConstCString subject, NYA_AccountUser* out_user) {
    nya_assert(arena != nullptr && out_user != nullptr);

    nya_memset(out_user, 0, sizeof(NYA_AccountUser));

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    char folded[NYA_ACCOUNTS_MAX_PROVIDER] = { 0 };
    if (!nya_account_provider_normalize(provider, folded, sizeof(folded))) return nya_error(NYA_ERROR_NOT_FOUND, "no such login");
    if (!_nya_account_subject_is_valid(subject)) return nya_error(NYA_ERROR_NOT_FOUND, "no such login");

    NYA_AccountIdentity identity = { 0 };
    if (!_nya_account_identity_find(arena, folded, subject, &identity)) return nya_error(NYA_ERROR_NOT_FOUND, "no such login");

    return nya_account_find_by_id(arena, identity.account_id, out_user);
}

NYA_Error nya_account_from_identity(
    NYA_Arena* arena, NYA_ConstCString provider, NYA_ConstCString subject, NYA_ConstCString display, NYA_AccountUser* out_user, b8* out_created
) {
    nya_assert(arena != nullptr && out_user != nullptr && out_created != nullptr);

    nya_memset(out_user, 0, sizeof(NYA_AccountUser));
    *out_created = false;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    char folded[NYA_ACCOUNTS_MAX_PROVIDER] = { 0 };
    if (!nya_account_provider_normalize(provider, folded, sizeof(folded))) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a provider name", provider != nullptr ? provider : "");
    }

    if (!_nya_account_subject_is_valid(subject)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is not a provider subject; see accounts_identity.h on why an email is refused");
    }

    NYA_AccountIdentity identity = { 0 };

    if (_nya_account_identity_find(arena, folded, subject, &identity)) {
        NYA_AccountUser user = { 0 };
        NYA_TRY(nya_account_find_by_id(arena, identity.account_id, &user));

        // A ban is a ban however somebody arrives, and in the same words a password login uses.
        if (user.disabled) return nya_error(NYA_ERROR_PERMISSION_DENIED, "that username and password do not match an account");

        identity.used_at_s = nya_clock_get_timestamp_s();
        (void)snprintf(identity.display, sizeof(identity.display), "%s", display != nullptr ? display : "");

        NYA_TRY(nya_orm_update(_NYA_ACCOUNTS.identities, &identity));

        *out_user = user;

        return NYA_OK;
    }

    // Nobody yet. The username only has to be unique and legal; it is theirs to change.
    char username[NYA_ACCOUNTS_MAX_USERNAME] = { 0 };
    NYA_TRY(_nya_account_identity_username(arena, folded, subject, username, sizeof(username)));

    NYA_AccountUser fresh = { 0 };
    NYA_TRY(nya_account_create_without_password(arena, username, &fresh));

    NYA_Error linked = nya_account_identity_link(arena, fresh.id, folded, subject, display);

    if (!linked.ok) {
        // The account would otherwise be one nobody can reach: no password and no way in.
        (void)nya_account_destroy(arena, fresh.id);
        return linked;
    }

    // Read back, so what the caller holds is the row as it is stored rather than as it was built.
    NYA_TRY(nya_account_find_by_id(arena, fresh.id, out_user));

    *out_created = true;

    return NYA_OK;
}

NYA_Error nya_account_identity_list(NYA_Arena* arena, u64 account_id, NYA_AccountIdentity** out_identities, u32* out_count) {
    nya_assert(arena != nullptr && out_identities != nullptr && out_count != nullptr);

    *out_identities = nullptr;
    *out_count      = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    void* rows  = nullptr;
    u32   count = 0;

    NYA_TRY(nya_orm_select(
        _NYA_ACCOUNTS.identities, arena, "WHERE account_id = ? ORDER BY linked_at_s ASC, id ASC", (NYA_SqlValue[]){ nya_sql_s64((s64)account_id) }, 1,
        &rows, &count
    ));

    *out_identities = rows;
    *out_count      = count;

    return NYA_OK;
}

NYA_Error nya_account_identity_count(NYA_Arena* arena, u64 account_id, u32* out_count) {
    nya_assert(arena != nullptr && out_count != nullptr);

    *out_count = 0;

    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_find_by_id(arena, account_id, &user));

    NYA_AccountIdentity* rows  = nullptr;
    u32                  count = 0;

    NYA_TRY(nya_account_identity_list(arena, account_id, &rows, &count));

    // A password is a way in too, which is the whole point of counting: see nya_account_identity_unlink.
    *out_count = count + (user.password[0] != '\0' ? 1 : 0);

    return NYA_OK;
}

b8 nya_account_provider_normalize(NYA_ConstCString provider, char* out_normalized, u64 capacity) {
    nya_assert(out_normalized != nullptr && capacity > 0);

    out_normalized[0] = '\0';

    if (provider == nullptr || provider[0] == '\0') return false;

    u64 length = strlen(provider);
    if (length >= capacity || length >= NYA_ACCOUNTS_MAX_PROVIDER) return false;

    for (u64 index = 0; index < length; index++) {
        u8 character = (u8)provider[index];

        if (character >= 'A' && character <= 'Z') character = (u8)(character - 'A' + 'a');

        b8 allowed = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '.' || character == '-' ||
                     character == '_';

        if (!allowed) return false;

        out_normalized[index] = (char)character;
    }

    out_normalized[length] = '\0';

    return true;
}

// PRIVATE API IMPLEMENTATION

b8 _nya_account_subject_is_valid(NYA_ConstCString subject) {
    if (subject == nullptr || subject[0] == '\0') return false;

    u64 length = strlen(subject);
    if (length >= NYA_ACCOUNTS_MAX_SUBJECT) return false;

    for (u64 index = 0; index < length; index++) {
        u8 character = (u8)subject[index];

        if (character < 0x20 || character == 0x7F) return false;

        // An email is refused: it gets re-assigned, and a re-assigned key turns one person's account into another's.
        if (character == '@') return false;
    }

    return true;
}

b8 _nya_account_identity_find(NYA_Arena* arena, NYA_ConstCString provider, NYA_ConstCString subject, NYA_AccountIdentity* out_identity) {
    nya_memset(out_identity, 0, sizeof(NYA_AccountIdentity));

    void* rows  = nullptr;
    u32   count = 0;

    NYA_Error found = nya_orm_select(
        _NYA_ACCOUNTS.identities, arena, "WHERE provider = ? AND subject = ? LIMIT 1",
        (NYA_SqlValue[]){ nya_sql_text(provider), nya_sql_text(subject) }, 2, &rows, &count
    );

    if (!found.ok || count == 0) return false;

    const NYA_AccountIdentity* row = nya_orm_at(_NYA_ACCOUNTS.identities, rows, 0);

    *out_identity = *row;

    return true;
}

NYA_Error _nya_account_identity_username(NYA_Arena* arena, NYA_ConstCString provider, NYA_ConstCString subject, char* out_username, u64 capacity) {
    // Provider plus the tail of the subject (`steam_0000000000`); the full id is not used since a username is public.
    u64 length = strlen(subject);
    u64 tail   = length > 10 ? length - 10 : 0;

    for (u32 attempt = 0; attempt < 1000; attempt++) {
        s32 written = attempt == 0 ? snprintf(out_username, capacity, "%s_%s", provider, subject + tail)
                                   : snprintf(out_username, capacity, "%s_%s%u", provider, subject + tail, attempt);

        if (written <= 0 || (u64)written >= capacity) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a username for that subject does not fit");

        NYA_AccountUser taken = { 0 };
        if (!nya_account_find(arena, out_username, &taken).ok) return NYA_OK;
    }

    return nya_error(NYA_ERROR_ALREADY_EXISTS, "no username could be made for that subject");
}

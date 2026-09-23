#include <stdio.h>
#include <string.h>

#include "nyangine/accounts/accounts_audit.h"
#include "nyangine/accounts/accounts_identity.h"
#include "nyangine/accounts/accounts_invite.h"
#include "nyangine/accounts/accounts_recovery.h"
#include "nyangine/accounts/accounts_session.h"
#include "nyangine/accounts/accounts_throttle.h"
#include "nyangine/accounts/accounts_user.h"
#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/crypto/crypto_encoding.h"
#include "nyangine/crypto/crypto_kdf.h"
#include "nyangine/crypto/crypto_secret.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/db/db_orm.h"
#include "nyangine/os/os_random.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The two tables, open for as long as this module is.
 *
 * A singleton rather than a handle a caller passes: there is one set of accounts in a process, the
 * same way there is one system registry, and a second one would be a second answer to "who is logged
 * in" — which is the question this exists to answer once.
 * */
typedef struct {
    NYA_Arena*    arena;
    NYA_Database* database;

    NYA_OrmTable* users;
    NYA_OrmTable* sessions;
    NYA_OrmTable* identities;
    NYA_OrmTable* recovery_codes;
    NYA_OrmTable* invites;
    NYA_OrmTable* audit;

    b8 open;
} _NYA_AccountsState;

NYA_INTERNAL _NYA_AccountsState _NYA_ACCOUNTS = { 0 };

/**
 * A hash of nothing, verified against when a username does not exist.
 *
 * The cost of a login must not say whether the account is there, and the only way to mean that is to
 * do the work either way. Built once at open from a password nobody has.
 * */
NYA_INTERNAL char _NYA_ACCOUNTS_ABSENT[NYA_ACCOUNTS_MAX_HASH] = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Hashes `password` into the encoded form described in the header, salt and parameters included. */
NYA_INTERNAL NYA_Error _nya_account_password_encode(NYA_ConstCString password, OUT char* out_encoded, u64 capacity) __attr_no_discard;

/**
 * Whether `password` made `encoded`. False for anything that is not that, including an encoded string
 * this cannot parse — an unreadable hash is not a password that matches.
 * */
NYA_INTERNAL b8 _nya_account_password_verify(NYA_ConstCString password, NYA_ConstCString encoded) __attr_no_discard;

/** The cost an encoded hash was made with. False when it is not one this understands. */
NYA_INTERNAL b8 _nya_account_password_cost(NYA_ConstCString encoded, OUT u32* out_memory_kib, OUT u32* out_passes, OUT u32* out_lanes) __attr_no_discard;

/**
 * Reads the decimal number at `*cursor` up to `terminator`, moving the cursor past it.
 *
 * False for anything that is not one, an empty one, or one past what a u32 holds — which is how a
 * cost that would overflow is refused rather than wrapped.
 * */
NYA_INTERNAL b8 _nya_account_number(const char** cursor, char terminator, OUT u32* out_value) __attr_no_discard;

/** Whether a password is inside the length bounds, said as the error a caller shows. */
NYA_INTERNAL NYA_Error _nya_account_password_check(NYA_ConstCString password) __attr_no_discard;

/** What both constructors are: a null password means an account that has none; see accounts_identity.h. */
NYA_INTERNAL NYA_Error _nya_account_create(NYA_Arena* arena, NYA_ConstCString username, NYA_ConstCString password, OUT NYA_AccountUser* out_user)
    __attr_no_discard;

/** The one refusal a login has. Always the same words, whatever actually went wrong. */
NYA_INTERNAL NYA_Error _nya_account_refused(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_accounts_open(NYA_Arena* arena, NYA_Database* database) {
    nya_assert(arena != nullptr);
    nya_assert(database != nullptr);

    if (_NYA_ACCOUNTS.open) return nya_error(NYA_ERROR_ALREADY_EXISTS, "the accounts tables are already open");

    _NYA_ACCOUNTS.arena    = arena;
    _NYA_ACCOUNTS.database = database;

    NYA_TRY(nya_orm_open(arena, database, nya_reflect_of(NYA_AccountUser), "accounts", &_NYA_ACCOUNTS.users));
    NYA_TRY(nya_orm_schema_migrate(_NYA_ACCOUNTS.users));

    NYA_TRY(nya_orm_open(arena, database, nya_reflect_of(NYA_AccountSession), "account_sessions", &_NYA_ACCOUNTS.sessions));
    NYA_TRY(nya_orm_schema_migrate(_NYA_ACCOUNTS.sessions));

    NYA_TRY(nya_orm_open(arena, database, nya_reflect_of(NYA_AccountIdentity), "account_identities", &_NYA_ACCOUNTS.identities));
    NYA_TRY(nya_orm_schema_migrate(_NYA_ACCOUNTS.identities));

    NYA_TRY(nya_orm_open(arena, database, nya_reflect_of(NYA_AccountRecoveryCode), "account_recovery_codes", &_NYA_ACCOUNTS.recovery_codes));
    NYA_TRY(nya_orm_schema_migrate(_NYA_ACCOUNTS.recovery_codes));

    NYA_TRY(nya_orm_open(arena, database, nya_reflect_of(NYA_AccountInvite), "account_invites", &_NYA_ACCOUNTS.invites));
    NYA_TRY(nya_orm_schema_migrate(_NYA_ACCOUNTS.invites));

    NYA_TRY(nya_orm_open(arena, database, nya_reflect_of(NYA_AccountAudit), "account_audit", &_NYA_ACCOUNTS.audit));
    NYA_TRY(nya_orm_schema_migrate(_NYA_ACCOUNTS.audit));

    /*
     * The hash a login verifies against when the username is not there. Made from bytes nobody will
     * ever type, so it can never match, and made *here* so that the first failed login for a name
     * that does not exist costs exactly what a real one does rather than one hash more.
     */
    u8 nobody[32] = { 0 };
    if (!nya_os_random_bytes(nobody, sizeof(nobody))) return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");

    char text[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };
    u64  length                              = 0;

    if (!nya_crypto_base64url_encode(nobody, sizeof(nobody), text, sizeof(text), &length)) {
        return nya_error(NYA_ERROR_NOT_OK, "the absent-account hash could not be made");
    }

    NYA_TRY(_nya_account_password_encode(text, _NYA_ACCOUNTS_ABSENT, sizeof(_NYA_ACCOUNTS_ABSENT)));

    nya_crypto_wipe(nobody, sizeof(nobody));
    nya_crypto_wipe(text, sizeof(text));

    _NYA_ACCOUNTS.open = true;

    return NYA_OK;
}

void nya_accounts_close(void) {
    if (!_NYA_ACCOUNTS.open) return;

    nya_orm_close(_NYA_ACCOUNTS.audit);
    nya_orm_close(_NYA_ACCOUNTS.invites);
    nya_orm_close(_NYA_ACCOUNTS.recovery_codes);
    nya_orm_close(_NYA_ACCOUNTS.identities);
    nya_orm_close(_NYA_ACCOUNTS.sessions);
    nya_orm_close(_NYA_ACCOUNTS.users);

    nya_crypto_wipe(_NYA_ACCOUNTS_ABSENT, sizeof(_NYA_ACCOUNTS_ABSENT));
    nya_memset(&_NYA_ACCOUNTS, 0, sizeof(_NYA_ACCOUNTS));
}

b8 nya_accounts_is_open(void) {
    return _NYA_ACCOUNTS.open;
}

NYA_Error nya_account_create(NYA_Arena* arena, NYA_ConstCString username, NYA_ConstCString password, NYA_AccountUser* out_user) {
    NYA_TRY(_nya_account_password_check(password));

    return _nya_account_create(arena, username, password, out_user);
}

NYA_Error nya_account_create_without_password(NYA_Arena* arena, NYA_ConstCString username, NYA_AccountUser* out_user) {
    return _nya_account_create(arena, username, nullptr, out_user);
}

NYA_Error _nya_account_create(NYA_Arena* arena, NYA_ConstCString username, NYA_ConstCString password, NYA_AccountUser* out_user) {
    nya_assert(arena != nullptr && out_user != nullptr);

    nya_memset(out_user, 0, sizeof(NYA_AccountUser));

    if (!_NYA_ACCOUNTS.open) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    char normalized[NYA_ACCOUNTS_MAX_USERNAME] = { 0 };
    if (!nya_account_username_normalize(username, normalized, sizeof(normalized))) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a username is one to %d characters with no control characters in it", NYA_ACCOUNTS_MAX_USERNAME - 1);
    }

    // The normalised form is what uniqueness is decided on, so this is the question a caller's own
    // "is that name taken" has to ask too; see nya_account_username_normalize.
    NYA_AccountUser existing = { 0 };
    if (nya_account_find(arena, username, &existing).ok) return nya_error(NYA_ERROR_ALREADY_EXISTS, "that username is taken");

    NYA_AccountUser user = { 0 };

    (void)snprintf(user.username, sizeof(user.username), "%s", username);
    (void)snprintf(user.normalized, sizeof(user.normalized), "%s", normalized);
    (void)snprintf(user.display, sizeof(user.display), "%s", username);

    // Null is an account with no password at all, which is what an account made from a Steam or a
    // Discord login is: the column stays empty and _nya_account_password_verify refuses an empty hash.
    if (password != nullptr) NYA_TRY(_nya_account_password_encode(password, user.password, sizeof(user.password)));

    user.created_at_s          = nya_clock_get_timestamp_s();
    user.password_changed_at_s = user.created_at_s;

    NYA_TRY(nya_orm_insert(_NYA_ACCOUNTS.users, &user));

    nya_account_audit_record(arena, 0, user.id, NYA_ACCOUNT_ACTION_CREATED, "");

    *out_user = user;

    return NYA_OK;
}

NYA_Error nya_account_find(NYA_Arena* arena, NYA_ConstCString username, NYA_AccountUser* out_user) {
    nya_assert(arena != nullptr && out_user != nullptr);

    nya_memset(out_user, 0, sizeof(NYA_AccountUser));

    if (!_NYA_ACCOUNTS.open) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    char normalized[NYA_ACCOUNTS_MAX_USERNAME] = { 0 };
    if (!nya_account_username_normalize(username, normalized, sizeof(normalized))) return nya_error(NYA_ERROR_NOT_FOUND, "no such account");

    void* rows  = nullptr;
    u32   count = 0;

    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.users, arena, "WHERE normalized = ? LIMIT 1", (NYA_SqlValue[]){ nya_sql_text(normalized) }, 1, &rows, &count));

    if (count == 0) return nya_error(NYA_ERROR_NOT_FOUND, "no such account");

    const NYA_AccountUser* row = nya_orm_at(_NYA_ACCOUNTS.users, rows, 0);

    *out_user = *row;

    return NYA_OK;
}

NYA_Error nya_account_find_by_id(NYA_Arena* arena, u64 id, NYA_AccountUser* out_user) {
    nya_assert(arena != nullptr && out_user != nullptr);

    nya_memset(out_user, 0, sizeof(NYA_AccountUser));

    if (!_NYA_ACCOUNTS.open) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    NYA_Error found = nya_orm_find(_NYA_ACCOUNTS.users, arena, nya_sql_s64((s64)id), out_user);

    if (!found.ok) return nya_error(NYA_ERROR_NOT_FOUND, "no such account");

    return NYA_OK;
}

NYA_Error nya_account_authenticate(
    NYA_Arena* arena, NYA_ConstCString username, NYA_ConstCString password, NYA_ConstCString address, NYA_AccountUser* out_user
) {
    nya_assert(arena != nullptr && out_user != nullptr);

    nya_memset(out_user, 0, sizeof(NYA_AccountUser));

    if (!_NYA_ACCOUNTS.open) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    /*
     * Before the hash, not after it. Argon2id is 30 ms of this process, and an attacker who can spend
     * that whenever they like has a denial of service whether or not they ever guess a password. See
     * accounts_throttle.h for what this leaks by refusing cheaply, and why that is the right trade.
     */
    u32 wait_s = 0;
    if (nya_account_throttle_check(username, address, &wait_s) == NYA_ACCOUNT_THROTTLE_WAIT) return _nya_account_refused();

    NYA_AccountUser user  = { 0 };
    NYA_Error       found = nya_account_find(arena, username, &user);

    /*
     * The verification happens whether or not the account exists, against a hash nothing can match.
     * Argon2id is deliberately expensive, so skipping it for an unknown name would make "no such
     * user" measurably faster than "wrong password" — which is a list of every username on the
     * service, available to anybody with a stopwatch.
     */
    NYA_ConstCString encoded = found.ok ? user.password : _NYA_ACCOUNTS_ABSENT;

    b8 matched = _nya_account_password_verify(password, encoded);

    // A disabled account is refused in the same words for the same reason: whether an account exists
    // and whether it is allowed in are both things an attacker would like to know.
    if (!found.ok || !matched || user.disabled) {
        nya_account_throttle_fail(username, address);
        return _nya_account_refused();
    }

    nya_account_throttle_succeed(username, address);

    *out_user = user;

    return NYA_OK;
}

NYA_Error nya_account_password_change(NYA_Arena* arena, u64 id, NYA_ConstCString current, NYA_ConstCString replacement) {
    nya_assert(arena != nullptr);

    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_find_by_id(arena, id, &user));

    // The current password first: a session that has been taken over must not be able to change the
    // password and lock the owner out of their own account.
    if (!_nya_account_password_verify(current, user.password)) return _nya_account_refused();

    return nya_account_password_reset(arena, id, replacement);
}

NYA_Error nya_account_password_reset(NYA_Arena* arena, u64 id, NYA_ConstCString replacement) {
    nya_assert(arena != nullptr);

    NYA_TRY(_nya_account_password_check(replacement));

    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_find_by_id(arena, id, &user));

    NYA_TRY(_nya_account_password_encode(replacement, user.password, sizeof(user.password)));

    user.password_changed_at_s = nya_clock_get_timestamp_s();

    NYA_TRY(nya_orm_update(_NYA_ACCOUNTS.users, &user));

    /*
     * Every session ends. Somebody changing a password is usually somebody who thinks a session is
     * not theirs any more, and leaving the old ones alive would answer that worry with nothing.
     */
    u32 ended = 0;
    NYA_TRY(nya_account_session_revoke_all(arena, id, &ended));

    nya_account_audit_record(arena, 0, id, NYA_ACCOUNT_ACTION_PASSWORD_CHANGED, "");

    return NYA_OK;
}

b8 nya_account_password_needs_rehash(const NYA_AccountUser* user) {
    nya_assert(user != nullptr);

    u32 memory_kib = 0;
    u32 passes     = 0;
    u32 lanes      = 0;

    // A hash this cannot read is one worth replacing the moment the password is known to be right.
    if (!_nya_account_password_cost(user->password, &memory_kib, &passes, &lanes)) return true;

    return memory_kib < NYA_ACCOUNTS_ARGON2ID_MEMORY_KIB || passes < NYA_ACCOUNTS_ARGON2ID_PASSES || lanes < NYA_ACCOUNTS_ARGON2ID_LANES;
}

/** Wraps an object as an array element's value, since the export's lists are arrays of objects. */
NYA_INTERNAL NYA_Value _nya_account_export_row(NYA_Arena* arena, const NYA_TypeReflection* type, const void* row) {
    // Redacted, because this is the person's own copy of their data and a hash or a token in it is not
    // theirs to keep any more than it was the database's to hand out; see the @redact fields.
    NYA_Object* object = nya_reflect_to_object_redacted(arena, type, row);

    return (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *object };
}

NYA_Error nya_account_export(NYA_Arena* arena, u64 id, NYA_Object** out_object) {
    nya_assert(arena != nullptr && out_object != nullptr);

    *out_object = nullptr;

    if (!_NYA_ACCOUNTS.open) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_find_by_id(arena, id, &user));

    NYA_Object* document = nya_object_create(arena);

    // The account itself, redacted: everything the row holds except the password hash, which is not the
    // person's password and is no use to them.
    nya_object_add(document, "account", _nya_account_export_row(arena, nya_reflect_of(NYA_AccountUser), &user));

    // Every linked provider.
    {
        NYA_AccountIdentity* rows  = nullptr;
        u32                  count = 0;

        NYA_TRY(nya_account_identity_list(arena, id, &rows, &count));

        NYA_ArrayᐸNYA_Valueᐳ* list = nya_array_create(arena, NYA_Value);

        for (u32 index = 0; index < count; index++) nya_array_add(list, _nya_account_export_row(arena, nya_reflect_of(NYA_AccountIdentity), &rows[index]));

        nya_object_add(document, "identities", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *list });
    }

    // Every session, its token hash redacted and its token never stored: the metadata a person is shown
    // — where from, what agent, when — is what they get, not a way to resume anything.
    {
        NYA_AccountSession* rows  = nullptr;
        u32                 count = 0;

        NYA_TRY(nya_account_session_list(arena, id, &rows, &count));

        NYA_ArrayᐸNYA_Valueᐳ* list = nya_array_create(arena, NYA_Value);

        for (u32 index = 0; index < count; index++) nya_array_add(list, _nya_account_export_row(arena, nya_reflect_of(NYA_AccountSession), &rows[index]));

        nya_object_add(document, "sessions", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *list });
    }

    // The count of unused recovery codes, never the codes: those are secrets this issued, not data about
    // the person, and printing them into an export would be handing every one of them out again.
    {
        u32 remaining = 0;
        NYA_TRY(nya_account_recovery_remaining(arena, id, &remaining));

        nya_object_add(document, "recovery_codes_remaining", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = remaining });
    }

    *out_object = document;

    return NYA_OK;
}

NYA_Error nya_account_destroy(NYA_Arena* arena, u64 id) {
    nya_assert(arena != nullptr);

    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_find_by_id(arena, id, &user));

    // The sessions go first: a session row whose user is gone would be a session nothing can revoke,
    // and one whose token still validated against a user that is not there.
    u32 removed = 0;
    NYA_TRY(nya_account_session_purge(arena, id, &removed));

    // and every way in, which would otherwise point at an account that is not there and would one day
    // point at whoever is given that row id next.
    NYA_AccountIdentity* identities = nullptr;
    u32                  linked     = 0;

    NYA_TRY(nya_account_identity_list(arena, id, &identities, &linked));

    for (u32 index = 0; index < linked; index++) {
        NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.identities, nya_sql_s64((s64)identities[index].id)));
    }

    // and the recovery codes, which would otherwise be a way into whoever gets this id next.
    void* codes       = nullptr;
    u32   code_count  = 0;

    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.recovery_codes, arena, "WHERE account_id = ?", (NYA_SqlValue[]){ nya_sql_s64((s64)id) }, 1, &codes, &code_count));

    for (u32 index = 0; index < code_count; index++) {
        const NYA_AccountRecoveryCode* row = nya_orm_at(_NYA_ACCOUNTS.recovery_codes, codes, index);
        NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.recovery_codes, nya_sql_s64((s64)row->id)));
    }

    nya_crypto_wipe(user.password, sizeof(user.password));

    // Written before the row goes, so the entry exists; the subject id it names now resolves to no
    // account, which is the tombstone the header describes.
    nya_account_audit_record(arena, 0, id, NYA_ACCOUNT_ACTION_DELETED, "");

    return nya_orm_delete(_NYA_ACCOUNTS.users, nya_sql_s64((s64)id));
}

NYA_Error nya_account_disabled_set(NYA_Arena* arena, u64 id, b8 disabled) {
    nya_assert(arena != nullptr);

    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_find_by_id(arena, id, &user));

    user.disabled = disabled;

    NYA_TRY(nya_orm_update(_NYA_ACCOUNTS.users, &user));

    // Disabling ends what is already open; enabling does not bring anything back, because a session
    // that was ended is ended.
    if (disabled) {
        u32 ended = 0;
        NYA_TRY(nya_account_session_revoke_all(arena, id, &ended));
    }

    // The actor is the caller's to know; this records that it happened, and the route that called it
    // records who by passing itself to nya_account_audit_record when it has a richer story to tell.
    nya_account_audit_record(arena, 0, id, disabled ? NYA_ACCOUNT_ACTION_DISABLED : NYA_ACCOUNT_ACTION_ENABLED, "");

    return NYA_OK;
}

NYA_Error nya_account_roles_set(NYA_Arena* arena, u64 id, u64 roles) {
    nya_assert(arena != nullptr);

    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_find_by_id(arena, id, &user));

    user.roles = roles;

    NYA_TRY(nya_orm_update(_NYA_ACCOUNTS.users, &user));

    nya_account_audit_record(arena, 0, id, NYA_ACCOUNT_ACTION_ROLES_SET, "");

    return NYA_OK;
}

NYA_Error nya_account_count(u64* out_count) {
    nya_assert(out_count != nullptr);

    *out_count = 0;

    if (!_NYA_ACCOUNTS.open) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    NYA_Arena* scratch = nya_arena_create(.name = "accounts_count");
    defer nya_arena_destroy(scratch);

    void* rows  = nullptr;
    u32   count = 0;

    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.users, scratch, "", nullptr, 0, &rows, &count));

    *out_count = count;

    return NYA_OK;
}

b8 nya_account_username_normalize(NYA_ConstCString username, char* out_normalized, u64 capacity) {
    nya_assert(out_normalized != nullptr && capacity > 0);

    out_normalized[0] = '\0';

    if (username == nullptr || username[0] == '\0') return false;

    u64 length = strlen(username);
    if (length >= capacity || length >= NYA_ACCOUNTS_MAX_USERNAME) return false;

    for (u64 index = 0; index < length; index++) {
        u8 character = (u8)username[index];

        // A control character in a username is a username that prints as something else in a log, a
        // terminal or a list. There is no legitimate one.
        if (character < 0x20 || character == 0x7F) return false;

        /*
         * Case folded, which is the whole of the normalisation for now. The look-alike folding the
         * header describes — the confusable sets of Unicode's UTS #39 — needs a table this module
         * does not carry yet, so what is here is the ASCII half, and a name in another script is
         * unique against itself but not yet against its look-alikes.
         */
        if (character >= 'A' && character <= 'Z') character = (u8)(character - 'A' + 'a');

        out_normalized[index] = (char)character;
    }

    out_normalized[length] = '\0';

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error _nya_account_password_encode(NYA_ConstCString password, char* out_encoded, u64 capacity) {
    out_encoded[0] = '\0';

    u8 salt[NYA_CRYPTO_ARGON2ID_SALT_BYTES] = { 0 };
    if (!nya_os_random_bytes(salt, sizeof(salt))) return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");

    // A scratch arena of its own, destroyed here: the work area is nineteen mebibytes, and an arena
    // that lives for the program would keep the region rather than hand it back. See crypto_kdf.h.
    NYA_Arena* scratch = nya_arena_create(.name = "accounts_argon2id");
    defer nya_arena_destroy(scratch);

    u8 hash[32] = { 0 };

    NYA_TRY(nya_crypto_argon2id(scratch, hash, sizeof(hash), .password = (const u8*)password, .password_size = strlen(password), .salt = salt,
                                .salt_size = sizeof(salt), .memory_kib = NYA_ACCOUNTS_ARGON2ID_MEMORY_KIB, .passes = NYA_ACCOUNTS_ARGON2ID_PASSES,
                                .lanes = NYA_ACCOUNTS_ARGON2ID_LANES));

    char salt_text[32] = { 0 };
    char hash_text[64] = { 0 };
    u64  salt_length   = 0;
    u64  hash_length   = 0;

    if (!nya_crypto_base64url_encode(salt, sizeof(salt), salt_text, sizeof(salt_text), &salt_length)) {
        return nya_error(NYA_ERROR_NOT_OK, "the salt could not be encoded");
    }

    if (!nya_crypto_base64url_encode(hash, sizeof(hash), hash_text, sizeof(hash_text), &hash_length)) {
        return nya_error(NYA_ERROR_NOT_OK, "the hash could not be encoded");
    }

    // The form everybody else writes, so a database this wrote is readable by something that is not
    // this; see the header.
    s32 written = snprintf(out_encoded, capacity, "$argon2id$v=19$m=%u,t=%u,p=%u$%s$%s", NYA_ACCOUNTS_ARGON2ID_MEMORY_KIB,
                           NYA_ACCOUNTS_ARGON2ID_PASSES, NYA_ACCOUNTS_ARGON2ID_LANES, salt_text, hash_text);

    nya_crypto_wipe(hash, sizeof(hash));

    if (written <= 0 || (u64)written >= capacity) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the encoded hash does not fit the column");

    return NYA_OK;
}

b8 _nya_account_password_verify(NYA_ConstCString password, NYA_ConstCString encoded) {
    if (password == nullptr || encoded == nullptr || encoded[0] == '\0') return false;

    u32 memory_kib = 0;
    u32 passes     = 0;
    u32 lanes      = 0;

    if (!_nya_account_password_cost(encoded, &memory_kib, &passes, &lanes)) return false;

    // The two fields after the cost: the salt and the hash, each base64url without padding.
    const char* salt_start = strrchr(encoded, '$');
    if (salt_start == nullptr) return false;

    const char* hash_start = salt_start + 1;

    // walk back to the separator before the salt.
    const char* cursor = salt_start - 1;
    while (cursor > encoded && *cursor != '$') cursor--;

    if (*cursor != '$') return false;

    u64 salt_length = (u64)(salt_start - cursor - 1);

    u8  salt[NYA_CRYPTO_ARGON2ID_SALT_BYTES * 2] = { 0 };
    u64 salt_size                                = 0;

    if (!nya_crypto_base64url_decode(cursor + 1, salt_length, salt, sizeof(salt), &salt_size)) return false;

    u8  expected[64] = { 0 };
    u64 expected_size = 0;

    if (!nya_crypto_base64url_decode(hash_start, strlen(hash_start), expected, sizeof(expected), &expected_size)) return false;
    if (expected_size == 0 || expected_size > sizeof(expected)) return false;

    NYA_Arena* scratch = nya_arena_create(.name = "accounts_argon2id");
    defer nya_arena_destroy(scratch);

    u8 actual[64] = { 0 };

    NYA_Error hashed = nya_crypto_argon2id(scratch, actual, expected_size, .password = (const u8*)password, .password_size = strlen(password),
                                           .salt = salt, .salt_size = salt_size, .memory_kib = memory_kib, .passes = passes, .lanes = lanes);

    if (!hashed.ok) return false;

    b8 same = nya_crypto_equals(actual, expected, expected_size);

    nya_crypto_wipe(actual, sizeof(actual));

    return same;
}

b8 _nya_account_password_cost(NYA_ConstCString encoded, u32* out_memory_kib, u32* out_passes, u32* out_lanes) {
    *out_memory_kib = 0;
    *out_passes     = 0;
    *out_lanes      = 0;

    if (encoded == nullptr) return false;

    // Only this algorithm and this version: a hash that says anything else was not made here, and
    // guessing at what it meant is how a verifier comes to accept something it should not.
    if (strncmp(encoded, "$argon2id$v=19$m=", 17) != 0) return false;

    u32 memory_kib = 0;
    u32 passes     = 0;
    u32 lanes      = 0;

    // Parsed by hand rather than by sscanf, which cannot say whether a number was too big for the
    // type it wrote into: a cost this misread is a cost this would then hash at.
    const char* cursor = encoded + strlen("$argon2id$v=19$m=");

    if (!_nya_account_number(&cursor, ',', &memory_kib)) return false;
    if (strncmp(cursor, "t=", 2) != 0) return false;

    cursor += 2;
    if (!_nya_account_number(&cursor, ',', &passes)) return false;
    if (strncmp(cursor, "p=", 2) != 0) return false;

    cursor += 2;
    if (!_nya_account_number(&cursor, '$', &lanes)) return false;

    if (memory_kib == 0 || passes == 0 || lanes == 0) return false;

    *out_memory_kib = memory_kib;
    *out_passes     = passes;
    *out_lanes      = lanes;

    return true;
}

b8 _nya_account_number(const char** cursor, char terminator, u32* out_value) {
    *out_value = 0;

    const char* start = *cursor;
    u64         value = 0;
    u64         digits = 0;

    while (**cursor >= '0' && **cursor <= '9') {
        value = (value * 10) + (u64)(**cursor - '0');

        if (value > 0xFFFFFFFF) return false;

        digits++;
        (*cursor)++;
    }

    if (digits == 0 || *cursor == start) return false;
    if (**cursor != terminator) return false;

    (*cursor)++;

    *out_value = (u32)value;

    return true;
}

NYA_Error _nya_account_password_check(NYA_ConstCString password) {
    if (password == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a password is needed");

    u64 length = strlen(password);

    if (length < NYA_ACCOUNTS_MIN_PASSWORD) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a password is at least %d characters", NYA_ACCOUNTS_MIN_PASSWORD);
    }

    if (length > NYA_ACCOUNTS_MAX_PASSWORD) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a password is at most %d characters", NYA_ACCOUNTS_MAX_PASSWORD);
    }

    return NYA_OK;
}

NYA_Error _nya_account_refused(void) {
    // One sentence for every way a login fails. See accounts.h: which of them happened is exactly
    // what an attacker is asking, and the answer is the same either way.
    return nya_error(NYA_ERROR_PERMISSION_DENIED, "that username and password do not match an account");
}

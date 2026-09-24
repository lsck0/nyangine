/**
 * The two account paths a login-guarded server pays: validating a session cookie on every request,
 * and verifying a password on the login route.
 *
 * Session validation is the per-request hot path — a cookie's token is hashed and looked up, so it
 * must be cheap. Password verification is deliberately the opposite: Argon2id is memory-hard on
 * purpose, so the number is milliseconds per login and its size is a security property, not a
 * regression. Both run against an in-memory accounts database.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define USERNAME "bench_user"
#define PASSWORD "correct horse battery staple"

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_accounts");
    defer      nya_arena_destroy(arena);

    NYA_Arena* scratch = nya_arena_create(.name = "bench_accounts_scratch");
    defer      nya_arena_destroy(scratch);

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db), "the accounts database opens");
    defer nya_sql_close(db);

    NYA_EXPECT(nya_accounts_open(arena, db), "the accounts system comes up on it");
    defer nya_accounts_close();

    NYA_AccountUser user = { 0 };
    NYA_EXPECT(nya_account_create(arena, USERNAME, PASSWORD, &user), "the bench user is created");

    NYA_AccountSession session = { 0 };
    NYA_EXPECT(nya_account_session_issue(arena, user.id, "127.0.0.1", "bench", &session), "a session is issued");

    nya_bench_begin("accounts (the two paths a login-guarded request pays)");

    // The per-request check: turn a cookie's token into "who, if anyone". A hash and a lookup, so it
    // has to clear many per second.
    nya_bench("session validate (per request)", 1, {
        nya_arena_free_all(scratch);
        NYA_AccountSession found = { 0 };
        NYA_Error valid = nya_account_session_validate(scratch, session.token, &found);
        nya_bench_keep(valid.ok ? found.user_id : 0);
    });

    // The login check: Argon2id verify. Slow by design — this measures ms/login, and a fast number
    // here would be the bug. Items 1, so the report is per verify.
    nya_bench("password verify (Argon2id, per login)", 1, {
        nya_arena_free_all(scratch);
        NYA_AccountUser found = { 0 };
        NYA_Error authed = nya_account_authenticate(scratch, USERNAME, PASSWORD, "127.0.0.1", &found);
        nya_bench_keep(authed.ok ? found.id : 0);
    });

    return nya_bench_end();
}

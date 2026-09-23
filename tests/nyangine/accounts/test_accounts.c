/**
 * Users and sessions: a password that is stored as something nobody can reverse, a login that says
 * the same thing however it fails, and every way a session stops being one.
 *
 * Everything runs against an in-memory database of its own, so nothing here touches a file and no
 * test can see another's rows. The passwords are long because the module refuses short ones, which is
 * itself one of the things asserted.
 *
 * Argon2id is deliberately expensive — about 30 ms a hash at the parameters accounts_user.h states,
 * and more under the sanitizers — so this test hashes as few times as it can and still check what it
 * is about.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Long enough to be accepted, and nothing anybody would use. */
#define PASSWORD     "a correct horse battery staple"
#define REPLACEMENT  "a different horse entirely, also long"

/** Who is asking, which is what the throttle counts against. */
#define ADDRESS "203.0.113.9"

/** Opens a fresh in-memory database with the accounts tables on it. */
static NYA_Database* open_accounts(NYA_Arena* arena) {
  // the throttle outlives a database, being about traffic rather than about rows, so each case starts
  // with a clean one or the failures of the last test would be waited out in this one.
  nya_account_throttle_reset();

  NYA_Database* db = nullptr;
  NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
  NYA_EXPECT(nya_accounts_open(arena, db));
  return db;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_accounts");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: what a name is folded to, which is what uniqueness is decided on
  // ─────────────────────────────────────────────────────────────────────────────
  {
    char folded[NYA_ACCOUNTS_MAX_USERNAME] = { 0 };

    nya_check(nya_account_username_normalize("Ada", folded, sizeof(folded)), "a name folds");
    nya_check(nya_string_equals(folded, "ada"), "to lower case, got '%s'", folded);

    nya_check(!nya_account_username_normalize("", folded, sizeof(folded)), "an empty name is not one");
    nya_check(!nya_account_username_normalize("ada\nroot", folded, sizeof(folded)), "and neither is one with a control character in it");

    char too_long[NYA_ACCOUNTS_MAX_USERNAME + 8] = { 0 };
    nya_memset(too_long, 'a', sizeof(too_long) - 1);

    nya_check(!nya_account_username_normalize(too_long, folded, sizeof(folded)), "a name past the bound is refused rather than cut short");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: creating an account, and every reason one is refused
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    u64 before = 0;
    nya_check(nya_account_count(&before).ok && before == 0, "a fresh install has no accounts, got %llu", (unsigned long long)before);

    NYA_AccountUser ada = { 0 };
    nya_check(nya_account_create(arena, "Ada", PASSWORD, &ada).ok, "an account is made");
    nya_check(ada.id != 0, "with an id the database assigned, got %llu", (unsigned long long)ada.id);
    nya_check(nya_string_equals(ada.username, "Ada"), "keeping what was typed, got '%s'", ada.username);
    nya_check(nya_string_equals(ada.normalized, "ada"), "beside the folded form, got '%s'", ada.normalized);
    nya_check(nya_string_equals(ada.display, "Ada"), "and a display name that is the username until they say otherwise");
    nya_check(ada.roles == 0, "holding no role at all");

    // the password is in the row, and the row is not the password.
    nya_check(!nya_string_contains(ada.password, PASSWORD), "the plaintext is nowhere in the stored hash");
    nya_check(nya_string_starts_with(ada.password, "$argon2id$v=19$m="), "which says what made it, got '%.24s'", ada.password);

    // the same name in a different case is the same name.
    NYA_AccountUser clash = { 0 };
    NYA_Error       taken = nya_account_create(arena, "ADA", PASSWORD, &clash);

    nya_check(!taken.ok && taken.kind == NYA_ERROR_ALREADY_EXISTS, "a name taken in its folded form is refused");

    NYA_Error short_password = nya_account_create(arena, "bob", "short", &clash);
    nya_check(!short_password.ok, "a password under the bound is refused");
    nya_check(short_password.message[0] != '\0', "saying why, because whoever is registering may know: %s", (NYA_ConstCString)short_password.message);

    u64 after = 0;
    nya_check(nya_account_count(&after).ok && after == 1, "and only the one account exists, got %llu", (unsigned long long)after);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a login, and the one answer every failure has
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_AccountUser ada = { 0 };
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &ada));

    NYA_AccountUser found = { 0 };
    nya_check(nya_account_authenticate(arena, "ADA", PASSWORD, ADDRESS, &found).ok, "the right password lets them in, whatever case they typed");
    nya_check(found.id == ada.id, "as the account that was made, got %llu", (unsigned long long)found.id);

    NYA_Error wrong   = nya_account_authenticate(arena, "ada", "the wrong password entirely", ADDRESS, &found);
    NYA_Error missing = nya_account_authenticate(arena, "nobody", PASSWORD, ADDRESS, &found);

    nya_check(!wrong.ok && wrong.kind == NYA_ERROR_PERMISSION_DENIED, "a wrong password is refused");
    nya_check(!missing.ok && missing.kind == NYA_ERROR_PERMISSION_DENIED, "and a name that does not exist");
    nya_check(nya_string_equals((NYA_ConstCString)wrong.message, (NYA_ConstCString)missing.message),
              "in the same words, so neither says which happened: '%s' against '%s'", (NYA_ConstCString)wrong.message,
              (NYA_ConstCString)missing.message);
    nya_check(found.id == 0, "and nothing comes back either way");

    // nya_account_find is the honest one, because nobody guessing names can reach it.
    NYA_AccountUser looked_up = { 0 };
    nya_check(nya_account_find(arena, "nobody", &looked_up).kind == NYA_ERROR_NOT_FOUND, "a lookup says plainly that there is no such account");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: disabling an account refuses it in the same words and ends its sessions
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_AccountUser ada = { 0 };
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &ada));

    NYA_AccountSession session = { 0 };
    NYA_EXPECT(nya_account_session_issue(arena, ada.id, "203.0.113.9", "a browser", &session));

    NYA_AccountSession live = { 0 };
    nya_check(nya_account_session_validate(arena, session.token, &live).ok, "the session is valid to begin with");

    nya_check(nya_account_disabled_set(arena, ada.id, true).ok, "the account is disabled");

    NYA_AccountUser refused_user = { 0 };
    NYA_Error       refused      = nya_account_authenticate(arena, "ada", PASSWORD, ADDRESS, &refused_user);

    nya_check(!refused.ok && refused.kind == NYA_ERROR_PERMISSION_DENIED, "and the right password no longer lets them in");

    nya_check(!nya_account_session_validate(arena, session.token, &live).ok, "the session it already had is over too");

    // and a new one cannot be opened while it is disabled.
    NYA_AccountSession attempted = { 0 };
    nya_check(!nya_account_session_issue(arena, ada.id, nullptr, nullptr, &attempted).ok, "nor can another be opened");

    nya_check(nya_account_disabled_set(arena, ada.id, false).ok, "it is enabled again");
    nya_check(nya_account_authenticate(arena, "ada", PASSWORD, ADDRESS, &refused_user).ok, "and the password works again");
    nya_check(!nya_account_session_validate(arena, session.token, &live).ok, "while the session that was ended stays ended");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a session's token, and what a row keeps of it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_AccountUser ada = { 0 };
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &ada));

    NYA_AccountSession first = { 0 };
    NYA_EXPECT(nya_account_session_issue(arena, ada.id, "203.0.113.9", "a browser", &first));

    nya_check(first.token[0] != '\0', "a session comes back with its token");
    nya_check(first.token_hash[0] != '\0', "and with the hash of it");
    nya_check(!nya_string_contains(first.token_hash, first.token), "which is not the token, got '%s'", first.token_hash);
    nya_check(first.expires_at_s > first.created_at_s, "and an expiry after its start");

    NYA_AccountSession second = { 0 };
    NYA_EXPECT(nya_account_session_issue(arena, ada.id, nullptr, nullptr, &second));

    nya_check(!nya_string_equals(first.token, second.token), "two sessions are two tokens");

    NYA_AccountSession validated = { 0 };
    nya_check(nya_account_session_validate(arena, first.token, &validated).ok, "a token validates");
    nya_check(validated.user_id == ada.id, "as its user's, got %llu", (unsigned long long)validated.user_id);
    nya_check(validated.token[0] == '\0', "and the token never travels back out");

    nya_check(!nya_account_session_validate(arena, "not a token", &validated).ok, "something that is not a token does not");
    nya_check(!nya_account_session_validate(arena, "", &validated).ok, "and neither does nothing at all");

    // the list is what a person is shown of their own sessions.
    NYA_AccountSession* sessions = nullptr;
    u32                 count    = 0;

    nya_check(nya_account_session_list(arena, ada.id, &sessions, &count).ok && count == 2, "both sessions are listed, got %u", count);

    // revoking one leaves the other.
    nya_check(nya_account_session_revoke(arena, first.id).ok, "one is revoked");
    nya_check(nya_account_session_revoke(arena, first.id).ok, "twice, which changes nothing");

    nya_check(!nya_account_session_validate(arena, first.token, &validated).ok, "and it is no longer valid");
    nya_check(nya_account_session_validate(arena, second.token, &validated).ok, "while the other one still is");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: changing a password ends every session there is
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_AccountUser ada = { 0 };
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &ada));

    NYA_AccountSession session = { 0 };
    NYA_EXPECT(nya_account_session_issue(arena, ada.id, nullptr, nullptr, &session));

    nya_check(!nya_account_password_change(arena, ada.id, "not the current password", REPLACEMENT).ok,
              "a change that does not know the current password is refused");

    NYA_AccountSession still = { 0 };
    nya_check(nya_account_session_validate(arena, session.token, &still).ok, "and the session it did not manage to end is still there");

    nya_check(nya_account_password_change(arena, ada.id, PASSWORD, REPLACEMENT).ok, "the password changes");

    NYA_AccountUser found = { 0 };
    nya_check(!nya_account_authenticate(arena, "ada", PASSWORD, ADDRESS, &found).ok, "the old one stops working");
    nya_check(nya_account_authenticate(arena, "ada", REPLACEMENT, ADDRESS, &found).ok, "the new one works");
    nya_check(found.password_changed_at_s >= found.created_at_s, "and the row says when it happened");

    nya_check(!nya_account_session_validate(arena, session.token, &still).ok, "every session is over, which is the whole reason to change one");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the cost a hash was made with, and when one is worth replacing
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_AccountUser ada = { 0 };
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &ada));

    nya_check(!nya_account_password_needs_rehash(&ada), "a hash this build just made is not worth replacing");

    // a hash from a server that used to be cheaper, which is what raising the parameters leaves behind.
    NYA_AccountUser older = ada;
    (void)snprintf(older.password, sizeof(older.password), "$argon2id$v=19$m=8,t=1,p=1$c2FsdHNhbHRzYWx0$aGFzaGhhc2hoYXNoaGFzaA");

    nya_check(nya_account_password_needs_rehash(&older), "one made cheaper is");

    // and one this cannot read at all, which is also one to replace.
    NYA_AccountUser strange = ada;
    (void)snprintf(strange.password, sizeof(strange.password), "$2y$10$somethingelseentirely");

    nya_check(nya_account_password_needs_rehash(&strange), "and so is one from some other scheme");

    NYA_AccountUser empty = ada;
    empty.password[0] = '\0';

    nya_check(nya_account_password_needs_rehash(&empty), "and an empty one");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a user holds only so many sessions, and the oldest goes first
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_AccountUser ada = { 0 };
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &ada));

    NYA_AccountSession first = { 0 };
    NYA_EXPECT(nya_account_session_issue(arena, ada.id, nullptr, nullptr, &first));

    for (u32 index = 1; index < NYA_ACCOUNTS_MAX_SESSIONS_PER_USER; index++) {
      NYA_AccountSession filler = { 0 };
      NYA_EXPECT(nya_account_session_issue(arena, ada.id, nullptr, nullptr, &filler));
    }

    NYA_AccountSession alive = { 0 };
    nya_check(nya_account_session_validate(arena, first.token, &alive).ok, "at the limit the first session is still there");

    NYA_AccountSession over_the_limit = { 0 };
    nya_check(nya_account_session_issue(arena, ada.id, nullptr, nullptr, &over_the_limit).ok, "one past it still opens");
    nya_check(!nya_account_session_validate(arena, first.token, &alive).ok, "and it is the oldest that was ended, not this one");
    nya_check(nya_account_session_validate(arena, over_the_limit.token, &alive).ok, "which is still valid");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: revoking everything, and clearing out what nobody will look at again
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_AccountUser ada = { 0 };
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &ada));

    NYA_AccountSession first  = { 0 };
    NYA_AccountSession second = { 0 };

    NYA_EXPECT(nya_account_session_issue(arena, ada.id, nullptr, nullptr, &first));
    NYA_EXPECT(nya_account_session_issue(arena, ada.id, nullptr, nullptr, &second));

    u32 ended = 0;
    nya_check(nya_account_session_revoke_all(arena, ada.id, &ended).ok && ended == 2, "both sessions end at once, got %u", ended);

    NYA_AccountSession gone = { 0 };
    nya_check(!nya_account_session_validate(arena, first.token, &gone).ok, "the first is over");
    nya_check(!nya_account_session_validate(arena, second.token, &gone).ok, "and the second");

    // revoked rows are kept, so the list can still say it happened, until something prunes them.
    NYA_AccountSession* listed = nullptr;
    u32                 count  = 0;

    nya_check(nya_account_session_list(arena, ada.id, &listed, &count).ok && count == 2, "and both are still listed, got %u", count);

    u32 removed = 0;
    nya_check(nya_account_session_prune(arena, 0, &removed).ok && removed == 2, "a prune that keeps nothing removes them, got %u", removed);

    nya_check(nya_account_session_list(arena, ada.id, &listed, &count).ok && count == 0, "leaving nothing behind, got %u", count);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: deleting an account, which is the thing disabling one is not
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_AccountUser ada = { 0 };
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &ada));

    NYA_AccountSession session = { 0 };
    NYA_EXPECT(nya_account_session_issue(arena, ada.id, nullptr, nullptr, &session));

    nya_check(nya_account_destroy(arena, ada.id).ok, "the account is deleted");

    NYA_AccountUser gone = { 0 };
    nya_check(nya_account_find(arena, "ada", &gone).kind == NYA_ERROR_NOT_FOUND, "and is no longer there");
    nya_check(!nya_account_authenticate(arena, "ada", PASSWORD, ADDRESS, &gone).ok, "its password opens nothing");

    NYA_AccountSession orphan = { 0 };
    nya_check(!nya_account_session_validate(arena, session.token, &orphan).ok, "and the session it held went with it");

    NYA_AccountSession* listed = nullptr;
    u32                 count  = 0;

    nya_check(nya_account_session_list(arena, ada.id, &listed, &count).ok && count == 0, "leaving no rows behind, got %u", count);

    nya_check(!nya_account_destroy(arena, ada.id).ok, "deleting it twice is not a thing that works");

    // the name is free again, because nothing is holding it.
    NYA_AccountUser again = { 0 };
    nya_check(nya_account_create(arena, "ada", PASSWORD, &again).ok, "and the username can be taken again");

    // the id may well be the old one: sqlite hands a freed rowid back out, which is why an id is only
    // ever an identity for as long as the row exists. What matters is that nothing came with it.
    nya_check(again.created_at_s >= ada.created_at_s, "by an account made after the old one");
    nya_check(nya_account_session_list(arena, again.id, &listed, &count).ok && count == 0, "holding no sessions, got %u", count);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: guessing is slowed, and a right password clears what the wrong ones cost
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_AccountUser ada = { 0 };
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &ada));

    u32 wait_s = 0;
    nya_check(nya_account_throttle_check("ada", ADDRESS, &wait_s) == NYA_ACCOUNT_THROTTLE_ALLOW, "nothing is owed to begin with");

    // the free attempts, which are what a mistyped password gets.
    for (u32 index = 0; index < NYA_ACCOUNTS_THROTTLE_FREE_ATTEMPTS; index++) {
      nya_account_throttle_fail("ada", ADDRESS);
    }

    nya_check(nya_account_throttle_check("ada", ADDRESS, &wait_s) == NYA_ACCOUNT_THROTTLE_ALLOW, "and they cost nothing, got %u s", wait_s);

    nya_account_throttle_fail("ada", ADDRESS);

    nya_check(nya_account_throttle_check("ada", ADDRESS, &wait_s) == NYA_ACCOUNT_THROTTLE_WAIT, "the one after them buys a wait");
    nya_check(wait_s > 0 && wait_s <= NYA_ACCOUNTS_THROTTLE_MAX_WAIT_S, "of a bounded number of seconds, got %u", wait_s);

    // the same name from somewhere else is still throttled: that is what the username counter is for.
    nya_check(nya_account_throttle_check("ada", "198.51.100.7", &wait_s) == NYA_ACCOUNT_THROTTLE_WAIT, "wherever the next guess comes from");

    // and folded, so a different spelling is not a different budget.
    nya_check(nya_account_throttle_check("ADA", "198.51.100.7", &wait_s) == NYA_ACCOUNT_THROTTLE_WAIT, "however it is spelt");

    // another name from another address is untouched, so one account cannot lock out the service.
    nya_check(nya_account_throttle_check("bob", "198.51.100.7", &wait_s) == NYA_ACCOUNT_THROTTLE_ALLOW, "while everybody else carries on");

    // the wait grows with each wrong answer.
    u32 first = 0;
    (void)nya_account_throttle_check("ada", ADDRESS, &first);

    nya_account_throttle_fail("ada", ADDRESS);

    u32 second = 0;
    (void)nya_account_throttle_check("ada", ADDRESS, &second);

    nya_check(second > first, "and the wait doubles, got %u s after %u s", second, first);

    // a login inside the wait is refused without hashing, in the same words as every other refusal.
    NYA_AccountUser found  = { 0 };
    NYA_Error       during = nya_account_authenticate(arena, "ada", PASSWORD, ADDRESS, &found);

    nya_check(!during.ok && during.kind == NYA_ERROR_PERMISSION_DENIED, "the right password is refused while a wait is owed");
    nya_check(found.id == 0, "and nothing comes back");

    // a right password clears it, which is the difference between slowing somebody and locking them out.
    nya_account_throttle_succeed("ada", ADDRESS);

    nya_check(nya_account_throttle_check("ada", ADDRESS, &wait_s) == NYA_ACCOUNT_THROTTLE_ALLOW, "and a right answer clears what is owed");
    nya_check(nya_account_authenticate(arena, "ada", PASSWORD, ADDRESS, &found).ok, "so the account is reachable again");

    // a login with nobody to name is still a login: there is simply no counter to key on.
    nya_check(nya_account_authenticate(arena, "ada", PASSWORD, nullptr, &found).ok, "an address nobody recorded is allowed");

    nya_check(nya_account_throttle_count() > 0, "the table holds what it has seen, got %u", nya_account_throttle_count());

    nya_account_throttle_reset();

    nya_check(nya_account_throttle_count() == 0, "and forgets all of it when asked");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: roles are a field here and a question `permission` answers
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_AccountUser ada = { 0 };
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &ada));

    nya_check(nya_account_roles_set(arena, ada.id, 0b1010).ok, "roles are set");

    NYA_AccountUser reloaded = { 0 };
    nya_check(nya_account_find_by_id(arena, ada.id, &reloaded).ok, "and read back");
    nya_check(reloaded.roles == 0b1010, "as what was written, got %llu", (unsigned long long)reloaded.roles);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the same person arriving through Steam, and then through Discord
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    char folded[NYA_ACCOUNTS_MAX_PROVIDER] = { 0 };

    nya_check(nya_account_provider_normalize("Steam", folded, sizeof(folded)), "a provider name folds");
    nya_check(nya_string_equals(folded, "steam"), "to lower case, got '%s'", folded);
    nya_check(nya_account_provider_normalize("accounts.google.com", folded, sizeof(folded)), "and an issuer host is one");
    nya_check(!nya_account_provider_normalize("two words", folded, sizeof(folded)), "while something with a space in it is not");
    nya_check(!nya_account_provider_normalize("", folded, sizeof(folded)), "and neither is nothing");

    // first sign-in: there is no account yet, so one is made.
    NYA_AccountUser player  = { 0 };
    b8              created = false;

    nya_check(nya_account_from_identity(arena, "steam", "76561198000000000", "ada", &player, &created).ok, "a first sign-in is answered");
    nya_check(created, "by making an account");
    nya_check(player.id != 0, "with an id, got %llu", (unsigned long long)player.id);
    nya_check(player.password[0] == '\0', "and no password at all");

    // which means no password gets in, whatever it is.
    NYA_AccountUser refused = { 0 };
    nya_check(!nya_account_authenticate(arena, player.username, "", ADDRESS, &refused).ok, "an empty password is not a way in");
    nya_check(!nya_account_authenticate(arena, player.username, PASSWORD, ADDRESS, &refused).ok, "and nor is any other");

    // second sign-in: the same subject is the same person, not a second account.
    NYA_AccountUser again        = { 0 };
    b8              created_again = false;

    nya_check(nya_account_from_identity(arena, "STEAM", "76561198000000000", "ada", &again, &created_again).ok, "a second sign-in is answered");
    nya_check(!created_again, "without making anything");
    nya_check(again.id == player.id, "as the same account, got %llu against %llu", (unsigned long long)again.id, (unsigned long long)player.id);

    u64 accounts = 0;
    nya_check(nya_account_count(&accounts).ok && accounts == 1, "so there is one account, got %llu", (unsigned long long)accounts);

    // and a different subject is a different person.
    NYA_AccountUser other = { 0 };
    nya_check(nya_account_from_identity(arena, "steam", "76561198000000001", "bob", &other, &created_again).ok, "somebody else signs in");
    nya_check(other.id != player.id, "and is somebody else");

    // the same person links Discord to the account they already have.
    nya_check(nya_account_identity_link(arena, player.id, "discord", "308994132968210433", "ada#0001").ok, "a second provider is linked");

    NYA_AccountUser found = { 0 };
    nya_check(nya_account_find_by_identity(arena, "discord", "308994132968210433", &found).ok, "and finds that account");
    nya_check(found.id == player.id, "which is the one they already had, got %llu", (unsigned long long)found.id);

    NYA_AccountIdentity* linked = nullptr;
    u32                  count  = 0;

    nya_check(nya_account_identity_list(arena, player.id, &linked, &count).ok && count == 2, "both ways in are listed, got %u", count);

    // what is refused: somebody else's subject, a second id for one provider, an email as a subject.
    nya_check(!nya_account_identity_link(arena, other.id, "discord", "308994132968210433", "bob").ok, "a subject cannot be taken from its account");
    nya_check(!nya_account_identity_link(arena, player.id, "steam", "76561198000000009", "ada").ok, "one provider is one login per account");
    nya_check(!nya_account_identity_link(arena, player.id, "google", "ada@example.test", "ada").ok, "an email is not a subject");
    nya_check(!nya_account_identity_link(arena, 999999, "google", "123", "nobody").ok, "and an account that is not there has no logins");

    // linking the same one again is how a display name is refreshed, not an error.
    nya_check(nya_account_identity_link(arena, player.id, "discord", "308994132968210433", "ada the second").ok, "the same link again is fine");
    nya_check(nya_account_identity_list(arena, player.id, &linked, &count).ok && count == 2, "and adds nothing, got %u", count);

    // a ban is a ban however somebody arrives.
    nya_check(nya_account_disabled_set(arena, player.id, true).ok, "the account is disabled");
    nya_check(!nya_account_from_identity(arena, "steam", "76561198000000000", "ada", &again, &created_again).ok, "and Steam no longer gets them in");
    nya_check(nya_account_disabled_set(arena, player.id, false).ok, "it is enabled again");

    // the last way in stays, so nobody unlinks themselves out of their own account.
    nya_check(nya_account_identity_unlink(arena, player.id, "discord").ok, "one of two is unlinked");
    nya_check(!nya_account_identity_unlink(arena, player.id, "steam").ok, "and the last one is not");

    nya_check(nya_account_password_reset(arena, player.id, PASSWORD).ok, "a password is set");
    nya_check(nya_account_identity_unlink(arena, player.id, "steam").ok, "and now the last identity may go");

    nya_check(nya_account_authenticate(arena, player.username, PASSWORD, ADDRESS, &found).ok, "leaving the password as the way in");

    // deleting the account takes its identities with it.
    nya_check(nya_account_destroy(arena, other.id).ok, "the other account is deleted");
    nya_check(nya_account_find_by_identity(arena, "steam", "76561198000000001", &found).kind == NYA_ERROR_NOT_FOUND, "and its Steam login with it");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: recovery codes get somebody back in, once each, and the throttle covers them
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    NYA_AccountUser ada = { 0 };
    NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &ada));

    char codes[NYA_ACCOUNTS_RECOVERY_CODE_COUNT][NYA_ACCOUNTS_RECOVERY_CODE_TEXT] = { 0 };
    u32  made                                                                     = 0;

    nya_check(nya_account_recovery_generate(arena, ada.id, codes, &made).ok, "codes are generated");
    nya_check(made == NYA_ACCOUNTS_RECOVERY_CODE_COUNT, "the full set, got %u", made);
    nya_check(codes[0][0] != '\0' && codes[9][0] != '\0', "each with text");
    nya_check(strchr(codes[0], '-') != nullptr, "grouped for reading, got '%s'", codes[0]);

    u32 remaining = 0;
    nya_check(nya_account_recovery_remaining(arena, ada.id, &remaining).ok && remaining == 10, "all ten are unused, got %u", remaining);

    // a code proves the account is theirs, and is accepted however it is spaced or cased.
    NYA_AccountUser who = { 0 };
    char            messy[32] = { 0 };
    (void)snprintf(messy, sizeof(messy), " %s ", codes[3]);
    for (u64 i = 0; messy[i] != '\0'; i++) if (messy[i] >= 'A' && messy[i] <= 'Z') messy[i] = (char)(messy[i] - 'A' + 'a');

    nya_check(nya_account_recovery_consume(arena, "ada", messy, ADDRESS, &who).ok, "a code lets them in, spacing and case and all");
    nya_check(who.id == ada.id, "as their account, got %llu", (unsigned long long)who.id);

    // single use: the same code does not work twice.
    nya_check(!nya_account_recovery_consume(arena, "ada", codes[3], ADDRESS, &who).ok, "a spent code does not work again");
    nya_check(nya_account_recovery_remaining(arena, ada.id, &remaining).ok && remaining == 9, "leaving nine, got %u", remaining);

    // a wrong code and a wrong username are refused in the same words.
    NYA_Error wrong_code = nya_account_recovery_consume(arena, "ada", "AAAA-AAAA", ADDRESS, &who);
    NYA_Error wrong_user = nya_account_recovery_consume(arena, "nobody", codes[4], ADDRESS, &who);
    nya_check(!wrong_code.ok && wrong_code.kind == NYA_ERROR_PERMISSION_DENIED, "a wrong code is refused");
    nya_check(!wrong_user.ok && wrong_user.kind == NYA_ERROR_PERMISSION_DENIED, "and a wrong username");
    nya_check(nya_string_equals((NYA_ConstCString)wrong_code.message, (NYA_ConstCString)wrong_user.message), "in the same words");

    // regenerating replaces the set: every old code stops working.
    nya_account_throttle_reset();
    char again[NYA_ACCOUNTS_RECOVERY_CODE_COUNT][NYA_ACCOUNTS_RECOVERY_CODE_TEXT] = { 0 };
    nya_check(nya_account_recovery_generate(arena, ada.id, again, &made).ok, "a new set is generated");
    nya_check(nya_account_recovery_remaining(arena, ada.id, &remaining).ok && remaining == 10, "ten again, got %u", remaining);

    nya_check(!nya_account_recovery_consume(arena, "ada", codes[5], ADDRESS, &who).ok, "an old code no longer works");
    nya_check(nya_account_recovery_consume(arena, "ada", again[0], ADDRESS, &who).ok, "and a new one does");

    // recovery is what stands in for a lost password: consume, then reset, which ends every session.
    nya_account_throttle_reset();
    NYA_AccountSession session = { 0 };
    NYA_EXPECT(nya_account_session_issue(arena, ada.id, nullptr, nullptr, &session));

    NYA_AccountUser recovered = { 0 };
    nya_check(nya_account_recovery_consume(arena, "ada", again[1], ADDRESS, &recovered).ok, "a code recovers the account");
    nya_check(nya_account_password_reset(arena, recovered.id, REPLACEMENT).ok, "a new password is set");

    NYA_AccountSession gone = { 0 };
    nya_check(!nya_account_session_validate(arena, session.token, &gone).ok, "and the old session is over");
    nya_check(nya_account_authenticate(arena, "ada", REPLACEMENT, ADDRESS, &recovered).ok, "and the new password works");

    // deleting the account takes its codes with it.
    nya_account_throttle_reset();
    nya_check(nya_account_destroy(arena, ada.id).ok, "the account is deleted");
    NYA_AccountUser back = { 0 };
    nya_check(!nya_account_recovery_consume(arena, "ada", again[2], ADDRESS, &back).ok, "and its codes are gone with it");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: registration policy — open, invite-only, and closed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_accounts(arena);
    defer nya_accounts_close();
    defer nya_sql_close(db);

    // the owner, made directly as the CLI bootstrap does, whatever the policy.
    NYA_AccountUser owner = { 0 };
    NYA_EXPECT(nya_account_create(arena, "owner", PASSWORD, &owner));

    // OPEN: anybody registers, no code needed.
    NYA_AccountUser open_user = { 0 };
    nya_check(nya_account_register(arena, NYA_ACCOUNT_REGISTRATION_OPEN, "bob", PASSWORD, nullptr, &open_user).ok, "open registration takes anybody");
    nya_check(open_user.id != 0 && open_user.roles == 0, "with an account and no roles");

    // CLOSED: nobody self-registers.
    NYA_AccountUser closed_user = { 0 };
    NYA_Error closed = nya_account_register(arena, NYA_ACCOUNT_REGISTRATION_CLOSED, "carol", PASSWORD, nullptr, &closed_user);
    nya_check(!closed.ok && closed.kind == NYA_ERROR_PERMISSION_DENIED, "closed registration refuses everyone");

    // INVITE: a code is needed, and a bad one is refused.
    NYA_AccountUser no_code = { 0 };
    nya_check(!nya_account_register(arena, NYA_ACCOUNT_REGISTRATION_INVITE, "dave", PASSWORD, nullptr, &no_code).ok, "invite-only needs a code");
    nya_check(!nya_account_register(arena, NYA_ACCOUNT_REGISTRATION_INVITE, "dave", PASSWORD, "AAAA-AAAA-AAAA-AAAA", &no_code).ok, "and a real one");

    // the owner hands out a code.
    char code[NYA_ACCOUNTS_INVITE_CODE_TEXT] = { 0 };
    nya_check(nya_account_invite_issue(arena, owner.id, 7 * 24 * 3600, code, sizeof(code)).ok, "an invite is made");
    nya_check(code[0] != '\0' && strchr(code, '-') != nullptr, "with a grouped code, got '%s'", code);
    char never[NYA_ACCOUNTS_INVITE_CODE_TEXT] = { 0 };
    nya_check(!nya_account_invite_issue(arena, owner.id, 0, never, sizeof(never)).ok, "an invite with no expiry is refused");

    // a taken username fails and leaves the code unused, so a second try with it still works.
    NYA_AccountUser clash = { 0 };
    nya_check(!nya_account_register(arena, NYA_ACCOUNT_REGISTRATION_INVITE, "owner", PASSWORD, code, &clash).ok, "a taken name fails");

    NYA_AccountUser dave = { 0 };
    nya_check(nya_account_register(arena, NYA_ACCOUNT_REGISTRATION_INVITE, "dave", PASSWORD, code, &dave).ok, "and the code is still good after");
    nya_check(dave.id != 0, "so dave gets in");

    // single use: the same code does not work twice.
    NYA_AccountUser eve = { 0 };
    nya_check(!nya_account_register(arena, NYA_ACCOUNT_REGISTRATION_INVITE, "eve", PASSWORD, code, &eve).ok, "a spent code is done");

    // the owner sees who they let in.
    NYA_AccountInvite* listed = nullptr;
    u32                count  = 0;
    nya_check(nya_account_invite_list(arena, owner.id, &listed, &count).ok && count == 1, "the owner's invite is listed, got %u", count);
    nya_check(listed[0].used_by == dave.id, "showing it was dave who used it");

    // an unused invite can be revoked before it is spent.
    char code2[NYA_ACCOUNTS_INVITE_CODE_TEXT] = { 0 };
    NYA_EXPECT(nya_account_invite_issue(arena, owner.id, 3600, code2, sizeof(code2)));
    nya_check(nya_account_invite_revoke(arena, code2).ok, "an unused invite is revoked");
    NYA_AccountUser frank = { 0 };
    nya_check(!nya_account_register(arena, NYA_ACCOUNT_REGISTRATION_INVITE, "frank", PASSWORD, code2, &frank).ok, "and no longer works");

    // a sweep clears the spent and expired invites, keeping the unused-and-valid ones.
    u32 removed = 0;
    nya_check(nya_account_invite_prune(arena, 0, &removed).ok && removed >= 1, "a prune clears the spent invite, got %u", removed);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: what every call answers before the tables are open
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_check(!nya_accounts_is_open(), "the tables are closed between tests");

    NYA_AccountUser user = { 0 };
    nya_check(!nya_account_create(arena, "ada", PASSWORD, &user).ok, "nothing is created without them");
    nya_check(!nya_account_find(arena, "ada", &user).ok, "and nothing is found");

    NYA_AccountSession session = { 0 };
    nya_check(!nya_account_session_validate(arena, "anything", &session).ok, "and no token is a session");

    u64 count = 0;
    nya_check(!nya_account_count(&count).ok, "and there is nothing to count");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}

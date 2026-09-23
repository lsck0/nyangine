/**
 * @file accounts.h
 *
 * ── the accounts module ──
 *
 * accounts_user.h      who somebody is: the row, the password, and what may be done to an account
 * accounts_session.h   the fact that they are logged in right now, and how that ends
 * accounts_throttle.h  what a wrong password costs the next one, so guessing is not free
 * accounts_identity.h  the same person arriving through Steam, through Discord, or with a password
 * accounts_recovery.h  the codes that get somebody back in when the password is gone
 *
 * Users and sessions over `db`, `crypto` and `permission`. What this module is for is the part of an
 * application every application writes again and worse: a password that is stored as a hash nobody
 * can reverse, a login that says the same thing whoever asks, a session that can be ended from
 * somewhere else, and an account that can be disabled without being deleted.
 *
 * ```c
 * NYA_TRY(nya_accounts_open(arena, database));
 * defer nya_accounts_close();
 *
 * NYA_AccountUser ada = { 0 };
 * NYA_TRY(nya_account_create(arena, "ada", "a long passphrase nobody else knows", &ada));
 *
 * NYA_AccountUser found = { 0 };
 * NYA_Error       allowed = nya_account_authenticate(arena, "ada", attempt, &found);
 *
 * if (allowed.ok) {
 *     NYA_AccountSession session = { 0 };
 *     NYA_TRY(nya_account_session_issue(arena, found.id, "203.0.113.9", "a browser", &session));
 * }
 * ```
 *
 * ── what this module decides, and what it leaves to a program ──
 *
 * It decides how a password is stored, what a login answers, when a session stops being valid, and
 * what ends every other session. It decides none of the policy above that: whether registration is
 * open, what a route does with a failed login, how a session travels (a cookie, a bearer token, a
 * control socket), or what a user may do once they are in — the last of those is `permission`'s, and
 * a user's roles are a field here that `permission` reads.
 *
 * That split is why this sits below `http` rather than inside it: a program with no HTTP server at
 * all — a CLI creating the first account, a game with a control socket — has users and sessions for
 * the same reasons a web server does.
 *
 * ── the shape of a failure ──
 *
 * Every refusal a login can produce is the same error, with the same message and the same cost: a
 * username that does not exist is verified against a hash that belongs to nobody, so the answer takes
 * as long as a real one and says as little. An account that is disabled, a password that is wrong and
 * a user who was never there are one answer on purpose — the alternative is an endpoint that tells an
 * attacker which usernames exist, which is the first thing they want.
 *
 * Everything else — creating an account, changing a password — says exactly what went wrong, because
 * the person doing it is already known.
 *
 * ── the tables ──
 *
 * `accounts` and `account_sessions`, created by nya_accounts_open through the ORM, which derives them
 * from the two reflected structs and migrates them the way db_migrate.h describes. A program that
 * wants them somewhere other than its main database opens a second `NYA_Database` and hands that in.
 *
 * Thread safety: none. One thread owns the tables, as with everything over `db`.
 * */
#pragma once

#include "nyangine/accounts/accounts_identity.h"
#include "nyangine/accounts/accounts_recovery.h"
#include "nyangine/accounts/accounts_session.h"
#include "nyangine/accounts/accounts_throttle.h"
#include "nyangine/accounts/accounts_user.h"

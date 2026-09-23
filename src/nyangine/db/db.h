/**
 * @file db.h
 *
 * Storage: one database file, a reflected struct stored as a row, and a schema kept level with the
 * structs by a migration derived from them rather than written by hand.
 *
 * Overview:
 *   db_sql.h        the connection, bound statements, transactions, and the key the file is under
 *   db_orm.h        a described type bound to a table: insert, update, delete, find, select
 *
 * It sits below `http` and `accounts` and above `base` and `crypto`: everything that has to survive a
 * restart — sessions, accounts, the permission cache, a bot's state — is a table here, and nothing in
 * this module knows any of them by name.
 *
 * ```c
 * NYA_Database* database = nullptr;
 * NYA_TRY(nya_sql_open(arena, "./server.db", &database));
 * defer nya_sql_close(database);
 *
 * NYA_OrmTable* notes = nullptr;
 * NYA_TRY(nya_orm_open(arena, database, nya_reflect_of(Note), "notes", &notes));
 * defer nya_orm_close(notes);
 *
 * NYA_TRY(nya_orm_schema_create(notes));
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * ENCRYPTION AT REST
 * ─────────────────────────────────────────────────────────
 *
 * The plan is SQLCipher in place of the vendored sqlite, so the whole file is encrypted; see
 * "Decisions", encryption at rest, in TODO.md. What is here is the seam and not the cipher:
 * nya_sql_open takes `.key`, 32 bytes, and nya_sql_encryption_available says whether this build can
 * honour one. It cannot, anywhere, today — the vendored sqlite has no cipher — so a key is refused
 * before the file is touched rather than written in the clear and not mentioned.
 *
 * What that leaves uncovered until SQLCipher is vendored: every byte of every database this module
 * writes, the temporary files sqlite writes beside it, and the write-ahead log. A password hash, a
 * session token or a TOTP secret stored through here is readable by anyone who can read the disk,
 * so anything of that kind has to be encrypted by whoever stores it, or not stored.
 *
 * ─────────────────────────────────────────────────────────
 * ONE THREAD PER CONNECTION
 * ─────────────────────────────────────────────────────────
 *
 * Nothing in this module is thread safe, exactly as the connection underneath is opened NOMUTEX. A
 * connection and every table over it belong to one thread. The threaded server in TODO.md's Phase 3
 * gives each worker its own connection, which is the shape this is written for.
 * */
#pragma once

#include "nyangine/db/db_sql.h"
// After db_sql.h: a table binds a described type to a connection and takes its key as an NYA_SqlValue.
#include "nyangine/db/db_orm.h"

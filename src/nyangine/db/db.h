/**
 * @file db.h
 *
 * Storage: one database file, a reflected struct stored as a row, and a schema kept level with the
 * structs by a migration derived from them rather than written by hand.
 *
 * Overview:
 *   db_sql.h        the connection, bound statements, transactions, and the key the file is under
 *   db_backup.h     a hot snapshot of a live database, a WAL checkpoint, and a defragmenting copy
 *   db_orm.h        a described type bound to a table: insert, update, delete, find, select
 *   db_migrate.h    what two schemas differ by, what of that is derivable, and what is refused
 *   db_blob.h       a content-addressed blob store: an object keyed by the SHA-256 of its own bytes
 *   db_jobs.h       a persistent job queue: retries, exponential backoff, deadlines, unique jobs
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
 * // Creates the table, or grows the one that is there by the columns the struct has gained.
 * NYA_TRY(nya_orm_schema_migrate(notes));
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
// After db_sql.h: the backup takes a connection and copies what is behind it to another file.
#include "nyangine/db/db_backup.h"
// After db_sql.h: a table binds a described type to a connection and takes its key as an NYA_SqlValue.
#include "nyangine/db/db_orm.h"
// After db_orm.h: a plan is derived from a table's columns and the schema the database has.
#include "nyangine/db/db_migrate.h"
// After db_sql.h, which it stores objects through: a content-addressed blob store, one SQLite table.
#include "nyangine/db/db_blob.h"
// After db_sql.h: a persistent job queue in one SQLite table.
#include "nyangine/db/db_jobs.h"
// After db_jobs.h: a pool of threads, each on its own connection, that runs that queue's jobs in-process.
#include "nyangine/db/db_jobworker.h"
